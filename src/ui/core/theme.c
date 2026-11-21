/* theme.c — paletas (modo escuro/claro x tema de cores), papel de parede e
 * estilos compartilhados. */
#include "theme.h"
#include "settings.h"
#include "wallpaper.h"
#include <time.h>

static bf_palette_t P;

lv_style_t bf_st_scrollbar;
lv_style_t bf_st_screen, bf_st_card, bf_st_btn, bf_st_btn_pr, bf_st_btn_primary,
           bf_st_btn_danger, bf_st_plain;
static bool styles_inited;
static lv_image_dsc_t wall;
static bool wall_ok;

/* O primeiro é o padrão: as cores do logotipo FOSTONOS. */
static const bf_preset_t presets[] = {
    { S_PAL_FOSTON,   0x6c5ce7, 0x26dda8, 0x0a226a, 0x1d3a9e, 34 },
    { S_PAL_CLASSIC,  BF_BLUE,  BF_CYAN,  0x1b2433, 0x23406a, 10 },
    { S_PAL_EMERALD,  0x1f9d6b, 0x6bed48, 0x0d3b2c, 0x13704f, 26 },
    { S_PAL_SUNSET,   0xe5484d, 0xff9a2e, 0x3d1424, 0x8a2a3a, 24 },
    { S_PAL_VIOLET,   0x8b5cf6, 0xe879f9, 0x2a1450, 0x55309a, 28 },
    { S_PAL_GRAPHITE, 0x5f6b7e, 0xa3adbd, 0x1f2329, 0x3a414d, 6 },
};
#define N_PRESETS (int)(sizeof presets / sizeof presets[0])

int theme_preset_count(void) { return N_PRESETS; }
const bf_preset_t *theme_preset(int i) { return &presets[i >= 0 && i < N_PRESETS ? i : 0]; }
const bf_palette_t *pal(void) { return &P; }
const lv_image_dsc_t *theme_wallpaper(void) { return wall_ok ? &wall : NULL; }

bool theme_dark_now(void)
{
    const bf_settings_t *s = settings();
    if(s->mode != BF_MODE_AUTO) return s->mode != BF_MODE_LIGHT;
    time_t n = time(NULL);
    struct tm tmv;
    if(!localtime_r(&n, &tmv) || tmv.tm_year + 1900 < 2024) return true;  /* relógio não acertado */
    return tmv.tm_hour < 7 || tmv.tm_hour >= 19;
}

static lv_color_t hex_mix(uint32_t a, uint32_t b, int pct)   /* pct% de b sobre a */
{
    return lv_color_mix(lv_color_hex(b), lv_color_hex(a), (uint8_t)(pct * 255 / 100));
}

static void set_palette(bool dark, const bf_preset_t *pr)
{
    P.dark = dark;
    P.primary = lv_color_hex(pr->primary);
    P.success = lv_color_hex(BF_GREEN);
    P.danger  = lv_color_hex(BF_RED);
    if(dark) {
        int t = pr->tint_pct;
        P.bg       = hex_mix(0x15181e, pr->tint, t);
        P.surface  = hex_mix(0x1f232b, pr->tint, t);
        P.surface2 = hex_mix(0x2a2f39, pr->tint, t);
        P.border   = hex_mix(0x3a4050, pr->tint, t);
        P.text     = lv_color_hex(0xeef1f6);
        P.text_dim = hex_mix(0x98a2b3, pr->accent, 10);
        P.accent   = lv_color_hex(pr->accent);
        P.warning  = lv_color_hex(BF_AMBER);
    }
    else {
        int t = pr->tint_pct / 4 + 3;
        P.bg       = hex_mix(0xeef1f6, pr->tint, t);
        P.surface  = lv_color_hex(0xffffff);
        P.surface2 = hex_mix(0xe2e7ef, pr->tint, t);
        P.border   = hex_mix(0xcfd6e2, pr->tint, t);
        P.text     = lv_color_hex(0x1b1f27);
        P.text_dim = lv_color_hex(0x586274);
        /* destaque escurecido p/ contraste em fundo claro */
        P.accent   = lv_color_darken(lv_color_hex(pr->accent), LV_OPA_40);
        P.primary  = lv_color_darken(lv_color_hex(pr->primary), LV_OPA_10);
        P.success  = lv_color_hex(0x1f9d5f);
        P.warning  = lv_color_hex(0xb3650b);
    }
}

static void style_reinit(lv_style_t *s)
{
    if(styles_inited) lv_style_reset(s);
    lv_style_init(s);
}

void theme_init(lv_display_t *disp)
{
    const bf_settings_t *cfg = settings();
    const bf_preset_t *pr = theme_preset(cfg->palette);
    bool dark = theme_dark_now();
    set_palette(dark, pr);
    wall_ok = cfg->wallpaper != BF_WALL_PLAIN &&
              wallpaper_build(&wall, &P, lv_color_hex(pr->glow), cfg->wallpaper == BF_WALL_LOGO);
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

    /* barra de rolagem visível (lv_obj_remove_style_all apaga a do tema) */
    style_reinit(&bf_st_scrollbar);
    lv_style_set_bg_color(&bf_st_scrollbar, P.text_dim);
    lv_style_set_bg_opa(&bf_st_scrollbar, LV_OPA_60);
    lv_style_set_width(&bf_st_scrollbar, 6);
    lv_style_set_radius(&bf_st_scrollbar, 3);
    lv_style_set_pad_right(&bf_st_scrollbar, 3);
    lv_style_set_pad_ver(&bf_st_scrollbar, 6);

    styles_inited = true;
}

void bf_scrollable(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_style(o, &bf_st_scrollbar, LV_PART_SCROLLBAR);
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
