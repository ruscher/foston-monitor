/* scr_date.c — Data: data/hora atuais, fuso e formato. Acertar o relógio
 * ainda não é possível (RTC/NTP e privilégios): dito com honestidade. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../widgets/page.h"
#include <stdlib.h>
#include <time.h>

static lv_timer_t *timer;
static lv_obj_t *now_lbl, *date_lbl;

static void tick(lv_timer_t *t)
{
    (void)t;
    time_t n = time(NULL);
    struct tm tmv;
    localtime_r(&n, &tmv);
    char b[48];
    strftime(b, sizeof b, settings()->clock_24h ? "%H:%M:%S" : "%I:%M:%S %p", &tmv);
    lv_label_set_text(now_lbl, b);
    if(i18n_get_lang() == LANG_EN) strftime(b, sizeof b, "%Y-%m-%d", &tmv);
    else strftime(b, sizeof b, "%d/%m/%Y", &tmv);
    lv_label_set_text(date_lbl, b);
}

static void sw_date(lv_event_t *e)
{
    settings()->show_date = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

static void sw_24h(lv_event_t *e)
{
    settings()->clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(parent);

    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, BF_SP_XL, 0);
    page_round_icon(c, BF_SYMBOL_CALENDAR, lv_color_hex(0xe5534b), 56);
    lv_obj_t *col = bf_plain(c);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    now_lbl = bf_label(col, "", BF_FONT_L, p->text);
    date_lbl = bf_label(col, "", BF_FONT_M, p->text_dim);
    const char *tz = getenv("TZ");
    lv_obj_t *tzl = lv_label_create(c);
    lv_label_set_text_fmt(tzl, "%s\n%s", tr(S_TIMEZONE), tz && *tz ? tz : "localtime");
    lv_obj_set_style_text_font(tzl, BF_FONT_S, 0);
    lv_obj_set_style_text_color(tzl, p->text_dim, 0);
    lv_obj_set_style_text_align(tzl, LV_TEXT_ALIGN_RIGHT, 0);

    page_switch_row(pg, tr(S_DATE_TOPBAR), settings()->show_date, sw_date, NULL);
    page_switch_row(pg, tr(S_DATE_24H), settings()->clock_24h, sw_24h, NULL);
    lv_obj_t *n = bf_label(pg, tr(S_DATE_SET_NOTE), BF_FONT_S, p->text_dim);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    tick(NULL);
    timer = lv_timer_create(tick, 1000, NULL);
}

static void destroy(void)
{
    if(timer) { lv_timer_delete(timer); timer = NULL; }
}

const bf_screen_def_t scr_date_def = { create, destroy, S_SET_DATE, NULL };
