/* scr_home.c — grade de ícones grandes, paginada (deslizar ou setas). */
#include "screens.h"
#include "../core/theme.h"
#include "../widgets/tile.h"
#include "../assets/icons/bf_icons.h"

typedef struct {
    const lv_image_dsc_t *icon;
    bf_str_t label;
    uint32_t color;
    bf_screen_t scr;
    int arg;
} app_t;

static const app_t apps[] = {
    { &bf_icon_multimedia, S_APP_MULTIMEDIA, 0xe5534b, SCR_MUSIC, 0 },
    { &bf_icon_tv,         S_APP_TV,         0x9b6ad6, SCR_FEATURE, FEAT_TV },
    { &bf_icon_bluetooth,  S_APP_BLUETOOTH,  0x3584e4, SCR_FEATURE, FEAT_BLUETOOTH },
    { &bf_icon_book,       S_APP_BOOK,       0xc98b2c, SCR_FEATURE, FEAT_BOOK },
    { &bf_icon_gps,        S_APP_GPS,        0x2ec27e, SCR_FEATURE, FEAT_GPS },
    { &bf_icon_files,      S_APP_FILES,      0xe0a40c, SCR_FEATURE, FEAT_FILES },
    { &bf_icon_terminal,   S_APP_TERMINAL,   0x4b5363, SCR_FEATURE, FEAT_TERMINAL },
    { &bf_icon_run,        S_APP_RUN,        0x1fb5cc, SCR_RUN,     RUN_ARG_NORMAL },
    { &bf_icon_bigscreen,  S_APP_BIGSCREEN,  0x2a6fd0, SCR_FEATURE, FEAT_BIGSCREEN },
    { &bf_icon_monitor,    S_APP_MONITOR,    0x26a269, SCR_MONITOR, 0 },
    { &bf_icon_settings,   S_APP_SETTINGS,   0x6c7383, SCR_SETTINGS, 0 },
};
#define N_APPS   (int)(sizeof apps / sizeof apps[0])
#define PER_PAGE 8
#define N_PAGES  ((N_APPS + PER_PAGE - 1) / PER_PAGE)

static lv_obj_t *tv, *arrow_l, *arrow_r, *dots[N_PAGES];
static int page;  /* lembrada entre visitas */

static void app_click(lv_event_t *e)
{
    const app_t *a = lv_event_get_user_data(e);
    nav_go(a->scr, a->arg);
}

static void sync_page(void)
{
    const bf_palette_t *p = pal();
    for(int i = 0; i < N_PAGES; i++) {
        lv_obj_set_width(dots[i], i == page ? 22 : 8);
        lv_obj_set_style_bg_color(dots[i], i == page ? p->accent : p->text_dim, 0);
        lv_obj_set_style_bg_opa(dots[i], i == page ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if(page > 0) lv_obj_remove_flag(arrow_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(arrow_l, LV_OBJ_FLAG_HIDDEN);
    if(page < N_PAGES - 1) lv_obj_remove_flag(arrow_r, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(arrow_r, LV_OBJ_FLAG_HIDDEN);
}

static void tv_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *t = lv_tileview_get_tile_active(tv);
    page = (int)lv_obj_get_x(t) / BF_SCREEN_W;
    sync_page();
}

void scr_home_show_page(int pg)
{
    if(!tv || pg < 0 || pg >= N_PAGES) return;
    page = pg;
    lv_tileview_set_tile_by_index(tv, (uint32_t)pg, 0, LV_ANIM_OFF);
    sync_page();
}

static void arrow_click(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int np = page + d;
    if(np < 0 || np >= N_PAGES) return;
    page = np;
    lv_tileview_set_tile_by_index(tv, (uint32_t)np, 0, LV_ANIM_ON);
    sync_page();
}

static lv_obj_t *arrow(lv_obj_t *parent, const char *sym, int dir)
{
    const bf_palette_t *p = pal();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 64, 146);
    lv_obj_set_style_radius(b, BF_RADIUS_M, 0);
    lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *l = bf_label(b, sym, BF_FONT_L, p->text_dim);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, arrow_click, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
    return b;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    tv = lv_tileview_create(parent);
    lv_obj_set_size(tv, BF_SCREEN_W, BF_CONTENT_H - 30);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    for(int pg = 0; pg < N_PAGES; pg++) {
        lv_dir_t dir = (pg > 0 ? LV_DIR_LEFT : 0) | (pg < N_PAGES - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(tv, (uint8_t)pg, 0, dir);
        lv_obj_t *grid = bf_plain(tile);
        lv_obj_set_size(grid, 4 * 152 + 3 * 16, 2 * 146 + 16);
        lv_obj_align(grid, LV_ALIGN_CENTER, 0, 4);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, 16, 0);
        lv_obj_set_style_pad_column(grid, 16, 0);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
        for(int i = pg * PER_PAGE; i < N_APPS && i < (pg + 1) * PER_PAGE; i++)
            tile_create(grid, apps[i].icon, tr(apps[i].label), apps[i].color, app_click, (void *)&apps[i]);
    }
    lv_obj_add_event_cb(tv, tv_changed, LV_EVENT_VALUE_CHANGED, NULL);

    arrow_l = arrow(parent, LV_SYMBOL_LEFT, -1);
    lv_obj_align(arrow_l, LV_ALIGN_LEFT_MID, 2, -11);
    arrow_r = arrow(parent, LV_SYMBOL_RIGHT, +1);
    lv_obj_align(arrow_r, LV_ALIGN_RIGHT_MID, -2, -11);

    lv_obj_t *dotrow = bf_plain(parent);
    lv_obj_set_size(dotrow, LV_SIZE_CONTENT, 8);
    lv_obj_align(dotrow, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(dotrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dotrow, 8, 0);
    for(int i = 0; i < N_PAGES; i++) {
        dots[i] = lv_obj_create(dotrow);
        lv_obj_remove_style_all(dots[i]);
        lv_obj_set_size(dots[i], 8, 8);
        lv_obj_set_style_radius(dots[i], 2, 0);  /* quadradinhos: detalhe "pixel" retrô */
        lv_obj_set_style_bg_color(dots[i], p->text_dim, 0);
    }
    lv_tileview_set_tile_by_index(tv, (uint32_t)page, 0, LV_ANIM_OFF);
    sync_page();
}

static void destroy(void) { tv = NULL; }

const bf_screen_def_t scr_home_def = { create, destroy, S_HOME, NULL };
