/* scr_settings.c — lista de Ajustes em duas colunas (linhas de 76 px). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../widgets/page.h"

typedef struct {
    const char *sym;
    uint32_t color;
    bf_str_t title, sub;
    bf_screen_t scr;
    int arg;
} item_t;

static const item_t items[] = {
    { BF_SYMBOL_PALETTE,   0x6c5ce7, S_SET_APPEARANCE, S_SUB_APPEARANCE, SCR_APPEARANCE, 0 },
    { BF_SYMBOL_SUN,       0xf5c211, S_SET_BACKLIGHT, S_SUB_BACKLIGHT, SCR_BACKLIGHT, 0 },
    { LV_SYMBOL_VOLUME_MAX,0x3584e4, S_SET_VOLUME,    S_SUB_VOLUME,    SCR_VOLUME, 0 },
    { BF_SYMBOL_LANGUAGE,  0x33c7de, S_SET_LANGUAGE,  S_SUB_LANGUAGE,  SCR_LANGUAGE, 0 },
    { BF_SYMBOL_SATELLITE, 0x2ec27e, S_SET_GPSINFO,   S_SUB_GPSINFO,   SCR_FEATURE, FEAT_GPSINFO },
    { BF_SYMBOL_CALENDAR,  0xe5534b, S_SET_DATE,      S_SUB_DATE,      SCR_DATE, 0 },
    { BF_SYMBOL_CROSSHAIR, 0x9b6ad6, S_SET_CALIBRATE, S_SUB_CALIBRATE, SCR_CALIBRATE, 0 },
    { BF_SYMBOL_ROCKET,    0x1fb5cc, S_SET_RUN,       S_SUB_RUN,       SCR_RUN, RUN_ARG_NORMAL },
    { LV_SYMBOL_BATTERY_3, 0x26a269, S_SET_POWER,     S_SUB_POWER,     SCR_POWER, 0 },
    { BF_SYMBOL_UNDO,      0xe5484d, S_SET_RESET,     S_SUB_RESET,     SCR_RESET, 0 },
    { BF_SYMBOL_INFO,      0x3584e4, S_SET_SYSINFO,   S_SUB_SYSINFO,   SCR_SYSINFO, 0 },
    { LV_SYMBOL_USB,       0x8a93a5, S_SET_USB,       S_SUB_USB,       SCR_FEATURE, FEAT_USB },
    { BF_SYMBOL_TOWER,     0xe0a40c, S_SET_FM,        S_SUB_FM,        SCR_FEATURE, FEAT_FM },
    { BF_SYMBOL_KEY,       0x33c7de, S_SET_SSH,       S_SUB_SSH,       SCR_SSH, 0 },
    { BF_SYMBOL_WRENCH,    0x8a93a5, S_SET_ADVANCED,  S_SUB_ADVANCED,  SCR_ADVANCED, 0 },
};
#define N_ITEMS (int)(sizeof items / sizeof items[0])

static int last_scroll;
static lv_obj_t *pg;

static void click(lv_event_t *e)
{
    const item_t *it = lv_event_get_user_data(e);
    nav_go(it->scr, it->arg);
}

/* Itens cujo hardware não tem driver ainda recebem selo "indisponível". */
static bool unavailable(const item_t *it)
{
    const bf_status_provider_t *p = status_provider();
    if(it->scr == SCR_BACKLIGHT) return p->backlight_get() < 0;
    if(it->scr == SCR_VOLUME) return p->volume_get() < 0;
    if(it->scr == SCR_FEATURE) return it->arg == FEAT_GPSINFO || it->arg == FEAT_FM || it->arg == FEAT_USB;
    return false;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    pg = page_create(parent);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(pg, BF_SP_L, 0);
    lv_obj_set_style_pad_row(pg, BF_SP_M, 0);

    for(int i = 0; i < N_ITEMS; i++) {
        const item_t *it = &items[i];
        lv_obj_t *row = lv_button_create(pg);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &bf_st_card, 0);
        lv_obj_set_style_bg_color(row, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, p->accent, LV_STATE_PRESSED);
        lv_obj_set_size(row, 368, 76);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_pad_left(row, BF_SP_M, 0);
        lv_obj_set_style_pad_right(row, BF_SP_M, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, BF_SP_M, 0);
        lv_obj_add_event_cb(row, click, LV_EVENT_CLICKED, (void *)it);

        page_round_icon(row, it->sym, lv_color_hex(it->color), 48);

        lv_obj_t *col = bf_plain(row);
        lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *t = bf_label(col, tr(it->title), BF_FONT_M, p->text);
        bf_label_ellipsis(t, LV_PCT(100));
        lv_obj_t *s = bf_label(col, tr(it->sub), BF_FONT_S, p->text_dim);
        bf_label_ellipsis(s, LV_PCT(100));
        if(unavailable(it)) {
            lv_label_set_text(s, tr(S_UNAVAILABLE_SHORT));
            lv_obj_set_style_text_color(s, p->warning, 0);
        }
        bf_label(row, LV_SYMBOL_RIGHT, BF_FONT_S, p->text_dim);
    }
    if(nav_is_return()) {   /* ao voltar de um item, mantém a posição da lista */
        lv_obj_update_layout(pg);
        lv_obj_scroll_to_y(pg, last_scroll, LV_ANIM_OFF);
    }
}

static void destroy(void)
{
    if(pg) last_scroll = lv_obj_get_scroll_y(pg);
    pg = NULL;
}

const bf_screen_def_t scr_settings_def = { create, destroy, S_APP_SETTINGS, NULL };
