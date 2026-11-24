#define _DEFAULT_SOURCE
#include "desktop.h"
#include "settings.h"
#include "i18n.h"
#include "../assets/icons/bf_icons.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void cpy(char *d, size_t n, const char *s)
{
    size_t L = strlen(s);
    if(L >= n) L = n - 1;
    memcpy(d, s, L);
    d[L] = 0;
}

static void rstrip(char *s)
{
    size_t n = strlen(s);
    while(n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

/* Remove códigos de campo (%f %F %u %U %i %c %k) do Exec. */
static void strip_codes(char *e)
{
    char *w = e;
    for(char *r = e; *r; r++) {
        if(r[0] == '%' && r[1]) {
            if(r[1] == '%') { *w++ = '%'; r++; }
            else r++;
            continue;
        }
        *w++ = *r;
    }
    *w = 0;
    rstrip(e);
}

/* "#7a4cd9" -> 0x7a4cd9 (0 se inválida) */
static uint32_t parse_color(const char *s)
{
    if(*s == '#') s++;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    return (end - s == 6 && !*end) ? (uint32_t)v : 0;
}

/* o ícone do app instalado: só um arquivo dentro da pasta dele (sem "..") */
static void icon_path(char *out, size_t n, const char *dir, const char *icon)
{
    out[0] = 0;
    if(!dir || !dir[0] || !icon[0] || icon[0] == '/' || strstr(icon, "..")) return;
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, icon);
    if(access(p, R_OK) && !strchr(icon, '.')) snprintf(p, sizeof p, "%s/%s.svg", dir, icon);
    if(!access(p, R_OK)) snprintf(out, n, "%s", p);
}

bool desktop_parse(const char *path, const char *dir, bf_app_entry_t *a)
{
    FILE *f = fopen(path, "r");
    if(!f) return false;
    memset(a, 0, sizeof *a);
    char line[512], locname[64] = "", loccom[96] = "", icon[128] = "";
    bool in_entry = false, hidden = false;
    const char *lc = i18n_lang_code(i18n_get_lang());
    char key_full[32], key_short[32], ckey_full[40], ckey_short[40];
    snprintf(key_full, sizeof key_full, "Name[%s]=", lc);
    snprintf(key_short, sizeof key_short, "Name[%.2s]=", lc);
    snprintf(ckey_full, sizeof ckey_full, "Comment[%s]=", lc);
    snprintf(ckey_short, sizeof ckey_short, "Comment[%.2s]=", lc);
    while(fgets(line, sizeof line, f)) {
        rstrip(line);
        if(line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if(!in_entry) continue;
        if(!strncmp(line, "Name=", 5)) cpy(a->name, sizeof a->name, line + 5);
        else if(!strncmp(line, key_full, strlen(key_full)))
            cpy(locname, sizeof locname, line + strlen(key_full));
        else if(!locname[0] && !strncmp(line, key_short, strlen(key_short)))
            cpy(locname, sizeof locname, line + strlen(key_short));
        else if(!strncmp(line, "Comment=", 8)) cpy(a->comment, sizeof a->comment, line + 8);
        else if(!strncmp(line, ckey_full, strlen(ckey_full)))
            cpy(loccom, sizeof loccom, line + strlen(ckey_full));
        else if(!loccom[0] && !strncmp(line, ckey_short, strlen(ckey_short)))
            cpy(loccom, sizeof loccom, line + strlen(ckey_short));
        else if(!strncmp(line, "Icon=", 5)) cpy(icon, sizeof icon, line + 5);
        else if(!strncmp(line, "Categories=", 11)) cpy(a->categories, sizeof a->categories, line + 11);
        else if(!strncmp(line, "X-BigFoston-Screen=", 19)) cpy(a->screen, sizeof a->screen, line + 19);
        else if(!strncmp(line, "X-BigFoston-Color=", 18)) a->color = parse_color(line + 18);
        else if(!strncmp(line, "Exec=", 5)) cpy(a->exec, sizeof a->exec, line + 5);
        else if(!strncmp(line, "Terminal=", 9)) a->terminal = !strcasecmp(line + 9, "true");
        else if(!strncmp(line, "NoDisplay=", 10) || !strncmp(line, "Hidden=", 7))
            hidden |= strstr(line, "true") != NULL;
    }
    fclose(f);
    if(locname[0]) cpy(a->name, sizeof a->name, locname);
    if(loccom[0]) cpy(a->comment, sizeof a->comment, loccom);
    strip_codes(a->exec);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if(dir && dir[0]) {
        const char *d = strrchr(dir, '/');
        cpy(a->id, sizeof a->id, d ? d + 1 : dir);
        cpy(a->dir, sizeof a->dir, dir);
        icon_path(a->icon, sizeof a->icon, dir, icon);
    } else {
        cpy(a->id, sizeof a->id, base);
        char *dot = strrchr(a->id, '.');
        if(dot) *dot = 0;
    }
    return !hidden && a->name[0] && (a->exec[0] || a->screen[0]);
}

static int cmp(const void *x, const void *y)
{
    return strcasecmp(((const bf_app_entry_t *)x)->name, ((const bf_app_entry_t *)y)->name);
}

int desktop_scan(bf_app_entry_t **out)
{
    char dirs[512];
    const char *env = getenv("BIGFOSTON_APPS");
    if(env && *env) snprintf(dirs, sizeof dirs, "%s", env);
    else snprintf(dirs, sizeof dirs, "%s/apps:/usr/share/bigfoston/apps", data_dir());

    int n = 0, cap = 16;
    bf_app_entry_t *v = malloc((size_t)cap * sizeof *v);
    if(!v) { *out = NULL; return 0; }
    char *save = NULL;
    for(char *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        DIR *dp = opendir(d);
        if(!dp) continue;
        struct dirent *e;
        while((e = readdir(dp))) {
            if(e->d_name[0] == '.') continue;
            char path[600];
            snprintf(path, sizeof path, "%s/%s", d, e->d_name);
            struct stat st;
            if(stat(path, &st)) continue;
            if(S_ISDIR(st.st_mode)) {                  /* app instalado: <apps>/<id>/<nome>.desktop */
                DIR *sp = opendir(path);
                if(!sp) continue;
                struct dirent *f;
                while((f = readdir(sp))) {
                    size_t L = strlen(f->d_name);
                    if(L < 9 || strcmp(f->d_name + L - 8, ".desktop")) continue;
                    char fp[900];
                    snprintf(fp, sizeof fp, "%s/%s", path, f->d_name);
                    if(n == cap) {
                        bf_app_entry_t *nv = realloc(v, (size_t)(cap *= 2) * sizeof *v);
                        if(!nv) break;
                        v = nv;
                    }
                    if(desktop_parse(fp, path, &v[n])) { n++; break; }    /* um app por pasta */
                }
                closedir(sp);
                continue;
            }
            size_t L = strlen(e->d_name);
            if(L < 9 || strcmp(e->d_name + L - 8, ".desktop")) continue;
            if(n == cap) {
                bf_app_entry_t *nv = realloc(v, (size_t)(cap *= 2) * sizeof *v);
                if(!nv) break;
                v = nv;
            }
            if(desktop_parse(path, NULL, &v[n])) n++;
        }
        closedir(dp);
    }
    qsort(v, (size_t)n, sizeof *v, cmp);
    *out = v;
    return n;
}

/* ------------------------------------------------- grupos */

bf_group_t desktop_group(const char *categories)
{
    static const struct { const char *cat; bf_group_t g; } map[] = {
        { "Game", BF_GRP_GAMES }, { "AudioVideo", BF_GRP_MEDIA }, { "Audio", BF_GRP_MEDIA },
        { "Video", BF_GRP_MEDIA }, { "Network", BF_GRP_INTERNET }, { "Graphics", BF_GRP_GRAPHICS },
        { "Office", BF_GRP_OFFICE }, { "Education", BF_GRP_EDU }, { "Science", BF_GRP_EDU },
        { "Development", BF_GRP_DEV }, { "Utility", BF_GRP_TOOLS }, { "System", BF_GRP_SYSTEM },
        { "Settings", BF_GRP_SYSTEM },
    };
    /* a ordem da lista do app decide ("Game;Utility;" é Jogos) */
    for(const char *p = categories; p && *p;) {
        const char *e = strchr(p, ';');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        for(size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if(strlen(map[i].cat) == L && !strncmp(p, map[i].cat, L)) return map[i].g;
        if(!e) break;
        p = e + 1;
    }
    return BF_GRP_OTHER;
}

static const struct { const char *key; bf_str_t name; uint32_t color; const lv_image_dsc_t *icon; } groups[BF_GRP__N] = {
    [BF_GRP_GAMES]    = { "games",    S_GRP_GAMES,    0x7a4cd9, &bf_icon_games },
    [BF_GRP_MEDIA]    = { "media",    S_GRP_MEDIA,    0xe5534b, &bf_icon_multimedia },
    [BF_GRP_INTERNET] = { "internet", S_GRP_INTERNET, 0x3584e4, &bf_icon_web },
    [BF_GRP_GRAPHICS] = { "graphics", S_GRP_GRAPHICS, 0xd6409f, &bf_icon_gallery },
    [BF_GRP_OFFICE]   = { "office",   S_GRP_OFFICE,   0xc98b2c, &bf_icon_book },
    [BF_GRP_EDU]      = { "edu",      S_GRP_EDU,      0x2ec27e, &bf_icon_book },
    [BF_GRP_DEV]      = { "dev",      S_GRP_DEV,      0x4b5363, &bf_icon_terminal },
    [BF_GRP_TOOLS]    = { "tools",    S_GRP_TOOLS,    0x1fb5cc, &bf_icon_run },
    [BF_GRP_SYSTEM]   = { "system",   S_GRP_SYSTEM,   0x6c7383, &bf_icon_settings },
    [BF_GRP_OTHER]    = { "other",    S_GRP_OTHER,    0x26a269, &bf_icon_files },
};

const char *desktop_group_name(bf_group_t g) { return g >= 0 && g < BF_GRP__N ? tr(groups[g].name) : ""; }
uint32_t desktop_group_color(bf_group_t g) { return g >= 0 && g < BF_GRP__N ? groups[g].color : 0x6c7383; }
const void *desktop_group_icon(bf_group_t g) { return g >= 0 && g < BF_GRP__N ? groups[g].icon : NULL; }
const char *desktop_group_key(bf_group_t g) { return g >= 0 && g < BF_GRP__N ? groups[g].key : ""; }

void strlist_load(bf_strlist_t *l, const char *file)
{
    l->n = 0;
    char p[300], line[512];
    data_path(p, sizeof p, file);
    FILE *f = fopen(p, "r");
    if(!f) return;
    while(l->n < BF_LIST_MAX && fgets(line, sizeof line, f)) {
        rstrip(line);
        if(line[0]) l->item[l->n++] = strdup(line);
    }
    fclose(f);
}

bool strlist_save(const bf_strlist_t *l, const char *file)
{
    char p[300];
    data_path(p, sizeof p, file);
    FILE *f = fopen(p, "w");
    if(!f) return false;
    for(int i = 0; i < l->n; i++) fprintf(f, "%s\n", l->item[i]);
    return fclose(f) == 0;
}

bool strlist_remove(bf_strlist_t *l, const char *s)
{
    for(int i = 0; i < l->n; i++) {
        if(strcmp(l->item[i], s)) continue;
        free(l->item[i]);
        memmove(&l->item[i], &l->item[i + 1], (size_t)(l->n - i - 1) * sizeof l->item[0]);
        l->n--;
        return true;
    }
    return false;
}

bool strlist_contains(const bf_strlist_t *l, const char *s)
{
    for(int i = 0; i < l->n; i++) if(!strcmp(l->item[i], s)) return true;
    return false;
}

void strlist_push_front(bf_strlist_t *l, const char *s)
{
    strlist_remove(l, s);
    if(l->n == BF_LIST_MAX) free(l->item[--l->n]);
    memmove(&l->item[1], &l->item[0], (size_t)l->n * sizeof l->item[0]);
    l->item[0] = strdup(s);
    l->n++;
}

void strlist_free(bf_strlist_t *l)
{
    for(int i = 0; i < l->n; i++) free(l->item[i]);
    l->n = 0;
}
