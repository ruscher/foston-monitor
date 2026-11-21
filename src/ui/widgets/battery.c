/* battery.c — ver battery.h. */
#include "battery.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include <string.h>

/* filhos do contêiner, na ordem de criação */
enum { CH_BOLT, CH_GLYPH };

static void set_text_if(lv_obj_t *l, const char *t)
{
    const char *old = lv_label_get_text(l);
    if(!old || strcmp(old, t)) lv_label_set_text(l, t);
}

lv_obj_t *battery_create(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 2, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bolt = lv_label_create(c);
    lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(bolt, font, 0);
    lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *glyph = lv_label_create(c);
    lv_label_set_text(glyph, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_font(glyph, font, 0);

    battery_set(c, -1, BF_BAT_UNKNOWN);
    return c;
}

void battery_set(lv_obj_t *c, int pct, bf_bat_state_t st)
{
    if(!c) return;
    const bf_palette_t *p = pal();
    lv_obj_t *bolt = lv_obj_get_child(c, CH_BOLT);
    lv_obj_t *glyph = lv_obj_get_child(c, CH_GLYPH);
    if(pct > 100) pct = 100;
    bool plugged = st == BF_BAT_CHARGING || st == BF_BAT_FULL || st == BF_BAT_NOT_CHARGING;
    if(st == BF_BAT_FULL) pct = 100;

    const char *sym = LV_SYMBOL_BATTERY_EMPTY;
    if(pct >= 0)
        sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
              pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    set_text_if(glyph, sym);

    lv_color_t col;
    if(plugged) col = p->success;
    else if(pct < 0) col = p->text_dim;
    else if(pct < 10) col = p->danger;
    else col = lv_color_hex(theme_dark_now() ? 0xff8c1a : 0xc8600e);   /* laranja */
    /* a barra superior chama a cada segundo: só mexe no estilo se a cor mudou */
    if(!lv_color_eq(lv_obj_get_style_text_color(glyph, 0), col)) {
        lv_obj_set_style_text_color(glyph, col, 0);
        lv_obj_set_style_text_color(bolt, col, 0);
    }
    if(plugged) lv_obj_remove_flag(bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);
}

const char *battery_state_text(bf_bat_state_t st)
{
    switch(st) {
    case BF_BAT_CHARGING:     return tr(S_CHARGING);
    case BF_BAT_FULL:         return tr(S_BAT_FULL);
    case BF_BAT_NOT_CHARGING: return tr(S_BAT_NOT_CHARGING);
    default:                  return "";
    }
}
