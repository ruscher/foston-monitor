/* scr_perf.c — Desempenho do sistema: efeitos visuais (core/powerplan.c), detalhe do mapa do
 * Big GPS (docs/performance/09-gps-map-engine.md) e atalhos para o plano de energia e o modo
 * performance, que trabalham juntos. */
#include "screens.h"
#include <stdio.h>
#include "../core/powerplan.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../widgets/page.h"

static lv_obj_t *pg_root;
static void build(lv_obj_t *parent);
static void rebuild(void);

static void pick(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if(d == settings()->map_detail) return;
    settings()->map_detail = d;
    settings_save();
    rebuild();
}

static void rebuild(void)
{
    lv_obj_t *parent = lv_obj_get_parent(pg_root);
    int32_t y = lv_obj_get_scroll_y(pg_root);
    lv_obj_delete(pg_root);
    build(parent);
    lv_obj_update_layout(pg_root);
    lv_obj_scroll_to_y(pg_root, y, LV_ANIM_OFF);
}

static void pick_fx(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if(f == settings()->fx_level) return;
    fx_level_apply(f);
    rebuild();
}

static void go_power(lv_event_t *e) { (void)e; nav_go(SCR_POWER, 0); }
static void go_perfmode(lv_event_t *e) { (void)e; nav_go(SCR_PERFMODE, 0); }

static void section(lv_obj_t *p, bf_str_t title)
{
    lv_obj_t *l = bf_label(p, tr(title), BF_FONT_S, pal()->text_dim);
    lv_obj_set_style_pad_top(l, BF_SP_M, 0);
}

static void build(lv_obj_t *parent)
{
    static const bf_str_t title[BF_DETAIL__N] = { S_PERF_AUTO, S_PERF_ECO, S_PERF_BAL, S_PERF_DET };
    static const bf_str_t sub[BF_DETAIL__N] = { S_PERF_AUTO_SUB, S_PERF_ECO_SUB, S_PERF_BAL_SUB, S_PERF_DET_SUB };
    static const bf_str_t fxt[BF_FX__N] = { S_FX_FULL, S_FX_RED, S_FX_MIN };
    static const bf_str_t fxs[BF_FX__N] = { S_FX_FULL_SUB, S_FX_RED_SUB, S_FX_MIN_SUB };
    static const bf_str_t plans[BF_PLAN__N] = { S_PLAN_ECO, S_PLAN_BAL, S_PLAN_PERF };
    const bf_palette_t *p = pal();
    lv_obj_t *pg = pg_root = page_create(parent);
    page_header(pg, BF_SYMBOL_ROCKET, p->accent, tr(S_SET_PERF), tr(S_PERF_HINT));

    /* o plano de energia e o modo performance trabalham junto com esta página */
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(c, BF_SP_S, 0);
    char b[80];
    int pl = settings()->power_plan;
    snprintf(b, sizeof b, tr(S_FX_PLAN_NOW), tr(plans[pl >= 0 && pl < BF_PLAN__N ? pl : BF_PLAN_BAL]));
    lv_obj_t *l = bf_label(c, b, BF_FONT_M, p->text);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *bp = bf_button(c, LV_SYMBOL_BATTERY_3, tr(S_SET_POWER), 0);
    lv_obj_set_height(bp, 46);
    lv_obj_add_event_cb(bp, go_power, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bm = bf_button(c, LV_SYMBOL_CHARGE, tr(S_SET_PERFMODE), 0);
    lv_obj_set_height(bm, 46);
    lv_obj_add_event_cb(bm, go_perfmode, LV_EVENT_CLICKED, NULL);

    section(pg, S_FX_TITLE);
    for(int i = 0; i < BF_FX__N; i++)
        page_choice(pg, NULL, tr(fxt[i]), tr(fxs[i]), i == settings()->fx_level, pick_fx, (void *)(intptr_t)i);

    section(pg, S_FX_MAP);
    for(int i = 0; i < BF_DETAIL__N; i++)
        page_choice(pg, NULL, tr(title[i]), tr(sub[i]), i == settings()->map_detail, pick, (void *)(intptr_t)i);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    build(parent);
}

const bf_screen_def_t scr_perf_def = { create, NULL, S_SET_PERF, NULL };
