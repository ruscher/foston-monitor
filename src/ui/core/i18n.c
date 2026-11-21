/* i18n.c — textos embutidos + sobrescrita opcional por catálogo externo. */
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *key;
    const char *s[LANG_COUNT];
} entry_t;

static const entry_t table[S__COUNT] = {
#define X(id, key, pt, en, es) [id] = { key, { pt, en, es } },
#include "i18n_strings.def"
#undef X
};

static bf_lang_t cur = LANG_PT;
static char *override[S__COUNT]; /* textos vindos de catálogo (strdup) */

static void clear_overrides(void)
{
    for(int i = 0; i < S__COUNT; i++) { free(override[i]); override[i] = NULL; }
}

void i18n_set_lang(bf_lang_t lang)
{
    if(lang >= LANG_COUNT) lang = LANG_PT;
    if(lang != cur) clear_overrides();
    cur = lang;
}

bf_lang_t i18n_get_lang(void) { return cur; }

const char *tr(bf_str_t id)
{
    if(id >= S__COUNT) return "?";
    if(override[id]) return override[id];
    const char *s = table[id].s[cur];
    return s ? s : table[id].s[LANG_PT];
}

const char *i18n_key(bf_str_t id) { return id < S__COUNT ? table[id].key : ""; }

static const char *const codes[LANG_COUNT] = { "pt_BR", "en", "es" };
static const char *const names[LANG_COUNT] = { "Português (Brasil)", "English", "Español" };

const char *i18n_lang_code(bf_lang_t l) { return l < LANG_COUNT ? codes[l] : codes[0]; }
const char *i18n_lang_name(bf_lang_t l) { return l < LANG_COUNT ? names[l] : names[0]; }

bf_lang_t i18n_lang_from_code(const char *code)
{
    if(!code) return LANG_PT;
    if(!strncmp(code, "en", 2)) return LANG_EN;
    if(!strncmp(code, "es", 2)) return LANG_ES;
    return LANG_PT;
}

static char *trim(char *s)
{
    while(*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

int i18n_load_catalog(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    char line[512];
    int n = 0;
    while(fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if(*p == '#' || !*p) continue;
        char *eq = strchr(p, '=');
        if(!eq) continue;
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        for(int i = 0; i < S__COUNT; i++) {
            if(!strcmp(table[i].key, k)) {
                free(override[i]);
                override[i] = strdup(v);
                n++;
                break;
            }
        }
    }
    fclose(f);
    return n;
}
