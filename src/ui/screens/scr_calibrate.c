/* scr_calibrate.c — assistente de calibração do toque (5 pontos, tela cheia).
 * Grava $BIGFOSTON_DATA/pointercal e aplica a calibração ao leitor de toque. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/touch_calib.h"
#include <stdio.h>

static const bf_pt_t targets[BF_CALIB_POINTS] = {
    { 50, 50 }, { 750, 50 }, { 750, 430 }, { 50, 430 }, { 400, 240 },
};

static lv_obj_t *ov, *panel, *cross, *hint;
static int step;            /* -1 intro, 0..4 alvos, 5 resultado */
static bf_pt_t got[BF_CALIB_POINTS];
static int32_t acc_x, acc_y, acc_n;
static bool demo;

static void show_step(void);

static void close_click(lv_event_t *e) { (void)e; nav_back(); }
static void start_click(lv_event_t *e) { (void)e; step = 0; show_step(); }

static void ov_event(lv_event_t *e)
{
    if(step < 0 || step >= BF_CALIB_POINTS) return;
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if(!in) return;
    if(c == LV_EVENT_PRESSED) { acc_x = acc_y = acc_n = 0; }
    if(c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING) {
        /* no aparelho, a leitura bruta do ADC; no simulador, o ponto do mouse */
        bf_pt_t raw;
        if(touch_last_raw(&raw)) { acc_x += raw.x; acc_y += raw.y; }
        else {
            lv_point_t pt;
            lv_indev_get_point(in, &pt);
            acc_x += pt.x; acc_y += pt.y;
        }
        acc_n++;
    }
    else if(c == LV_EVENT_RELEASED && acc_n) {
        got[step].x = acc_x / acc_n;
        got[step].y = acc_y / acc_n;
        step++;
        show_step();
    }
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *sym, const char *txt, int kind, lv_event_cb_t cb)
{
    lv_obj_t *b = bf_button(parent, sym, txt, kind);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_panel(const char *title, const char *body, lv_color_t body_color)
{
    const bf_palette_t *p = pal();
    if(panel) lv_obj_delete(panel);
    panel = bf_card(ov);
    lv_obj_set_width(panel, 560);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, BF_SP_XL, 0);
    lv_obj_set_style_radius(panel, BF_RADIUS_L, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, BF_SP_L, 0);
    lv_obj_t *h = bf_plain(panel);
    lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, BF_SP_M, 0);
    bf_label(h, BF_SYMBOL_CROSSHAIR, BF_FONT_L, p->accent);
    bf_label(h, title, BF_FONT_L, p->text);
    lv_obj_t *b = bf_label(panel, body, BF_FONT_M, body_color);
    lv_obj_set_width(b, LV_PCT(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
}

static void show_step(void)
{
    const bf_palette_t *p = pal();
    if(cross) { lv_obj_delete(cross); cross = NULL; }
    if(hint) { lv_obj_delete(hint); hint = NULL; }
    if(panel) { lv_obj_delete(panel); panel = NULL; }

    if(step < 0) {
        build_panel(tr(S_SET_CALIBRATE), tr(S_CAL_INTRO), p->text_dim);
        lv_obj_t *row = bf_plain(panel);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, BF_SP_M, 0);
        mk_btn(row, NULL, tr(S_CANCEL), 0, close_click);
        mk_btn(row, LV_SYMBOL_PLAY, tr(S_CAL_START), 1, start_click);
        return;
    }
    if(step < BF_CALIB_POINTS) {
        /* alvo: anel + mira, 64 px */
        cross = lv_obj_create(ov);
        lv_obj_remove_style_all(cross);
        lv_obj_set_size(cross, 64, 64);
        lv_obj_set_pos(cross, targets[step].x - 32, targets[step].y - 32);
        lv_obj_set_style_radius(cross, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(cross, 3, 0);
        lv_obj_set_style_border_color(cross, p->accent, 0);
        lv_obj_remove_flag(cross, LV_OBJ_FLAG_CLICKABLE);
        for(int i = 0; i < 2; i++) {
            lv_obj_t *l = lv_obj_create(cross);
            lv_obj_remove_style_all(l);
            lv_obj_set_size(l, i ? 2 : 84, i ? 84 : 2);
            lv_obj_set_style_bg_color(l, p->text, 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_center(l);
        }
        lv_obj_t *dot = lv_obj_create(cross);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, p->danger, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_center(dot);
        lv_obj_set_style_clip_corner(cross, false, 0);
        lv_obj_add_flag(cross, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        hint = lv_label_create(ov);
        lv_label_set_text_fmt(hint, "%s %d %s %d\n%s", tr(S_CAL_POINT), step + 1, tr(S_CAL_OF),
                              BF_CALIB_POINTS, tr(S_CAL_INTRO));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hint, p->text_dim, 0);
        lv_obj_set_width(hint, 520);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, step == 4 ? 110 : 0);
        return;
    }

    /* resultado */
    bf_calib_t cal;
    int err = 0;
    char body[300];
    lv_color_t bc = p->text;
    if(calib_compute(targets, got, &cal, &err) && err < 40) {
        bool saved = demo ? true : calib_save(&cal);
        bool applied = demo ? false : touch_calibration_apply(&cal);
        snprintf(body, sizeof body, "%s: %d px\n%s%s", tr(S_CAL_ERROR), err,
                 saved ? "" : "(!) ", applied ? tr(S_SAVED) : tr(S_CAL_STUB));
        build_panel(tr(S_CAL_DONE), body, bc);
    }
    else {
        build_panel(tr(S_SET_CALIBRATE), tr(S_CAL_FAIL), p->warning);
    }
    lv_obj_t *row = bf_plain(panel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    mk_btn(row, BF_SYMBOL_UNDO, tr(S_CAL_RETRY), 0, start_click);
    mk_btn(row, LV_SYMBOL_OK, tr(S_CLOSE), 1, close_click);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)parent; (void)arg;
    ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, BF_SCREEN_W, BF_SCREEN_H);
    lv_obj_set_style_bg_color(ov, pal()->bg, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, ov_event, LV_EVENT_ALL, NULL);
    step = -1;
    demo = false;
    panel = cross = hint = NULL;
    show_step();
}

static void destroy(void)
{
    if(ov) { lv_obj_delete(ov); ov = NULL; }
    panel = cross = hint = NULL;
}

void scr_calibrate_demo_step(int s)
{
    if(!ov) return;
    demo = true;
    if(s >= BF_CALIB_POINTS) {
        /* toques sintéticos com pequeno desvio (somente p/ captura de tela) */
        for(int i = 0; i < BF_CALIB_POINTS; i++) {
            got[i].x = targets[i].x + (i % 2 ? 3 : -2);
            got[i].y = targets[i].y + (i % 3 ? -2 : 3);
        }
    }
    step = s;
    show_step();
}

const bf_screen_def_t scr_calibrate_def = { create, destroy, S_SET_CALIBRATE, NULL };
