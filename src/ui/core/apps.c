/* apps.c — ver apps.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include "apps.h"
#include "pkg.h"
#include "svgimg.h"
#include "settings.h"
#include "nav.h"
#include "osd.h"
#include "runner.h"
#include "gamepad.h"
#include "theme.h"
#include "../widgets/dialog.h"
#include "../screens/screens.h"
#include "i18n.h"
#include "launcher.h"
#include "../assets/icons/bf_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bf_app_entry_t *list;
static int n_list = -1;                  /* -1: ainda não lido */

void apps_dir(char *out, size_t n) { data_path(out, n, "apps"); }

/* ------------------------------------------------------------ ícones */

#define ICON_MAX 24
static struct { char path[300]; lv_image_dsc_t img; bool ok; } icons[ICON_MAX];

static void icons_flush(void)
{
    for(int i = 0; i < ICON_MAX; i++) {
        if(icons[i].ok) svg_image_free(&icons[i].img);
        memset(&icons[i], 0, sizeof icons[i]);
    }
}

const lv_image_dsc_t *apps_icon(const bf_app_entry_t *a)
{
    if(!a || !a->icon[0]) return NULL;
    int freei = -1;
    for(int i = 0; i < ICON_MAX; i++) {
        if(icons[i].path[0] && !strcmp(icons[i].path, a->icon)) return icons[i].ok ? &icons[i].img : NULL;
        if(!icons[i].path[0] && freei < 0) freei = i;
    }
    if(freei < 0) return NULL;
    snprintf(icons[freei].path, sizeof icons[freei].path, "%s", a->icon);
    /* SVG: rasterizado uma vez (o nanosvg usa float, emulado neste ARM) */
    size_t L = strlen(a->icon);
    icons[freei].ok = L > 4 && !strcasecmp(a->icon + L - 4, ".svg") && svg_render_mask(a->icon, 56, &icons[freei].img);
    return icons[freei].ok ? &icons[freei].img : NULL;
}

/* ------------------------------------------------------------ lista */

static int by_name(const void *x, const void *y)
{
    return strcasecmp(((const bf_app_entry_t *)x)->name, ((const bf_app_entry_t *)y)->name);
}

static void reload(void)
{
    free(list);
    list = NULL;
    bf_app_entry_t *all = NULL;
    int n = desktop_scan(&all), k = 0;
    for(int i = 0; i < n; i++)
        if(all[i].dir[0]) all[k++] = all[i];           /* só os instalados (pasta própria) */
    list = all;
    n_list = k;
    qsort(list, (size_t)n_list, sizeof *list, by_name);
}

static void ensure(void) { if(n_list < 0) reload(); }

int apps_count(void) { ensure(); return n_list; }
const bf_app_entry_t *apps_get(int i) { ensure(); return i >= 0 && i < n_list ? &list[i] : NULL; }

const bf_app_entry_t *apps_find(const char *id)
{
    ensure();
    for(int i = 0; i < n_list; i++) if(!strcmp(list[i].id, id)) return &list[i];
    return NULL;
}

bf_group_t apps_group_of(const bf_app_entry_t *a) { return desktop_group(a->categories); }

int apps_in_group(bf_group_t g, int *out, int max)
{
    ensure();
    int n = 0;
    for(int i = 0; i < n_list && n < max; i++) if(apps_group_of(&list[i]) == g) out[n++] = i;
    return n;
}

uint32_t apps_color(const bf_app_entry_t *a) { return a->color ? a->color : desktop_group_color(apps_group_of(a)); }

/* algo mudou: relê, refaz os ícones, procura os mapas dos controles de novo (o app pode
 * trazer um gamecontrollerdb.txt) e redesenha a grade/o grupo à vista */
static void changed(void)
{
    icons_flush();
    reload();
    gamepad_remap();
    bf_screen_t s = nav_current().scr;
    if(s == SCR_HOME || s == SCR_APPGROUP) nav_show_now();
}

/* ------------------------------------------------------------ abrir */

static const struct { const char *name; bf_screen_t scr; } screens[] = {
    { "sc-controller", SCR_SCC },
    { "big-remote-play", SCR_BRP },
};

static char term_cmd[300];

static void term_type(lv_timer_t *t)
{
    lv_timer_delete(t);
    if(nav_current().scr == SCR_TERM) scr_term_demo_type(term_cmd);
}

void apps_launch(const bf_app_entry_t *a)
{
    if(!a) return;
    if(a->screen[0]) {
        for(size_t i = 0; i < sizeof screens / sizeof screens[0]; i++)
            if(!strcmp(a->screen, screens[i].name)) { nav_go(screens[i].scr, 0); return; }
        osd_show(&bf_icon_info, 0xe0a40c, a->name, tr(S_PKG_SCREEN_MISSING));
        return;
    }
    if(a->terminal) {                                  /* no Terminal, como se tivesse digitado */
        snprintf(term_cmd, sizeof term_cmd, "%s\r", a->exec);
        nav_go(SCR_TERM, 0);
        lv_timer_create(term_type, 500, NULL);
        return;
    }
    static char buf[256];
    char *argv[32], msg[200];
    snprintf(buf, sizeof buf, "%s", a->exec);
    if(runner_split(buf, argv, 32) == 0) return;
    int rc = runner_exec(argv, false, NULL, NULL, NULL, msg, sizeof msg);
    osd_show(rc < 0 ? &bf_icon_info : apps_icon(a) ? apps_icon(a) : &bf_icon_run, apps_color(a), a->name,
             rc < 0 ? msg : tr(S_RUN_LAUNCHED));
}

/* ------------------------------------------------------------ instalar */

static char pending[600];
static pkg_info_t pinfo;

static const char *err_text(pkg_err_t e)
{
    switch(e) {
    case PKG_E_READ: return tr(S_PKG_E_READ);
    case PKG_E_FORMAT: return tr(S_PKG_E_FORMAT);
    case PKG_E_UNSAFE: return tr(S_PKG_E_UNSAFE);
    case PKG_E_BIG: return tr(S_PKG_E_BIG);
    case PKG_E_NODESKTOP: return tr(S_PKG_E_NODESKTOP);
    default: return tr(S_PKG_E_WRITE);
    }
}

static void fmt_size(char *o, size_t n, size_t b)
{
    if(b < 1024 * 1024) snprintf(o, n, "%zu KB", (b + 1023) / 1024);
    else snprintf(o, n, "%zu,%zu MB", b >> 20, ((b & 0xfffff) * 10) >> 20);
}

static void done_answer(int b, void *ud)
{
    (void)ud;
    if(b == 1) apps_launch(apps_find(pinfo.id));
}

static void fail_dialog(pkg_err_t e)
{
    const char *ok[1] = { tr(S_PKG_OK) };
    dialog_open(LV_SYMBOL_WARNING, pal()->danger, tr(S_PKG_FAIL), err_text(e), ok, 1, 0, -1, NULL, NULL);
}

static pkg_err_t install(const char *file)
{
    char dir[300];
    apps_dir(dir, sizeof dir);
    pkg_err_t e = pkg_install(file, dir, i18n_lang_code(i18n_get_lang()), &pinfo);   /* cria <dados>/apps se faltar */
    fprintf(stderr, "bigfoston: instalar %s: %s%s%s\n", file, pkg_err_name(e), e ? "" : " -> ", e ? "" : pinfo.id);
    if(!e) changed();
    return e;
}

/* o grupo onde o app instalado ficou (o da categoria, ou o que o usuário escolheu antes) */
static const char *where(const char *pkg_id)
{
    char id[100];
    launcher_id_pkg(id, sizeof id, pkg_id);
    launcher_sync();
    int g = ag_parent(id);
    return g > 0 ? launcher_group_name(g) : tr(S_GL_APPS);
}

/* sem diálogo (FIFO de controle): o aviso flutuante diz onde o app ficou */
bool apps_install_now(const char *file)
{
    if(install(file) != PKG_OK) return false;
    const bf_app_entry_t *a = apps_find(pinfo.id);
    if(a && settings()->notify) {
        char t[120];
        snprintf(t, sizeof t, tr(pinfo.installed ? S_PKG_UPDATED : S_PKG_DONE), pinfo.name);
        osd_show(apps_icon(a) ? apps_icon(a) : desktop_group_icon(apps_group_of(a)), apps_color(a), t,
                 where(a->id));
    }
    return true;
}

static void install_answer(int b, void *ud)
{
    (void)ud;
    if(b != 1) return;
    pkg_err_t e = install(pending);
    if(e) { fail_dialog(e); return; }
    char title[120], body[200];
    snprintf(title, sizeof title, tr(pinfo.installed ? S_PKG_UPDATED : S_PKG_DONE), pinfo.name);
    snprintf(body, sizeof body, tr(S_PKG_DONE_BODY), where(pinfo.id));
    const char *btn[2] = { tr(S_PKG_OK), tr(S_PKG_OPEN) };
    dialog_open(LV_SYMBOL_OK, pal()->success, title, body, btn, 2, 1, -1, done_answer, NULL);
}

void apps_install_ask(const char *file)
{
    char dir[300];
    apps_dir(dir, sizeof dir);
    pkg_err_t e = pkg_info(file, dir, i18n_lang_code(i18n_get_lang()), &pinfo);
    if(e) { fail_dialog(e); return; }
    snprintf(pending, sizeof pending, "%s", file);
    char title[120], body[300], size[32];
    snprintf(title, sizeof title, tr(pinfo.installed ? S_PKG_UPDATE_Q : S_PKG_INSTALL_Q), pinfo.name);
    fmt_size(size, sizeof size, pinfo.bytes);
    snprintf(body, sizeof body, tr(S_PKG_BODY), pinfo.comment[0] ? pinfo.comment : pinfo.id,
             desktop_group_name(desktop_group(pinfo.categories)), size);
    const char *btn[2] = { tr(S_CANCEL), tr(pinfo.installed ? S_PKG_UPDATE : S_PKG_INSTALL) };
    dialog_open(LV_SYMBOL_DOWNLOAD, lv_color_hex(desktop_group_color(desktop_group(pinfo.categories))), title, body,
                btn, 2, 1, -1, install_answer, NULL);
}

/* ------------------------------------------------------------ desinstalar */

static char rm_id[48], rm_name[64];

bool apps_uninstall_now(const char *id)
{
    char dir[300];
    apps_dir(dir, sizeof dir);
    pkg_err_t e = pkg_uninstall(dir, id);
    fprintf(stderr, "bigfoston: desinstalar %s: %s\n", id, pkg_err_name(e));
    if(e) return false;
    launcher_forget_pkg(id);                           /* sai da organização (grupos) de vez */
    changed();
    return true;
}

static void uninstall_answer(int b, void *ud)
{
    (void)ud;
    if(b != 1) return;
    if(!apps_uninstall_now(rm_id)) { fail_dialog(PKG_E_WRITE); return; }
    char t[120];
    snprintf(t, sizeof t, tr(S_PKG_REMOVED), rm_name);
    if(settings()->notify) osd_show(&bf_icon_info, 0x8a93a5, t, NULL);
}

void apps_uninstall_ask(const bf_app_entry_t *a)
{
    if(!a) return;
    snprintf(rm_id, sizeof rm_id, "%s", a->id);
    snprintf(rm_name, sizeof rm_name, "%s", a->name);
    char title[120];
    snprintf(title, sizeof title, tr(S_PKG_UNINSTALL_Q), a->name);
    const char *btn[2] = { tr(S_CANCEL), tr(S_PKG_UNINSTALL) };
    dialog_open(LV_SYMBOL_TRASH, pal()->danger, title, tr(S_PKG_UNINSTALL_BODY), btn, 2, -1, 1, uninstall_answer, NULL);
}
