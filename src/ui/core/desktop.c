#define _DEFAULT_SOURCE
#include "desktop.h"
#include "settings.h"
#include "i18n.h"
#include <dirent.h>
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

static bool parse(const char *path, bf_app_entry_t *a)
{
    FILE *f = fopen(path, "r");
    if(!f) return false;
    memset(a, 0, sizeof *a);
    char line[512], locname[64] = "";
    bool in_entry = false, hidden = false;
    const char *lc = i18n_lang_code(i18n_get_lang());
    char key_full[32], key_short[32];
    snprintf(key_full, sizeof key_full, "Name[%s]=", lc);
    snprintf(key_short, sizeof key_short, "Name[%.2s]=", lc);
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
        else if(!strncmp(line, "Exec=", 5)) cpy(a->exec, sizeof a->exec, line + 5);
        else if(!strncmp(line, "Terminal=", 9)) a->terminal = !strcasecmp(line + 9, "true");
        else if(!strncmp(line, "NoDisplay=", 10) || !strncmp(line, "Hidden=", 7))
            hidden |= strstr(line, "true") != NULL;
    }
    fclose(f);
    if(locname[0]) cpy(a->name, sizeof a->name, locname);
    strip_codes(a->exec);
    return !hidden && a->name[0] && a->exec[0];
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
            size_t L = strlen(e->d_name);
            if(L < 9 || strcmp(e->d_name + L - 8, ".desktop")) continue;
            char path[600];
            snprintf(path, sizeof path, "%s/%s", d, e->d_name);
            if(n == cap) {
                bf_app_entry_t *nv = realloc(v, (size_t)(cap *= 2) * sizeof *v);
                if(!nv) break;
                v = nv;
            }
            if(parse(path, &v[n])) n++;
        }
        closedir(dp);
    }
    qsort(v, (size_t)n, sizeof *v, cmp);
    *out = v;
    return n;
}

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
