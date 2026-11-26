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
    s->wallpaper = BF_WALL_IMAGE;
    snprintf(s->wall_image, sizeof s->wall_image, "%s", BF_WALL_DEFAULT_IMAGE);
    s->anim = true;
    s->sounds = true;
    s->boot_sound = false;
    s->notify = true;
    s->dim_min = 0;
    snprintf(s->tz, sizeof s->tz, "%s", "brasilia");
    s->dst = false;
    s->auto_time = true;
    s->gps_time = true;
    snprintf(s->ntp, sizeof s->ntp, "%s", BF_NTP_DEFAULT);
    s->home_mode = BF_HOME_GRID;
    s->home_apps = BF_HOME_APPS_DEFAULT;
    s->backlight = 80;
    s->volume = 60;
    s->book_font = 1;
    s->book_space = 1;
    s->book_margin = 1;
    s->web_zoom = 100;
    s->web_pc = true;
    s->web_fs_sharp = false;
    s->perf_auto = true;
    s->perf_enter_s = 2;
    s->perf_exit_s = 10;
    s->perf_svc = 0xf;                  /* GPS, Bluetooth, telefone e tarefas */
    s->perf_quiet = true;
    s->perf_prio = true;
    s->perf_tag = true;
    s->perf_modal = true;
    s->perf_pad = true;
    s->perf_tv = true;
    s->perf_stream = true;
    s->perf_wall = true;
    s->perf_always = false;
    s->power_plan = BF_PLAN_BAL;
    s->fx_level = BF_FX_FULL;
    s->show_date = true;
    s->clock_24h = true;
    s->nav_voice = true;
    s->map_detail = BF_DETAIL_AUTO;
    s->map_avoid_toll = false;
    s->map_alerts = true;
    s->map_persp = true;
    for(int i = 0; i < 4; i++) { s->map_veh_model[i] = -1; s->map_veh_color[i] = 0; s->map_veh_size[i] = 1; }
    s->kbd_layout = 0;                  /* automático: ABNT2 em português, EUA nas outras */
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
    char line[512], k[64], v[300];
    while(fgets(line, sizeof line, f)) {
        if(line[0] == '#') continue;
        if(sscanf(line, " %63[^= ] = %299[^\n]", k, v) != 2) continue;
        if(!strcmp(k, "language"))       cur.lang = i18n_lang_from_code(v);
        else if(!strcmp(k, "theme"))     cur.mode = !strcmp(v, "light") ? BF_MODE_LIGHT :
                                                !strcmp(v, "auto") ? BF_MODE_AUTO : BF_MODE_DARK;
        else if(!strcmp(k, "palette"))   cur.palette = clampi(atoi(v), 0, 63);
        else if(!strcmp(k, "wallpaper")) cur.wallpaper = clampi(atoi(v), 0, BF_WALL__N - 1);
        else if(!strcmp(k, "wallpaper_image")) snprintf(cur.wall_image, sizeof cur.wall_image, "%.255s", v);
        else if(!strcmp(k, "animations")) cur.anim = atoi(v) != 0;
        else if(!strcmp(k, "sounds"))    cur.sounds = atoi(v) != 0;
        else if(!strcmp(k, "boot_sound")) cur.boot_sound = atoi(v) != 0;
        else if(!strcmp(k, "device_notices")) cur.notify = atoi(v) != 0;
        else if(!strcmp(k, "dim_minutes")) cur.dim_min = clampi(atoi(v), 0, 60);
        else if(!strcmp(k, "timezone"))  snprintf(cur.tz, sizeof cur.tz, "%.*s", (int)sizeof cur.tz - 1, v);
        else if(!strcmp(k, "dst"))       cur.dst = atoi(v) != 0;
        else if(!strcmp(k, "auto_time")) cur.auto_time = atoi(v) != 0;
        else if(!strcmp(k, "gps_time"))  cur.gps_time = atoi(v) != 0;
        else if(!strcmp(k, "home_mode")) cur.home_mode = clampi(atoi(v), 0, BF_HOME__N - 1);
        else if(!strcmp(k, "home_apps")) cur.home_apps = (unsigned)strtoul(v, NULL, 0);
        else if(!strcmp(k, "ntp_servers")) snprintf(cur.ntp, sizeof cur.ntp, "%.159s", v);
        else if(!strcmp(k, "backlight")) cur.backlight = clampi(atoi(v), 5, 100);
        else if(!strcmp(k, "volume"))    cur.volume = clampi(atoi(v), 0, 100);
        else if(!strcmp(k, "music_repeat"))  cur.music_repeat = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "music_shuffle")) cur.music_shuffle = atoi(v) != 0;
        else if(!strcmp(k, "music_vis"))     cur.music_vis = clampi(atoi(v), 0, 3);
        else if(!strcmp(k, "music_color"))   cur.music_color = clampi(atoi(v), 0, 4);
        else if(!strcmp(k, "music_bg"))      cur.music_bg = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "music_light"))   cur.music_light = atoi(v) != 0;
        else if(!strcmp(k, "gallery_sort"))  cur.gallery_sort = atoi(v) != 0;
        else if(!strcmp(k, "web_js"))        cur.web_js = atoi(v) != 0;
        else if(!strcmp(k, "web_no_images")) cur.web_no_images = atoi(v) != 0;
        else if(!strcmp(k, "web_search"))    cur.web_search = clampi(atoi(v), 0, 15);
        else if(!strcmp(k, "web_zoom"))      cur.web_zoom = clampi(atoi(v), 50, 200);
        else if(!strcmp(k, "web_pc"))        cur.web_pc = atoi(v) != 0;
        else if(!strcmp(k, "web_fs_sharp"))  cur.web_fs_sharp = atoi(v) != 0;
        else if(!strcmp(k, "web_perf") || !strcmp(k, "perf_auto")) cur.perf_auto = atoi(v) != 0;   /* web_perf: 0.4 */
        else if(!strcmp(k, "perf_enter_s"))  cur.perf_enter_s = clampi(atoi(v), 0, 60);
        else if(!strcmp(k, "perf_exit_s"))   cur.perf_exit_s = clampi(atoi(v), 1, 600);
        else if(!strcmp(k, "perf_svc"))      cur.perf_svc = (unsigned)strtoul(v, NULL, 0) & 0xfu;
        else if(!strcmp(k, "perf_quiet"))    cur.perf_quiet = atoi(v) != 0;
        else if(!strcmp(k, "perf_prio"))     cur.perf_prio = atoi(v) != 0;
        else if(!strcmp(k, "perf_tag"))      cur.perf_tag = atoi(v) != 0;
        else if(!strcmp(k, "perf_modal"))    cur.perf_modal = atoi(v) != 0;
        else if(!strcmp(k, "perf_pad"))      cur.perf_pad = atoi(v) != 0;
        else if(!strcmp(k, "perf_tv"))       cur.perf_tv = atoi(v) != 0;
        else if(!strcmp(k, "perf_stream"))   cur.perf_stream = atoi(v) != 0;
        else if(!strcmp(k, "perf_wall"))     cur.perf_wall = atoi(v) != 0;
        else if(!strcmp(k, "perf_always"))   cur.perf_always = atoi(v) != 0;
        else if(!strcmp(k, "power_plan"))    cur.power_plan = clampi(atoi(v), 0, BF_PLAN__N - 1);
        else if(!strcmp(k, "fx_level"))      cur.fx_level = clampi(atoi(v), 0, BF_FX__N - 1);
        else if(!strcmp(k, "book_font"))     cur.book_font = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_space"))    cur.book_space = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_margin"))   cur.book_margin = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "book_theme"))    cur.book_theme = clampi(atoi(v), 0, 2);
        else if(!strcmp(k, "nav_voice"))     cur.nav_voice = atoi(v) != 0;
        else if(!strcmp(k, "map_detail"))    { int d = atoi(v); cur.map_detail = d >= 0 && d < BF_DETAIL__N ? d : BF_DETAIL_AUTO; }
        else if(!strcmp(k, "map_avoid_toll")) cur.map_avoid_toll = atoi(v) != 0;
        else if(!strcmp(k, "map_alerts"))   cur.map_alerts = atoi(v) != 0;
        else if(!strcmp(k, "map_persp"))    cur.map_persp = atoi(v) != 0;
        else if(!strncmp(k, "map_veh_", 8) && k[8] >= '0' && k[8] <= '3' && !k[9]) {
            int i = k[8] - '0', m = -1, c = 0, z = 1;
            if(sscanf(v, "%d %d %d", &m, &c, &z) == 3) {
                cur.map_veh_model[i] = (signed char)clampi(m, -1, 15);
                cur.map_veh_color[i] = (signed char)clampi(c, 0, 7);
                cur.map_veh_size[i] = (signed char)clampi(z, 0, 2);
            }
        }
        else if(!strcmp(k, "show_date")) cur.show_date = atoi(v) != 0;
        else if(!strcmp(k, "clock_24h")) cur.clock_24h = atoi(v) != 0;
        else if(!strcmp(k, "kbd_layout")) cur.kbd_layout = clampi(atoi(v), 0, 3);
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
    fprintf(f, "palette = %d\nwallpaper = %d\nwallpaper_image = %s\n", cur.palette, cur.wallpaper, cur.wall_image);
    fprintf(f, "animations = %d\nsounds = %d\nboot_sound = %d\n", cur.anim, cur.sounds, cur.boot_sound);
    fprintf(f, "dim_minutes = %d\ndevice_notices = %d\n", cur.dim_min, cur.notify);
    fprintf(f, "timezone = %s\ndst = %d\nauto_time = %d\ngps_time = %d\nntp_servers = %s\n",
            cur.tz, cur.dst, cur.auto_time, cur.gps_time, cur.ntp);
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
    fprintf(f, "web_js = %d\nweb_no_images = %d\nweb_search = %d\nweb_zoom = %d\nweb_pc = %d\n",
            cur.web_js ? 1 : 0, cur.web_no_images ? 1 : 0, cur.web_search, cur.web_zoom, cur.web_pc ? 1 : 0);
    fprintf(f, "web_fs_sharp = %d\n", cur.web_fs_sharp ? 1 : 0);
    fprintf(f, "perf_auto = %d\nperf_enter_s = %d\nperf_exit_s = %d\nperf_svc = %u\nperf_quiet = %d\n"
               "perf_prio = %d\nperf_tag = %d\nperf_modal = %d\nperf_pad = %d\nperf_tv = %d\nperf_wall = %d\n"
               "perf_always = %d\npower_plan = %d\nfx_level = %d\n",
            cur.perf_auto ? 1 : 0, cur.perf_enter_s, cur.perf_exit_s, cur.perf_svc, cur.perf_quiet ? 1 : 0,
            cur.perf_prio ? 1 : 0, cur.perf_tag ? 1 : 0, cur.perf_modal ? 1 : 0, cur.perf_pad ? 1 : 0,
            cur.perf_tv ? 1 : 0, cur.perf_wall ? 1 : 0, cur.perf_always ? 1 : 0, cur.power_plan, cur.fx_level);
    fprintf(f, "perf_stream = %d\n", cur.perf_stream ? 1 : 0);
    fprintf(f, "book_font = %d\nbook_space = %d\nbook_margin = %d\nbook_theme = %d\n",
            cur.book_font, cur.book_space, cur.book_margin, cur.book_theme);
    fprintf(f, "nav_voice = %d\n", cur.nav_voice ? 1 : 0);
    fprintf(f, "map_detail = %d\n", cur.map_detail);
    fprintf(f, "map_avoid_toll = %d\n", cur.map_avoid_toll ? 1 : 0);
    fprintf(f, "map_alerts = %d\n", cur.map_alerts ? 1 : 0);
    fprintf(f, "map_persp = %d\n", cur.map_persp ? 1 : 0);
    for(int i = 0; i < 4; i++)
        fprintf(f, "map_veh_%d = %d %d %d\n", i, cur.map_veh_model[i], cur.map_veh_color[i], cur.map_veh_size[i]);
    fprintf(f, "show_date = %d\n", cur.show_date);
    fprintf(f, "clock_24h = %d\n", cur.clock_24h);
    fprintf(f, "kbd_layout = %d\n", cur.kbd_layout);
    /* no cartão SD (FAT): o conteúdo vai ao cartão antes da troca de nome */
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if(ok) ok = rename(tmp, path) == 0;
    return ok;
}
