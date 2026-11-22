/* glass.c — ver glass.h. */
#include "glass.h"
#include "../core/theme.h"

lv_obj_t *glass_card(lv_obj_t *parent, int x, int y, int w, int h, int r)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, r, 0);
    /* vidro: azul translúcido levemente mais claro no alto, contorno branco fino */
    lv_obj_set_style_bg_color(c, lv_color_hex(GL_BLUE), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_40, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *glass_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t *glass_tile(lv_obj_t *parent, int size, int radius, uint32_t top, uint32_t bottom,
                     const lv_image_dsc_t *icon, const char *symbol, const lv_font_t *font)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, size, size);
    lv_obj_set_style_radius(t, radius, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, lv_color_white(), 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_opa(t, LV_OPA_30, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);    /* o toque é do item que o contém */
    if(icon) {
        lv_obj_t *img = lv_image_create(t);
        lv_image_set_src(img, icon);
        lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
    } else if(symbol) {
        lv_obj_t *l = glass_label(t, symbol, font, GL_WHITE);
        lv_obj_center(l);
    }
    return t;
}

lv_obj_t *glass_bars(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 30, 26);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for(int i = 0; i < 4; i++) {
        lv_obj_t *s = lv_obj_create(b);
        lv_obj_remove_style_all(s);
        int h = 8 + i * 6;
        lv_obj_set_size(s, 5, h);
        lv_obj_set_pos(s, i * 8, 26 - h);
        lv_obj_set_style_radius(s, 2, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(GL_ACTIVE), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    }
    return b;
}

void glass_bars_set(lv_obj_t *bars, int level)
{
    if(!bars) return;
    if(level < 0) { lv_obj_add_flag(bars, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_HIDDEN);
    for(int i = 0; i < 4; i++) {
        lv_obj_t *s = lv_obj_get_child(bars, i);
        lv_opa_t o = i < level ? LV_OPA_COVER : LV_OPA_20;
        if(lv_obj_get_style_bg_opa(s, 0) != o) lv_obj_set_style_bg_opa(s, o, 0);
    }
}
