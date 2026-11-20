#include "settings.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef BIGFOSTON_TARGET
#define DEFAULT_DATA_DIR "/data/bigfoston"
#else
#define DEFAULT_DATA_DIR "./data"
#endif

static bf_settings_t cur;
static char ddir[256] = DEFAULT_DATA_DIR;

bf_settings_t *settings(void) { return &cur; }

void settings_defaults(bf_settings_t *s)
{
    s->lang = LANG_PT;
    s->dark = true;
    s->backlight = 80;
    s->volume = 60;
    s->show_date = true;
    s->clock_24h = true;
}

static void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for(char *p = tmp + 1; *p; p++) {
        if(*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

void data_dir_init(const char *override)
{
    const char *env = override ? override : getenv("BIGFOSTON_DATA");
    if(env && *env) snprintf(ddir, sizeof ddir, "%s", env);
    mkdir_p(ddir);
}

const char *data_dir(void) { return ddir; }

void data_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", ddir, name);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_load(void)
{
    settings_defaults(&cur);
    char path[300];
    data_path(path, sizeof path, "settings.conf");
    FILE *f = fopen(path, "r");
    if(!f) return;
    char line[256], k[64], v[128];
    while(fgets(line, sizeof line, f)) {
        if(line[0] == '#') continue;
        if(sscanf(line, " %63[^= ] = %127[^\n]", k, v) != 2) continue;
        if(!strcmp(k, "language"))       cur.lang = i18n_lang_from_code(v);
        else if(!strcmp(k, "theme"))     cur.dark = strcmp(v, "light") != 0;
        else if(!strcmp(k, "backlight")) cur.backlight = clampi(atoi(v), 5, 100);
        else if(!strcmp(k, "volume"))    cur.volume = clampi(atoi(v), 0, 100);
        else if(!strcmp(k, "show_date")) cur.show_date = atoi(v) != 0;
        else if(!strcmp(k, "clock_24h")) cur.clock_24h = atoi(v) != 0;
    }
    fclose(f);
}

bool settings_save(void)
{
    char path[300], tmp[310];
    data_path(path, sizeof path, "settings.conf");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) {
        fprintf(stderr, "bigfoston: não foi possível salvar %s: %s\n", tmp, strerror(errno));
        return false;
    }
    fprintf(f, "# BigFoston shell — ajustes (gerado automaticamente)\n");
    fprintf(f, "language = %s\n", i18n_lang_code(cur.lang));
    fprintf(f, "theme = %s\n", cur.dark ? "dark" : "light");
    fprintf(f, "backlight = %d\n", cur.backlight);
    fprintf(f, "volume = %d\n", cur.volume);
    fprintf(f, "show_date = %d\n", cur.show_date);
    fprintf(f, "clock_24h = %d\n", cur.clock_24h);
    bool ok = fflush(f) == 0;
    fclose(f);
    if(ok) ok = rename(tmp, path) == 0;
    return ok;
}
