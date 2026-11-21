/* osd.c — ver osd.h. */
#include "osd.h"
#include "theme.h"
#include "fx.h"

static lv_obj_t *card;
static lv_timer_t *hide_timer;

static void deleted(lv_event_t *e)
{
    if(lv_event_get_target(e) == card) card = NULL;
}

/* fim do aviso: some aos poucos (o fade_in e o fade_out animam a mesma
 * propriedade, então o segundo só pode começar depois do primeiro) */
static void hide_cb(lv_timer_t *t)
{
    (void)t;
    hide_timer = NULL;
    if(!card) return;
    if(fx_anim() == LV_ANIM_ON) {
        lv_obj_fade_out(card, 300, 0);
        lv_obj_delete_delayed(card, 320);
    }
    else {
        lv_obj_delete(card);
    }
}

void osd_show(const lv_image_dsc_t *icon, uint32_t color, const char *title, const char *sub)
{
    const bf_palette_t *p = pal();
    if(card) lv_obj_delete(card);              /* o deleted() zera o ponteiro */

    card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 360, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, p->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BF_RADIUS_L + 4, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(card, BF_SP_XL, 0);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *circle = lv_obj_create(card);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 92, 92);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    if(icon) {
        lv_obj_t *img = lv_image_create(circle);
        lv_image_set_src(img, icon);
        lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
    }
    lv_obj_t *t = bf_label(card, title, BF_FONT_L, p->text);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if(sub && *sub) {
        lv_obj_t *s = bf_label(card, sub, BF_FONT_M, p->text_dim);
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }

    if(fx_anim() == LV_ANIM_ON) lv_obj_fade_in(card, 160, 0);
    if(hide_timer) lv_timer_delete(hide_timer);
    hide_timer = lv_timer_create(hide_cb, OSD_MS, NULL);
    lv_timer_set_repeat_count(hide_timer, 1);
}
