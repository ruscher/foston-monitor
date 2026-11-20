/* scr_feature.c — página honesta para funções ainda em desenvolvimento.
 * Mostra descrição, estado real e o que falta; nunca controles falsos. */
#include "screens.h"
#include "../core/theme.h"
#include "../widgets/page.h"
#include "../assets/icons/bf_icons.h"

typedef struct {
    const lv_image_dsc_t *icon;
    uint32_t color;
    bf_str_t title, desc, missing;
    bool needs_hw;   /* depende de driver/hardware ainda inexistente */
} feature_t;

static const feature_t feats[FEAT_COUNT] = {
    [FEAT_MULTIMEDIA] = { &bf_icon_multimedia, 0xe5534b, S_APP_MULTIMEDIA, S_DESC_MULTIMEDIA, S_MISS_MULTIMEDIA, true },
    [FEAT_TV]         = { &bf_icon_tv,         0x9b6ad6, S_APP_TV,         S_DESC_TV,         S_MISS_TV,         true },
    [FEAT_BLUETOOTH]  = { &bf_icon_bluetooth,  0x3584e4, S_APP_BLUETOOTH,  S_DESC_BLUETOOTH,  S_MISS_BLUETOOTH,  true },
    [FEAT_BOOK]       = { &bf_icon_book,       0xc98b2c, S_APP_BOOK,       S_DESC_BOOK,       S_MISS_BOOK,       false },
    [FEAT_GPS]        = { &bf_icon_gps,        0x2ec27e, S_APP_GPS,        S_DESC_GPS,        S_MISS_GPS,        true },
    [FEAT_FILES]      = { &bf_icon_files,      0xe0a40c, S_APP_FILES,      S_DESC_FILES,      S_MISS_FILES,      false },
    [FEAT_TERMINAL]   = { &bf_icon_terminal,   0x4b5363, S_APP_TERMINAL,   S_DESC_TERMINAL,   S_MISS_TERMINAL,   false },
    [FEAT_BIGSCREEN]  = { &bf_icon_bigscreen,  0x2a6fd0, S_APP_BIGSCREEN,  S_DESC_BIGSCREEN,  S_MISS_BIGSCREEN,  true },
    [FEAT_FM]         = { &bf_icon_fm,         0xe0a40c, S_SET_FM,         S_DESC_FM,         S_MISS_FM,         true },
    [FEAT_USB]        = { &bf_icon_usb,        0x6c7383, S_SET_USB,        S_DESC_USB,        S_MISS_USB,        true },
    [FEAT_GPSINFO]    = { &bf_icon_gps,        0x2ec27e, S_SET_GPSINFO,    S_DESC_GPSINFO,    S_MISS_GPSINFO,    true },
};

static void open_terminal(lv_event_t *e) { (void)e; nav_go(SCR_RUN, RUN_ARG_TERMINAL); }

static void create(lv_obj_t *parent, int arg)
{
    if(arg < 0 || arg >= FEAT_COUNT) arg = 0;
    const feature_t *f = &feats[arg];
    const bf_palette_t *p = pal();

    lv_obj_t *pg = page_create(parent);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pg, BF_SP_XL, 0);
    lv_obj_set_style_pad_top(pg, BF_SP_XL, 0);

    /* ícone grande no mesmo estilo da grade inicial */
    lv_color_t c = lv_color_hex(f->color);
    lv_obj_t *sq = lv_obj_create(pg);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 112, 112);
    lv_obj_set_style_radius(sq, 30, 0);
    lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = lv_image_create(sq);
    lv_image_set_src(img, f->icon);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);

    lv_obj_t *col = bf_plain(pg);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, BF_SP_M, 0);

    bf_label(col, tr(f->title), BF_FONT_L, p->text);

    lv_obj_t *pills = bf_plain(col);
    lv_obj_set_size(pills, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(pills, BF_SP_S, 0);
    lv_obj_set_style_pad_row(pills, BF_SP_S, 0);
    bf_pill(pills, tr(S_IN_DEV), p->accent);
    if(f->needs_hw) bf_pill(pills, tr(S_UNAVAILABLE), p->warning);

    lv_obj_t *d = bf_label(col, tr(f->desc), BF_FONT_M, p->text);
    lv_obj_set_width(d, LV_PCT(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);

    lv_obj_t *m = lv_label_create(col);
    lv_label_set_text_fmt(m, "%s: %s", tr(S_WHAT_MISSING), tr(f->missing));
    lv_obj_set_style_text_font(m, BF_FONT_S, 0);
    lv_obj_set_style_text_color(m, p->text_dim, 0);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);

    if(arg == FEAT_TERMINAL) {
        lv_obj_t *b = bf_button(col, BF_SYMBOL_TERMINAL, tr(S_OPEN_RUN_TERMINAL), 1);
        lv_obj_add_event_cb(b, open_terminal, LV_EVENT_CLICKED, NULL);
    }
}

static const char *title_fn(int arg)
{
    if(arg < 0 || arg >= FEAT_COUNT) arg = 0;
    return tr(feats[arg].title);
}

const bf_screen_def_t scr_feature_def = { create, NULL, S_IN_DEV, title_fn };
