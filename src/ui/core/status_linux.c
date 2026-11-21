/* status_linux.c — estado real lido de sysfs/proc (alvo e --real-status no PC).
 * Nada é inventado: o que não existe no sistema vira BF_UNKNOWN / -1. */
#define _DEFAULT_SOURCE
#include "status.h"
#include "gpsstat.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    if(!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int read_int(const char *path, int def)
{
    char b[32];
    return read_line(path, b, sizeof b) == 0 ? atoi(b) : def;
}

static bool dir_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void read_power(bf_status_t *s)
{
    s->battery_present = BF_NO;
    s->battery_pct = -1;
    s->charging = BF_UNKNOWN;
    s->battery_state = BF_BAT_UNKNOWN;
    s->ext_power = BF_UNKNOWN;
    DIR *d = opendir("/sys/class/power_supply");
    if(!d) { s->battery_present = BF_UNKNOWN; return; }
    struct dirent *e;
    char p[300], b[64];
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if(read_line(p, b, sizeof b)) continue;
        if(!strcmp(b, "Battery")) {
            s->battery_present = BF_YES;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            int c = read_int(p, -1);
            if(c >= 0 && c <= 100) s->battery_pct = c;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
            if(!read_line(p, b, sizeof b)) {
                if(!strcmp(b, "Charging")) { s->charging = BF_YES; s->battery_state = BF_BAT_CHARGING; }
                else if(!strcmp(b, "Discharging")) { s->charging = BF_NO; s->battery_state = BF_BAT_DISCHARGING; }
                else if(!strcmp(b, "Full")) { s->charging = BF_NO; s->battery_state = BF_BAT_FULL; }
                else if(!strcmp(b, "Not charging")) { s->charging = BF_NO; s->battery_state = BF_BAT_NOT_CHARGING; }
            }
        }
        else {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            int on = read_int(p, -1);
            if(on == 1) s->ext_power = BF_YES;
            else if(on == 0 && s->ext_power == BF_UNKNOWN) s->ext_power = BF_NO;
        }
    }
    closedir(d);
}

static void read_usb(bf_status_t *s)
{
    s->usb = BF_UNKNOWN;
    DIR *d = opendir("/sys/class/udc");
    if(!d) return;
    struct dirent *e;
    char p[300], b[64];
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/udc/%s/state", e->d_name);
        if(!read_line(p, b, sizeof b))
            s->usb = (!strcmp(b, "configured") || !strcmp(b, "addressed")) ? BF_YES : BF_NO;
    }
    closedir(d);
}

static void read_bt(bf_status_t *s)
{
    s->bluetooth = BF_UNKNOWN;
    DIR *d = opendir("/sys/class/rfkill");
    if(d) {
        struct dirent *e;
        char p[300], b[64];
        while((e = readdir(d))) {
            if(e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
            if(read_line(p, b, sizeof b) || strcmp(b, "bluetooth")) continue;
            snprintf(p, sizeof p, "/sys/class/rfkill/%s/soft", e->d_name);
            int soft = read_int(p, 0);
            snprintf(p, sizeof p, "/sys/class/rfkill/%s/hard", e->d_name);
            int hard = read_int(p, 0);
            s->bluetooth = (soft || hard) ? BF_NO : BF_YES;
        }
        closedir(d);
    }
    if(s->bluetooth == BF_UNKNOWN && dir_exists("/sys/class/bluetooth")) {
        /* subsistema presente; sem rfkill, considera ligado se houver hciN */
        DIR *b = opendir("/sys/class/bluetooth");
        struct dirent *e;
        s->bluetooth = BF_NO;
        while(b && (e = readdir(b))) if(!strncmp(e->d_name, "hci", 3)) s->bluetooth = BF_YES;
        if(b) closedir(b);
    }
}

static void read_net(bf_status_t *s)
{
    s->network = BF_NO;
    s->ip[0] = 0;
    struct ifaddrs *ifa, *i;
    if(getifaddrs(&ifa)) { s->network = BF_UNKNOWN; return; }
    for(i = ifa; i; i = i->ifa_next) {
        if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if(i->ifa_flags & IFF_LOOPBACK) continue;
        if(!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, s->ip, sizeof s->ip);
        s->network = BF_YES;
        break;
    }
    freeifaddrs(ifa);
}

/* ---- volume (mixer OSS do kernel; amixer como alternativa) ---- */
static int vol_cache = 60;

static bool have_soundcard(void)
{
    char b[128];
    if(read_line("/proc/asound/cards", b, sizeof b)) return false;
    return strstr(b, "no soundcards") == NULL;
}

/* OSS mixer (kernel emulation over ALSA): no alsa-lib in the static rootfs.
 * The Master control maps to SOUND_MIXER_VOLUME, value = left | right << 8, 0..100. */
static int oss_volume(int set_pct)
{
    int fd = open("/dev/mixer", O_RDWR | O_CLOEXEC);
    if(fd < 0) return -1;
    int v, ok;
    if(set_pct >= 0) {
        v = set_pct | set_pct << 8;
        ok = ioctl(fd, MIXER_WRITE(SOUND_MIXER_VOLUME), &v) == 0;
    }
    else ok = ioctl(fd, MIXER_READ(SOUND_MIXER_VOLUME), &v) == 0;
    close(fd);
    return ok ? (v & 0xff) : -1;
}

static const char *amixer_path(void)
{
    static const char *c[] = { "/usr/bin/amixer", "/bin/amixer", NULL };
    for(int i = 0; c[i]; i++) if(access(c[i], X_OK) == 0) return c[i];
    return NULL;
}

static int lx_vol_get(void)
{
    if(!have_soundcard()) return -1;
    int v = oss_volume(-1);
    return v >= 0 ? v : vol_cache;
}

static bool lx_vol_set(int pct)
{
    if(!have_soundcard()) return false;
    vol_cache = pct;
    if(oss_volume(pct) >= 0) return true;
    const char *am = amixer_path();
    if(!am) return false;
    char arg[8];
    snprintf(arg, sizeof arg, "%d%%", pct);
    pid_t pid = fork();
    if(pid == 0) {
        execl(am, "amixer", "-q", "sset", "Master", arg, (char *)NULL);
        _exit(127);
    }
    if(pid < 0) return false;
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ---- luz posterior (/sys/class/backlight) ---- */
static char bl_dir[280];

static bool bl_find(void)
{
    if(bl_dir[0]) return true;
    DIR *d = opendir("/sys/class/backlight");
    if(!d) return false;
    struct dirent *e;
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        snprintf(bl_dir, sizeof bl_dir, "/sys/class/backlight/%s", e->d_name);
        break;
    }
    closedir(d);
    return bl_dir[0] != 0;
}

static int lx_bl_get(void)
{
    if(!bl_find()) return -1;
    char p[320];
    snprintf(p, sizeof p, "%s/max_brightness", bl_dir);
    int mx = read_int(p, -1);
    snprintf(p, sizeof p, "%s/brightness", bl_dir);
    int v = read_int(p, -1);
    if(mx <= 0 || v < 0) return -1;
    return (v * 100 + mx / 2) / mx;
}

static bool lx_bl_set(int pct)
{
    if(!bl_find()) return false;
    char p[320];
    snprintf(p, sizeof p, "%s/max_brightness", bl_dir);
    int mx = read_int(p, -1);
    if(mx <= 0) return false;
    snprintf(p, sizeof p, "%s/brightness", bl_dir);
    FILE *f = fopen(p, "w");
    if(!f) return false;
    int v = (pct * mx + 50) / 100;
    if(v < 1) v = 1; /* nunca apagar a tela pelo slider */
    bool ok = fprintf(f, "%d\n", v) > 0;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

static const char *lx_bl_dev(void) { return bl_find() ? bl_dir : NULL; }

static void lx_read(bf_status_t *s)
{
    read_power(s);
    read_usb(s);
    read_bt(s);
    read_net(s);
    s->volume_pct = lx_vol_get();
    s->muted = s->volume_pct < 0 ? BF_UNKNOWN : (s->volume_pct == 0 ? BF_YES : BF_NO);
    /* GPS: estado publicado pelo bfgps (desconhecido sem o serviço) */
    gps_info_t g;
    gps_read(&g);
    if(!g.running) { s->gps_fix = BF_UNKNOWN; s->gps_sats = -1; }
    else { s->gps_fix = g.state >= GPS_FIX2D ? BF_YES : BF_NO; s->gps_sats = g.used; }
}

const bf_status_provider_t bf_status_linux = {
    .name = "linux", .simulated = false, .read = lx_read,
    .backlight_get = lx_bl_get, .backlight_set = lx_bl_set, .backlight_device = lx_bl_dev,
    .volume_get = lx_vol_get, .volume_set = lx_vol_set,
};
