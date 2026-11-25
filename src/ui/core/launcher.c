/* launcher.c — ver launcher.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "launcher.h"
#include "apps.h"
#include "desktop.h"
#include "settings.h"
#include "webdata.h"
#include "nav.h"
#include "theme.h"
#include "../screens/screens.h"
#include "../widgets/tile.h"
#include "../assets/icons/bf_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool opened;

/* ------------------------------------------------------------ categorias */

static int cat_of_key(const char *key)
{
    for(int g = 0; g < BF_GRP__N; g++) if(key && !strcmp(desktop_group_key((bf_group_t)g), key)) return g;
    return -1;
}

static const char *cat_name(const char *key)
{
    int g = cat_of_key(key);
    return g >= 0 ? desktop_group_name((bf_group_t)g) : NULL;
}

/* ------------------------------------------------------------ escolhas de aparência */

static const struct { const char *key; const lv_image_dsc_t *img; } icon_tab[] = {
    { "folder", &bf_icon_files },     { "games", &bf_icon_games },   { "media", &bf_icon_multimedia },
    { "tv", &bf_icon_tv },            { "music", &bf_icon_headphones }, { "web", &bf_icon_web },
    { "photo", &bf_icon_gallery },    { "book", &bf_icon_book },     { "tools", &bf_icon_run },
    { "code", &bf_icon_terminal },    { "system", &bf_icon_settings }, { "phone", &bf_icon_phone },
    { "map", &bf_icon_gps },          { "radio", &bf_icon_fm },      { "screen", &bf_icon_bigscreen },
    { "chart", &bf_icon_monitor },
};
#define N_ICONS (int)(sizeof icon_tab / sizeof icon_tab[0])

static const uint32_t color_tab[] = {
    0x3584e4, 0x7a4cd9, 0xd6409f, 0xe5534b, 0xe0a40c, 0x2ec27e, 0x1fb5cc, 0x6c7383,
};
#define N_COLORS (int)(sizeof color_tab / sizeof color_tab[0])
#define USER_COLOR 0x3584e4

int launcher_icon_choices(void) { return N_ICONS; }
const char *launcher_icon_choice_key(int i) { return i >= 0 && i < N_ICONS ? icon_tab[i].key : ""; }
const lv_image_dsc_t *launcher_icon_choice(int i) { return i >= 0 && i < N_ICONS ? icon_tab[i].img : NULL; }
int launcher_color_choices(void) { return N_COLORS; }
uint32_t launcher_color_choice(int i) { return i >= 0 && i < N_COLORS ? color_tab[i] : USER_COLOR; }

/* ------------------------------------------------------------ IDs */

void launcher_id_pkg(char *out, size_t n, const char *pkg_id) { snprintf(out, n, "pkg:%s", pkg_id); }

static int sys_index(const char *key)
{
    for(int i = 0; i < home_app_count(); i++) if(!strcmp(home_app_key(i), key)) return i;
    return -1;
}

static int pkg_index(const char *id)
{
    for(int i = 0; i < apps_count(); i++) if(!strcmp(apps_get(i)->id, id)) return i;
    return -1;
}

bool launcher_resolve(const char *id, li_item_t *out)
{
    if(!id) return false;
    out->id = id;
    if(!strncmp(id, "sys:", 4)) { out->kind = LI_SYS; out->idx = sys_index(id + 4); }
    else if(!strncmp(id, "pkg:", 4)) { out->kind = LI_PKG; out->idx = pkg_index(id + 4); }
    else if(!strncmp(id, "web:", 4)) { out->kind = LI_WEB; out->idx = web_app_find(id + 4); }
    else if(ag_marker_gid(id)) {
        out->kind = LI_GROUP;
        out->idx = ag_marker_gid(id);
        return ag_group_get(out->idx) != NULL;
    }
    else return false;
    return out->idx >= 0;
}

/* ------------------------------------------------------------ sincronização */

static void ensure_open(void)
{
    if(opened) return;
    char p[300];
    data_path(p, sizeof p, "appgroups.conf");
    ag_set_namer(cat_name);
    ag_open(p);
    opened = true;
}

void launcher_reopen(void) { opened = false; }
void launcher_load(void) { ensure_open(); }

void launcher_sync(void)
{
    ensure_open();
    web_apps_load();
    int ns = home_app_count(), np = apps_count(), nw = web_apps_count();
    int n = 0, cap = ns + np + nw + 1;
    ag_avail_t *a = malloc(sizeof *a * (size_t)cap);
    char (*ids)[1100] = malloc(sizeof *ids * (size_t)cap);
    if(!a || !ids) { free(a); free(ids); return; }
    for(int i = 0; i < ns; i++) {
        snprintf(ids[n], sizeof ids[n], "sys:%s", home_app_key(i));
        a[n] = (ag_avail_t){ ids[n], NULL };
        n++;
    }
    a[n++] = (ag_avail_t){ NULL, "games" };              /* Jogos aparece mesmo vazio (migração) */
    for(int i = 0; i < np; i++) {
        const bf_app_entry_t *e = apps_get(i);
        snprintf(ids[n], sizeof ids[n], "pkg:%s", e->id);
        a[n] = (ag_avail_t){ ids[n], desktop_group_key(apps_group_of(e)) };
        n++;
    }
    for(int j = 0; j < nw; j++) {
        snprintf(ids[n], sizeof ids[n], "web:%s", web_app(j)->url);
        a[n] = (ag_avail_t){ ids[n], NULL };
        n++;
    }
    ag_sync(a, n);
    free(a);
    free(ids);
}

bool launcher_kiosk(void) { return settings()->home_mode == BF_HOME_KIOSK; }

/* no quiosque o Ajustes só abre segurando 2 s na barra: fora da grade, das pastas e da busca */
static bool shown(const char *id) { return !(launcher_kiosk() && !strcmp(id, "sys:settings")); }

int launcher_items(int parent, li_item_t *out, int max)
{
    int total = ag_children(parent, NULL, 0);
    if(total <= 0) return 0;
    const char **v = malloc(sizeof *v * (size_t)total);
    if(!v) return 0;
    ag_children(parent, v, total);
    int n = 0;
    for(int i = 0; i < total && n < max; i++)
        if(shown(v[i]) && launcher_resolve(v[i], &out[n])) n++;   /* o que não existe agora: escondido */
    free(v);
    return n;
}

/* ------------------------------------------------------------ grupos */

const char *launcher_group_name(int gid) { return ag_group_name(ag_group_get(gid)); }

uint32_t launcher_group_color(int gid)
{
    const ag_group_t *g = ag_group_get(gid);
    if(!g) return USER_COLOR;
    if(g->color) return g->color;
    int c = cat_of_key(g->key);
    return c >= 0 ? desktop_group_color((bf_group_t)c) : USER_COLOR;
}

const lv_image_dsc_t *launcher_group_icon(int gid)
{
    const ag_group_t *g = ag_group_get(gid);
    if(g && g->icon[0])
        for(int i = 0; i < N_ICONS; i++) if(!strcmp(icon_tab[i].key, g->icon)) return icon_tab[i].img;
    int c = g ? cat_of_key(g->key) : -1;
    return c >= 0 ? desktop_group_icon((bf_group_t)c) : &bf_icon_files;
}

int launcher_group_size(int gid)
{
    li_item_t tmp[1];
    int total = ag_children(gid, NULL, 0);
    if(total <= 0) return 0;
    /* conta só os que existem agora */
    const char **v = malloc(sizeof *v * (size_t)total);
    if(!v) return 0;
    ag_children(gid, v, total);
    int n = 0;
    for(int i = 0; i < total; i++) if(shown(v[i]) && launcher_resolve(v[i], tmp)) n++;
    free(v);
    return n;
}

/* ------------------------------------------------------------ itens */

const char *launcher_name(const li_item_t *it, char *buf, size_t n)
{
    switch(it->kind) {
    case LI_SYS: snprintf(buf, n, "%s", home_app_name(it->idx)); break;
    case LI_PKG: snprintf(buf, n, "%s", apps_get(it->idx)->name); break;
    case LI_WEB: {
        char host[64];
        snprintf(buf, n, "%s", web_display_name(web_app(it->idx)->name, web_app(it->idx)->url, host, sizeof host));
        break;
    }
    case LI_GROUP: snprintf(buf, n, "%s", launcher_group_name(it->idx)); break;
    }
    return buf;
}

const lv_image_dsc_t *launcher_icon(const li_item_t *it)
{
    switch(it->kind) {
    case LI_SYS: return home_app_icon(it->idx);
    case LI_PKG: return apps_icon(apps_get(it->idx));
    case LI_GROUP: return launcher_group_icon(it->idx);
    default: return NULL;
    }
}

uint32_t launcher_color(const li_item_t *it)
{
    switch(it->kind) {
    case LI_SYS: return home_app_color(it->idx);
    case LI_PKG: return apps_color(apps_get(it->idx));
    case LI_WEB: return web_app(it->idx)->color;
    case LI_GROUP: return launcher_group_color(it->idx);
    }
    return USER_COLOR;
}

void launcher_open(const li_item_t *it)
{
    switch(it->kind) {
    case LI_SYS: home_app_open(it->idx); break;
    case LI_PKG: apps_launch(apps_get(it->idx)); break;
    case LI_WEB: nav_go(SCR_WEB, WEB_ARG_APP0 + it->idx); break;
    case LI_GROUP: nav_go(SCR_APPGROUP, it->idx); break;
    }
}

lv_obj_t *launcher_tile(lv_obj_t *parent, const li_item_t *it)
{
    char name[96];
    launcher_name(it, name, sizeof name);
    if(it->kind == LI_GROUP) {
        li_item_t in[4];
        int n = launcher_items(it->idx, in, 4);
        tile_mini_t m[4];
        char ini[4][8], nm[96];
        for(int i = 0; i < n; i++) {
            web_initial(launcher_name(&in[i], nm, sizeof nm), ini[i], sizeof ini[i]);
            m[i] = (tile_mini_t){ in[i].kind == LI_GROUP ? NULL : launcher_icon(&in[i]), ini[i], launcher_color(&in[i]) };
        }
        return tile_create_folder(parent, name, launcher_group_color(it->idx), m, launcher_group_size(it->idx), NULL, NULL);
    }
    const lv_image_dsc_t *ic = launcher_icon(it);
    if(ic) return tile_create(parent, ic, name, launcher_color(it), NULL, NULL);
    char ini[8];
    web_initial(name, ini, sizeof ini);
    return tile_create_text(parent, ini, name, launcher_color(it), NULL, NULL);
}

/* ------------------------------------------------------------ esquecer */

void launcher_forget_pkg(const char *pkg_id)
{
    char id[100];
    launcher_id_pkg(id, sizeof id, pkg_id);
    ensure_open();
    ag_forget(id);
}

void launcher_forget_web(const char *url)
{
    char id[1100];
    snprintf(id, sizeof id, "web:%s", url);
    ensure_open();
    ag_forget(id);
}
