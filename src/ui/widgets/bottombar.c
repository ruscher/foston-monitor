/* bottombar.c — Início, Voltar, Recentes, Terminal, Ajustes, Desligar. */
#include "bottombar.h"
#include "dialog.h"
#include "../core/app.h"
#include "../core/theme.h"
#include "../core/nav.h"
#include "../core/status.h"
#include "../screens/screens.h"

enum { B_HOME, B_BACK, B_RECENTS, B_TERM, B_SETTINGS, B_POWER, B__N };
static lv_obj_t *btn[B__N];

static void power_cb(int b, void *ud)
{
    (void)ud;
    if(b == 1) app_power(true);          /* botões: 0 Cancelar, 1 Reiniciar, 2 Desligar */
    else if(b == 2) app_power(false);
}

void bottombar_open_power(void)
{
    static const char *btns[3];
    btns[0] = tr(S_CANCEL);
    btns[1] = tr(S_REBOOT);
    btns[2] = tr(S_POWER_OFF);
    lv_obj_t *card = dialog_open(LV_SYMBOL_POWER, pal()->danger, tr(S_POWER_TITLE), tr(S_POWER_BODY),
                                 btns, 3, -1, 2, power_cb, NULL);
    if(status_provider()->simulated) {
        lv_obj_t *n = bf_label(card, tr(S_POWER_SIM), BF_FONT_S, pal()->warning);
        lv_obj_set_width(n, LV_PCT(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_move_to_index(n, 2);
    }
}

static bf_nav_entry_t rec[6];

static void recent_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    dialog_close_all();
    nav_go(rec[i].scr, rec[i].arg);
}

void bottombar_open_recents(void)
{
    static const char *btns[1];
    btns[0] = tr(S_CLOSE);
    int n = nav_recents(rec, 6);
    lv_obj_t *card = dialog_open(LV_SYMBOL_LIST, pal()->accent, tr(S_RECENTS_TITLE),
                                 n ? NULL : tr(S_RECENTS_EMPTY), btns, 1, -1, -1, NULL, NULL);
    if(!n) return;
    lv_obj_t *grid = bf_plain(card);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BF_SP_S, 0);
    lv_obj_set_style_pad_column(grid, BF_SP_S, 0);
    lv_obj_move_to_index(grid, 1);
    for(int i = 0; i < n; i++) {
        lv_obj_t *b = bf_button(grid, NULL, nav_title(rec[i].scr, rec[i].arg), 0);
        lv_obj_set_width(b, LV_PCT(49));
        lv_obj_t *l = lv_obj_get_child(b, 0);
        bf_label_ellipsis(l, 210);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_event_cb(b, recent_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void click(lv_event_t *e)
{
    switch((int)(intptr_t)lv_event_get_user_data(e)) {
        case B_HOME:     nav_home(); break;
        case B_BACK:     nav_back(); break;
        case B_RECENTS:  bottombar_open_recents(); break;
        case B_TERM:     nav_go(SCR_TERM, 0); break;
        case B_SETTINGS: nav_go(SCR_SETTINGS, 0); break;
        case B_POWER:    bottombar_open_power(); break;
    }
}

lv_obj_t *bottombar_create(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BF_SCREEN_W, BF_BOTBAR_H);
    lv_obj_set_style_bg_color(bar, p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, p->border, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    static const char *const sym[B__N] = { LV_SYMBOL_HOME, LV_SYMBOL_LEFT, LV_SYMBOL_LIST,
                                          BF_SYMBOL_TERMINAL, LV_SYMBOL_SETTINGS, LV_SYMBOL_POWER };
    static const bf_str_t txt[B__N] = { S_NAV_HOME, S_NAV_BACK, S_NAV_RECENTS, S_NAV_TERMINAL,
                                        S_NAV_SETTINGS, S_NAV_POWER };
    for(int i = 0; i < B__N; i++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_remove_style_all(b);
        btn[i] = b;
        lv_obj_set_height(b, LV_PCT(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, BF_RADIUS_S, 0);
        lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(b, p->text_dim, 0);
        lv_obj_set_style_text_color(b, p->accent, LV_STATE_CHECKED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 2, 0);
        lv_obj_t *ic = lv_label_create(b);
        lv_label_set_text(ic, sym[i]);
        lv_obj_set_style_text_font(ic, BF_FONT_L, 0);
        if(i == B_POWER) lv_obj_set_style_text_color(ic, p->danger, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, tr(txt[i]));
        lv_obj_set_style_text_font(l, BF_FONT_S, 0);
        lv_obj_add_event_cb(b, click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    return bar;
}

void bottombar_set_active(int scr)
{
    if(!btn[0]) return;
    for(int i = 0; i < B__N; i++) lv_obj_remove_state(btn[i], LV_STATE_CHECKED);
    if(scr == SCR_HOME) lv_obj_add_state(btn[B_HOME], LV_STATE_CHECKED);
    else if(scr == SCR_SETTINGS) lv_obj_add_state(btn[B_SETTINGS], LV_STATE_CHECKED);
    else if(scr == SCR_RUN) lv_obj_add_state(btn[B_TERM], LV_STATE_CHECKED);
}
