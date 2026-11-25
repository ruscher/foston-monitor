/* gamepad.c — ver gamepad.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include "gamepad.h"
#include "hwkbd.h"
#include "btstat.h"
#include "osd.h"
#include "settings.h"
#include "i18n.h"
#include "../assets/icons/bf_icons.h"
#include "lvgl.h"
#include <dirent.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#define DEAD      180          /* zona morta dos analógicos (de 1000): o DS4 deriva ~80 */
#define TICK_MS   16
#define GP_REP_DELAY 400       /* tecla segurada: repete depois de... */
#define GP_REP_EVERY 90        /* ...a cada */
#define HIDE_MS   8000         /* cursor some parado */
#define CUR_SIZE  26
#define TRIG_ON   600          /* gatilho analógico vira "apertado" / "solto" */
#define TRIG_OFF  300

/* DualShock 4 e DualSense pelo Bluetooth com o hid-generic (o kernel não tem o driver da
 * Sony): as linhas do SDL_GameControllerDB, para funcionarem mesmo sem o app instalado */
static const char *const builtin[] = {
    "050000004c050000c405000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Linux,",
    "050000004c050000cc09000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Linux,",
    "050000004c050000e60c000000010000,PS5 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Linux,",
    "050000004c050000f20d000000010000,PS5 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Linux,",
};

#define NRAWB 64                /* botões crus (bN) acompanhados */

typedef struct {
    bool used;
    char name[64], addr[18];
    gp_id_t id;
    pm_bits_t bits;
    gp_axis_t abs[GP_NABS];
    padmap_t map;
    int map_from;
    int8_t btn_idx[0x300];      /* código EV_KEY -> bN (-1) */
    int8_t axis_idx[GP_NABS];   /* código EV_ABS -> aN (-1) */
    int8_t hat_idx[GP_NABS];    /* código ABS_HAT* -> hN (-1) */
    bool rawb[NRAWB];           /* botões crus apertados (bN) */
    int16_t rawa[GP_NABS];      /* eixos crus normalizados (-1000..1000), por código */
    int8_t hat[4][2];           /* direcionais: x, y (-1, 0, 1) */
    bool btn[PC__NBTN];
    int16_t ax[PC__NAXIS];
    bool trig[2];               /* gatilhos como botões (volume) */
    uint32_t events;
    uint32_t active_at;         /* último uso (desliga parado) */
    bool idle_sent;
    uint16_t rep_key;           /* tecla que repete enquanto segura */
    uint32_t rep_at;
    uint32_t scroll_at;         /* próxima rolagem do analógico direito (0 = parado) */
} pad_t;

static pad_t pads[GAMEPAD_MAX];
static lv_indev_t *pind;
static lv_obj_t *cur;
static lv_timer_t *tick_t, *idle_t;
static int32_t cx = -1, cy = -1;
static float fx_, fy_;          /* frações de pixel acumuladas */
static bool down, shown_, fast; /* fast: o laço está a TICK_MS (movendo) */
static uint32_t moved_at, last_tick;
static bool capture;
static gamepad_obs_t observer;

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

void gamepad_user_db(char *out, size_t n) { data_path(out, n, "controles/gamecontrollerdb.txt"); }

/* ============================================================ mapa */

/* os bancos: o do usuário e o de cada app instalado (<dados>/apps/<app>/) */
#define MAX_DB 9
static int db_files(char files[MAX_DB][320])
{
    int n = 0;
    gamepad_user_db(files[n++], sizeof files[0]);
    char apps[300];
    data_path(apps, sizeof apps, "apps");
    DIR *d = opendir(apps);
    if(!d) return n;
    struct dirent *e;
    while(n < MAX_DB && (e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        if(snprintf(files[n], sizeof files[0], "%s/%s/gamecontrollerdb.txt", apps, e->d_name) < (int)sizeof files[0])
            n++;
    }
    closedir(d);
    return n;
}

static void resolve(pad_t *p)
{
    char guid[33];
    padmap_guid(p->id.bus, p->id.vendor, p->id.product, p->id.version, guid);
    char files[MAX_DB][320];
    const char *fp[MAX_DB];
    int nf = db_files(files);
    for(int i = 0; i < nf; i++) fp[i] = files[i];
    p->map_from = GPM_DEFAULT;
    if(padmap_find(fp, 1, guid, &p->map)) p->map_from = GPM_USER;
    else if(nf > 1 && padmap_find(fp + 1, nf - 1, guid, &p->map)) p->map_from = GPM_DB;
    else {
        for(size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
            if(!strncmp(builtin[i], guid, 32) && padmap_parse(builtin[i], &p->map)) {
                p->map_from = GPM_BUILTIN;
                break;
            }
    }
    if(p->map_from == GPM_DEFAULT) {
        memcpy(p->map.guid, guid, sizeof guid);
        padmap_linux_default(&p->bits, &p->map);
    }
    memcpy(p->map.guid, guid, sizeof guid);
    static const char *const from[] = { "padrão do Linux", "embutido", "banco do SC Controller", "registrado" };
    fprintf(stderr, "bigfoston: controle %s (%s): mapa %s \"%s\"\n", p->name, guid, from[p->map_from], p->map.name);
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

/* a tecla de cada controle; *rep = repete segurando */
static uint16_t ctl_key(int c, bool *rep)
{
    *rep = false;
    switch(c) {
    case PC_B: return KEY_ESC;
    case PC_Y: return KEY_ENTER;
    case PC_X: return KEY_SPACE;
    case PC_START: return KEY_PLAYPAUSE;
    case PC_GUIDE: return KEY_HOMEPAGE;
    default: break;
    }
    *rep = true;
    switch(c) {
    case PC_LSHOULDER: return KEY_PAGEUP;
    case PC_RSHOULDER: return KEY_PAGEDOWN;
    case PC_DPUP: return KEY_UP;
    case PC_DPDOWN: return KEY_DOWN;
    case PC_DPLEFT: return KEY_LEFT;
    case PC_DPRIGHT: return KEY_RIGHT;
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

static int16_t axv(const pad_t *p, int c) { return p->ax[c - PC__NBTN]; }

static bool busy(void)
{
    if(down) return true;
    if(capture) return false;
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        const pad_t *p = &pads[i];
        if(!p->used) continue;
        if(p->rep_key || dz(axv(p, PC_LEFTX)) || dz(axv(p, PC_LEFTY)) || p->scroll_at) return true;
    }
    return false;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    float dt = (float)(uint32_t)(now - last_tick) / 1000.f;
    if(dt > 0.1f) dt = 0.1f;
    last_tick = now;
    int32_t ox = cx, oy = cy;
    for(int i = 0; i < GAMEPAD_MAX && !capture; i++) {
        pad_t *p = &pads[i];
        if(!p->used) continue;
        int32_t dx = dz(axv(p, PC_LEFTX)), dy = dz(axv(p, PC_LEFTY));
        if(dx) fx_ += speed(dx) * dt;
        if(dy) fy_ += speed(dy) * dt;
        if(p->rep_key && (int32_t)(now - p->rep_at) >= 0) {
            hwkbd_feed(p->rep_key, 2);
            p->rep_at = now + GP_REP_EVERY;
        }
        /* analógico direito: rola (setas) mais rápido quanto mais inclinado */
        int32_t ry = dz(axv(p, PC_RIGHTY));
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
    if(shown_ && !down && (uint32_t)(now - moved_at) > HIDE_MS) cur_show(false);
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
        if((uint32_t)(now - p->active_at) < GAMEPAD_IDLE_MIN * 60000u) continue;
        p->idle_sent = true;
        fprintf(stderr, "bigfoston: controle parado há %d min: desconectando\n", GAMEPAD_IDLE_MIN);
        if(bt_cmd("disconnect %s", p->addr) && settings()->notify)
            osd_show(&bf_icon_bluetooth, 0x8a93a5, tr(S_PAD_IDLE), tr(S_PAD_IDLE_SUB));
    }
}

/* ============================================================ ações */

static void touch(bool press)
{
    if(press) {
        if(!shown_) { moved_at = lv_tick_get(); cur_show(true); tick_need(); return; }   /* só mostra */
        down = true;
    } else if(!down) return;
    else down = false;
    moved_at = lv_tick_get();
    cur_style();
    if(pind) lv_indev_read(pind);
    tick_need();
}

static void used_now(pad_t *p)
{
    p->active_at = lv_tick_get();
    p->idle_sent = false;
}

/* um controle padrão mudou */
static void on_button(pad_t *p, int c, bool on)
{
    used_now(p);
    if(capture) { if(on) hwkbd_activity(); return; }
    if(on && hwkbd_activity()) return;                 /* tela apagada: só acende */
    if(c == PC_A) { touch(on); return; }
    bool rep;
    uint16_t k = ctl_key(c, &rep);
    if(!k) return;
    if(on) key_down(p, k, rep);
    else key_up(p, k);
}

static void on_axis(pad_t *p, int c, int16_t was, int16_t now)
{
    if(c == PC_LTRIGGER || c == PC_RTRIGGER) {         /* gatilho = volume, como botão */
        bool *t = &p->trig[c == PC_RTRIGGER];
        bool on = *t ? now > TRIG_OFF : now > TRIG_ON;
        if(on == *t) return;
        *t = on;
        used_now(p);
        if(capture) return;
        if(on && hwkbd_activity()) return;
        uint16_t k = c == PC_RTRIGGER ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
        if(on) key_down(p, k, true);
        else key_up(p, k);
        return;
    }
    if(c != PC_LEFTX && c != PC_LEFTY && c != PC_RIGHTY) return;
    if(!dz(now)) return;
    used_now(p);
    if(capture) return;
    if(!dz(was) && hwkbd_activity()) { p->ax[c - PC__NBTN] = 0; return; }   /* tela apagada: só acende */
    tick_need();
}

/* ============================================================ mapa aplicado */

static int32_t norm(const gp_axis_t *a, int32_t v)
{
    if(!a->have || a->max <= a->min) return 0;
    int64_t c2 = (int64_t)a->min + a->max, half2 = (int64_t)a->max - a->min;   /* em dobro */
    int64_t r = ((int64_t)v * 2 - c2) * 1000 / half2;
    if(r > 1000) r = 1000;
    if(r < -1000) r = -1000;
    return (int32_t)r;
}

static bool is_trigger(int t) { return t == PC_LTRIGGER || t == PC_RTRIGGER; }

/* o valor de um controle padrão a partir do estado cru */
static int32_t src_value(const pad_t *p, int t, const pm_src_t *s)
{
    switch(s->kind) {
    case PS_BUTTON:
        return s->idx < NRAWB && p->rawb[s->idx] ? 1000 : 0;
    case PS_HAT: {
        if(s->idx > 3) return 0;
        int x = p->hat[s->idx][0], y = p->hat[s->idx][1];
        uint8_t m = (uint8_t)((y < 0 ? 1 : 0) | (x > 0 ? 2 : 0) | (y > 0 ? 4 : 0) | (x < 0 ? 8 : 0));
        return (m & s->mask) ? 1000 : 0;
    }
    case PS_AXIS: {
        int code = pm_axis_code(&p->bits, s->idx);
        int32_t n = code >= 0 && code < GP_NABS ? p->rawa[code] : 0;
        if(s->invert) n = -n;
        if(s->half > 0) return n < 0 ? 0 : n;
        if(s->half < 0) return n > 0 ? 0 : -n;
        if(t < PC__NBTN) return n;                     /* botão de eixo inteiro: lado positivo */
        return is_trigger(t) ? (n + 1000) / 2 : n;     /* gatilho: o eixo inteiro vai de 0 a 1000 */
    }
    default: return 0;
    }
}

static void recompute(pad_t *p)
{
    bool nb[PC__NBTN];
    int16_t na[PC__NAXIS];
    for(int t = 0; t < PC__N; t++) {
        int32_t v = src_value(p, t, &p->map.src[t]);
        if(t < PC__NBTN) { nb[t] = v > 500; continue; }
        const pm_src_t *h = p->map.half[t - PC__NBTN];
        if(h[0].kind || h[1].kind) v += src_value(p, t, &h[0]) - src_value(p, t, &h[1]);
        if(v > 1000) v = 1000;
        if(v < -1000) v = -1000;
        na[t - PC__NBTN] = (int16_t)v;
    }
    for(int t = 0; t < PC__NBTN; t++)
        if(nb[t] != p->btn[t]) {
            p->btn[t] = nb[t];
            on_button(p, t, nb[t]);
            if(!p->used) return;                       /* a ação pode ter tirado o controle */
        }
    for(int t = 0; t < PC__NAXIS; t++)
        if(na[t] != p->ax[t]) {
            int16_t was = p->ax[t];
            p->ax[t] = na[t];
            on_axis(p, t + PC__NBTN, was, na[t]);
        }
}

static void build_index(pad_t *p)
{
    for(unsigned c = 0; c < 0x300; c++) {
        int i = pm_button_index(&p->bits, c);
        p->btn_idx[c] = (int8_t)(i >= 0 && i < NRAWB ? i : -1);
    }
    for(unsigned c = 0; c < GP_NABS; c++) {
        p->axis_idx[c] = (int8_t)pm_axis_index(&p->bits, c);
        p->hat_idx[c] = (int8_t)pm_hat_index(&p->bits, c);
    }
}

/* ============================================================ entrada */

int gamepad_attach(const char *name, const gp_id_t *id, const pm_bits_t *bits,
                   const gp_axis_t abs[GP_NABS], const char *addr)
{
    int s = 0;
    while(s < GAMEPAD_MAX && pads[s].used) s++;
    if(s == GAMEPAD_MAX) return -1;
    pad_t *p = &pads[s];
    memset(p, 0, sizeof *p);
    p->used = true;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    snprintf(p->addr, sizeof p->addr, "%s", addr ? addr : "");
    p->id = *id;
    p->bits = *bits;
    memcpy(p->abs, abs, sizeof p->abs);
    build_index(p);
    /* repouso: o centro de cada eixo */
    for(int c = 0; c < GP_NABS; c++) p->rawa[c] = (int16_t)norm(&p->abs[c], (abs[c].min + abs[c].max) / 2);
    resolve(p);
    /* o estado inicial sem ações; os gatilhos começam soltos */
    for(int t = PC__NBTN; t < PC__N; t++)
        p->ax[t - PC__NBTN] = (int16_t)(is_trigger(t) ? 0 : src_value(p, t, &p->map.src[t]));
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
    memset(p, 0, sizeof *p);
    if(down) { down = false; if(pind) lv_indev_read(pind); }
    if(!gamepad_count()) {
        cur_show(false);
        if(idle_t) { lv_timer_delete(idle_t); idle_t = NULL; }
    }
}

void gamepad_event(int slot, uint16_t type, uint16_t code, int32_t value)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used) return;
    pad_t *p = &pads[slot];
    if(type == EV_KEY) {
        if(value == 2 || code >= 0x300) return;        /* o repetir é nosso */
        p->events++;
        if(observer) observer(slot, type, code, value);
        int b = p->btn_idx[code];
        if(b < 0) return;
        p->rawb[b] = value != 0;
    } else if(type == EV_ABS) {
        if(code >= GP_NABS) return;
        p->events++;
        if(observer) observer(slot, type, code, value);
        int h = p->hat_idx[code];
        if(h >= 0 && h < 4) p->hat[h][(code - ABS_HAT0X) & 1] = (int8_t)(value < 0 ? -1 : value > 0 ? 1 : 0);
        else p->rawa[code] = (int16_t)norm(&p->abs[code], value);
    } else return;
    if(!p->used) return;                               /* o observador pode ter tirado o controle */
    recompute(p);
}

/* ============================================================ SC Controller */

bool gamepad_info(int slot, gp_info_t *out)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used) return false;
    const pad_t *p = &pads[slot];
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", p->name);
    memcpy(out->guid, p->map.guid, sizeof out->guid);
    snprintf(out->addr, sizeof out->addr, "%s", p->addr);
    snprintf(out->map_name, sizeof out->map_name, "%s", p->map.name);
    out->map_from = p->map_from;
    out->bt = p->id.bus == 0x05;                       /* BUS_BLUETOOTH */
    memcpy(out->btn, p->btn, sizeof out->btn);
    memcpy(out->ax, p->ax, sizeof out->ax);
    out->events = p->events;
    return true;
}

const pm_bits_t *gamepad_bits(int slot)
{
    return slot >= 0 && slot < GAMEPAD_MAX && pads[slot].used ? &pads[slot].bits : NULL;
}

void gamepad_capture(bool on, gamepad_obs_t obs)
{
    observer = on ? obs : NULL;
    if(on == capture) return;
    capture = on;
    /* nada fica "apertado" na interface ao trocar de modo */
    for(int i = 0; i < GAMEPAD_MAX; i++)
        if(pads[i].used && pads[i].rep_key) { hwkbd_feed(pads[i].rep_key, 0); pads[i].rep_key = 0; }
    if(down) { down = false; cur_style(); if(pind) lv_indev_read(pind); }
    if(on) cur_show(false);
}

void gamepad_remap(void)
{
    bool cap = capture;
    capture = true;                                    /* a troca de mapa não aperta nada */
    for(int i = 0; i < GAMEPAD_MAX; i++)
        if(pads[i].used) {
            resolve(&pads[i]);
            recompute(&pads[i]);
        }
    capture = cap;
}

int gamepad_axis_norm(int slot, uint16_t code, int32_t value)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used || code >= GP_NABS) return 0;
    return norm(&pads[slot].abs[code], value);
}

int gamepad_axis_now(int slot, uint16_t code)
{
    if(slot < 0 || slot >= GAMEPAD_MAX || !pads[slot].used || code >= GP_NABS) return 0;
    return pads[slot].rawa[code];
}
