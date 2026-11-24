/* gamepad.c — ver gamepad.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gamepad.h"
#include "hwkbd.h"
#include "btstat.h"
#include "osd.h"
#include "settings.h"
#include "i18n.h"
#include "../assets/icons/bf_icons.h"
#include "lvgl.h"
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#define DEAD      180          /* zona morta dos analógicos (de 1000): o DS4 deriva ~80 */
#define TICK_MS   16
#define GP_REP_DELAY 400          /* tecla segurada: repete depois de... */
#define GP_REP_EVERY 90           /* ...a cada */
#define HIDE_MS   8000         /* cursor some parado */
#define CUR_SIZE  26

typedef struct {
    bool used, sony;
    gp_axis_t ax[GPA__N];
    int32_t v[GPA__N];          /* -1000..1000 */
    char addr[18];
    uint32_t active_at;         /* último uso (desliga parado) */
    bool idle_sent;
    uint16_t rep_key;           /* tecla que repete enquanto segura */
    uint32_t rep_at;
    uint16_t hat_key[2];        /* tecla do direcional em X e em Y (0 = solto) */
    uint32_t scroll_at;         /* próxima rolagem do analógico direito (0 = parado) */
} pad_t;

static pad_t pads[GAMEPAD_MAX];
static lv_indev_t *pind;
static lv_obj_t *cur;
static lv_timer_t *tick_t, *idle_t;
static int32_t cx = -1, cy = -1;
static float fx_, fy_;          /* frações de pixel acumuladas */
static bool down, shown_, fast;   /* fast: o laço está a TICK_MS (movendo) */
static uint32_t moved_at, last_tick;

int gamepad_count(void)
{
    int n = 0;
    for(int i = 0; i < GAMEPAD_MAX; i++) n += pads[i].used;
    return n;
}

bool gamepad_cursor(int32_t *x, int32_t *y)
{
    if(x) *x = cx;
    if(y) *y = cy;
    return shown_;
}

/* ============================================================ cursor */

static void pread(lv_indev_t *in, lv_indev_data_t *d)
{
    (void)in;
    d->point.x = cx;
    d->point.y = cy;
    d->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void cur_deleted(lv_event_t *e) { (void)e; cur = NULL; }

static void cur_style(void)
{
    if(!cur) return;
    lv_obj_set_style_bg_opa(cur, down ? LV_OPA_80 : LV_OPA_20, 0);
    lv_obj_set_pos(cur, cx - CUR_SIZE / 2, cy - CUR_SIZE / 2);
}

static void cur_show(bool on)
{
    if(on && !cur) {
        cur = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(cur);
        lv_obj_set_size(cur, CUR_SIZE, CUR_SIZE);
        lv_obj_set_style_radius(cur, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cur, lv_color_white(), 0);
        lv_obj_set_style_border_width(cur, 3, 0);
        lv_obj_set_style_border_color(cur, lv_color_white(), 0);
        lv_obj_set_style_outline_width(cur, 2, 0);
        lv_obj_set_style_outline_color(cur, lv_color_black(), 0);
        lv_obj_set_style_outline_opa(cur, LV_OPA_60, 0);
        lv_obj_remove_flag(cur, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cur, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_add_event_cb(cur, cur_deleted, LV_EVENT_DELETE, NULL);
    }
    if(!cur) return;
    if(on) {
        lv_obj_remove_flag(cur, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cur);              /* acima do escurecimento e dos avisos */
        cur_style();
    } else lv_obj_add_flag(cur, LV_OBJ_FLAG_HIDDEN);
    shown_ = on;
}

static void cur_init(void)
{
    if(pind) return;
    pind = lv_indev_create();
    lv_indev_set_type(pind, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pind, pread);
    lv_indev_set_mode(pind, LV_INDEV_MODE_EVENT);   /* lido só quando o cursor muda */
    lv_indev_set_scroll_limit(pind, 10);
}

static void cur_home(void)
{
    if(cx >= 0) return;
    cx = lv_display_get_horizontal_resolution(NULL) / 2;
    cy = lv_display_get_vertical_resolution(NULL) / 2;
}

/* ============================================================ teclas */

static uint16_t std_btn(const pad_t *p, uint16_t c)
{
    /* hid-generic com o controle do PlayStation: botões na ordem do relatório HID */
    static const uint16_t sony[14] = { BTN_WEST, BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_TL, BTN_TR, BTN_TL2,
                                       BTN_TR2, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, BTN_MODE, 0 };
    if(p->sony && c >= BTN_GAMEPAD && c < BTN_GAMEPAD + 14) return sony[c - BTN_GAMEPAD];
    /* joystick genérico (botão 1, 2, 3, 4): o 1 é o principal, o 2 volta */
    static const uint16_t joy[4] = { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH };
    if(c >= BTN_JOYSTICK && c < BTN_JOYSTICK + 4) return joy[c - BTN_JOYSTICK];
    return c;
}

/* a tecla de cada botão; *rep = repete segurando */
static uint16_t btn_key(uint16_t b, bool *rep)
{
    *rep = false;
    switch(b) {
    case BTN_EAST: return KEY_ESC;
    case BTN_NORTH: return KEY_ENTER;
    case BTN_WEST: return KEY_SPACE;
    case BTN_START: return KEY_PLAYPAUSE;
    case BTN_MODE: return KEY_HOMEPAGE;
    default: break;
    }
    *rep = true;
    switch(b) {
    case BTN_TL: return KEY_PAGEUP;
    case BTN_TR: return KEY_PAGEDOWN;
    case BTN_TL2: return KEY_VOLUMEDOWN;
    case BTN_TR2: return KEY_VOLUMEUP;
    case BTN_DPAD_UP: return KEY_UP;
    case BTN_DPAD_DOWN: return KEY_DOWN;
    case BTN_DPAD_LEFT: return KEY_LEFT;
    case BTN_DPAD_RIGHT: return KEY_RIGHT;
    default: return 0;
    }
}

static void tick_need(void);

static void key_down(pad_t *p, uint16_t k, bool rep)
{
    hwkbd_feed(k, 1);
    if(rep) { p->rep_key = k; p->rep_at = lv_tick_get() + GP_REP_DELAY; tick_need(); }
}

static void key_up(pad_t *p, uint16_t k)
{
    hwkbd_feed(k, 0);
    if(p->rep_key == k) p->rep_key = 0;
}

/* ============================================================ laço */

static int32_t dz(int32_t v)
{
    if(v > DEAD) return v - DEAD;
    if(v < -DEAD) return v + DEAD;
    return 0;
}

static float speed(int32_t d)                  /* px/s: fino perto do centro, rápido no fim */
{
    float m = (float)(d < 0 ? -d : d) / (1000 - DEAD);
    float s = 60.f + 840.f * m * m;
    return d < 0 ? -s : s;
}

static bool busy(void)
{
    if(down) return true;
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        const pad_t *p = &pads[i];
        if(!p->used) continue;
        if(p->rep_key || dz(p->v[GPA_X]) || dz(p->v[GPA_Y]) || p->scroll_at) return true;
    }
    return false;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    float dt = (float)((uint32_t)(now - last_tick)) / 1000.f;
    if(dt > 0.1f) dt = 0.1f;
    last_tick = now;
    int32_t ox = cx, oy = cy;
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        pad_t *p = &pads[i];
        if(!p->used) continue;
        int32_t dx = dz(p->v[GPA_X]), dy = dz(p->v[GPA_Y]);
        if(dx) fx_ += speed(dx) * dt;
        if(dy) fy_ += speed(dy) * dt;
        if(p->rep_key && (int32_t)(now - p->rep_at) >= 0) {
            hwkbd_feed(p->rep_key, 2);
            p->rep_at = now + GP_REP_EVERY;
        }
        /* analógico direito: rola (setas) mais rápido quanto mais inclinado */
        int32_t ry = dz(p->v[p->sony ? GPA_RZ : GPA_RY]);
        if(!ry) p->scroll_at = 0;
        else if(!p->scroll_at || (int32_t)(now - p->scroll_at) >= 0) {
            bool first = !p->scroll_at;
            int32_t m = ry < 0 ? -ry : ry;
            hwkbd_feed(ry < 0 ? KEY_UP : KEY_DOWN, first ? 1 : 2);
            p->scroll_at = now + (uint32_t)(220 - 170 * m / (1000 - DEAD));
        }
    }
    if(fx_ >= 1.f || fx_ <= -1.f || fy_ >= 1.f || fy_ <= -1.f) {
        int32_t w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
        cx += (int32_t)fx_;
        cy += (int32_t)fy_;
        fx_ -= (float)(int32_t)fx_;
        fy_ -= (float)(int32_t)fy_;
        if(cx < 0) cx = 0;
        if(cx > w - 1) cx = w - 1;
        if(cy < 0) cy = 0;
        if(cy > h - 1) cy = h - 1;
    }
    if(cx != ox || cy != oy) {
        moved_at = now;
        if(!shown_) cur_show(true);
        else cur_style();
    }
    if(pind && (down || cx != ox || cy != oy)) lv_indev_read(pind);
    if(shown_ && !down && ((uint32_t)(now - moved_at)) > HIDE_MS) cur_show(false);
    fast = busy();
    if(!fast && !shown_) lv_timer_pause(tick_t);
    else lv_timer_set_period(tick_t, fast ? TICK_MS : 500);   /* 500: só espera para esconder o cursor */
}

static void tick_need(void)
{
    if(!tick_t) tick_t = lv_timer_create(tick, TICK_MS, NULL);
    if(!fast) last_tick = lv_tick_get();             /* parado até agora: o movimento começa já */
    fast = true;
    lv_timer_set_period(tick_t, TICK_MS);
    lv_timer_resume(tick_t);
}

/* parado há GAMEPAD_IDLE_MIN minutos: desconecta (o controle desliga) */
static void idle_check(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        pad_t *p = &pads[i];
        if(!p->used || !p->addr[0] || p->idle_sent) continue;
        if(((uint32_t)(now - p->active_at)) < GAMEPAD_IDLE_MIN * 60000u) continue;
        p->idle_sent = true;
        fprintf(stderr, "bigfoston: controle parado há %d min: desconectando\n", GAMEPAD_IDLE_MIN);
        if(bt_cmd("disconnect %s", p->addr) && settings()->notify)
            osd_show(&bf_icon_bluetooth, 0x8a93a5, tr(S_PAD_IDLE), tr(S_PAD_IDLE_SUB));
    }
}

/* ============================================================ entrada */

int gamepad_attach(bool sony_generic, const gp_axis_t ax[GPA__N], const char *addr)
{
    int s = 0;
    while(s < GAMEPAD_MAX && pads[s].used) s++;
    if(s == GAMEPAD_MAX) return -1;
    pad_t *p = &pads[s];
    memset(p, 0, sizeof *p);
    p->used = true;
    p->sony = sony_generic;
    for(int i = 0; i < GPA__N; i++) p->ax[i] = ax[i];
    snprintf(p->addr, sizeof p->addr, "%s", addr ? addr : "");
    p->active_at = lv_tick_get();
    cur_init();
    cur_home();
    if(!idle_t) idle_t = lv_timer_create(idle_check, 30000, NULL);
    return s;
}

void gamepad_detach(int slot)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used) return;
    pad_t *p = &pads[slot];
    if(p->rep_key) hwkbd_feed(p->rep_key, 0);
    for(int k = 0; k < 2; k++) if(p->hat_key[k]) hwkbd_feed(p->hat_key[k], 0);
    memset(p, 0, sizeof *p);
    if(down) { down = false; if(pind) lv_indev_read(pind); }
    if(!gamepad_count()) {
        cur_show(false);
        if(idle_t) { lv_timer_delete(idle_t); idle_t = NULL; }
    }
}

static int axis_of(uint16_t code)
{
    switch(code) {
    case ABS_X: return GPA_X;
    case ABS_Y: return GPA_Y;
    case ABS_Z: return GPA_Z;
    case ABS_RX: return GPA_RX;
    case ABS_RY: return GPA_RY;
    case ABS_RZ: return GPA_RZ;
    case ABS_HAT0X: return GPA_HX;
    case ABS_HAT0Y: return GPA_HY;
    default: return -1;
    }
}

static int32_t norm(const gp_axis_t *a, int32_t v)
{
    if(!a->have || a->max <= a->min) return 0;
    int64_t c2 = (int64_t)a->min + a->max, half2 = (int64_t)a->max - a->min;   /* em dobro */
    int64_t r = ((int64_t)v * 2 - c2) * 1000 / half2;
    if(r > 1000) r = 1000;
    if(r < -1000) r = -1000;
    return (int32_t)r;
}

static void hat(pad_t *p, int k, int32_t v)
{
    uint16_t want = v < -500 ? (k ? KEY_UP : KEY_LEFT) : v > 500 ? (k ? KEY_DOWN : KEY_RIGHT) : 0;
    if(want == p->hat_key[k]) return;
    if(p->hat_key[k]) key_up(p, p->hat_key[k]);
    p->hat_key[k] = want;
    if(want) key_down(p, want, true);
}

void gamepad_event(int slot, uint16_t type, uint16_t code, int32_t value)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used) return;
    pad_t *p = &pads[slot];
    if(type == EV_KEY) {
        if(value == 2) return;                     /* o repetir é nosso */
        uint16_t b = std_btn(p, code);
        if(!b) return;
        p->active_at = lv_tick_get();
        p->idle_sent = false;
        if(b == BTN_SOUTH) {                       /* toca onde está o cursor */
            if(value) {
                if(hwkbd_activity()) return;       /* tela apagada: só acende */
                if(!shown_) { moved_at = lv_tick_get(); cur_show(true); tick_need(); return; }
                down = true;
            } else if(!down) return;
            else down = false;
            moved_at = lv_tick_get();
            cur_style();
            if(pind) lv_indev_read(pind);
            tick_need();
            return;
        }
        bool rep;
        uint16_t k = btn_key(b, &rep);
        if(!k) return;
        if(value) key_down(p, k, rep);
        else key_up(p, k);
        return;
    }
    if(type != EV_ABS) return;
    int a = axis_of(code);
    if(a < 0) return;
    int32_t was = p->v[a];
    p->v[a] = norm(&p->ax[a], value);
    if(a == GPA_HX || a == GPA_HY) {
        if(p->v[a] != was) {
            p->active_at = lv_tick_get();
            p->idle_sent = false;
        }
        hat(p, a == GPA_HY, p->v[a]);
        return;
    }
    bool stick = a == GPA_X || a == GPA_Y || a == (p->sony ? GPA_RZ : GPA_RY);
    if(!stick || !dz(p->v[a])) return;
    p->active_at = lv_tick_get();
    p->idle_sent = false;
    if(!dz(was) && hwkbd_activity()) { p->v[a] = 0; return; }   /* tela apagada: só acende */
    tick_need();
}
