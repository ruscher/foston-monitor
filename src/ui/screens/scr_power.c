/* scr_power.c — Potência: bateria (só valores reais), plano de energia (core/powerplan.c) e
 * desligar/reiniciar. */
#include "screens.h"
#include "../core/powerplan.h"
#include "../core/settings.h"
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

static lv_obj_t *pg_root;
static void create(lv_obj_t *parent, int arg);

static void pick_plan(lv_event_t *e)
{
    int pl = (int)(intptr_t)lv_event_get_user_data(e);
    powerplan_apply(pl);                              /* aplica de novo mesmo se já era (refaz os ajustes) */
    lv_obj_t *parent = lv_obj_get_parent(pg_root);
    int32_t y = lv_obj_get_scroll_y(pg_root);
    lv_obj_delete(pg_root);
    create(parent, 0);
    lv_obj_update_layout(pg_root);
    lv_obj_scroll_to_y(pg_root, y, LV_ANIM_OFF);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    status_refresh();
    const bf_status_t *s = status_get();
    lv_obj_t *pg = pg_root = page_create(parent);

    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, BF_SP_XL, 0);
    bool known = s->battery_pct >= 0;
    lv_obj_t *bat = battery_create(c, BF_FONT_L);
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

    /* plano de energia (core/powerplan.c): junto com o modo performance e os efeitos */
    lv_obj_t *plt = bf_label(pg, tr(S_PLAN_TITLE), BF_FONT_S, p->text_dim);
    lv_obj_set_style_pad_top(plt, BF_SP_M, 0);
    static const bf_str_t pt[BF_PLAN__N] = { S_PLAN_ECO, S_PLAN_BAL, S_PLAN_PERF };
    static const bf_str_t ps[BF_PLAN__N] = { S_PLAN_ECO_SUB, S_PLAN_BAL_SUB, S_PLAN_PERF_SUB };
    static const char *const sym[BF_PLAN__N] = { LV_SYMBOL_BATTERY_3, LV_SYMBOL_SHUFFLE, LV_SYMBOL_CHARGE };
    for(int i = 0; i < BF_PLAN__N; i++)
        page_choice(pg, sym[i], tr(pt[i]), tr(ps[i]), i == settings()->power_plan, pick_plan, (void *)(intptr_t)i);
    lv_obj_t *note = bf_label(pg, tr(S_PLAN_NOTE), BF_FONT_S, p->text_dim);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    lv_obj_t *row = bf_plain(pg);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_t *bt = bf_button(row, LV_SYMBOL_POWER, tr(S_NAV_POWER), 2);
    lv_obj_set_width(bt, 320);
    lv_obj_add_event_cb(bt, power_click, LV_EVENT_CLICKED, NULL);
}

const bf_screen_def_t scr_power_def = { create, NULL, S_SET_POWER, NULL };
