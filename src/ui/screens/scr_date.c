/* scr_date.c — Ajustes > Data.
 *   arg 0: data/hora atuais, origem, fuso, atualização automática (NTP),
 *          horário de verão e formato
 *   arg 1: ajustar data e hora (dia, mês, ano, hora, minuto)
 *   arg 2: escolher o fuso horário
 * O relógio em si fica em core/clock.c. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/clock.h"
#include "../widgets/page.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { ARG_MAIN, ARG_EDIT, ARG_ZONE };

static lv_timer_t *timer;
static lv_obj_t *now_lbl, *date_lbl, *src_lbl, *sync_lbl, *sync_btn, *zone_btn_lbl;
static lv_obj_t *dst_sw, *dst_hint;

/* -------------------------------------------------------- nomes de datas */

static const char *const wday[LANG_COUNT][7] = {
    { "domingo", "segunda-feira", "terça-feira", "quarta-feira", "quinta-feira", "sexta-feira", "sábado" },
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
    { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" },
};
static const char *const mon[LANG_COUNT][12] = {
    { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro",
      "outubro", "novembro", "dezembro" },
    { "January", "February", "March", "April", "May", "June", "July", "August", "September",
      "October", "November", "December" },
    { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre",
      "octubre", "noviembre", "diciembre" },
};

static void long_date(const struct tm *t, char *b, size_t n)
{
    bf_lang_t l = i18n_get_lang();
    if(l == LANG_EN)
        snprintf(b, n, "%s, %s %d, %d", wday[l][t->tm_wday], mon[l][t->tm_mon], t->tm_mday, t->tm_year + 1900);
    else
        snprintf(b, n, "%s, %d de %s de %d", wday[l][t->tm_wday], t->tm_mday, mon[l][t->tm_mon], t->tm_year + 1900);
}

static void hhmm(time_t v, char *b, size_t n)
{
    struct tm t;
    localtime_r(&v, &t);
    if(settings()->clock_24h) strftime(b, n, "%H:%M", &t);
    else strftime(b, n, "%I:%M %p", &t);
}

static void set_text_if(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static lv_obj_t *hint(lv_obj_t *parent, const char *txt, lv_color_t c)
{
    lv_obj_t *l = bf_label(parent, txt, BF_FONT_S, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* interruptor sem moldura dentro de um cartão */
static lv_obj_t *inner_switch(lv_obj_t *card, const char *txt, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = page_switch_row(card, txt, on, cb, NULL);
    lv_obj_t *row = lv_obj_get_parent(sw);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_height(row, 60);
    return sw;
}

static lv_obj_t *column_card(lv_obj_t *pg)
{
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    return c;
}

/* ================================================================ principal */

static void main_tick(lv_timer_t *t)
{
    (void)t;
    const bf_palette_t *p = pal();
    char b[160], w[40];
    time_t n = time(NULL);
    struct tm tmv;
    localtime_r(&n, &tmv);
    bool ok = clock_valid();
    if(!ok) set_text_if(now_lbl, "--:--:--");
    else {
        strftime(b, sizeof b, settings()->clock_24h ? "%H:%M:%S" : "%I:%M:%S %p", &tmv);
        set_text_if(now_lbl, b);
    }
    if(ok) long_date(&tmv, b, sizeof b);
    else b[0] = 0;
    set_text_if(date_lbl, b);

    time_t when;
    switch(clock_source(&when)) {
        case CLK_SYSTEM: snprintf(b, sizeof b, "%s", tr(S_DATE_SRC_SYSTEM)); break;
        case CLK_BOOT:   snprintf(b, sizeof b, "%s", tr(S_DATE_SRC_BOOT)); break;
        case CLK_MANUAL: hhmm(when, w, sizeof w); snprintf(b, sizeof b, tr(S_DATE_SRC_MANUAL), w); break;
        case CLK_NTP:    hhmm(when, w, sizeof w);
                         snprintf(b, sizeof b, tr(S_DATE_SRC_NTP), clock_source_detail(), w); break;
        case CLK_GPS:    hhmm(when, w, sizeof w); snprintf(b, sizeof b, tr(S_DATE_SRC_GPS), w); break;
        default:         snprintf(b, sizeof b, "%s", tr(S_DATE_SRC_UNSET)); break;
    }
    set_text_if(src_lbl, b);
    lv_obj_set_style_text_color(src_lbl, ok ? p->text_dim : p->warning, 0);

    char err[96];
    int r = clock_sync_result(&when, err, sizeof err);
    bool busy = clock_sync_busy();
    if(busy) snprintf(b, sizeof b, "%s", tr(S_DATE_SYNCING));
    else if(r > 0) { hhmm(when, w, sizeof w); snprintf(b, sizeof b, tr(S_DATE_SYNC_OK), w); }
    else if(r < 0) { hhmm(when, w, sizeof w); snprintf(b, sizeof b, tr(S_DATE_SYNC_FAIL), w, err); }
    else snprintf(b, sizeof b, "%s", tr(S_DATE_SYNC_NEVER));
    set_text_if(sync_lbl, b);
    lv_obj_set_style_text_color(sync_lbl, r < 0 && !busy ? p->warning : p->text_dim, 0);
    if(busy) lv_obj_add_state(sync_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(sync_btn, LV_STATE_DISABLED);
}

static void go_edit(lv_event_t *e) { (void)e; nav_go(SCR_DATE, ARG_EDIT); }
static void go_zone(lv_event_t *e) { (void)e; nav_go(SCR_DATE, ARG_ZONE); }
static void sync_click(lv_event_t *e) { (void)e; clock_sync_now(); main_tick(NULL); }

static void sw_auto(lv_event_t *e)
{
    settings()->auto_time = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    if(settings()->auto_time) clock_sync_now();
}

static void sw_dst(lv_event_t *e)
{
    settings()->dst = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    clock_apply_tz();
    main_tick(NULL);
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
    main_tick(NULL);
}

static void create_main(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    const clock_zone_t *z = clock_zone_current();
    lv_obj_t *pg = page_create(parent);

    /* hora atual */
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, BF_SP_L, 0);
    page_round_icon(c, BF_SYMBOL_CALENDAR, lv_color_hex(0xe5534b), 56);
    lv_obj_t *col = bf_plain(c);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    now_lbl = bf_label(col, "", BF_FONT_L, p->text);
    date_lbl = bf_label(col, "", BF_FONT_M, p->text);
    src_lbl = hint(col, "", p->text_dim);

    /* ajustar / fuso */
    lv_obj_t *r = bf_plain(pg);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, BF_SP_M, 0);
    lv_obj_t *b = bf_button(r, BF_SYMBOL_CLOCK, tr(S_DATE_ADJUST), 1);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_event_cb(b, go_edit, LV_EVENT_CLICKED, NULL);
    char lbl[96], u[16];
    clock_utc_label(z->utc_min, u, sizeof u);
    snprintf(lbl, sizeof lbl, "%s  %s", u, z->name);
    b = bf_button(r, BF_SYMBOL_GLOBE, NULL, 0);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *bc = bf_plain(b);
    lv_obj_set_size(bc, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(bc, 1);
    lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bc, LV_OBJ_FLAG_CLICKABLE);
    bf_label(bc, tr(S_TIMEZONE), BF_FONT_S, p->text_dim);
    zone_btn_lbl = bf_label(bc, lbl, BF_FONT_S, p->text);
    bf_label_ellipsis(zone_btn_lbl, LV_PCT(100));
    bf_label(b, LV_SYMBOL_RIGHT, BF_FONT_S, p->text_dim);
    lv_obj_add_event_cb(b, go_zone, LV_EVENT_CLICKED, NULL);

    /* atualização automática */
    c = column_card(pg);
    inner_switch(c, tr(S_DATE_AUTO), settings()->auto_time, sw_auto);
    lv_obj_t *kv = page_kv(c, tr(S_DATE_SERVERS), settings()->ntp);
    lv_obj_set_style_text_font(kv, BF_FONT_S, 0);
    lv_obj_t *sr = bf_plain(c);
    lv_obj_set_size(sr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sr, BF_SP_M, 0);
    sync_lbl = bf_label(sr, "", BF_FONT_S, p->text_dim);
    lv_obj_set_flex_grow(sync_lbl, 1);
    lv_label_set_long_mode(sync_lbl, LV_LABEL_LONG_WRAP);
    sync_btn = bf_button(sr, LV_SYMBOL_REFRESH, tr(S_DATE_SYNC_NOW), 0);
    lv_obj_set_style_bg_opa(sync_btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(sync_btn, sync_click, LV_EVENT_CLICKED, NULL);
    hint(c, tr(S_DATE_AUTO_HINT), p->text_dim);

    /* horário de verão */
    c = column_card(pg);
    dst_sw = inner_switch(c, tr(S_DATE_DST), settings()->dst && !z->rules, sw_dst);
    if(z->rules) lv_obj_add_state(dst_sw, LV_STATE_DISABLED);
    dst_hint = hint(c, tr(z->rules ? S_DATE_DST_AUTO : S_DATE_DST_HINT), p->text_dim);

    /* formato */
    c = column_card(pg);
    bf_label(c, tr(S_DATE_FORMAT), BF_FONT_M, p->text);
    inner_switch(c, tr(S_DATE_24H), settings()->clock_24h, sw_24h);
    inner_switch(c, tr(S_DATE_TOPBAR), settings()->show_date, sw_date);

    hint(pg, tr(S_DATE_VOLATILE), p->text_dim);

    main_tick(NULL);
    timer = lv_timer_create(main_tick, 1000, NULL);
}

/* ================================================================== editor */

enum { F_DAY, F_MON, F_YEAR, F_HOUR, F_MIN, F__N };
static int val[F__N];
static lv_obj_t *val_lbl[F__N], *preview_lbl, *msg_lbl;

static int days_in(int m, int y)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1];
}

static void edit_refresh(void)
{
    int dim = days_in(val[F_MON], val[F_YEAR]);
    if(val[F_DAY] > dim) val[F_DAY] = dim;
    for(int i = 0; i < F__N; i++) {
        char b[8];
        snprintf(b, sizeof b, i == F_YEAR ? "%04d" : "%02d", val[i]);
        set_text_if(val_lbl[i], b);
    }
    struct tm t = { .tm_mday = val[F_DAY], .tm_mon = val[F_MON] - 1, .tm_year = val[F_YEAR] - 1900,
                    .tm_hour = val[F_HOUR], .tm_min = val[F_MIN], .tm_isdst = -1 };
    mktime(&t);                     /* preenche o dia da semana */
    char b[96];
    long_date(&t, b, sizeof b);
    set_text_if(preview_lbl, b);
}

static void step(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int f = code / 2, d = code % 2 ? -1 : 1;
    static const int lo[F__N] = { 1, 1, 2024, 0, 0 }, hi[F__N] = { 31, 12, 2099, 23, 59 };
    int max = f == F_DAY ? days_in(val[F_MON], val[F_YEAR]) : hi[f];
    val[f] += d;
    if(val[f] > max) val[f] = lo[f];
    if(val[f] < lo[f]) val[f] = max;
    lv_label_set_text(msg_lbl, "");
    edit_refresh();
}

static void apply_click(lv_event_t *e)
{
    (void)e;
    const bf_palette_t *p = pal();
    if(!clock_can_set()) {
        lv_label_set_text(msg_lbl, tr(S_DATE_SIM));
        lv_obj_set_style_text_color(msg_lbl, p->warning, 0);
        return;
    }
    struct tm t = { .tm_mday = val[F_DAY], .tm_mon = val[F_MON] - 1, .tm_year = val[F_YEAR] - 1900,
                    .tm_hour = val[F_HOUR], .tm_min = val[F_MIN], .tm_sec = 0 };
    char err[80];
    if(!clock_set_local(&t, err, sizeof err)) {
        lv_label_set_text_fmt(msg_lbl, "%s: %s", tr(S_DATE_SET_FAIL), err);
        lv_obj_set_style_text_color(msg_lbl, p->danger, 0);
        return;
    }
    /* acertou à mão: a sincronização automática não deve desfazer logo em seguida
     * com um servidor errado; ela segue no intervalo normal */
    nav_back();
}

static void cancel_click(lv_event_t *e) { (void)e; nav_back(); }

static void create_edit(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    time_t n = time(NULL);
    struct tm t;
    localtime_r(&n, &t);
    if(clock_valid()) {
        val[F_DAY] = t.tm_mday; val[F_MON] = t.tm_mon + 1; val[F_YEAR] = t.tm_year + 1900;
        val[F_HOUR] = t.tm_hour; val[F_MIN] = t.tm_min;
    }
    else { val[F_DAY] = 1; val[F_MON] = 1; val[F_YEAR] = 2026; val[F_HOUR] = 12; val[F_MIN] = 0; }

    lv_obj_t *pg = page_create(parent);
    lv_obj_set_style_pad_ver(pg, BF_SP_M, 0);
    lv_obj_set_style_pad_row(pg, BF_SP_S, 0);
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(c, BF_SP_S, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const bf_str_t names[F__N] = { S_DATE_DAY, S_DATE_MONTH, S_DATE_YEAR, S_DATE_HOUR, S_DATE_MINUTE };
    for(int i = 0; i < F__N; i++) {
        if(i == F_HOUR) {            /* separa data de hora */
            lv_obj_t *sep = lv_obj_create(c);
            lv_obj_remove_style_all(sep);
            lv_obj_set_size(sep, 2, 150);
            lv_obj_set_style_bg_color(sep, p->border, 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        }
        lv_obj_t *col = bf_plain(c);
        lv_obj_set_size(col, i == F_YEAR ? 112 : 96, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 4, 0);
        bf_label(col, tr(names[i]), BF_FONT_S, p->text_dim);
        lv_obj_t *up = bf_button(col, LV_SYMBOL_UP, NULL, 0);
        lv_obj_set_size(up, LV_PCT(100), 56);
        lv_obj_add_event_cb(up, step, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)(i * 2));
        lv_obj_add_event_cb(up, step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(i * 2));
        val_lbl[i] = bf_label(col, "", BF_FONT_L, p->text);
        lv_obj_t *dn = bf_button(col, LV_SYMBOL_DOWN, NULL, 0);
        lv_obj_set_size(dn, LV_PCT(100), 56);
        lv_obj_add_event_cb(dn, step, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)(i * 2 + 1));
        lv_obj_add_event_cb(dn, step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(i * 2 + 1));
    }

    lv_obj_t *r = bf_plain(pg);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, BF_SP_M, 0);
    lv_obj_t *info = bf_plain(r);
    lv_obj_set_size(info, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    preview_lbl = bf_label(info, "", BF_FONT_M, p->text);
    bf_label_ellipsis(preview_lbl, LV_PCT(100));
    msg_lbl = hint(info, "", p->text_dim);
    lv_obj_t *b = bf_button(r, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0);
    lv_obj_add_event_cb(b, cancel_click, LV_EVENT_CLICKED, NULL);
    b = bf_button(r, LV_SYMBOL_OK, tr(S_DATE_APPLY), 1);
    lv_obj_add_event_cb(b, apply_click, LV_EVENT_CLICKED, NULL);
    edit_refresh();
}

/* ==================================================================== fusos */

static lv_obj_t *zone_pg;

static void zone_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const clock_zone_t *z = clock_zone(i);
    snprintf(settings()->tz, sizeof settings()->tz, "%s", z->id);
    if(z->rules) settings()->dst = false;
    settings_save();
    clock_apply_tz();
    nav_back();
}

static void create_zone(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    int cur = clock_zone_index(settings()->tz);
    zone_pg = page_create(parent);
    lv_obj_set_style_pad_row(zone_pg, BF_SP_S, 0);
    lv_obj_t *sel = NULL;
    for(int i = 0; i < clock_zone_count(); i++) {
        const clock_zone_t *z = clock_zone(i);
        bool on = i == cur;
        lv_obj_t *b = lv_button_create(zone_pg);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &bf_st_card, 0);
        lv_obj_set_size(b, LV_PCT(100), 64);
        lv_obj_set_style_pad_ver(b, 0, 0);
        lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
        if(on) lv_obj_set_style_border_color(b, p->accent, 0);
        if(on) lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, BF_SP_M, 0);
        lv_obj_add_event_cb(b, zone_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = bf_label(b, z->name, BF_FONT_M, p->text);
        lv_obj_set_flex_grow(n, 1);
        bf_label_ellipsis(n, 1);
        if(z->rules) bf_pill(b, tr(S_DATE_DST_SHORT), p->accent);
        char u[16];
        clock_utc_label(z->utc_min, u, sizeof u);
        bf_label(b, u, BF_FONT_S, p->text_dim);
        if(on) { bf_label(b, LV_SYMBOL_OK, BF_FONT_M, p->accent); sel = b; }
    }
    if(sel) {
        lv_obj_update_layout(zone_pg);
        lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
    }
}

/* ==================================================================== tela */

static void create(lv_obj_t *parent, int arg)
{
    now_lbl = date_lbl = src_lbl = sync_lbl = sync_btn = zone_btn_lbl = dst_sw = dst_hint = NULL;
    if(arg == ARG_EDIT) create_edit(parent);
    else if(arg == ARG_ZONE) create_zone(parent);
    else create_main(parent);
}

static void destroy(void)
{
    if(timer) { lv_timer_delete(timer); timer = NULL; }
    zone_pg = NULL;
}

static const char *title(int arg)
{
    return tr(arg == ARG_EDIT ? S_DATE_ADJUST : arg == ARG_ZONE ? S_TIMEZONE : S_SET_DATE);
}

const bf_screen_def_t scr_date_def = { create, destroy, S_SET_DATE, title };
