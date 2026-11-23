/* scr_bigscreen.c — app Big Screen: o FOSTON vira o painel do Big Screen
 * Monitor Display do PC (core/bigscreen.c, FSP por TCP na rede USB).
 * Enquanto espera: instruções e status. Conectado: o quadro do PC cobre a
 * tela inteira (camada superior) e os toques voltam ao PC; um toque mostra,
 * por alguns segundos, uma barra no topo só com o "X", que sai para a Início. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/bigscreen.h"
#include "../core/osd.h"
#include "../widgets/page.h"
#include "../assets/icons/bf_icons.h"
#include "../third_party/fsp/fsp.h"
#include <stdio.h>
#include <string.h>

#define BAR_H       64                 /* barra de sair no topo */
#define BAR_SHOW_MS 4000               /* some sozinha após este tempo sem toque */

static lv_timer_t *timer;
static lv_obj_t *state_lbl, *detail_lbl, *err_lbl, *panel, *bar;
static uint32_t bar_t;
static bs_state_t last_state = BS_OFF;
static bool leaving;                   /* saída já pedida */

static void leave(bool home)
{
    if(leaving) return;
    leaving = true;
    if(home) nav_home();               /* "X": volta à tela principal do FOSTON */
    else nav_back();                   /* destroy() para o cliente */
}

static void set_text_if(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* ------------------------------------------------------------ painel */

static void bar_close(lv_event_t *e) { (void)e; leave(true); }

/* barra translúcida no topo, só com o "X" à direita; por cima do painel */
static void bar_show(void)
{
    if(!panel) return;
    bar_t = lv_tick_get();
    if(bar) return;
    bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BF_SCREEN_W, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* a faixa não vai ao PC */

    lv_obj_t *x = lv_obj_create(bar);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, 52, 52);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(x, 6);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(x, bar_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(x);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(l, BF_FONT_L, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
}

static void bar_hide(void)
{
    if(!bar) return;
    lv_obj_delete(bar);
    bar = NULL;
}

static void panel_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if(c == LV_EVENT_PRESSED) {
        bs_touch(FSP_TOUCH_DOWN, pt.x, pt.y);
        bar_show();                    /* todo toque mostra (ou segura) a barra de sair */
    }
    else if(c == LV_EVENT_PRESSING) bs_touch(FSP_TOUCH_MOVE, pt.x, pt.y);
    else if(c == LV_EVENT_RELEASED) bs_touch(FSP_TOUCH_UP, pt.x, pt.y);
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
    bar_hide();
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
            bar_show();                /* primeiro quadro: a barra aparece e mostra onde sair */
        }
        lv_area_t a = bs_last_dirty();
        if(a.x2 >= a.x1 && a.y2 >= a.y1) lv_obj_invalidate_area(panel, &a);
        lv_refr_now(NULL);
        bs_presented(lv_tick_elaps(t0));
    }
    if(bar && lv_tick_elaps(bar_t) >= BAR_SHOW_MS) bar_hide();
    const bs_status_t *s = bs_status();
    if(s->quit_requested) { leave(false); return; }
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
