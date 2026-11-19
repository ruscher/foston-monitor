/* scr_monitor.c — monitor de desempenho: CPU, RAM, carga e processos.
 * Um único timer de 2 s, existente só enquanto a tela está aberta. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/sysinfo.h"
#include <stdio.h>

#define POINTS 60   /* 60 x 2 s = 2 min */

static lv_timer_t *timer;
static lv_obj_t *chart, *v_cpu, *v_ram, *v_load, *v_procs, *bar_cpu, *bar_ram;
static lv_chart_series_t *s_cpu, *s_ram;
static bf_cpu_sample_t prev;

static lv_obj_t *stat_card(lv_obj_t *parent, const char *title, lv_color_t color, lv_obj_t **bar)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_card(parent);
    lv_obj_set_size(c, 176, 100);
    lv_obj_set_style_pad_all(c, BF_SP_M, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_t *t = bf_label(c, title, BF_FONT_S, p->text_dim);
    bf_label_ellipsis(t, LV_PCT(100));
    lv_obj_t *v = bf_label(c, "—", BF_FONT_L, color);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(v, LV_PCT(100));
    if(bar) {
        *bar = lv_bar_create(c);
        lv_obj_set_size(*bar, LV_PCT(100), 6);
        lv_bar_set_range(*bar, 0, 100);
        lv_obj_set_style_bg_color(*bar, p->surface2, 0);
        lv_obj_set_style_bg_color(*bar, color, LV_PART_INDICATOR);
    }
    return v;
}

static void update(lv_timer_t *t)
{
    lv_timer_set_period(t, 2000);
    char buf[48];
    bf_cpu_sample_t now;
    if(sysinfo_cpu_sample(&now)) {
        int pm = sysinfo_cpu_permille(&prev, &now);
        prev = now;
        snprintf(buf, sizeof buf, "%d,%d%%", pm / 10, pm % 10);
        lv_label_set_text(v_cpu, buf);
        lv_bar_set_value(bar_cpu, pm / 10, LV_ANIM_OFF);
        lv_chart_set_next_value(chart, s_cpu, pm / 10);
    }
    unsigned long tot, av;
    if(sysinfo_mem(&tot, &av) && tot) {
        int pct = (int)((tot - av) * 100 / tot);
        char used[24];
        fmt_bytes((unsigned long long)(tot - av) * 1024, used, sizeof used);
        lv_label_set_text(v_ram, used);
        lv_bar_set_value(bar_ram, pct, LV_ANIM_OFF);
        lv_chart_set_next_value(chart, s_ram, pct);
    }
    if(sysinfo_loadavg(buf, sizeof buf)) lv_label_set_text(v_load, buf);
    int n = sysinfo_proc_count();
    if(n >= 0) lv_label_set_text_fmt(v_procs, "%d", n);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = bf_plain(parent);
    lv_obj_set_size(pg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(pg, BF_SP_XL, 0);
    lv_obj_set_style_pad_ver(pg, BF_SP_L, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pg, BF_SP_M, 0);

    lv_obj_t *row = bf_plain(pg);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    v_cpu = stat_card(row, tr(S_MON_CPU), p->primary, &bar_cpu);
    v_ram = stat_card(row, tr(S_MON_RAM), p->accent, &bar_ram);
    v_load = stat_card(row, tr(S_MON_LOAD), p->text, NULL);
    lv_obj_set_style_text_font(v_load, BF_FONT_M, 0);
    lv_obj_set_style_margin_top(v_load, 6, 0);
    v_procs = stat_card(row, tr(S_MON_PROCS), p->text, NULL);

    lv_obj_t *cc = bf_card(pg);
    lv_obj_set_width(cc, LV_PCT(100));
    lv_obj_set_flex_grow(cc, 1);
    lv_obj_set_style_pad_all(cc, BF_SP_M, 0);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cc, 6, 0);

    lv_obj_t *leg = bf_plain(cc);
    lv_obj_set_size(leg, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(leg, BF_SP_M, 0);
    bf_label(leg, LV_SYMBOL_MINUS " CPU %", BF_FONT_S, p->primary);
    bf_label(leg, LV_SYMBOL_MINUS " RAM %", BF_FONT_S, p->accent);
    lv_obj_t *sp = bf_plain(leg);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);
    bf_label(leg, tr(S_MON_CHART), BF_FONT_S, p->text_dim);

    chart = lv_chart_create(cc);
    lv_obj_set_width(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 5, 7);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 2, 0);
    lv_obj_set_style_line_color(chart, p->border, LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);  /* sem bolinhas nos pontos */
    s_cpu = lv_chart_add_series(chart, p->primary, LV_CHART_AXIS_PRIMARY_Y);
    s_ram = lv_chart_add_series(chart, p->accent, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, s_cpu, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart, s_ram, LV_CHART_POINT_NONE);

    sysinfo_cpu_sample(&prev);
    /* primeira leitura após 400 ms (para não abrir vazia); depois a cada 2 s */
    timer = lv_timer_create(update, 400, NULL);
}

static void destroy(void)
{
    if(timer) { lv_timer_delete(timer); timer = NULL; }
    chart = NULL;
}

const bf_screen_def_t scr_monitor_def = { create, destroy, S_APP_MONITOR, NULL };
