/* namebox.c — ver namebox.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "namebox.h"
#include "kbd.h"
#include "glass.h"
#include "../core/theme.h"
#include "../core/fx.h"
#include <stdio.h>
#include <string.h>

#define KB_H 210

static lv_obj_t *bd, *ta, *err;
static bool err_on;
static char hint[160];
static namebox_cb_t cur_cb;
static void *cur_ud;

bool namebox_is_open(void) { return bd != NULL; }

/* a janela pode ser apagada por outro caminho (troca de tela limpando a camada de cima):
 * esquece os ponteiros, senão a próxima abertura apagaria um objeto já liberado */
static void bd_deleted(lv_event_t *e)
{
    if(lv_event_get_target(e) != bd) return;
    bd = ta = err = NULL;
    cur_cb = NULL;
}

static void drop(void)
{
    if(bd) lv_obj_delete_async(bd);
    bd = ta = err = NULL;
    cur_cb = NULL;
}

static void cancel(void)
{
    namebox_cb_t cb = cur_cb;
    void *ud = cur_ud;
    drop();
    if(cb) cb(NULL, ud);
}

void namebox_close(void) { if(bd) cancel(); }

static const char *submit(void)
{
    if(!bd || !cur_cb) return NULL;
    const char *msg = cur_cb(lv_textarea_get_text(ta), cur_ud);
    if(!bd) return msg;                     /* o callback já fechou */
    if(msg) {                               /* erro: a janela continua para corrigir */
        lv_label_set_text(err, msg);
        lv_obj_set_style_text_color(err, pal()->danger, 0);
        err_on = true;
        return msg;
    }
    drop();
    return NULL;
}

static void ok_click(lv_event_t *e) { (void)e; submit(); }
static void cancel_click(lv_event_t *e) { (void)e; cancel(); }

/* digitar apaga o aviso de erro */
static void ta_changed(lv_event_t *e)
{
    (void)e;
    if(err && err_on) {
        lv_obj_set_style_text_color(err, theme_glass() ? lv_color_hex(GL_TEXT2) : pal()->text_dim, 0);
        lv_label_set_text(err, hint);
        err_on = false;
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *sym, const char *txt, int kind, lv_event_cb_t cb)
{
    lv_obj_t *b = bf_button(parent, sym, txt, kind);
    lv_obj_set_size(b, 170, 52);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void namebox_open(const char *symbol, const char *title, const char *label, const char *initial,
                  const char *ok_text, int max_chars, namebox_cb_t cb, void *ud)
{
    if(bd) { cur_cb = NULL; lv_obj_delete(bd); bd = NULL; }
    const bf_palette_t *p = pal();
    bool glass = theme_glass();
    cur_cb = cb;
    cur_ud = ud;

    bd = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(bd, bd_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_50, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);       /* bloqueia toques atrás */
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);

    /* cartão no alto, acima do teclado (210 px) */
    lv_obj_t *card = bf_card(bd);
    lv_obj_set_size(card, 768, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(card, BF_RADIUS_L, 0);
    lv_obj_set_style_pad_all(card, BF_SP_L, 0);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if(glass) {
        lv_obj_set_style_bg_color(card, lv_color_hex(GL_DEEP), 0);
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(GL_BLUE), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(GL_SKY), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    }
    lv_color_t fg = glass ? lv_color_hex(GL_WHITE) : p->text, dim = glass ? lv_color_hex(GL_TEXT2) : p->text_dim;

    lv_obj_t *head = bf_plain(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, BF_SP_M, 0);
    if(symbol) bf_label(head, symbol, BF_FONT_L, glass ? lv_color_hex(GL_SKY) : p->accent);
    bf_label(head, title, BF_FONT_L, fg);

    lv_obj_t *row = bf_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max_chars);
    lv_textarea_set_placeholder_text(ta, label);
    lv_textarea_set_text(ta, initial ? initial : "");
    lv_obj_set_height(ta, 52);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_text_font(ta, BF_FONT_M, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);            /* o cursor e o teclado físico vêm para cá */
    lv_obj_add_event_cb(ta, ta_changed, LV_EVENT_VALUE_CHANGED, NULL);
    button(row, NULL, tr(S_CANCEL), 0, cancel_click);
    button(row, NULL, ok_text, 1, ok_click);

    snprintf(hint, sizeof hint, "%s", label ? label : "");
    err_on = false;
    err = bf_label(card, hint, BF_FONT_S, dim);       /* dica; vira o erro em vermelho */
    lv_obj_set_width(err, LV_PCT(100));
    lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);

    lv_obj_t *kb = kbd_create(bd, ta);
    lv_obj_set_size(kb, BF_SCREEN_W, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, BF_FONT_M, 0);
    lv_obj_set_style_bg_color(kb, glass ? lv_color_hex(GL_DEEP) : p->surface, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);
    lv_obj_set_style_bg_color(kb, glass ? lv_color_hex(GL_BLUE) : p->surface2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, fg, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, glass ? lv_color_hex(0x1E4878) : p->border, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, glass ? lv_color_hex(GL_LIGHT) : p->accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(kb, ok_click, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, cancel_click, LV_EVENT_CANCEL, NULL);
    if(fx_anim() == LV_ANIM_ON) lv_obj_fade_in(card, 120, 0);
}

const char *namebox_demo_set(const char *text)
{
    if(!bd) return "";
    lv_textarea_set_text(ta, text);
    return lv_textarea_get_text(ta);
}

const char *namebox_demo_submit(const char *text)
{
    if(!bd) return "fechada";
    lv_textarea_set_text(ta, text);
    return submit();
}
