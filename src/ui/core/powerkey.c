/* powerkey.c — botão de energia e modo Hibernar (ver powerkey.h).
 *
 * O botão chega como KEY_POWER num evdev do kernel (driver mt3351-pwrkey,
 * "MT3351 power button"). Um toque curto abre o diálogo de energia; outro
 * toque com o diálogo aberto o fecha. Durante o Hibernar, o botão ou um
 * toque na tela acordam o aparelho.
 *
 * Hibernar aqui NÃO suspende o processador: o kernel deste aparelho não tem
 * suspensão (sem /sys/power). O modo cobre a tela de preto, baixa a luz de
 * fundo ao mínimo que o hardware aceita (bl_power; o driver nunca escreve
 * abaixo do nível 3, o mesmo piso do Windows CE) e engole os toques até
 * acordar. Música, GPS e rotas continuam funcionando. */
#include "powerkey.h"
#include "app.h"
#include "status.h"
#include "../widgets/bottombar.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

static int key_fd = -2;              /* -2 ainda não procurado, -1 ausente */
static unsigned scans;               /* novas buscas enquanto ausente */
static lv_timer_t *timer;
static lv_obj_t *sleep_layer;        /* camada preta do Hibernar */

/* ---- luz de fundo: bl_power 4 = apagada (o driver usa o nível mínimo) ---- */
static void bl_power(int v)
{
#ifdef __linux__
    DIR *d = opendir("/sys/class/backlight");
    if(!d) return;
    struct dirent *e;
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof p, "/sys/class/backlight/%s/bl_power", e->d_name);
        FILE *f = fopen(p, "w");
        if(f) {
            fprintf(f, "%d\n", v);
            fclose(f);
        }
        break;
    }
    closedir(d);
#else
    (void)v;
#endif
}

#if defined(__linux__) && defined(EVIOCGBIT)
static bool has_key_power(int fd)
{
    unsigned long ev[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
    unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
    const size_t bpl = 8 * sizeof(long);
    if(ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0) return false;
    if(!((ev[EV_KEY / bpl] >> (EV_KEY % bpl)) & 1)) return false;
    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return false;
    return (keys[KEY_POWER / bpl] >> (KEY_POWER % bpl)) & 1;
}

static void find_key(void)
{
    key_fd = -1;
    for(int i = 0; i < 16 && key_fd < 0; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        if(has_key_power(fd)) {
            char name[96] = "";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            fprintf(stderr, "bigfoston: botão de energia em %s (%s)\n", path, name);
            key_fd = fd;
        } else {
            close(fd);
        }
    }
}
#endif

static void wake_cb(lv_event_t *e)
{
    (void)e;
    powerkey_wake();
}

bool powerkey_sleeping(void) { return sleep_layer != NULL; }

void powerkey_hibernate(void)
{
    if(sleep_layer || app_shutting_down()) return;
    lv_obj_t *o = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);        /* engole os toques */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* acorda ao soltar o dedo: o toque inteiro fica com a camada e não
     * chega ao que estiver por baixo */
    lv_obj_add_event_cb(o, wake_cb, LV_EVENT_RELEASED, NULL);
    sleep_layer = o;
    lv_refr_now(NULL);                                /* preto antes de apagar a luz */
    if(!status_provider()->simulated) bl_power(4);
    fprintf(stderr, "bigfoston: hibernar (tela apagada)\n");
}

void powerkey_wake(void)
{
    if(!sleep_layer) return;
    if(!status_provider()->simulated) bl_power(0);
    lv_obj_delete_async(sleep_layer);
    sleep_layer = NULL;
    fprintf(stderr, "bigfoston: acordou\n");
}

void powerkey_press(void)
{
    if(app_shutting_down()) return;
    if(sleep_layer) { powerkey_wake(); return; }
    if(bottombar_power_is_open()) bottombar_close_power();
    else bottombar_open_power();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
#if defined(__linux__) && defined(EVIOCGBIT)
    if(key_fd == -2 || (key_fd == -1 && ++scans % 125 == 0)) find_key();   /* ausente: tenta a cada ~5 s */
    if(key_fd < 0) return;
    struct input_event ev;
    ssize_t n;
    while((n = read(key_fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
        if(ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) powerkey_press();
    }
    if(n < 0 && errno != EAGAIN && errno != EINTR) {     /* dispositivo sumiu */
        close(key_fd);
        key_fd = -1;
    }
#endif
}

void powerkey_init(void)
{
    if(timer || status_provider()->simulated) return;
    timer = lv_timer_create(poll_cb, 40, NULL);
}
