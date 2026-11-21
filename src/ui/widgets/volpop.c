/* volpop.c — ver volpop.h. */
#include "volpop.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../core/fx.h"
#include <stdio.h>
#include <string.h>

#define PANEL_W 84
#define PANEL_H 300
#define IDLE_MS 5000

static lv_obj_t *scrim, *panel, *slider, *pct_lbl, *mute_lbl;
static lv_timer_t *idle;
static int before_mute = 60;         /* volume a restaurar ao desmutar */

const char *volpop_symbol(int pct)
{
    return pct == 0 ? LV_SYMBOL_MUTE : pct < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
}

static void show(int pct)
{
    char b[16];
    if(!panel) return;
    snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(pct_lbl, b);
    lv_label_set_text(mute_lbl, volpop_symbol(pct));
    if(lv_slider_get_value(slider) != pct) lv_slider_set_value(slider, pct, LV_ANIM_OFF);
}

static void poke(void)
{
    if(idle) lv_timer_reset(idle);
}

void volpop_close(void)
{
    if(idle) { lv_timer_delete(idle); idle = NULL; }
    if(scrim) { lv_obj_delete(scrim); scrim = NULL; }
    panel = slider = pct_lbl = mute_lbl = NULL;
}

static void idle_cb(lv_timer_t *t) { (void)t; volpop_close(); }
static void scrim_click(lv_event_t *e) { if(lv_event_get_target(e) == scrim) volpop_close(); }

static void slider_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    if(v > 0) before_mute = v;
    status_provider()->volume_set(v);
    status_refresh();
    show(v);
    poke();
}

void volpop_toggle_mute(void)
{
    int v = status_provider()->volume_get();
    if(v < 0) return;
    if(v > 0) { before_mute = v; v = 0; }
    else v = before_mute > 0 ? before_mute : 60;
    status_provider()->volume_set(v);
    status_refresh();
    show(v);
    fx_sound(FX_TOGGLE);
}

static void mute_click(lv_event_t *e) { (void)e; volpop_toggle_mute(); poke(); }

void volpop_open(int x, int y)
{
    const bf_palette_t *p = pal();
    int v = status_provider()->volume_get();
    if(panel) { volpop_close(); return; }        /* segundo toque no mesmo botão fecha */
    if(v < 0) return;                             /* sem placa de som */
    if(v > 0) before_mute = v;

    scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, scrim_click, LV_EVENT_CLICKED, NULL);

    panel = bf_card(scrim);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    int px = x - PANEL_W / 2, py = y;
    if(px < 8) px = 8;
    if(px > 800 - PANEL_W - 8) px = 800 - PANEL_W - 8;
    if(py < 8) py = 8;
    if(py > 480 - PANEL_H - 8) py = 480 - PANEL_H - 8;
    lv_obj_set_pos(panel, px, py);
    lv_obj_set_style_pad_all(panel, BF_SP_S, 0);
    lv_obj_set_style_pad_row(panel, BF_SP_S, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_50, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);    /* toque no painel não fecha */

    pct_lbl = bf_label(panel, "", BF_FONT_S, p->text);
    slider = lv_slider_create(panel);               /* mais alto que largo: vertical */
    lv_obj_set_size(slider, 12, PANEL_H - 2 * BF_SP_S - 3 * BF_SP_S - 18 - 56);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_ext_click_area(slider, 24);
    lv_obj_set_style_bg_color(slider, p->surface2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, p->primary, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, p->text, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *mb = bf_button(panel, LV_SYMBOL_VOLUME_MAX, NULL, 0);
    lv_obj_set_size(mb, 60, 56);
    lv_obj_add_event_cb(mb, mute_click, LV_EVENT_CLICKED, NULL);
    mute_lbl = lv_obj_get_child(mb, 0);

    idle = lv_timer_create(idle_cb, IDLE_MS, NULL);
    show(v);
}

bool volpop_is_open(void) { return panel != NULL; }
