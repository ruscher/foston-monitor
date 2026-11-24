/* hwkbd.c — ver hwkbd.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "hwkbd.h"
#include "kbdmap.h"
#include "settings.h"
#include "nav.h"
#include "fx.h"
#include "osd.h"
#include "player.h"
#include "powerkey.h"
#include "i18n.h"
#include "../widgets/dialog.h"
#include "../widgets/volpop.h"
#include "../assets/icons/bf_icons.h"
#include "lvgl.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include <linux/input.h>               /* os códigos KEY_* (também no simulador) */
/* só no aparelho: no simulador o /dev/input é o teclado do PC */
#ifdef BIGFOSTON_TARGET
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define HAVE_EVDEV 1
#else
#define HAVE_EVDEV 0
#endif

/* ================================================================ tradução */

int hwkbd_layout(void)
{
    int l = settings()->kbd_layout;
    if(l > BF_KBD_AUTO && l < BF_KBD__N) return l;
    return settings()->lang == LANG_PT ? BF_KBD_ABNT2 : BF_KBD_US;
}

static kbdmap_t kst = { .numlock = true };

static void translate(hwkey_t *k)
{
    kbdmap_translate(&kst, hwkbd_layout(), &k->code, k->mods, k->repeat, k->text);
}

/* ============================================================ entrega */

static hwkbd_handler_t handler;

void hwkbd_set_handler(hwkbd_handler_t h) { handler = h; }

const char *hwkbd_key_name(uint16_t code)
{
    switch(code) {
    case KEY_ENTER: return "enter";
    case KEY_BACKSPACE: return "backspace";
    case KEY_DELETE: return "delete";
    case KEY_LEFT: return "left";
    case KEY_RIGHT: return "right";
    case KEY_UP: return "up";
    case KEY_DOWN: return "down";
    case KEY_TAB: return "tab";
    case KEY_ESC: return "escape";
    case KEY_HOME: return "home";
    case KEY_END: return "end";
    case KEY_PAGEUP: return "pageup";
    case KEY_PAGEDOWN: return "pagedown";
    default: return NULL;
    }
}

/* visível de verdade: ele e todos os pais sem HIDDEN */
static bool shown(lv_obj_t *o)
{
    for(; o; o = lv_obj_get_parent(o))
        if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

/* primeiro teclado da tela visível (o último criado: está por cima) */
static lv_obj_t *find_kb(lv_obj_t *o)
{
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(o, &lv_keyboard_class)) return o;
    for(int32_t i = (int32_t)lv_obj_get_child_count(o) - 1; i >= 0; i--) {
        lv_obj_t *r = find_kb(lv_obj_get_child(o, i));
        if(r) return r;
    }
    return NULL;
}

static lv_obj_t *find_ta(lv_obj_t *o)
{
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(o, &lv_textarea_class) && lv_obj_has_state(o, LV_STATE_FOCUSED) &&
       !lv_obj_has_state(o, LV_STATE_DISABLED)) return o;
    for(int32_t i = (int32_t)lv_obj_get_child_count(o) - 1; i >= 0; i--) {
        lv_obj_t *r = find_ta(lv_obj_get_child(o, i));
        if(r) return r;
    }
    return NULL;
}

/* o campo que recebe o texto e o teclado da tela ligado a ele (ou NULL) */
static lv_obj_t *target(lv_obj_t **kb_out)
{
    *kb_out = NULL;
    lv_obj_t *kb = find_kb(lv_layer_top());
    if(!kb && !dialog_is_open()) kb = find_kb(lv_screen_active());
    if(kb) {
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        if(ta && shown(ta)) { *kb_out = kb; return ta; }
        if(!ta) return NULL;                         /* teclado sem campo (página web): a tela trata */
    }
    lv_obj_t *ta = find_ta(lv_layer_top());
    if(!ta && !dialog_is_open()) ta = find_ta(lv_screen_active());
    return ta;
}

/* aperta o botão do teclado da tela com este texto (OK, fechar): os tratadores das telas
 * (READY/CANCEL) rodam como num toque */
static bool press_kb_button(lv_obj_t *kb, const char *const *texts)
{
    const char *const *map = lv_buttonmatrix_get_map(kb);
    if(!map) return false;
    uint32_t id = 0;
    for(int i = 0; map[i] && map[i][0]; i++) {
        if(!strcmp(map[i], "\n")) continue;
        for(int t = 0; texts[t]; t++)
            if(!strcmp(map[i], texts[t])) {
                lv_buttonmatrix_set_selected_button(kb, id);
                lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, &id);
                if(lv_obj_is_valid(kb)) lv_buttonmatrix_set_selected_button(kb, LV_BUTTONMATRIX_BUTTON_NONE);
                return true;
            }
        id++;
    }
    return false;
}

/* posição em bytes do caractere ci (UTF-8) */
static uint32_t byte_of(const char *t, uint32_t ci)
{
    uint32_t b = 0;
    while(ci && t[b]) {
        b++;
        while((t[b] & 0xC0) == 0x80) b++;
        ci--;
    }
    return b;
}

static bool type_into(lv_obj_t *ta, lv_obj_t *kb, const hwkey_t *k)
{
    static const char *const ok[] = { LV_SYMBOL_OK, NULL };
    static const char *const close_[] = { LV_SYMBOL_KEYBOARD, LV_SYMBOL_CLOSE, NULL };
    if(k->text[0]) { lv_textarea_add_text(ta, k->text); return true; }
    if(k->mods & (HK_CTRL | HK_ALT | HK_META)) {
        if((k->mods & HK_CTRL) && k->code == KEY_A) {            /* Ctrl+A: cursor no fim */
            lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
            return true;
        }
        if((k->mods & HK_CTRL) && k->code == KEY_BACKSPACE) {    /* Ctrl+Backspace: apaga a palavra */
            bool word = false;
            for(uint32_t pos; (pos = lv_textarea_get_cursor_pos(ta)) > 0;) {
                const char *t = lv_textarea_get_text(ta);
                bool sp = t[byte_of(t, pos - 1)] == ' ';
                if(sp && word) break;
                if(!sp) word = true;
                lv_textarea_delete_char(ta);
            }
            return true;
        }
        return false;                                           /* Alt+←, Ctrl+L...: atalhos */
    }
    uint32_t key = 0;
    switch(k->code) {
    case KEY_BACKSPACE: key = LV_KEY_BACKSPACE; break;
    case KEY_DELETE: key = LV_KEY_DEL; break;
    case KEY_LEFT: key = LV_KEY_LEFT; break;
    case KEY_RIGHT: key = LV_KEY_RIGHT; break;
    case KEY_UP: key = LV_KEY_UP; break;
    case KEY_DOWN: key = LV_KEY_DOWN; break;
    case KEY_HOME: key = LV_KEY_HOME; break;
    case KEY_END: key = LV_KEY_END; break;
    case KEY_ENTER:
        if(!lv_textarea_get_one_line(ta)) { lv_textarea_add_char(ta, '\n'); return true; }
        if(kb && press_kb_button(kb, ok)) return true;
        lv_obj_send_event(ta, LV_EVENT_READY, NULL);
        return true;
    case KEY_ESC:
        if(k->repeat) return true;
        if(kb && press_kb_button(kb, close_)) return true;
        if(kb) {
            lv_obj_send_event(kb, LV_EVENT_CANCEL, NULL);
            return true;
        }
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);              /* sai do campo; o próximo Esc volta */
        return true;
    case KEY_TAB: return true;                                  /* não escreve tabulação nos campos */
    default: return false;
    }
    lv_obj_send_event(ta, LV_EVENT_KEY, &key);
    return true;
}

/* a página rolável principal da tela (lista de Ajustes, arquivos...) */
static lv_obj_t *find_scroll(lv_obj_t *o, int depth)
{
    if(!o || depth > 6 || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
       (lv_obj_get_scroll_bottom(o) > 0 || lv_obj_get_scroll_top(o) > 0)) return o;
    uint32_t n = lv_obj_get_child_count(o);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_scroll(lv_obj_get_child(o, (int32_t)i), depth + 1);
        if(r) return r;
    }
    return NULL;
}

static bool global_key(const hwkey_t *k)
{
    switch(k->code) {
    case KEY_ESC:
        if(!k->repeat) nav_back();
        return true;
    case KEY_LEFT:
        if((k->mods & HK_ALT) && !k->repeat) { nav_back(); return true; }
        return false;
    case KEY_HOMEPAGE:
        if(!k->repeat) nav_home();
        return true;
    case KEY_UP: case KEY_DOWN: case KEY_PAGEUP: case KEY_PAGEDOWN: case KEY_HOME: case KEY_END: {
        lv_obj_t *s = find_scroll(lv_screen_active(), 0);
        if(!s) return false;
        int32_t h = lv_obj_get_content_height(s);
        int32_t dy = k->code == KEY_UP ? 60 : k->code == KEY_DOWN ? -60 :
                     k->code == KEY_PAGEUP ? h - 40 : k->code == KEY_PAGEDOWN ? -(h - 40) :
                     k->code == KEY_HOME ? 1 << 20 : -(1 << 20);
        lv_obj_scroll_by_bounded(s, 0, dy, k->repeat ? LV_ANIM_OFF : LV_ANIM_ON);
        return true;
    }
    default:
        return false;
    }
}

/* volume e música valem em qualquer lugar, até digitando */
static bool media_key(const hwkey_t *k)
{
    switch(k->code) {
    case KEY_VOLUMEUP: volpop_step(5); return true;
    case KEY_VOLUMEDOWN: volpop_step(-5); return true;
    case KEY_MUTE: if(!k->repeat) volpop_toggle_mute(); return true;
    case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD:
        if(!k->repeat && player_state() != PL_STOPPED) player_toggle();
        return true;
    case KEY_POWER: case KEY_SLEEP: case KEY_WAKEUP:
        return true;                                  /* não desliga o FOSTON pelo teclado */
    default:
        return false;
    }
}

void hwkbd_dispatch(const hwkey_t *k)
{
    if(!k->code) return;
    lv_display_trigger_activity(NULL);
    if(powerkey_sleeping()) { if(!k->repeat) powerkey_wake(); return; }
    if(fx_undim()) return;                            /* a primeira tecla só acende a tela */
    if(media_key(k)) return;
    lv_obj_t *kb, *ta = target(&kb);
    if(ta && type_into(ta, kb, k)) return;
    if(!k->repeat && (k->code == KEY_ENTER || k->code == KEY_ESC) && !(k->mods & (HK_CTRL | HK_ALT)) &&
       dialog_key(k->code == KEY_ENTER)) return;
    if(handler && handler(k)) return;
    global_key(k);
}

/* ============================================================ dispositivos */

typedef struct { int fd; char path[24]; char name[64]; bool bt; } kdev_t;
static kdev_t devs[HWKBD_MAX];
static int ndev;
#if HAVE_EVDEV
static int ino_fd = -1;
#endif
static uint16_t held;                 /* modificadores apertados: bits de MOD_* */
enum { MOD_LSHIFT = 1, MOD_RSHIFT = 2, MOD_LCTRL = 4, MOD_RCTRL = 8, MOD_LALT = 16, MOD_RALT = 32,
       MOD_LMETA = 64, MOD_RMETA = 128 };

int hwkbd_count(void) { return ndev; }
const char *hwkbd_name(int i) { return i >= 0 && i < ndev ? devs[i].name : ""; }
bool hwkbd_is_bt(int i) { return i >= 0 && i < ndev && devs[i].bt; }
bool hwkbd_bt_connected(void)
{
    for(int i = 0; i < ndev; i++) if(devs[i].bt) return true;
    return false;
}

static int mod_bit(uint16_t code)
{
    switch(code) {
    case KEY_LEFTSHIFT: return MOD_LSHIFT;
    case KEY_RIGHTSHIFT: return MOD_RSHIFT;
    case KEY_LEFTCTRL: return MOD_LCTRL;
    case KEY_RIGHTCTRL: return MOD_RCTRL;
    case KEY_LEFTALT: return MOD_LALT;
    case KEY_RIGHTALT: return MOD_RALT;
    case KEY_LEFTMETA: return MOD_LMETA;
    case KEY_RIGHTMETA: return MOD_RMETA;
    default: return 0;
    }
}

static uint8_t mods_now(void)
{
    uint8_t m = 0;
    if(held & (MOD_LSHIFT | MOD_RSHIFT)) m |= HK_SHIFT;
    if(held & (MOD_LCTRL | MOD_RCTRL)) m |= HK_CTRL;
    if(held & MOD_LALT) m |= HK_ALT;
    /* no layout dos EUA não há AltGr: o Alt da direita é um Alt */
    if(held & MOD_RALT) m |= hwkbd_layout() == BF_KBD_US ? HK_ALT : HK_ALTGR;
    if(held & (MOD_LMETA | MOD_RMETA)) m |= HK_META;
    return m;
}

#if HAVE_EVDEV
static void set_leds(int fd)
{
    struct input_event ev[3];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_LED; ev[0].code = LED_CAPSL; ev[0].value = kst.caps;
    ev[1].type = EV_LED; ev[1].code = LED_NUML; ev[1].value = kst.numlock;
    ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
    if(write(fd, ev, sizeof ev) < 0) { /* aberto só para leitura: sem luzes */ }
}

static void leds_all(void) { for(int i = 0; i < ndev; i++) set_leds(devs[i].fd); }
#else
static void leds_all(void) {}
#endif

/* uma tecla do evdev: value 1 apertou, 2 repetiu, 0 soltou */
void hwkbd_feed(uint16_t code, int value)
{
    int mb = mod_bit(code);
    if(mb) {
        if(value) held |= (uint16_t)mb; else held &= (uint16_t)~mb;
        return;
    }
    if(!value) return;
    if(code == KEY_CAPSLOCK) { if(value == 1) { kst.caps = !kst.caps; leds_all(); } return; }
    if(code == KEY_NUMLOCK) { if(value == 1) { kst.numlock = !kst.numlock; leds_all(); } return; }
    if(code >= BTN_MISC && code < KEY_OK) return;     /* botões de mouse/joystick */
    hwkey_t k = { .code = code, .mods = mods_now(), .repeat = value == 2 };
    translate(&k);
    hwkbd_dispatch(&k);
}

#if HAVE_EVDEV
#define BITS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TEST(b, a) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static bool is_keyboard(int fd)
{
    unsigned long ev[BITS(EV_MAX + 1)] = { 0 }, keys[BITS(KEY_MAX + 1)] = { 0 };
    if(ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !TEST(EV_KEY, ev)) return false;
    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return false;
    return TEST(KEY_A, keys) && TEST(KEY_Z, keys) && TEST(KEY_SPACE, keys) && TEST(KEY_ENTER, keys) &&
           !TEST(BTN_TOUCH, keys);
}

static void drop(int i, bool announce)
{
    char name[64];
    snprintf(name, sizeof name, "%s", devs[i].name);
    close(devs[i].fd);
    fprintf(stderr, "bigfoston: teclado desconectado: %s\n", name);
    devs[i] = devs[--ndev];
    held = 0;                                         /* nada fica "apertado" */
    kst.dead = 0;
    if(announce && settings()->notify)
        osd_show(&bf_icon_bluetooth, 0x8a93a5, tr(S_KBD_OUT), name);
}

static void scan(bool announce)
{
    /* os que sumiram (o mesmo nome de nó pode voltar com outro aparelho) */
    for(int i = ndev - 1; i >= 0; i--) {
        struct input_id id;
        if(ioctl(devs[i].fd, EVIOCGID, &id) < 0) drop(i, announce);
    }
    DIR *d = opendir("/dev/input");
    if(!d) return;
    struct dirent *e;
    while(ndev < HWKBD_MAX && (e = readdir(d))) {
        if(strncmp(e->d_name, "event", 5)) continue;
        char path[24];
        snprintf(path, sizeof path, "/dev/input/%.12s", e->d_name);
        bool have = false;
        for(int i = 0; i < ndev; i++) if(!strcmp(devs[i].path, path)) have = true;
        if(have) continue;
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        if(!is_keyboard(fd)) { close(fd); continue; }
        kdev_t *k = &devs[ndev++];
        k->fd = fd;
        snprintf(k->path, sizeof k->path, "%s", path);
        k->name[0] = 0;
        if(ioctl(fd, EVIOCGNAME(sizeof k->name - 1), k->name) < 0 || !k->name[0])
            snprintf(k->name, sizeof k->name, "%s", tr(S_KBD_GENERIC));
        k->name[sizeof k->name - 1] = 0;
        struct input_id id = { 0 };
        ioctl(fd, EVIOCGID, &id);
        k->bt = id.bustype == BUS_BLUETOOTH;
        set_leds(fd);
        fprintf(stderr, "bigfoston: teclado em %s: %s (%s)\n", path, k->name, k->bt ? "Bluetooth" : "USB/outro");
        if(announce && settings()->notify)
            osd_show(k->bt ? &bf_icon_bluetooth : &bf_icon_usb, 0x3584e4, tr(S_KBD_IN), k->name);
    }
    closedir(d);
}

static void dev_read(int i)
{
    struct input_event ev[32];
    for(;;) {
        ssize_t n = read(devs[i].fd, ev, sizeof ev);
        if(n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if(n <= 0) { drop(i, true); return; }        /* ENODEV: o teclado saiu */
        int fd = devs[i].fd;
        for(size_t j = 0; j < (size_t)n / sizeof ev[0]; j++)
            if(ev[j].type == EV_KEY) {
                hwkbd_feed(ev[j].code, ev[j].value);
                /* a tecla pode ter mudado a lista (não muda: drop só aqui e no scan) */
                if(i >= ndev || devs[i].fd != fd) return;
            }
        if((size_t)n < sizeof ev) return;
    }
}
#endif

void hwkbd_init(void)
{
#if HAVE_EVDEV
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if(ino_fd >= 0 && inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
        close(ino_fd);
        ino_fd = -1;
    }
    scan(false);
#endif
}

int hwkbd_poll_fds(struct pollfd *pf, int n_max)
{
    int n = 0;
#if HAVE_EVDEV
    if(ino_fd >= 0 && n < n_max) pf[n++] = (struct pollfd){ ino_fd, POLLIN, 0 };
    for(int i = 0; i < ndev && n < n_max; i++) pf[n++] = (struct pollfd){ devs[i].fd, POLLIN, 0 };
#else
    (void)pf; (void)n_max;
#endif
    return n;
}

void hwkbd_poll_done(const struct pollfd *pf, int n)
{
#if HAVE_EVDEV
    bool rescan = false;
    int fds[HWKBD_MAX + 1], nf = 0;
    for(int j = 0; j < n; j++) {
        if(!(pf[j].revents & (POLLIN | POLLERR | POLLHUP))) continue;
        if(pf[j].fd == ino_fd) {
            char buf[512];
            while(read(ino_fd, buf, sizeof buf) > 0) {}
            rescan = true;
        }
        else if(nf < HWKBD_MAX + 1) fds[nf++] = pf[j].fd;
    }
    /* pelo fd: dev_read pode tirar um teclado da lista e mudar os índices */
    for(int j = 0; j < nf; j++)
        for(int i = 0; i < ndev; i++)
            if(devs[i].fd == fds[j]) { dev_read(i); break; }
    if(rescan) scan(true);
#else
    (void)pf; (void)n;
#endif
}
