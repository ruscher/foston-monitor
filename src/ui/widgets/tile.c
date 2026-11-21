/* tile.c — ícone da grade inicial (alvo de toque 152x146). */
#include "tile.h"
#include "../core/theme.h"

lv_obj_t *tile_create(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *label,
                      uint32_t color, lv_event_cb_t cb, void *ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *t = lv_button_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 152, 146);
    lv_obj_set_style_radius(t, BF_RADIUS_L, 0);
    /* vidro: deixa o papel de parede aparecer de leve */
    lv_obj_set_style_bg_color(t, p->surface, 0);
    lv_obj_set_style_bg_opa(t, p->dark ? LV_OPA_70 : LV_OPA_80, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, p->dark ? lv_color_white() : p->border, 0);
    lv_obj_set_style_border_opa(t, p->dark ? LV_OPA_10 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(t, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(t, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(t, 2, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 10, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_color_t c = lv_color_hex(color);
    lv_obj_t *sq = lv_obj_create(t);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 80, 80);
    lv_obj_set_style_radius(sq, 22, 0);
    lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(sq);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);

    lv_obj_t *l = lv_label_create(t);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, BF_FONT_M, 0);
    lv_obj_set_style_text_color(l, p->text, 0);
    bf_label_ellipsis(l, 140);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    if(cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, ud);
    return t;
}
