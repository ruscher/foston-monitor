/* scr_perf.c — Desempenho: nível de detalhe do mapa do Big GPS
 * (docs/performance/09-gps-map-engine.md). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../widgets/page.h"

static lv_obj_t *pg_root;
static void build(lv_obj_t *parent);

static void pick(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if(d == settings()->map_detail) return;
    settings()->map_detail = d;
    settings_save();
    lv_obj_t *parent = lv_obj_get_parent(pg_root);
    lv_obj_delete(pg_root);
    build(parent);
}

static void build(lv_obj_t *parent)
{
    static const bf_str_t title[BF_DETAIL__N] = { S_PERF_AUTO, S_PERF_ECO, S_PERF_BAL, S_PERF_DET };
    static const bf_str_t sub[BF_DETAIL__N] = { S_PERF_AUTO_SUB, S_PERF_ECO_SUB, S_PERF_BAL_SUB, S_PERF_DET_SUB };
    const bf_palette_t *p = pal();
    lv_obj_t *pg = pg_root = page_create(parent);
    page_header(pg, BF_SYMBOL_ROCKET, p->accent, tr(S_SET_PERF), tr(S_PERF_HINT));
    for(int i = 0; i < BF_DETAIL__N; i++) {
        bool cur = i == settings()->map_detail;
        lv_obj_t *b = lv_button_create(pg);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &bf_st_card, 0);
        lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
        if(cur) {
            lv_obj_set_style_border_color(b, p->primary, 0);
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        }
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, BF_SP_L, 0);
        lv_obj_t *col = bf_plain(b);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *t = bf_label(col, tr(title[i]), BF_FONT_M, p->text);
        lv_obj_set_width(t, LV_PCT(100));
        lv_obj_t *s = bf_label(col, tr(sub[i]), BF_FONT_S, p->text_dim);
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        if(cur) bf_label(b, LV_SYMBOL_OK, BF_FONT_M, p->primary);
        lv_obj_add_event_cb(b, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    build(parent);
}

const bf_screen_def_t scr_perf_def = { create, NULL, S_SET_PERF, NULL };
