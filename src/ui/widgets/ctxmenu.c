/* ctxmenu.c — ver ctxmenu.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ctxmenu.h"
#include "glass.h"
#include "../core/theme.h"
#include "../core/fx.h"
#include <string.h>

#define W      316
#define ROW_H  54
#define MARGIN 8

static lv_obj_t *bd;                   /* fundo do menu aberto */
static ctx_cb_t cur_cb;
static void *cur_ud;

static void finish(int id)
{
    ctx_cb_t cb = cur_cb;
    void *ud = cur_ud;
    cur_cb = NULL;
    if(bd) { lv_obj_delete_async(bd); bd = NULL; }
    if(cb) cb(id, ud);
}

static void bd_click(lv_event_t *e)
{
    if(lv_event_get_target(e) != lv_event_get_current_target(e)) return;   /* toque no cartão */
    finish(-1);
}

static void row_click(lv_event_t *e) { finish((int)(intptr_t)lv_event_get_user_data(e)); }

static void on_delete(lv_event_t *e)
{
    if(lv_event_get_target(e) == bd) bd = NULL;
}

bool ctxmenu_is_open(void) { return bd != NULL; }

void ctxmenu_pick(int id) { if(bd) finish(id); }

bool ctxmenu_close(void)
{
    if(!bd) return false;
    finish(-1);
    return true;
}

static lv_obj_t *row(lv_obj_t *list, const ctx_item_t *it, bool glass)
{
    const bf_palette_t *p = pal();
    lv_color_t fg = glass ? lv_color_hex(GL_WHITE) : p->text;
    lv_color_t dim = glass ? lv_color_hex(GL_TEXT2) : p->text_dim;
    if(it->danger) fg = p->danger;
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(r, glass ? lv_color_white() : p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, glass ? LV_OPA_20 : LV_OPA_COVER, LV_STATE_PRESSED);
    if(it->current) {
        lv_obj_set_style_bg_color(r, glass ? lv_color_white() : p->surface2, 0);
        lv_obj_set_style_bg_opa(r, glass ? LV_OPA_10 : LV_OPA_60, 0);
    }
    if(it->img) {                                      /* ícone do grupo: quadrado na cor dele */
        lv_color_t c = lv_color_hex(it->color);
        lv_obj_t *sq = lv_obj_create(r);
        lv_obj_remove_style_all(sq);
        lv_obj_set_size(sq, 36, 36);
        lv_obj_set_style_radius(sq, 10, 0);
        lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
        lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
        lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *img = lv_image_create(sq);
        lv_image_set_src(img, it->img);
        lv_image_set_scale(img, 256 * 26 / 56);
        lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
    } else {
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, it->symbol ? it->symbol : "");
        lv_obj_set_style_text_font(s, BF_FONT_M, 0);
        lv_obj_set_style_text_color(s, it->danger ? p->danger : glass ? lv_color_hex(GL_SKY) : p->accent, 0);
        lv_obj_set_width(s, 26);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, it->text);
    lv_obj_set_style_text_font(l, BF_FONT_M, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    if(it->note || it->current) {
        lv_obj_t *n = lv_label_create(r);
        lv_label_set_text(n, it->current ? LV_SYMBOL_OK : it->note);
        lv_obj_set_style_text_font(n, BF_FONT_S, 0);
        lv_obj_set_style_text_color(n, it->current ? (glass ? lv_color_hex(GL_ACTIVE) : p->success) : dim, 0);
    }
    lv_obj_add_event_cb(r, row_click, LV_EVENT_CLICKED, (void *)(intptr_t)it->id);
    return r;
}

lv_obj_t *ctxmenu_open(lv_obj_t *anchor, const char *title, const char *subtitle, const ctx_item_t *items,
                       int n, ctx_cb_t cb, void *ud)
{
    if(bd) { cur_cb = NULL; lv_obj_delete(bd); bd = NULL; }
    const bf_palette_t *p = pal();
    bool glass = theme_glass();
    cur_cb = cb;
    cur_ud = ud;

    bd = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_40, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bd, bd_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bd, on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, BF_RADIUS_L, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    if(glass) {                                       /* vidro escuro (legível sobre qualquer fundo) */
        lv_obj_set_style_bg_color(card, lv_color_hex(GL_DEEP), 0);
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(GL_BLUE), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(GL_SKY), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
    } else {
        lv_obj_set_style_bg_color(card, p->surface, 0);
        lv_obj_set_style_border_color(card, p->dark ? lv_color_white() : p->border, 0);
        lv_obj_set_style_border_opa(card, p->dark ? LV_OPA_20 : LV_OPA_COVER, 0);
    }
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);      /* toque entre as linhas não fecha */

    if(title) {
        lv_obj_t *h = lv_obj_create(card);
        lv_obj_remove_style_all(h);
        lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(h, 10, 0);
        lv_obj_set_style_pad_ver(h, 4, 0);
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *t = lv_label_create(h);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, BF_FONT_M, 0);
        lv_obj_set_style_text_color(t, glass ? lv_color_hex(GL_WHITE) : p->text, 0);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        if(subtitle && *subtitle) {
            lv_obj_t *s = lv_label_create(h);
            lv_label_set_text(s, subtitle);
            lv_obj_set_style_text_font(s, BF_FONT_S, 0);
            lv_obj_set_style_text_color(s, glass ? lv_color_hex(GL_TEXT2) : p->text_dim, 0);
            lv_obj_set_width(s, LV_PCT(100));
            lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        }
        lv_obj_t *line = lv_obj_create(card);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(line, glass ? lv_color_white() : p->border, 0);
        lv_obj_set_style_bg_opa(line, glass ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_margin_ver(line, 4, 0);
    }
    /* linhas numa lista que rola se não couber (muitos grupos) */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    for(int i = 0; i < n; i++) row(list, &items[i], glass);

    /* altura máxima: a tela; acima disso a lista rola */
    lv_obj_update_layout(card);
    int32_t maxh = BF_SCREEN_H - 2 * MARGIN;
    if(lv_obj_get_height(card) > maxh) {
        int32_t extra = lv_obj_get_height(card) - maxh;
        lv_obj_set_height(list, lv_obj_get_height(list) - extra);
        bf_scrollable(list);
        lv_obj_update_layout(card);
    }
    int32_t h = lv_obj_get_height(card), x, y;
    if(anchor) {                                       /* ao lado do ícone, sem sair da tela */
        lv_area_t a;
        lv_obj_get_coords(anchor, &a);
        x = a.x2 + MARGIN;
        if(x + W > BF_SCREEN_W - MARGIN) x = a.x1 - MARGIN - W;
        if(x < MARGIN) x = (a.x1 + a.x2) / 2 - W / 2;
        y = a.y1;
    } else {
        x = (BF_SCREEN_W - W) / 2;
        y = (BF_SCREEN_H - h) / 2;
    }
    if(x < MARGIN) x = MARGIN;
    if(x + W > BF_SCREEN_W - MARGIN) x = BF_SCREEN_W - MARGIN - W;
    if(y + h > BF_SCREEN_H - MARGIN) y = BF_SCREEN_H - MARGIN - h;
    if(y < MARGIN) y = MARGIN;
    lv_obj_set_pos(card, x, y);
    if(fx_anim() == LV_ANIM_ON) lv_obj_fade_in(card, 120, 0);
    return card;
}
