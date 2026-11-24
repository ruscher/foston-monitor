/* scr_keyboard.c — Ajustes > Teclado físico: teclados conectados agora, layout (ABNT2, EUA,
 * EUA internacional ou automático pelo idioma), um campo para testar e os atalhos
 * (core/hwkbd.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/hwkbd.h"
#include "../widgets/page.h"
#include <stdio.h>
#include <string.h>

#define C_KBD 0x3584e4

static lv_obj_t *pg, *list_lbl, *bt_btn, *test_ta;
static lv_timer_t *list_t;
static char list_now[300];
static char test_text[200];

static void list_update(lv_timer_t *t)
{
    (void)t;
    if(!list_lbl) return;
    char b[300] = "";
    size_t n = 0;
    for(int i = 0; i < hwkbd_count() && n < sizeof b - 80; i++)
        n += (size_t)snprintf(b + n, sizeof b - n, "%s%s  %s", i ? "\n" : "",
                              hwkbd_is_bt(i) ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_USB, hwkbd_name(i));
    if(!b[0]) snprintf(b, sizeof b, "%s", tr(S_KBD_NONE));
    if(!strcmp(b, list_now)) return;
    snprintf(list_now, sizeof list_now, "%s", b);
    lv_label_set_text(list_lbl, b);
    lv_obj_set_style_text_color(list_lbl, hwkbd_count() ? pal()->text : pal()->text_dim, 0);
    if(hwkbd_count()) lv_obj_add_flag(bt_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(bt_btn, LV_OBJ_FLAG_HIDDEN);
}

static void bt_click(lv_event_t *e) { (void)e; nav_go(SCR_BLUETOOTH, 0); }

static void build(lv_obj_t *parent);

static void layout_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if(settings()->kbd_layout == v) return;
    settings()->kbd_layout = v;
    settings_save();
    if(test_ta) snprintf(test_text, sizeof test_text, "%s", lv_textarea_get_text(test_ta));
    lv_obj_t *par = lv_obj_get_parent(pg);
    int32_t y = lv_obj_get_scroll_y(pg);
    lv_obj_delete(pg);                               /* refaz com a escolha marcada */
    build(par);
    lv_obj_update_layout(pg);
    lv_obj_scroll_to_y(pg, y, LV_ANIM_OFF);
}

static void section(lv_obj_t *p, bf_str_t title)
{
    lv_obj_t *l = bf_label(p, tr(title), BF_FONT_S, pal()->text_dim);
    lv_obj_set_style_pad_top(l, BF_SP_M, 0);
}

static lv_obj_t *note(lv_obj_t *p, const char *txt)
{
    lv_obj_t *n = bf_label(p, txt, BF_FONT_S, pal()->text_dim);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    return n;
}

static void build(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    pg = page_create(parent);
    page_header(pg, LV_SYMBOL_KEYBOARD, lv_color_hex(C_KBD), tr(S_SET_KBD), tr(S_KBD_HINT));

    section(pg, S_KBD_SEC_NOW);
    lv_obj_t *card = bf_card(pg);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 64, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, BF_SP_M, 0);
    list_lbl = bf_label(card, "", BF_FONT_M, p->text);
    lv_obj_set_flex_grow(list_lbl, 1);
    lv_label_set_long_mode(list_lbl, LV_LABEL_LONG_WRAP);
    bt_btn = bf_button(card, LV_SYMBOL_BLUETOOTH, tr(S_KBD_OPEN_BT), 1);
    lv_obj_set_height(bt_btn, 52);
    lv_obj_add_event_cb(bt_btn, bt_click, LV_EVENT_CLICKED, NULL);
    list_now[0] = 0;
    list_update(NULL);

    section(pg, S_KBD_SEC_TEST);
    test_ta = lv_textarea_create(pg);
    lv_textarea_set_one_line(test_ta, true);
    lv_textarea_set_placeholder_text(test_ta, tr(S_KBD_TEST_PH));
    lv_textarea_set_max_length(test_ta, sizeof test_text - 1);
    lv_obj_set_width(test_ta, LV_PCT(100));
    lv_obj_set_height(test_ta, 64);
    lv_obj_set_style_text_font(test_ta, BF_FONT_M, 0);
    lv_obj_set_style_pad_ver(test_ta, 18, 0);
    lv_obj_set_style_radius(test_ta, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(test_ta, p->surface, 0);
    lv_obj_set_style_text_color(test_ta, p->text, 0);
    lv_obj_set_style_border_color(test_ta, p->border, 0);
    lv_obj_set_style_border_color(test_ta, p->accent, LV_STATE_FOCUSED);
    lv_obj_set_scrollbar_mode(test_ta, LV_SCROLLBAR_MODE_OFF);
    lv_textarea_set_text(test_ta, test_text);
    lv_obj_add_state(test_ta, LV_STATE_FOCUSED);     /* já pronto para digitar */

    section(pg, S_KBD_SEC_LAYOUT);
    lv_obj_t *row = bf_card(pg);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, BF_SP_S, 0);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    static const bf_str_t names[BF_KBD__N] = { S_KBD_AUTO, S_KBD_ABNT2, S_KBD_US, S_KBD_US_INTL };
    for(int i = 0; i < BF_KBD__N; i++) {
        char t[80];
        if(i == BF_KBD_AUTO)
            snprintf(t, sizeof t, "%s: %s", tr(names[i]), tr(names[settings()->lang == LANG_PT ? BF_KBD_ABNT2 : BF_KBD_US]));
        else snprintf(t, sizeof t, "%s", tr(names[i]));
        lv_obj_t *b = bf_button(row, NULL, t, settings()->kbd_layout == i ? 1 : 0);
        lv_obj_set_size(b, LV_PCT(48), 52);             /* dois por linha: os nomes cabem */
        lv_obj_add_event_cb(b, layout_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    note(pg, tr(S_KBD_LAYOUT_NOTE));

    section(pg, S_KBD_SEC_KEYS);
    lv_obj_t *keys = bf_card(pg);
    lv_obj_set_size(keys, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *k = bf_label(keys, tr(S_KBD_KEYS), BF_FONT_S, p->text);
    lv_obj_set_width(k, LV_PCT(100));
    lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(k, 6, 0);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    test_text[0] = 0;
    build(parent);
    list_t = lv_timer_create(list_update, 1000, NULL);
}

static void destroy(void)
{
    if(list_t) { lv_timer_delete(list_t); list_t = NULL; }
    pg = list_lbl = bt_btn = test_ta = NULL;
}

const bf_screen_def_t scr_keyboard_def = { create, destroy, S_SET_KBD, NULL };
