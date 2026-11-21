/* scr_power.c — Potência: bateria (só valores reais) e desligar/reiniciar. */
#include "screens.h"
#include "../core/theme.h"
#include "../widgets/battery.h"
#include "../core/status.h"
#include "../widgets/page.h"
#include "../widgets/bottombar.h"
#include <stdio.h>

static void power_click(lv_event_t *e) { (void)e; bottombar_open_power(); }

static const char *tri(bf_tri_t t)
{
    return t == BF_YES ? tr(S_ON) : t == BF_NO ? tr(S_OFF) : tr(S_UNKNOWN);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    status_refresh();
    const bf_status_t *s = status_get();
    lv_obj_t *pg = page_create(parent);

    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, BF_SP_XL, 0);
    bool known = s->battery_pct >= 0;
    lv_obj_t *bat = battery_create(c, 84, 42, BF_FONT_L);
    battery_set(bat, s->battery_pct, s->battery_state);
    lv_obj_t *col = bf_plain(c);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    bf_label(col, tr(S_BATTERY), BF_FONT_S, p->text_dim);
    char b[64];
    if(known) snprintf(b, sizeof b, "%d%%", s->battery_pct);
    else snprintf(b, sizeof b, "—");
    bf_label(col, b, BF_FONT_L, known ? p->text : p->text_dim);
    const char *st = s->battery_state == BF_BAT_DISCHARGING ? tr(S_DISCHARGING) :
                     s->battery_state == BF_BAT_UNKNOWN ? tr(S_UNKNOWN) : battery_state_text(s->battery_state);
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text_fmt(l, "%s  " LV_SYMBOL_BULLET "  %s: %s", st, tr(S_EXT_POWER), tri(s->ext_power));
    lv_obj_set_style_text_font(l, BF_FONT_S, 0);
    lv_obj_set_style_text_color(l, p->text_dim, 0);
    if(s->simulated) bf_pill(c, tr(S_SIMULATED), p->warning);

    if(s->battery_present != BF_YES) page_unavailable(pg, tr(S_MISS_BATTERY));

    lv_obj_t *row = bf_plain(pg);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_t *bt = bf_button(row, LV_SYMBOL_POWER, tr(S_NAV_POWER), 2);
    lv_obj_set_width(bt, 320);
    lv_obj_add_event_cb(bt, power_click, LV_EVENT_CLICKED, NULL);
}

const bf_screen_def_t scr_power_def = { create, NULL, S_SET_POWER, NULL };
