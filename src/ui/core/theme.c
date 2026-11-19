/* theme.c — paletas escura/clara e estilos compartilhados. */
#include "theme.h"

static bf_palette_t P;

lv_style_t bf_st_screen, bf_st_card, bf_st_btn, bf_st_btn_pr, bf_st_btn_primary,
           bf_st_btn_danger, bf_st_plain;
static bool styles_inited;

const bf_palette_t *pal(void) { return &P; }

static void set_palette(bool dark)
{
    P.dark = dark;
    P.primary = lv_color_hex(BF_BLUE);
    P.success = lv_color_hex(BF_GREEN);
    P.warning = dark ? lv_color_hex(BF_AMBER) : lv_color_hex(0xb3650b); /* âmbar escuro no tema claro */
    P.danger  = lv_color_hex(BF_RED);
    if(dark) {
        P.bg       = lv_color_hex(0x1b1f27);
        P.surface  = lv_color_hex(0x232833);
        P.surface2 = lv_color_hex(0x2d3341);
        P.border   = lv_color_hex(0x3a4152);
        P.text     = lv_color_hex(0xe9edf4);
        P.text_dim = lv_color_hex(0x98a2b3);
        P.accent   = lv_color_hex(BF_CYAN);
    }
    else {
        P.bg       = lv_color_hex(0xeef1f6);
        P.surface  = lv_color_hex(0xffffff);
        P.surface2 = lv_color_hex(0xe2e7ef);
        P.border   = lv_color_hex(0xcfd6e2);
        P.text     = lv_color_hex(0x1b1f27);
        P.text_dim = lv_color_hex(0x586274);
        P.accent   = lv_color_hex(0x1597ad); /* ciano escurecido p/ contraste em fundo claro */
        P.success  = lv_color_hex(0x1f9d5f);
    }
}

static void style_reinit(lv_style_t *s)
{
    if(styles_inited) lv_style_reset(s);
    lv_style_init(s);
}

void theme_init(lv_display_t *disp, bool dark)
{
    set_palette(dark);
    lv_theme_t *th = lv_theme_default_init(disp, P.primary, P.accent, dark, BF_FONT_M);
    lv_display_set_theme(disp, th);

    style_reinit(&bf_st_screen);
    lv_style_set_bg_color(&bf_st_screen, P.bg);
    lv_style_set_bg_opa(&bf_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&bf_st_screen, P.text);
    lv_style_set_text_font(&bf_st_screen, BF_FONT_M);

    style_reinit(&bf_st_card);
    lv_style_set_bg_color(&bf_st_card, P.surface);
    lv_style_set_bg_opa(&bf_st_card, LV_OPA_COVER);
    lv_style_set_radius(&bf_st_card, BF_RADIUS_M);
    lv_style_set_border_width(&bf_st_card, 1);
    lv_style_set_border_color(&bf_st_card, P.border);
    lv_style_set_border_opa(&bf_st_card, dark ? LV_OPA_60 : LV_OPA_COVER);
    lv_style_set_pad_all(&bf_st_card, BF_SP_L);
    lv_style_set_shadow_width(&bf_st_card, 0);
    lv_style_set_text_color(&bf_st_card, P.text);

    style_reinit(&bf_st_btn);
    lv_style_set_bg_color(&bf_st_btn, P.surface2);
    lv_style_set_bg_opa(&bf_st_btn, LV_OPA_COVER);
    lv_style_set_radius(&bf_st_btn, BF_RADIUS_S);
    lv_style_set_shadow_width(&bf_st_btn, 0);
    lv_style_set_text_color(&bf_st_btn, P.text);
    lv_style_set_border_width(&bf_st_btn, 0);
    lv_style_set_pad_hor(&bf_st_btn, BF_SP_L);
    lv_style_set_min_height(&bf_st_btn, BF_TOUCH_MIN);
    lv_style_set_min_width(&bf_st_btn, BF_TOUCH_MIN);

    style_reinit(&bf_st_btn_pr);
    lv_style_set_bg_color(&bf_st_btn_pr, P.primary);
    lv_style_set_text_color(&bf_st_btn_pr, lv_color_white());

    style_reinit(&bf_st_btn_primary);
    lv_style_set_bg_color(&bf_st_btn_primary, P.primary);
    lv_style_set_text_color(&bf_st_btn_primary, lv_color_white());

    style_reinit(&bf_st_btn_danger);
    lv_style_set_bg_color(&bf_st_btn_danger, P.danger);
    lv_style_set_text_color(&bf_st_btn_danger, lv_color_white());

    style_reinit(&bf_st_plain);
    lv_style_set_bg_opa(&bf_st_plain, LV_OPA_TRANSP);
    lv_style_set_border_width(&bf_st_plain, 0);
    lv_style_set_pad_all(&bf_st_plain, 0);
    lv_style_set_radius(&bf_st_plain, 0);
    lv_style_set_shadow_width(&bf_st_plain, 0);

    styles_inited = true;
}

lv_obj_t *bf_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &bf_st_plain, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *bf_card(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &bf_st_card, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *bf_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    if(font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t *bf_button(lv_obj_t *parent, const char *symbol, const char *txt, int kind)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &bf_st_btn, 0);
    if(kind == 1) lv_obj_add_style(b, &bf_st_btn_primary, 0);
    else if(kind == 2) lv_obj_add_style(b, &bf_st_btn_danger, 0);
    lv_obj_add_style(b, &bf_st_btn_pr, LV_STATE_PRESSED);
    if(kind) lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_style(b, &bf_st_btn_pr, LV_STATE_CHECKED);
    lv_obj_set_height(b, BF_TOUCH_MIN);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, BF_SP_S, 0);
    if(symbol) lv_label_set_text(lv_label_create(b), symbol);
    if(txt) lv_label_set_text(lv_label_create(b), txt);
    return b;
}

void bf_label_ellipsis(lv_obj_t *l, int32_t width)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, width);
    lv_obj_set_height(l, lv_font_get_line_height(f));
}

lv_obj_t *bf_pill(lv_obj_t *parent, const char *txt, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, BF_FONT_S, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_bg_color(l, color, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_20, 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l, 10, 0);
    lv_obj_set_style_pad_ver(l, 3, 0);
    return l;
}
