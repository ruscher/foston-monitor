/* page.c — blocos comuns de página (cabeçalho, avisos, linhas chave/valor). */
#include "page.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../core/status.h"

lv_obj_t *page_create(lv_obj_t *parent)
{
    lv_obj_t *pg = bf_plain(parent);
    lv_obj_set_size(pg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(pg, BF_SP_XL, 0);
    lv_obj_set_style_pad_ver(pg, BF_SP_L, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pg, BF_SP_M, 0);
    lv_obj_add_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pg, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_AUTO);
    return pg;
}

lv_obj_t *page_round_icon(lv_obj_t *parent, const char *symbol, lv_color_t color, int size)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, size / 3, 0);
    lv_obj_set_style_bg_color(c, color, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = bf_label(c, symbol, size >= 56 ? BF_FONT_L : BF_FONT_M, color);
    lv_obj_center(l);
    return c;
}

lv_obj_t *page_header(lv_obj_t *page, const char *symbol, lv_color_t color,
                      const char *title, const char *subtitle)
{
    const bf_palette_t *p = pal();
    lv_obj_t *h = bf_plain(page);
    lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, BF_SP_L, 0);
    if(symbol) page_round_icon(h, symbol, color, 56);
    lv_obj_t *col = bf_plain(h);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    bf_label(col, title, BF_FONT_L, p->text);
    if(subtitle) {
        lv_obj_t *s = bf_label(col, subtitle, BF_FONT_S, p->text_dim);
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    }
    return h;
}

lv_obj_t *page_unavailable(lv_obj_t *page, const char *missing)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_card(page);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(c, p->warning, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_60, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    lv_obj_t *row = bf_plain(c);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    bf_label(row, BF_SYMBOL_ALERT, BF_FONT_M, p->warning);
    lv_obj_t *t = bf_label(row, tr(S_UNAVAILABLE), BF_FONT_M, p->text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    if(missing) {
        lv_obj_t *m = lv_label_create(c);
        lv_label_set_text_fmt(m, "%s: %s", tr(S_WHAT_MISSING), missing);
        lv_obj_set_style_text_font(m, BF_FONT_S, 0);
        lv_obj_set_style_text_color(m, p->text_dim, 0);
        lv_obj_set_width(m, LV_PCT(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    }
    return c;
}

lv_obj_t *page_kv(lv_obj_t *parent, const char *key, const char *value)
{
    const bf_palette_t *p = pal();
    lv_obj_t *row = bf_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *k = bf_label(row, key, BF_FONT_M, p->text_dim);
    lv_obj_set_width(k, 230);
    lv_obj_t *v = bf_label(row, value, BF_FONT_M, p->text);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    return v;
}

lv_obj_t *page_switch_row(lv_obj_t *parent, const char *label, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *row = bf_card(parent);
    lv_obj_set_size(row, LV_PCT(100), 76);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = bf_label(row, label, BF_FONT_M, pal()->text);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 96, 52);   /* grande o bastante para o dedo */
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 12);
    if(cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    return sw;
}

void page_sim_note(lv_obj_t *parent)
{
    if(!status_provider()->simulated) return;
    lv_obj_t *n = bf_pill(parent, tr(S_SIM_NOTE), pal()->warning);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_radius(n, BF_RADIUS_S, 0);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_style_pad_ver(n, 6, 0);
}

static void slider_label_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lbl = lv_event_get_user_data(e);
    lv_label_set_text_fmt(lbl, "%d%%", (int)lv_slider_get_value(sl));
}

lv_obj_t *page_slider_card(lv_obj_t *parent, const char *label, const char *sym_lo,
                           const char *sym_hi, int min, int value, lv_event_cb_t cb, void *ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_card(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_L, 0);
    lv_obj_t *top = bf_plain(c);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bf_label(top, label, BF_FONT_M, p->text);
    lv_obj_t *val = bf_label(top, "", BF_FONT_L, p->accent);
    lv_label_set_text_fmt(val, "%d%%", value);

    lv_obj_t *row = bf_plain(c);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_L, 0);
    bf_label(row, sym_lo, BF_FONT_M, p->text_dim);
    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_height(sl, 16);
    lv_obj_set_flex_grow(sl, 1);
    lv_slider_set_range(sl, min, 100);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, p->surface2, 0);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sl, 16, LV_PART_KNOB);      /* botão ~48 px */
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, p->primary, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 4, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 24);                    /* faixa de toque de 64 px */
    bf_label(row, sym_hi, BF_FONT_L, p->text_dim);
    lv_obj_add_event_cb(sl, slider_label_cb, LV_EVENT_VALUE_CHANGED, val);
    if(cb) {
        lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, ud);
        lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, ud);
    }
    return sl;
}
