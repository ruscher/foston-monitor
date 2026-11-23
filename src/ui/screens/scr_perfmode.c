/* scr_perfmode.c — Ajustes > Modo performance: estado e botão Ativar/Desativar agora,
 * automático (ligar com vídeo, esperas), o que pausar, interface e vídeo (core/perfui.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/perfmode.h"
#include "../core/perfui.h"
#include "../widgets/page.h"
#include <stdio.h>
#include <string.h>

#define C_PERF 0xe66100

static lv_obj_t *pg, *st_lbl, *st_btn, *st_btn_lbl;
static lv_timer_t *st_t;

/* ---------------------------------------------------------------- estado */

static void state_update(lv_timer_t *t)
{
    (void)t;
    if(!st_lbl) return;
    pm_state_t s = perfmode_state();
    bf_str_t txt = s == PM_ENTERING ? S_PERFM_ENTERING : s == PM_LEAVING ? S_PERFM_LEAVING
                 : s == PM_OFF ? S_PERFM_OFF : perfui_why() == PERF_WHY_MANUAL ? S_PERFM_ON_MANUAL : S_PERFM_ON_VIDEO;
    const char *now = tr(txt);
    if(strcmp(lv_label_get_text(st_lbl), now)) lv_label_set_text(st_lbl, now);
    lv_obj_set_style_text_color(st_lbl, s == PM_OFF ? pal()->text_dim : lv_color_hex(C_PERF), 0);
    bool busy = s == PM_ENTERING || s == PM_LEAVING;
    const char *b = tr(s == PM_OFF ? S_PERFM_NOW_ON : S_PERFM_NOW_OFF);
    if(strcmp(lv_label_get_text(st_btn_lbl), b)) lv_label_set_text(st_btn_lbl, b);
    if(busy) lv_obj_add_state(st_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(st_btn, LV_STATE_DISABLED);
}

static void now_click(lv_event_t *e)
{
    (void)e;
    if(perfmode_state() == PM_OFF) perfui_manual(true);
    else perfui_disable();
    state_update(NULL);
}

/* ---------------------------------------------------------------- opções */

enum { O_AUTO, O_QUIET, O_PRIO, O_TAG, O_MODAL, O_SHARP, O_SVC = 16 };

static void opt_toggle(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    bf_settings_t *s = settings();
    if(w >= O_SVC) {
        unsigned bit = 1u << (w - O_SVC);
        s->perf_svc = on ? s->perf_svc | bit : s->perf_svc & ~bit;
    }
    else if(w == O_AUTO) s->perf_auto = on;
    else if(w == O_QUIET) s->perf_quiet = on;
    else if(w == O_PRIO) s->perf_prio = on;
    else if(w == O_TAG) s->perf_tag = on;
    else if(w == O_MODAL) s->perf_modal = on;
    else if(w == O_SHARP) s->web_fs_sharp = on;
    settings_save();
    perfui_settings_changed();
}

/* linha com título, explicação embaixo e switch */
static void switch_sub(lv_obj_t *parent, const char *title, const char *sub, bool on, int id)
{
    const bf_palette_t *p = pal();
    lv_obj_t *row = bf_card(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 76, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_t *col = bf_plain(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    bf_label(col, title, BF_FONT_M, p->text);
    lv_obj_t *s = bf_label(col, sub, BF_FONT_S, p->text_dim);
    lv_obj_set_width(s, LV_PCT(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 96, 52);
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 12);
    lv_obj_add_event_cb(sw, opt_toggle, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
}

/* escolha entre poucos valores em segundos (botões lado a lado) */
typedef struct { int *target; int value; } choice_t;

static void build(lv_obj_t *parent);

static void choice_click(lv_event_t *e)
{
    const choice_t *c = lv_event_get_user_data(e);
    if(*c->target == c->value) return;
    *c->target = c->value;
    settings_save();
    lv_obj_t *par = lv_obj_get_parent(pg);
    int32_t y = lv_obj_get_scroll_y(pg);
    lv_obj_delete(pg);                               /* refaz a página com a escolha marcada */
    build(par);
    lv_obj_scroll_to_y(pg, y, LV_ANIM_OFF);
}

static void choice_row(lv_obj_t *parent, const char *label, int *target, const int *vals, choice_t *store, int n)
{
    const bf_palette_t *p = pal();
    lv_obj_t *row = bf_card(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, BF_SP_S, 0);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    lv_obj_t *l = bf_label(row, label, BF_FONT_M, p->text);
    lv_obj_set_flex_grow(l, 1);
    for(int i = 0; i < n; i++) {
        char t[16];
        snprintf(t, sizeof t, "%d s", vals[i]);
        bool cur = *target == vals[i];
        lv_obj_t *b = bf_button(row, NULL, t, cur ? 1 : 0);
        lv_obj_set_size(b, 76, 48);
        store[i] = (choice_t){ target, vals[i] };
        lv_obj_add_event_cb(b, choice_click, LV_EVENT_CLICKED, &store[i]);
    }
}

static void section(lv_obj_t *p, bf_str_t title)
{
    lv_obj_t *l = bf_label(p, tr(title), BF_FONT_S, pal()->text_dim);
    lv_obj_set_style_pad_top(l, BF_SP_M, 0);
}

static void build(lv_obj_t *parent)
{
    static const int enter_vals[] = { 0, 2, 5, 10 }, exit_vals[] = { 5, 10, 30, 60 };
    static choice_t enter_store[4], exit_store[4];
    const bf_palette_t *p = pal();
    bf_settings_t *s = settings();
    pg = page_create(parent);
    page_header(pg, LV_SYMBOL_CHARGE, lv_color_hex(C_PERF), tr(S_SET_PERFMODE), tr(S_PERFM_HINT));

    /* estado + ligar/desligar agora */
    lv_obj_t *card = bf_card(pg);
    lv_obj_set_size(card, LV_PCT(100), 76);
    lv_obj_set_style_pad_ver(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    st_lbl = bf_label(card, "", BF_FONT_M, p->text);
    lv_obj_set_flex_grow(st_lbl, 1);
    st_btn = bf_button(card, LV_SYMBOL_CHARGE, "", 1);
    lv_obj_set_height(st_btn, 52);
    st_btn_lbl = lv_obj_get_child(st_btn, -1);
    lv_obj_add_event_cb(st_btn, now_click, LV_EVENT_CLICKED, NULL);

    section(pg, S_PERFM_SEC_AUTO);
    page_switch_row(pg, tr(S_PERFM_AUTO), s->perf_auto, opt_toggle, (void *)O_AUTO);
    choice_row(pg, tr(S_PERFM_ENTER_DELAY), &s->perf_enter_s, enter_vals, enter_store, 4);
    choice_row(pg, tr(S_PERFM_EXIT_DELAY), &s->perf_exit_s, exit_vals, exit_store, 4);

    section(pg, S_PERFM_SEC_PAUSE);
    switch_sub(pg, tr(S_PERFM_GPS), tr(S_PERFM_GPS_SUB), s->perf_svc & PM_SVC_GPS, O_SVC + 0);
    switch_sub(pg, tr(S_PERFM_BT), tr(S_PERFM_BT_SUB), s->perf_svc & PM_SVC_BT, O_SVC + 1);
    switch_sub(pg, tr(S_PERFM_PHONE), tr(S_PERFM_PHONE_SUB), s->perf_svc & PM_SVC_PHONE, O_SVC + 2);
    switch_sub(pg, tr(S_PERFM_CRON), tr(S_PERFM_CRON_SUB), s->perf_svc & PM_SVC_CRON, O_SVC + 3);
    lv_obj_t *n = bf_label(pg, tr(S_PERFM_NEXT), BF_FONT_S, p->text_dim);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    section(pg, S_PERFM_SEC_UI);
    page_switch_row(pg, tr(S_PERFM_QUIET), s->perf_quiet, opt_toggle, (void *)O_QUIET);
    page_switch_row(pg, tr(S_PERFM_PRIO), s->perf_prio, opt_toggle, (void *)O_PRIO);
    page_switch_row(pg, tr(S_PERFM_TAGSW), s->perf_tag, opt_toggle, (void *)O_TAG);
    page_switch_row(pg, tr(S_PERFM_MODAL), s->perf_modal, opt_toggle, (void *)O_MODAL);
    lv_obj_t *a = bf_label(pg, tr(S_PERFM_AWAKE), BF_FONT_S, p->text_dim);
    lv_obj_set_width(a, LV_PCT(100));
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);

    section(pg, S_PERFM_SEC_VIDEO);
    page_switch_row(pg, tr(S_WEB_FS_SHARP), s->web_fs_sharp, opt_toggle, (void *)O_SHARP);

    state_update(NULL);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    build(parent);
    st_t = lv_timer_create(state_update, 300, NULL);
}

static void destroy(void)
{
    if(st_t) { lv_timer_delete(st_t); st_t = NULL; }
    pg = st_lbl = st_btn = st_btn_lbl = NULL;
}

const bf_screen_def_t scr_perfmode_def = { create, destroy, S_SET_PERFMODE, NULL };
