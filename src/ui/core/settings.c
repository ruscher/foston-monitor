#include "settings.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statfs.h>

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
    s->mode = BF_MODE_DARK;
    s->palette = 0;
    s->wallpaper = BF_WALL_LOGO;
    s->anim = true;
    s->sounds = true;
    s->boot_sound = false;
    s->notify = true;
    s->dim_min = 0;
    snprintf(s->tz, sizeof s->tz, "%s", "brasilia");
    s->dst = false;
    s->auto_time = true;
    snprintf(s->ntp, sizeof s->ntp, "%s", BF_NTP_DEFAULT);
    s->home_mode = BF_HOME_GRID;
    s->home_apps = BF_HOME_APPS_DEFAULT;
    s->backlight = 80;
    s->volume = 60;
    s->book_font = 1;
    s->book_space = 1;
    s->book_margin = 1;
    s->show_date = true;
    s->clock_24h = true;
    s->nav_voice = true;
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

bool data_dir_volatile(void)
{
    struct statfs st;
    if(statfs(ddir, &st)) return false;
    /* RAMFS_MAGIC (initramfs) e TMPFS_MAGIC */
    return (unsigned long)st.f_type == 0x858458f6UL || (unsigned long)st.f_type == 0x01021994UL;
}

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
        else if(!strcmp(k, "theme"))     cur.mode = !strcmp(v, "light") ? BF_MODE_LIGHT :
                                                !strcmp(v, "auto") ? BF_MODE_AUTO : BF_MODE_DARK;
        else if(!strcmp(k, "palette"))   cur.palette = clampi(atoi(v), 0, 63);
        else if(!strcmp(k, "wallpaper")) cur.wallpaper = clampi(atoi(v), 0, BF_WALL__N - 1);
        else if(!strcmp(k, "animations")) cur.anim = atoi(v) != 0;
        else if(!strcmp(k, "sounds"))    cur.sounds = atoi(v) != 0;
        else if(!strcmp(k, "boot_sound")) cur.boot_sound = atoi(v) != 0;
        else if(!strcmp(k, "device_notices")) cur.notify = atoi(v) != 0;
        else if(!strcmp(k, "dim_minutes")) cur.dim_min = clampi(atoi(v), 0, 60);
        else if(!strcmp(k, "timezone"))  snprintf(cur.tz, sizeof cur.tz, "%.*s", (int)sizeof cur.tz - 1, v);
        else if(!strcmp(k, "dst"))       cur.dst = atoi(v) != 0;
        else if(!strcmp(k, "auto_time")) cur.auto_time = atoi(v) != 0;
        else if(!strcmp(k, "home_mode")) cur.home_mode = clampi(atoi(v), 0, BF_HOME__N - 1);
        else if(!strcmp(k, "home_apps")) cur.home_apps = (unsigned)strtoul(v, NULL, 0);
        else if(!strcmp(k, "ntp_servers")) snprintf(cur.ntp, sizeof cur.ntp, "%s", v);
        else if(!strcmp(k, "backlight")) cur.backlight = clampi(atoi(v), 5, 100);
        else if(!strcmp(k, "volume"))    cur.volume = clampi(atoi(v), 0, 100);
        else if(!strcmp(k, "music_repeat"))  cur.music_repeat = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "music_shuffle")) cur.music_shuffle = atoi(v) != 0;
        else if(!strcmp(k, "music_vis"))     cur.music_vis = clampi(atoi(v), 0, 3);
        else if(!strcmp(k, "music_color"))   cur.music_color = clampi(atoi(v), 0, 4);
        else if(!strcmp(k, "music_bg"))      cur.music_bg = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "music_light"))   cur.music_light = atoi(v) != 0;
        else if(!strcmp(k, "gallery_sort"))  cur.gallery_sort = atoi(v) != 0;
        else if(!strcmp(k, "book_font"))     cur.book_font = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_space"))    cur.book_space = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_margin"))   cur.book_margin = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_theme"))    cur.book_theme = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "nav_voice"))     cur.nav_voice = atoi(v) != 0;
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
    fprintf(f, "theme = %s\n", cur.mode == BF_MODE_LIGHT ? "light" : cur.mode == BF_MODE_AUTO ? "auto" : "dark");
    fprintf(f, "palette = %d\nwallpaper = %d\n", cur.palette, cur.wallpaper);
    fprintf(f, "animations = %d\nsounds = %d\nboot_sound = %d\n", cur.anim, cur.sounds, cur.boot_sound);
    fprintf(f, "dim_minutes = %d\ndevice_notices = %d\n", cur.dim_min, cur.notify);
    fprintf(f, "timezone = %s\ndst = %d\nauto_time = %d\nntp_servers = %s\n",
            cur.tz, cur.dst, cur.auto_time, cur.ntp);
    fprintf(f, "home_mode = %d\nhome_apps = 0x%x\n", cur.home_mode, cur.home_apps);
    fprintf(f, "backlight = %d\n", cur.backlight);
    fprintf(f, "volume = %d\n", cur.volume);
    fprintf(f, "music_repeat = %d\n", cur.music_repeat);
    fprintf(f, "music_shuffle = %d\n", cur.music_shuffle ? 1 : 0);
    fprintf(f, "music_vis = %d\n", cur.music_vis);
    fprintf(f, "music_color = %d\n", cur.music_color);
    fprintf(f, "music_bg = %d\n", cur.music_bg);
    fprintf(f, "music_light = %d\n", cur.music_light ? 1 : 0);
    fprintf(f, "gallery_sort = %d\n", cur.gallery_sort ? 1 : 0);
    fprintf(f, "book_font = %d\nbook_space = %d\nbook_margin = %d\nbook_theme = %d\n",
            cur.book_font, cur.book_space, cur.book_margin, cur.book_theme);
    fprintf(f, "nav_voice = %d\n", cur.nav_voice ? 1 : 0);
    fprintf(f, "show_date = %d\n", cur.show_date);
    fprintf(f, "clock_24h = %d\n", cur.clock_24h);
    /* no cartão SD (FAT): o conteúdo vai ao cartão antes da troca de nome */
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if(ok) ok = rename(tmp, path) == 0;
    return ok;
}
