/* scr_home.c — tela Início, em um de três modos (Ajustes > Aparência):
 *   Grade     ícones grandes, paginada (deslizar ou setas)
 *   Painel    relógio grande, bateria, "Tocando agora" e atalhos
 *   Quiosque  a grade só com os apps escolhidos (sem Ajustes; ver bottombar.c) */
#include "screens.h"
#include "../core/theme.h"
#include "../widgets/battery.h"
#include "../core/fx.h"
#include "../core/settings.h"
#include "../core/status.h"
#include "../core/clock.h"
#include "../core/player.h"
#include "../core/cover.h"
#include "../widgets/tile.h"
#include "../assets/icons/bf_icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    const lv_image_dsc_t *icon;
    bf_str_t label;
    uint32_t color;
    bf_screen_t scr;
    int arg;
} app_t;

/* A ordem é a dos bits de settings()->home_apps: só acrescentar no fim. */
static const app_t apps[] = {
    { &bf_icon_multimedia, S_APP_MULTIMEDIA, 0xe5534b, SCR_MUSIC, 0 },
    { &bf_icon_tv,         S_APP_TV,         0x9b6ad6, SCR_FEATURE, FEAT_TV },
    { &bf_icon_bluetooth,  S_APP_BLUETOOTH,  0x3584e4, SCR_BLUETOOTH, 0 },
    { &bf_icon_book,       S_APP_BOOK,       0xc98b2c, SCR_BOOK, 0 },
    { &bf_icon_gps,        S_APP_GPS,        0x2ec27e, SCR_MAP, 0 },
    { &bf_icon_files,      S_APP_FILES,      0xe0a40c, SCR_FILES, 0 },
    { &bf_icon_terminal,   S_APP_TERMINAL,   0x4b5363, SCR_TERM, 0 },
    { &bf_icon_run,        S_APP_RUN,        0x1fb5cc, SCR_RUN,     RUN_ARG_NORMAL },
    { &bf_icon_bigscreen,  S_APP_BIGSCREEN,  0x2a6fd0, SCR_BIGSCREEN, 0 },
    { &bf_icon_monitor,    S_APP_MONITOR,    0x26a269, SCR_MONITOR, 0 },
    { &bf_icon_settings,   S_APP_SETTINGS,   0x6c7383, SCR_SETTINGS, 0 },
    { &bf_icon_gallery,    S_APP_GALLERY,    0xd6409f, SCR_GALLERY, 0 },
};
#define N_APPS    (int)(sizeof apps / sizeof apps[0])
#define PER_PAGE  8
#define MAX_PAGES ((N_APPS + PER_PAGE - 1) / PER_PAGE)
#define I_SETTINGS 10

int home_app_count(void) { return N_APPS; }
const char *home_app_name(int i) { return tr(apps[i].label); }
const lv_image_dsc_t *home_app_icon(int i) { return apps[i].icon; }
uint32_t home_app_color(int i) { return apps[i].color; }
bool home_app_is_settings(int i) { return i == I_SETTINGS; }
void home_app_open(int i) { if(i >= 0 && i < N_APPS) nav_go(apps[i].scr, apps[i].arg); }

static void app_click(lv_event_t *e) { home_app_open((int)(intptr_t)lv_event_get_user_data(e)); }

/* ================================================================== grade */

static lv_obj_t *tv, *arrow_l, *arrow_r, *dots[MAX_PAGES];
static int page, n_pages;  /* página lembrada entre visitas */

static void sync_page(void)
{
    const bf_palette_t *p = pal();
    for(int i = 0; i < n_pages; i++) {
        lv_obj_set_width(dots[i], i == page ? 24 : 8);
        lv_obj_set_style_bg_color(dots[i], i == page ? p->accent : p->text_dim, 0);
        lv_obj_set_style_bg_opa(dots[i], i == page ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if(page > 0) lv_obj_remove_flag(arrow_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(arrow_l, LV_OBJ_FLAG_HIDDEN);
    if(page < n_pages - 1) lv_obj_remove_flag(arrow_r, LV_OBJ_FLAG_HIDDEN);
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
    if(!tv || pg < 0 || pg >= n_pages) return;
    page = pg;
    lv_tileview_set_tile_by_index(tv, (uint32_t)pg, 0, LV_ANIM_OFF);
    sync_page();
}

static void arrow_click(lv_event_t *e)
{
    int np = page + (int)(intptr_t)lv_event_get_user_data(e);
    if(np < 0 || np >= n_pages) return;
    page = np;
    lv_tileview_set_tile_by_index(tv, (uint32_t)np, 0, fx_anim());
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

static void create_grid(lv_obj_t *parent, bool kiosk)
{
    const bf_palette_t *p = pal();
    int list[N_APPS], n = 0;
    for(int i = 0; i < N_APPS; i++)
        if(!kiosk || ((settings()->home_apps >> i & 1u) && i != I_SETTINGS)) list[n++] = i;
    if(!n) {                           /* quiosque sem apps escolhidos */
        lv_obj_t *l = bf_label(parent, tr(S_HOME_KIOSK_EMPTY), BF_FONT_M, p->text_dim);
        lv_obj_set_width(l, 600);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        tv = NULL;
        return;
    }
    n_pages = (n + PER_PAGE - 1) / PER_PAGE;
    if(page >= n_pages) page = 0;

    tv = lv_tileview_create(parent);
    lv_obj_set_size(tv, BF_SCREEN_W, BF_CONTENT_H - 30);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    for(int pg = 0; pg < n_pages; pg++) {
        lv_dir_t dir = (pg > 0 ? LV_DIR_LEFT : 0) | (pg < n_pages - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(tv, (uint8_t)pg, 0, dir);
        lv_obj_t *grid = bf_plain(tile);
        lv_obj_set_size(grid, 4 * 152 + 3 * 16, 2 * 146 + 16);
        lv_obj_align(grid, LV_ALIGN_CENTER, 0, 4);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, 16, 0);
        lv_obj_set_style_pad_column(grid, 16, 0);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
        for(int k = pg * PER_PAGE; k < n && k < (pg + 1) * PER_PAGE; k++) {
            int i = list[k];
            lv_obj_t *t = tile_create(grid, apps[i].icon, tr(apps[i].label), apps[i].color, app_click,
                                      (void *)(intptr_t)i);
            /* entrada: os ícones da página visível sobem em sequência */
            if(pg == page) fx_slide_in(t, 0, 24, (uint32_t)(k - pg * PER_PAGE) * 30, 220);
        }
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
    for(int i = 0; i < n_pages; i++) {
        dots[i] = lv_obj_create(dotrow);
        lv_obj_remove_style_all(dots[i]);
        lv_obj_set_size(dots[i], 8, 8);
        lv_obj_set_style_radius(dots[i], 4, 0);
        lv_obj_set_style_bg_color(dots[i], p->text_dim, 0);
    }
    if(n_pages < 2) lv_obj_add_flag(dotrow, LV_OBJ_FLAG_HIDDEN);
    lv_tileview_set_tile_by_index(tv, (uint32_t)page, 0, LV_ANIM_OFF);
    sync_page();
}

/* ================================================================= painel */

static lv_timer_t *panel_timer;
static lv_obj_t *clk_lbl, *ampm_lbl, *date_lbl, *bat_icon, *bat_lbl, *song_lbl, *artist_lbl, *bar, *play_lbl;
static lv_obj_t *pos_lbl, *len_lbl, *thumb_img, *thumb_note;
static lv_image_dsc_t thumb;        /* capa reduzida (jpeg_fit) */
static int thumb_for = -2;          /* faixa cuja capa está no cartão */

static void set_text_if(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void fmt_ms(char *out, size_t n, int ms)
{
    int sec = ms < 0 ? 0 : ms / 1000;
    if(sec >= 3600) snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

static void panel_tick(lv_timer_t *t)
{
    (void)t;
    char b[96];
    time_t n = time(NULL);
    struct tm tm;
    localtime_r(&n, &tm);
    bool ok = clock_valid();
    bool h24 = settings()->clock_24h;
    if(!ok) snprintf(b, sizeof b, "--:--");
    else snprintf(b, sizeof b, "%02d:%02d", h24 ? tm.tm_hour : (tm.tm_hour + 11) % 12 + 1, tm.tm_min);
    set_text_if(clk_lbl, b);
    set_text_if(ampm_lbl, ok && !h24 ? (tm.tm_hour < 12 ? "AM" : "PM") : "");
    if(ok) {
        static const char *const wd[LANG_COUNT][7] = {
            { "domingo", "segunda", "terça", "quarta", "quinta", "sexta", "sábado" },
            { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
            { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" } };
        bf_lang_t l = i18n_get_lang();
        if(l == LANG_EN) snprintf(b, sizeof b, "%s, %02d/%02d/%d", wd[l][tm.tm_wday], tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
        else snprintf(b, sizeof b, "%s, %02d/%02d/%d", wd[l][tm.tm_wday], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    }
    else snprintf(b, sizeof b, "%s", tr(S_DATE_SRC_UNSET));
    set_text_if(date_lbl, b);

    const bf_status_t *s = status_get();
    battery_set(bat_icon, s->battery_pct, s->battery_state);
    const char *bst = battery_state_text(s->battery_state);
    if(s->battery_pct >= 0) snprintf(b, sizeof b, "%d%%%s%s", s->battery_pct, bst[0] ? "  " : "", bst);
    else snprintf(b, sizeof b, "—%s%s", bst[0] ? "  " : "", bst);
    set_text_if(bat_lbl, b);

    /* tocando agora: capa, título (tag ou nome do arquivo), artista — álbum, tempos */
    pl_state_t st = player_state();
    char title[160], sub[200];
    int idx = -1;
    if(st != PL_STOPPED && music_now(title, sizeof title, sub, sizeof sub, &idx)) {
        set_text_if(song_lbl, title);
        set_text_if(artist_lbl, sub);
        int len = player_len_ms(), pos = player_pos_ms();
        int v = len > 0 ? (int)((long long)pos * 1000 / len) : 0;
        if(lv_bar_get_value(bar) != v) lv_bar_set_value(bar, v, LV_ANIM_OFF);
        fmt_ms(b, sizeof b, pos);
        set_text_if(pos_lbl, b);
        if(len > 0) fmt_ms(b, sizeof b, len);
        set_text_if(len_lbl, len > 0 ? b : "--:--");
    }
    else {
        set_text_if(song_lbl, tr(music_has_queue() ? S_HOME_STOPPED : S_HOME_NOTHING));
        set_text_if(artist_lbl, tr(S_HOME_OPEN_HINT));
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        set_text_if(pos_lbl, "");
        set_text_if(len_lbl, "");
        idx = -1;
    }
    if(idx != thumb_for) {                  /* troca de faixa: capa nova */
        thumb_for = idx;
        const uint8_t *d;
        size_t n;
        const char *mime;
        lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);   /* antes de liberar o buffer */
        jpeg_free(&thumb);
        bool ok = idx >= 0 && player_cover(&d, &n, &mime) && jpeg_fit(d, n, 60, 60, &thumb);
        if(ok) {
            lv_image_set_src(thumb_img, &thumb);
            lv_obj_invalidate(thumb_img);
            lv_obj_remove_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(thumb_note, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(thumb_note, LV_OBJ_FLAG_HIDDEN);
        }
    }
    set_text_if(play_lbl, st == PL_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void media_click(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    if(!music_has_queue()) { nav_go(SCR_MUSIC, 0); return; }   /* a fila nasce no Multimídia */
    music_command(cmd);
    panel_tick(NULL);
}

static void open_music(lv_event_t *e) { (void)e; nav_go(SCR_MUSIC, 0); }

static lv_obj_t *panel_card(lv_obj_t *parent, int w, int h)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_card(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, p->dark ? LV_OPA_70 : LV_OPA_80, 0);
    lv_obj_set_style_border_color(c, p->dark ? lv_color_white() : p->border, 0);
    lv_obj_set_style_border_opa(c, p->dark ? LV_OPA_10 : LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, BF_RADIUS_L, 0);
    return c;
}

static lv_obj_t *media_btn(lv_obj_t *parent, const char *sym, int cmd, bool primary)
{
    lv_obj_t *b = bf_button(parent, sym, NULL, primary ? 1 : 0);
    lv_obj_set_size(b, primary ? 80 : 68, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_add_event_cb(b, media_click, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return lv_obj_get_child(b, 0);
}

static void shortcut(lv_obj_t *parent, int i)
{
    const bf_palette_t *p = pal();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 118, 112);
    lv_obj_set_style_radius(b, BF_RADIUS_L, 0);
    lv_obj_set_style_bg_color(b, p->surface, 0);
    lv_obj_set_style_bg_opa(b, p->dark ? LV_OPA_70 : LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, p->dark ? lv_color_white() : p->border, 0);
    lv_obj_set_style_border_opa(b, p->dark ? LV_OPA_10 : LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 6, 0);
    lv_obj_add_event_cb(b, app_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_color_t c = lv_color_hex(apps[i].color);
    lv_obj_t *sq = lv_obj_create(b);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 56, 56);
    lv_obj_set_style_radius(sq, 16, 0);
    lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *img = lv_image_create(sq);
    lv_image_set_src(img, apps[i].icon);
    lv_image_set_scale(img, 180);             /* 56 px -> ~40 px */
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    lv_obj_t *l = bf_label(b, tr(apps[i].label), BF_FONT_S, p->text);
    bf_label_ellipsis(l, 110);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void create_panel(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    lv_obj_t *root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, BF_SP_L, 0);
    lv_obj_set_style_pad_row(root, BF_SP_M, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *top = bf_plain(root);
    lv_obj_set_size(top, LV_PCT(100), 200);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, BF_SP_M, 0);

    /* relógio */
    lv_obj_t *c = panel_card(top, 396, 200);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_t *row = bf_plain(c);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    clk_lbl = bf_label(row, "", BF_FONT_CLOCK, p->text);
    ampm_lbl = bf_label(row, "", BF_FONT_M, p->text_dim);
    lv_obj_set_style_pad_bottom(ampm_lbl, 14, 0);
    date_lbl = bf_label(c, "", BF_FONT_M, p->text);
    bf_label_ellipsis(date_lbl, LV_PCT(100));
    lv_obj_t *brow = bf_plain(c);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brow, BF_SP_S, 0);
    bat_icon = battery_create(brow, BF_FONT_M);
    bat_lbl = bf_label(brow, "", BF_FONT_M, p->text);

    /* tocando agora */
    c = panel_card(top, 360, 200);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, open_music, LV_EVENT_CLICKED, NULL);
    lv_obj_t *head = bf_plain(c);
    lv_obj_set_size(head, LV_PCT(100), 62);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(head, BF_SP_S, 0);
    lv_obj_t *cv = lv_obj_create(head);             /* capa (ou nota musical) */
    lv_obj_remove_style_all(cv);
    lv_obj_set_size(cv, 60, 60);
    lv_obj_set_style_radius(cv, BF_RADIUS_S, 0);
    lv_obj_set_style_clip_corner(cv, true, 0);
    lv_obj_set_style_bg_color(cv, lv_color_hex(0xf07a72), 0);
    lv_obj_set_style_bg_grad_color(cv, lv_color_hex(0x8a2c26), 0);
    lv_obj_set_style_bg_grad_dir(cv, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cv, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    thumb_note = bf_label(cv, LV_SYMBOL_AUDIO, BF_FONT_M, lv_color_white());
    lv_obj_center(thumb_note);
    thumb_img = lv_image_create(cv);
    lv_obj_center(thumb_img);
    lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
    thumb_for = -2;
    lv_obj_t *tc = bf_plain(head);
    lv_obj_set_height(tc, LV_PCT(100));
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tc, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    bf_label(tc, tr(S_HOME_NOW_PLAYING), BF_FONT_S, p->text_dim);
    song_lbl = bf_label(tc, "", BF_FONT_M, p->text);
    bf_label_ellipsis(song_lbl, 256);
    artist_lbl = bf_label(tc, "", BF_FONT_S, p->text_dim);
    bf_label_ellipsis(artist_lbl, 256);
    bar = lv_bar_create(c);
    lv_obj_set_size(bar, LV_PCT(100), 6);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_bg_color(bar, p->text_dim, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, p->accent, LV_PART_INDICATOR);
    lv_obj_t *tr_ = bf_plain(c);                   /* decorrido · total */
    lv_obj_set_size(tr_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(tr_, -4, 0);
    pos_lbl = bf_label(tr_, "", BF_FONT_S, p->text_dim);
    lv_obj_align(pos_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    len_lbl = bf_label(tr_, "", BF_FONT_S, p->text_dim);
    lv_obj_align(len_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *ctl = bf_plain(c);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    media_btn(ctl, LV_SYMBOL_PREV, MUSIC_PREV, false);
    play_lbl = media_btn(ctl, LV_SYMBOL_PLAY, MUSIC_TOGGLE, true);
    media_btn(ctl, LV_SYMBOL_NEXT, MUSIC_NEXT, false);

    /* atalhos */
    lv_obj_t *sc = bf_plain(root);
    lv_obj_set_size(sc, LV_PCT(100), 112);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int n = 0;
    for(int i = 0; i < N_APPS && n < 6; i++)
        if(settings()->home_apps >> i & 1u) { shortcut(sc, i); n++; }
    if(!n) bf_label(sc, tr(S_HOME_NO_SHORTCUTS), BF_FONT_S, p->text_dim);
    else if(n < 6) {                  /* poucos atalhos: juntos à esquerda */
        lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(sc, 10, 0);
    }
    fx_slide_in(sc, 0, 24, 60, 220);

    panel_tick(NULL);
    panel_timer = lv_timer_create(panel_tick, 500, NULL);
}

/* =================================================================== tela */

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    tv = NULL;
    int m = settings()->home_mode;
    if(m == BF_HOME_PANEL) create_panel(parent);
    else create_grid(parent, m == BF_HOME_KIOSK);
}

static void destroy(void)
{
    if(panel_timer) { lv_timer_delete(panel_timer); panel_timer = NULL; }
    tv = NULL;
    clk_lbl = ampm_lbl = date_lbl = bat_icon = bat_lbl = song_lbl = artist_lbl = bar = play_lbl = NULL;
    pos_lbl = len_lbl = thumb_img = thumb_note = NULL;
    jpeg_free(&thumb);
    thumb_for = -2;
}

const bf_screen_def_t scr_home_def = { create, destroy, S_HOME, NULL };
