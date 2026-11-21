/* scr_bigscreen.c — app Big Screen: o FOSTON vira o painel do Big Screen
 * Monitor Display do PC (core/bigscreen.c, FSP por TCP na rede USB).
 * Enquanto espera: instruções e status. Conectado: o quadro do PC cobre a
 * tela inteira (camada superior) e os toques voltam ao PC; segurar o canto
 * superior esquerdo por 3 s sai. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/bigscreen.h"
#include "../core/osd.h"
#include "../widgets/page.h"
#include "../assets/icons/bf_icons.h"
#include "../third_party/fsp/fsp.h"
#include <stdio.h>
#include <string.h>

#define EXIT_CORNER 96
#define EXIT_HOLD_MS 3000

static lv_timer_t *timer;
static lv_obj_t *state_lbl, *detail_lbl, *err_lbl, *panel;
static uint32_t corner_t;
static bool corner_on;
static bs_state_t last_state = BS_OFF;
static bool leaving;                   /* nav_back já pedido */

static void leave(void)
{
    if(leaving) return;
    leaving = true;
    nav_back();                        /* destroy() para o cliente */
}

static void set_text_if(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* ------------------------------------------------------------ painel */

static void panel_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    bool corner = pt.x < EXIT_CORNER && pt.y < EXIT_CORNER;
    if(c == LV_EVENT_PRESSED) {
        bs_touch(FSP_TOUCH_DOWN, pt.x, pt.y);
        corner_on = corner;
        corner_t = lv_tick_get();
    }
    else if(c == LV_EVENT_PRESSING) {
        bs_touch(FSP_TOUCH_MOVE, pt.x, pt.y);
        if(!corner) corner_on = false;
        if(corner_on && lv_tick_elaps(corner_t) >= EXIT_HOLD_MS) {
            corner_on = false;
            lv_indev_wait_release(lv_indev_active());
            bs_touch(FSP_TOUCH_UP, pt.x, pt.y);
            leave();
        }
    }
    else if(c == LV_EVENT_RELEASED) {
        bs_touch(FSP_TOUCH_UP, pt.x, pt.y);
        corner_on = false;
    }
}

static void panel_show(void)
{
    if(panel) return;
    panel = lv_image_create(lv_layer_top());
    lv_image_set_src(panel, bs_image());
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* USER_1: sem som de toque */
    lv_obj_add_event_cb(panel, panel_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(panel, panel_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(panel, panel_event, LV_EVENT_RELEASED, NULL);
}

static void panel_hide(void)
{
    if(!panel) return;
    lv_obj_delete(panel);
    panel = NULL;
    /* o PC saiu: avisa que voltou a esperar (sem aviso quando quem sai é o usuário) */
    if(!leaving) osd_show(&bf_icon_bigscreen, 0x6c7383, tr(S_BS_OSD_OFF), tr(S_BS_WAITING));
}

/* ------------------------------------------------------------ espera */

static void tick(lv_timer_t *t)
{
    (void)t;
    const bf_palette_t *p = pal();
    if(bs_poll()) {
        uint32_t t0 = lv_tick_get();
        if(!panel) {
            panel_show();
            /* primeiro quadro: o aviso fica por cima do painel e ensina a sair */
            osd_show(&bf_icon_bigscreen, 0x2a6fd0, tr(S_BS_OSD_ON), tr(S_BS_OSD_EXIT));
        }
        lv_area_t a = bs_last_dirty();
        if(a.x2 >= a.x1 && a.y2 >= a.y1) lv_obj_invalidate_area(panel, &a);
        lv_refr_now(NULL);
        bs_presented(lv_tick_elaps(t0));
    }
    const bs_status_t *s = bs_status();
    if(s->quit_requested) { leave(); return; }
    if(s->state != BS_CONNECTED) panel_hide();
    if(s->state == last_state && s->state != BS_CONNECTED) return;
    last_state = s->state;
    char b[160];
    if(s->state == BS_CONNECTED) {
        snprintf(b, sizeof b, tr(S_BS_CONNECTED), s->host[0] ? s->host : s->peer);
        set_text_if(state_lbl, b);
        lv_obj_set_style_text_color(state_lbl, p->success, 0);
        snprintf(b, sizeof b, tr(S_BS_FRAMES), (unsigned)s->frames);
        set_text_if(detail_lbl, b);
    }
    else if(s->state == BS_WAITING) {
        set_text_if(state_lbl, tr(S_BS_WAITING));
        lv_obj_set_style_text_color(state_lbl, p->accent, 0);
        set_text_if(detail_lbl, "192.168.7.2 · TCP 7707");
    }
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(parent);
    page_header(pg, BF_SYMBOL_NET, lv_color_hex(0x2a6fd0), tr(S_APP_BIGSCREEN), tr(S_BS_SUB));

    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    state_lbl = bf_label(c, "", BF_FONT_L, p->accent);
    detail_lbl = bf_label(c, "", BF_FONT_M, p->text_dim);
    err_lbl = bf_label(c, "", BF_FONT_S, p->danger);
    lv_obj_add_flag(err_lbl, LV_OBJ_FLAG_HIDDEN);

    c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *steps = bf_label(c, tr(S_BS_STEPS), BF_FONT_S, p->text);
    lv_obj_set_width(steps, LV_PCT(100));
    lv_label_set_long_mode(steps, LV_LABEL_LONG_WRAP);

    lv_obj_t *h = bf_label(pg, tr(S_BS_EXIT_HINT), BF_FONT_S, p->text_dim);
    lv_obj_set_width(h, LV_PCT(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);

    char err[96];
    last_state = BS_OFF;
    leaving = false;
    if(!bs_start(err, sizeof err)) {
        set_text_if(state_lbl, tr(S_BS_FAILED));
        lv_obj_set_style_text_color(state_lbl, p->danger, 0);
        lv_label_set_text(err_lbl, err);
        lv_obj_remove_flag(err_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    tick(NULL);
    timer = lv_timer_create(tick, 10, NULL);
}

static void destroy(void)
{
    if(timer) { lv_timer_delete(timer); timer = NULL; }
    leaving = true;                     /* saindo do app: sem aviso de "desconectado" */
    panel_hide();
    bs_stop();
    state_lbl = detail_lbl = err_lbl = NULL;
}

const bf_screen_def_t scr_bigscreen_def = { create, destroy, S_APP_BIGSCREEN, NULL };
