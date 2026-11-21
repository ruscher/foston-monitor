/* devwatch.c — avisos de conexão de dispositivos (ver devwatch.h).
 *
 * Fontes, lidas a cada 700 ms (só arquivos do sysfs e um ioctl, sem custo):
 *   fone        evdev do driver de áudio ("... Headphone"), SW_HEADPHONE_INSERT
 *   carregador  /sys/class/power_supply/charger/online
 *   PC (USB)    /sys/class/udc/<udc>/state == "configured"
 *   cartão SD   /sys/block/mmcblk0
 * A primeira leitura só guarda o estado: nada aparece ao ligar. Estado
 * desconhecido (simulador, driver ausente) nunca gera aviso. */
#include "devwatch.h"
#include "osd.h"
#include "fx.h"
#include "settings.h"
#include "status.h"
#include "i18n.h"
#include "../assets/icons/bf_icons.h"
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

enum { D_HP, D_CHG, D_USB, D_SD, D__N };
static int state[D__N] = { -2, -2, -2, -2 };   /* -2 nunca lido, -1 desconhecido, 0/1 */
static int hp_fd = -2;                         /* -2 ainda não procurado */
static lv_timer_t *timer;

static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;
    if(f) {
        if(fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

static int read_usb(void)
{
    glob_t g;
    int v = -1;
    if(glob("/sys/class/udc/*/state", 0, NULL, &g) == 0 && g.gl_pathc) {
        FILE *f = fopen(g.gl_pathv[0], "r");
        char s[32] = "";
        if(f) {
            if(fgets(s, sizeof s, f)) v = !strncmp(s, "configured", 10);
            fclose(f);
        }
    }
    globfree(&g);
    return v;
}

static int read_hp(void)
{
#if defined(__linux__) && defined(EVIOCGSW)
    if(hp_fd == -2) {                          /* procura o evdev do conector uma vez */
        hp_fd = -1;
        for(int i = 0; i < 16 && hp_fd < 0; i++) {
            char path[32], name[96] = "";
            snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if(fd < 0) continue;
            if(ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strstr(name, "Headphone")) hp_fd = fd;
            else close(fd);
        }
    }
    if(hp_fd < 0) return -1;
    unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
    memset(sw, 0, sizeof sw);
    if(ioctl(hp_fd, EVIOCGSW(sizeof sw), sw) < 0) return -1;
    struct input_event ev;                     /* esvazia a fila (o estado vem do ioctl) */
    while(read(hp_fd, &ev, sizeof ev) == sizeof ev) {}
    return (sw[SW_HEADPHONE_INSERT / (8 * sizeof(long))] >> (SW_HEADPHONE_INSERT % (8 * sizeof(long)))) & 1;
#else
    return -1;
#endif
}

static void battery_line(char *b, size_t n, bool on_charger)
{
    int pct = read_int("/sys/class/power_supply/battery/capacity");
    if(pct < 0) { b[0] = 0; return; }
    snprintf(b, n, tr(on_charger ? S_DEV_BATTERY_CHG : S_DEV_BATTERY), pct);
}

static void announce(int dev, int v)
{
    static const char *const names[D__N] = { "fone", "carregador", "USB", "SD" };
    char sub[96];
    fprintf(stderr, "dispositivo: %s %s\n", names[dev], v ? "conectado" : "desconectado");
    switch(dev) {
        case D_HP:
            osd_show(&bf_icon_headphones, 0x9b6ad6, tr(v ? S_DEV_HP_IN : S_DEV_HP_OUT),
                     tr(v ? S_DEV_HP_IN_SUB : S_DEV_HP_OUT_SUB));
            break;
        case D_CHG:
            battery_line(sub, sizeof sub, v);
            osd_show(&bf_icon_plug, v ? 0x2ec27e : 0xe0a40c, tr(v ? S_DEV_CHG_IN : S_DEV_CHG_OUT), sub);
            break;
        case D_USB:
            osd_show(&bf_icon_usb, 0x3584e4, tr(v ? S_DEV_USB_IN : S_DEV_USB_OUT),
                     v ? "192.168.7.2" : "");
            break;
        case D_SD:
            osd_show(&bf_icon_sdcard, 0xe0a40c, tr(v ? S_DEV_SD_IN : S_DEV_SD_OUT), "");
            break;
    }
    fx_sound(FX_TOGGLE);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    int now[D__N];
    now[D_HP] = read_hp();
    now[D_CHG] = read_int("/sys/class/power_supply/charger/online");
    now[D_USB] = read_usb();
    now[D_SD] = access("/sys/block/mmcblk0", F_OK) == 0 ? 1 : (access("/sys/block", F_OK) == 0 ? 0 : -1);
    /* uma mudança só vale depois de N leituras iguais (700 ms cada): o bit de
     * carregador da PMU oscila na USB do PC e o estado do USB passa por
     * estados intermediários; o fone é imediato */
    static const int need[D__N] = { 1, 3, 3, 2 };
    static int cand[D__N] = { -2, -2, -2, -2 }, seen[D__N];
    for(int d = 0; d < D__N; d++) {
        if(state[d] == -2 || now[d] < 0) { state[d] = now[d]; cand[d] = -2; continue; }
        if(now[d] == state[d]) { cand[d] = -2; continue; }
        if(now[d] != cand[d]) { cand[d] = now[d]; seen[d] = 0; }
        if(++seen[d] < need[d]) continue;
        if(state[d] >= 0 && settings()->notify) announce(d, now[d]);
        state[d] = now[d];
        cand[d] = -2;
    }
}

void devwatch_init(void)
{
    if(timer || status_provider()->simulated) return;   /* simulador: nada a vigiar */
    timer = lv_timer_create(poll_cb, 700, NULL);
    poll_cb(timer);                            /* estado inicial, sem aviso */
}

void devwatch_demo(int dev, int on)
{
    if(dev >= 0 && dev < D__N) announce(dev, on);
}
