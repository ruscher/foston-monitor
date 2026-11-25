/* scr_scc.c — SC Controller (app do grupo Jogos, pacote sc-controller.bfapp): controles
 * conectados e o mapa de cada um, testador ao vivo e registro de controle (ensinar cada
 * botão, como o "Device registration" do sc-controller). O mapa e o banco de controles são
 * os do SDL GameControllerDB (core/padmap.c); o banco vem no pacote do app.
 *
 * No testador e no registro o controle não mexe na interface (gamepad_capture). Para sair
 * do testador pelo controle: segurar B por 2 s.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/gamepad.h"
#include "../core/padmap.h"
#include "../core/osd.h"
#include "../core/settings.h"
#include "../widgets/page.h"
#include "../widgets/padview.h"
#include "../assets/icons/bf_icons.h"
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define C_SCC 0x7a4cd9
enum { TAB_PADS, TAB_TEST, TAB_REG };

static lv_obj_t *root, *tabs[3], *body;
static lv_timer_t *tmr;
static int tab, sel;                    /* aba e controle escolhidos */
static char pads_sig[200];              /* o que a aba Controles mostra (refaz se mudar) */

static void show_tab(int t);

/* ============================================================== comuns */

static lv_obj_t *gp_badge(lv_obj_t *parent, int size, uint32_t color)
{
    lv_color_t c = lv_color_hex(color);
    lv_obj_t *sq = lv_obj_create(parent);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, size, size);
    lv_obj_set_style_radius(sq, size * 22 / 80, 0);
    lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = lv_image_create(sq);
    lv_image_set_src(img, &bf_icon_games);
    lv_image_set_scale(img, (uint32_t)(256 * size * 7 / 10 / 56));
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    return sq;
}

static lv_obj_t *wrap_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = bf_label(p, t, f, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void open_bt(lv_event_t *e) { (void)e; nav_go(SCR_BLUETOOTH, 0); }

/* nenhum controle: como parear */
static void empty_state(lv_obj_t *p)
{
    const bf_palette_t *pl = pal();
    lv_obj_t *card = bf_card(p);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, BF_SP_M, 0);
    gp_badge(card, 64, 0x8a93a5);
    lv_obj_t *col = bf_plain(card);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    bf_label(col, tr(S_SCC_NONE), BF_FONT_M, pl->text);
    wrap_label(col, tr(S_SCC_NONE_BODY), BF_FONT_S, pl->text_dim);
    lv_obj_t *b = bf_button(card, LV_SYMBOL_BLUETOOTH, tr(S_SCC_OPEN_BT), 1);
    lv_obj_set_height(b, 52);
    lv_obj_add_event_cb(b, open_bt, LV_EVENT_CLICKED, NULL);
}

static bool sony(const gp_info_t *g) { return !strncmp(g->guid + 8, "4c05", 4); }

static int first_pad(void)
{
    gp_info_t g;
    if(gamepad_info(sel, &g)) return sel;
    for(int i = 0; i < GAMEPAD_MAX; i++) if(gamepad_info(i, &g)) return sel = i;
    return -1;
}

/* linhas do banco: modelos para Linux nos bancos dos apps e no do usuário */
static int count_db(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    char l[1024];
    int n = 0;
    while(fgets(l, sizeof l, f)) if(strstr(l, "platform:Linux")) n++;
    fclose(f);
    return n;
}

static void db_counts(int *db, int *user)
{
    char p[320];
    gamepad_user_db(p, sizeof p);
    *user = count_db(p);
    *db = 0;
    char apps[300];
    data_path(apps, sizeof apps, "apps");
    DIR *d = opendir(apps);
    if(!d) return;
    struct dirent *e;
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        if(snprintf(p, sizeof p, "%s/%s/gamecontrollerdb.txt", apps, e->d_name) < (int)sizeof p) *db += count_db(p);
    }
    closedir(d);
}

/* ============================================================== Controles */

static void test_click(lv_event_t *e) { sel = (int)(intptr_t)lv_event_get_user_data(e); show_tab(TAB_TEST); }
static void reg_click(lv_event_t *e) { sel = (int)(intptr_t)lv_event_get_user_data(e); show_tab(TAB_REG); }

static const char *from_text(int f)
{
    switch(f) {
    case GPM_USER: return tr(S_SCC_FROM_USER);
    case GPM_DB: return tr(S_SCC_FROM_DB);
    case GPM_BUILTIN: return tr(S_SCC_FROM_BUILTIN);
    default: return tr(S_SCC_FROM_DEFAULT);
    }
}

static void pads_signature(char *o, size_t n)
{
    size_t k = 0;
    o[0] = 0;
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        gp_info_t g;
        if(gamepad_info(i, &g) && k < n)
            k += (size_t)snprintf(o + k, n - k, "%d%s%d|", i, g.guid, g.map_from);
    }
}

/* bateria do controle (o hid-playstation a publica em /sys/class/power_supply) */
static bool pad_battery(const char *addr, int *pct, bool *charging)
{
    char low[18], path[160], b[32];
    if(!addr[0]) return false;
    for(int i = 0; i < 18; i++) low[i] = (char)(addr[i] >= 'A' && addr[i] <= 'F' ? addr[i] + 32 : addr[i]);
    snprintf(path, sizeof path, "/sys/class/power_supply/ps-controller-battery-%s/capacity", low);
    FILE *f = fopen(path, "r");
    if(!f) return false;
    bool ok = fgets(b, sizeof b, f) != NULL;
    fclose(f);
    if(!ok) return false;
    *pct = atoi(b);
    snprintf(path, sizeof path, "/sys/class/power_supply/ps-controller-battery-%s/status", low);
    *charging = false;
    if((f = fopen(path, "r"))) {
        if(fgets(b, sizeof b, f)) *charging = !strncmp(b, "Charging", 8) || !strncmp(b, "Full", 4);
        fclose(f);
    }
    return true;
}

/* selo pequeno arredondado: símbolo + texto */
static lv_obj_t *chip_w(lv_obj_t *parent, const char *sym, const char *txt, lv_color_t col, int maxw)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_plain(parent);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_mix(col, p->surface2, 40), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 4, 0);
    char t[120];
    snprintf(t, sizeof t, "%s%s%s", sym ? sym : "", sym ? "  " : "", txt);
    lv_obj_t *l = bf_label(c, t, BF_FONT_S, col);
    /* do tamanho do texto; só corta com "…" o que não cabe */
    if(lv_text_get_width(t, (uint32_t)strlen(t), BF_FONT_S, 0) > maxw) bf_label_ellipsis(l, maxw);
    return c;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *sym, const char *txt, lv_color_t col)
{
    return chip_w(parent, sym, txt, col, 330);
}

static const char *battery_sym(int pct)
{
    return pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 : pct > 30 ? LV_SYMBOL_BATTERY_2
         : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
}

static void build_pads(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(body);
    lv_obj_set_style_pad_top(pg, BF_SP_S, 0);
    pads_signature(pads_sig, sizeof pads_sig);
    int shown = 0;
    for(int i = 0; i < GAMEPAD_MAX; i++) {
        gp_info_t g;
        if(!gamepad_info(i, &g)) continue;
        shown++;
        lv_obj_t *card = bf_card(pg);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card, BF_SP_L, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_SCC), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
        padview_create(card, 160, sony(&g));
        lv_obj_t *col = bf_plain(card);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 6, 0);
        lv_obj_t *nm = bf_label(col, g.name, BF_FONT_M, p->text);
        bf_label_ellipsis(nm, 360);
        lv_obj_t *chips = bf_plain(col);
        lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(chips, 6, 0);
        chip(chips, g.bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_USB, g.bt ? (g.addr[0] ? g.addr : "Bluetooth") : "USB",
             lv_color_hex(0x7fb2ff));
        int pct;
        bool chg;
        if(pad_battery(g.addr, &pct, &chg)) {
            char b[24];
            snprintf(b, sizeof b, tr(S_SCC_BATTERY), pct);
            chip(chips, chg ? LV_SYMBOL_CHARGE : battery_sym(pct), b,
                 pct <= 15 && !chg ? p->danger : lv_color_hex(0x6fdc8c));
        }
        char m[160];
        snprintf(m, sizeof m, "%s  ·  %s", g.map_name, from_text(g.map_from));
        chip(chips, LV_SYMBOL_SHUFFLE, m, g.map_from == GPM_DEFAULT ? p->warning : lv_color_hex(0xb9a6ff));
        lv_obj_t *gl = bf_label(col, g.guid, BF_FONT_MONO, p->text_dim);
        lv_obj_set_style_text_opa(gl, LV_OPA_60, 0);
        lv_obj_t *bc = bf_plain(card);
        lv_obj_set_size(bc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(bc, BF_SP_S, 0);
        lv_obj_t *bt = bf_button(bc, LV_SYMBOL_EYE_OPEN, tr(S_SCC_TAB_TEST), 1);
        lv_obj_set_size(bt, 168, 46);
        lv_obj_add_event_cb(bt, test_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *br = bf_button(bc, LV_SYMBOL_EDIT, tr(S_SCC_TAB_REG), 0);
        lv_obj_set_size(br, 168, 46);
        lv_obj_add_event_cb(br, reg_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if(!shown) empty_state(pg);
    int db, user;
    db_counts(&db, &user);
    char b[200];
    snprintf(b, sizeof b, tr(S_SCC_DB_SHORT), db, user);
    lv_obj_t *info = bf_plain(pg);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(info, BF_SP_S, 0);
    chip_w(info, LV_SYMBOL_LIST, b, p->text_dim, 700);
}

/* ============================================================== Testar */

static lv_obj_t *pv, *raw_lbl, *cnt_lbl, *bat_lbl, *hold_bar, *st_val[2], *tr_bar[2];
static char raw_txt[96];
static uint32_t b_since;                /* B segurado desde (sair do testador) */
static int16_t last_ax[PC__NAXIS];

static void test_obs(int slot, uint16_t type, uint16_t code, int32_t value)
{
    if(slot != sel) return;
    const pm_bits_t *b = gamepad_bits(slot);
    char src[16] = "";
    if(b && type == EV_KEY && pm_button_index(b, code) >= 0) snprintf(src, sizeof src, "b%d", pm_button_index(b, code));
    else if(b && type == EV_ABS && pm_hat_index(b, code) >= 0) snprintf(src, sizeof src, "h%d", pm_hat_index(b, code));
    else if(b && type == EV_ABS && pm_axis_index(b, code) >= 0) snprintf(src, sizeof src, "a%d", pm_axis_index(b, code));
    snprintf(raw_txt, sizeof raw_txt, "%s 0x%03x %s = %d", type == EV_KEY ? "KEY" : "ABS", code, src, (int)value);
}

/* cartão lateral: uma linha "título ... valor" */
static lv_obj_t *side_row(lv_obj_t *parent, const char *title, lv_obj_t **val)
{
    const bf_palette_t *p = pal();
    lv_obj_t *r = bf_plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bf_label(r, title, BF_FONT_S, p->text_dim);
    if(val) *val = bf_label(r, "", BF_FONT_MONO, p->text);
    return r;
}

static lv_obj_t *mini_bar(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, 96, 10);
    lv_bar_set_range(b, 0, 1000);
    lv_obj_set_style_bg_color(b, p->surface2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x9f86ff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 5, LV_PART_INDICATOR);
    return b;
}

/* o maior controle que cabe na altura do conteúdo (e na largura dada) */
static int pad_width(int maxw)
{
    lv_obj_update_layout(body);
    int w = (lv_obj_get_height(body) - 16) * 400 / 250;
    return w > maxw ? maxw : w < 200 ? 200 : w;
}

static void build_test(void)
{
    const bf_palette_t *p = pal();
    int s = first_pad();
    if(s < 0) { lv_obj_t *pg = page_create(body); empty_state(pg); return; }
    gp_info_t g;
    gamepad_info(s, &g);
    raw_txt[0] = 0;
    b_since = 0;
    memset(last_ax, 0x7f, sizeof last_ax);
    gamepad_capture(true, test_obs);

    /* o controle à esquerda, centrado na altura que sobra */
    lv_obj_t *left = bf_plain(body);
    lv_obj_set_size(left, 470, LV_PCT(100));
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    pv = padview_create(left, pad_width(450), sony(&g));
    lv_obj_center(pv);

    /* leituras à direita */
    lv_obj_t *card = bf_card(body);
    lv_obj_set_size(card, 300, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_RIGHT_MID, -BF_SP_M, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_t *head = bf_plain(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *nm = bf_label(head, g.name, BF_FONT_M, p->text);
    bf_label_ellipsis(nm, 190);
    bat_lbl = bf_label(head, "", BF_FONT_S, lv_color_hex(0x6fdc8c));
    side_row(card, tr(S_SCC_LSTICK), &st_val[0]);
    side_row(card, tr(S_SCC_RSTICK), &st_val[1]);
    lv_obj_t *trow = side_row(card, tr(S_SCC_TRIGGERS), NULL);
    lv_obj_t *bars = bf_plain(trow);
    lv_obj_set_size(bars, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bars, 6, 0);
    tr_bar[0] = mini_bar(bars);
    tr_bar[1] = mini_bar(bars);
    lv_obj_set_size(tr_bar[0], 64, 10);
    lv_obj_set_size(tr_bar[1], 64, 10);
    lv_obj_t *sep = bf_plain(card);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, p->border, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    bf_label(card, tr(S_SCC_LAST), BF_FONT_S, p->text_dim);
    raw_lbl = bf_label(card, "-", BF_FONT_MONO, p->text);
    cnt_lbl = bf_label(card, "", BF_FONT_S, p->text_dim);
    lv_obj_t *hint = bf_label(card, tr(S_SCC_HOLD_B), BF_FONT_S, p->text_dim);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    hold_bar = mini_bar(card);
    lv_obj_set_size(hold_bar, LV_PCT(100), 6);
    lv_bar_set_range(hold_bar, 0, 2000);
    lv_obj_set_style_bg_color(hold_bar, lv_color_hex(0xff6666), LV_PART_INDICATOR);
}

static void tick_test(void)
{
    gp_info_t g;
    if(!gamepad_info(sel, &g)) { show_tab(TAB_TEST); return; }   /* saiu: mostra como parear */
    padview_state(pv, g.btn, g.ax);
    char b[96];
    for(int k = 0; k < 2; k++) {
        int ix = (k ? PC_RIGHTX : PC_LEFTX) - PC__NBTN;
        if(g.ax[ix] != last_ax[ix] || g.ax[ix + 1] != last_ax[ix + 1]) {
            last_ax[ix] = g.ax[ix];
            last_ax[ix + 1] = g.ax[ix + 1];
            snprintf(b, sizeof b, "%+5d %+5d", g.ax[ix] / 10, g.ax[ix + 1] / 10);
            lv_label_set_text(st_val[k], b);
        }
        int it = (k ? PC_RTRIGGER : PC_LTRIGGER) - PC__NBTN;
        if(g.ax[it] != last_ax[it]) {
            last_ax[it] = g.ax[it];
            lv_bar_set_value(tr_bar[k], g.ax[it], LV_ANIM_OFF);
        }
    }
    if(strcmp(lv_label_get_text(raw_lbl), raw_txt[0] ? raw_txt : "-")) lv_label_set_text(raw_lbl, raw_txt[0] ? raw_txt : "-");
    snprintf(b, sizeof b, tr(S_SCC_EVENTS), (unsigned)g.events);
    if(strcmp(lv_label_get_text(cnt_lbl), b)) lv_label_set_text(cnt_lbl, b);
    int pct;
    bool chg;
    b[0] = 0;
    if(pad_battery(g.addr, &pct, &chg)) {
        char n[16];
        snprintf(n, sizeof n, tr(S_SCC_BATTERY), pct);
        snprintf(b, sizeof b, "%s %s", chg ? LV_SYMBOL_CHARGE : battery_sym(pct), n);
    }
    if(strcmp(lv_label_get_text(bat_lbl), b)) lv_label_set_text(bat_lbl, b);
    /* B segurado 2 s: sai do testador (a barra mostra o tempo) */
    uint32_t now = lv_tick_get();
    if(!g.btn[PC_B]) b_since = 0;
    else if(!b_since) b_since = now ? now : 1;
    lv_bar_set_value(hold_bar, b_since ? (int32_t)(now - b_since) : 0, LV_ANIM_OFF);
    if(b_since && now - b_since > 2000) show_tab(TAB_PADS);
}

/* ============================================================== Registrar */

static const struct { int ctl; bf_str_t prompt; } steps[] = {
    { PC_A, S_SCC_P_A }, { PC_B, S_SCC_P_B }, { PC_X, S_SCC_P_X }, { PC_Y, S_SCC_P_Y },
    { PC_LSHOULDER, S_SCC_P_LB }, { PC_RSHOULDER, S_SCC_P_RB }, { PC_LTRIGGER, S_SCC_P_LT },
    { PC_RTRIGGER, S_SCC_P_RT }, { PC_BACK, S_SCC_P_BACK }, { PC_START, S_SCC_P_START },
    { PC_GUIDE, S_SCC_P_GUIDE }, { PC_LSTICK, S_SCC_P_LSTICK }, { PC_RSTICK, S_SCC_P_RSTICK },
    { PC_DPUP, S_SCC_P_UP }, { PC_DPDOWN, S_SCC_P_DOWN }, { PC_DPLEFT, S_SCC_P_LEFT },
    { PC_DPRIGHT, S_SCC_P_RIGHT }, { PC_LEFTX, S_SCC_P_LX }, { PC_LEFTY, S_SCC_P_LY },
    { PC_RIGHTX, S_SCC_P_RX }, { PC_RIGHTY, S_SCC_P_RY },
};
#define N_STEPS (int)(sizeof steps / sizeof steps[0])

static int step;                        /* -1 = introdução, N_STEPS = fim */
static padmap_t reg;
static int16_t base[GP_NABS];
static bool waiting_rest;               /* recebido: espera soltar tudo antes do próximo */
static int keys_down;
static int8_t hats[GP_NABS];
static lv_obj_t *reg_prompt, *reg_status, *reg_bar, *reg_count;

static bool is_axis_ctl(int c) { return c >= PC__NBTN; }

/* a mesma fonte já ensinada para outro controle? (um botão por controle) */
static int used_by(const pm_src_t *s, int except)
{
    for(int t = 0; t < PC__N; t++) {
        const pm_src_t *r = &reg.src[t];
        if(t != except && r->kind && r->kind == s->kind && r->idx == s->idx && r->mask == s->mask &&
           (r->kind != PS_AXIS || r->half == s->half || !r->half || !s->half))
            return t;
    }
    return -1;
}

static void reg_show(void);

static void reg_got(const pm_src_t *s)
{
    int c = steps[step].ctl;
    int other = used_by(s, c);
    if(other >= 0) {                                 /* já é de outro: avisa e espera outra coisa */
        char t[80];
        snprintf(t, sizeof t, LV_SYMBOL_WARNING " %s", padmap_ctl_name(other));
        lv_label_set_text(reg_status, t);
        lv_obj_set_style_text_color(reg_status, pal()->warning, 0);
        waiting_rest = true;
        return;
    }
    reg.src[c] = *s;
    step++;
    waiting_rest = true;
    reg_show();
}

static bool at_rest(void)
{
    if(keys_down > 0) return false;
    for(int c = 0; c < GP_NABS; c++) {
        if(hats[c]) return false;
        int d = gamepad_axis_now(sel, (uint16_t)c) - base[c];
        if(d > 350 || d < -350) return false;
    }
    return true;
}

static void reg_baseline(void)
{
    for(int c = 0; c < GP_NABS; c++) base[c] = (int16_t)gamepad_axis_now(sel, (uint16_t)c);
}

static void reg_obs(int slot, uint16_t type, uint16_t code, int32_t value)
{
    if(slot != sel || step < 0 || step >= N_STEPS) return;
    const pm_bits_t *b = gamepad_bits(slot);
    if(!b) return;
    if(type == EV_KEY) keys_down += value ? 1 : -1;
    if(keys_down < 0) keys_down = 0;
    int h = type == EV_ABS ? pm_hat_index(b, code) : -1;
    if(h >= 0 && code < GP_NABS) hats[code] = (int8_t)(value < 0 ? -1 : value > 0 ? 1 : 0);
    if(waiting_rest) {
        if(at_rest()) {
            waiting_rest = false;
            reg_baseline();
            lv_label_set_text(reg_status, "");
        }
        return;
    }
    int c = steps[step].ctl;
    pm_src_t s;
    memset(&s, 0, sizeof s);
    if(type == EV_KEY && value == 1) {
        int i = pm_button_index(b, code);
        if(i < 0) return;
        s.kind = PS_BUTTON;
        s.idx = (uint8_t)i;
    } else if(h >= 0 && value) {
        bool y = (code - ABS_HAT0X) & 1;
        s.kind = PS_HAT;
        s.idx = (uint8_t)h;
        s.mask = (uint8_t)(y ? (value < 0 ? 1 : 4) : (value < 0 ? 8 : 2));
    } else if(type == EV_ABS && h < 0) {
        int i = pm_axis_index(b, code);
        if(i < 0 || code >= GP_NABS) return;
        int n = gamepad_axis_norm(slot, code, value), d = n - base[code];
        if(d < 600 && d > -600) return;
        s.kind = PS_AXIS;
        s.idx = (uint8_t)i;
        if(c == PC_LTRIGGER || c == PC_RTRIGGER) {
            if(base[code] > -700) s.half = (int8_t)(d > 0 ? 1 : -1);   /* repousa no meio: meio eixo */
            else if(d < 0) s.invert = true;
        } else if(is_axis_ctl(c)) s.invert = d < 0;   /* pedimos direita/baixo: o sinal decide */
        else s.half = (int8_t)(d > 0 ? 1 : -1);       /* botão de eixo (direcional analógico) */
    } else return;
    reg_got(&s);
}

static void reg_skip(lv_event_t *e)
{
    (void)e;
    if(step < 0 || step >= N_STEPS) return;
    memset(&reg.src[steps[step].ctl], 0, sizeof reg.src[0]);
    step++;
    reg_show();
}

static void reg_back(lv_event_t *e)
{
    (void)e;
    if(step <= 0) return;
    step--;
    memset(&reg.src[steps[step].ctl], 0, sizeof reg.src[0]);
    waiting_rest = true;
    reg_show();
}

static void reg_begin(lv_event_t *e)
{
    (void)e;
    gp_info_t g;
    if(!gamepad_info(sel, &g)) return;
    memset(&reg, 0, sizeof reg);
    memcpy(reg.guid, g.guid, sizeof reg.guid);
    snprintf(reg.name, sizeof reg.name, "%s", g.name);
    step = 0;
    keys_down = 0;
    memset(hats, 0, sizeof hats);
    reg_baseline();                                  /* repouso = como o controle está agora */
    waiting_rest = true;
    reg_show();
}

/* grava a linha no banco do usuário (troca a anterior do mesmo controle) */
static bool user_db_put(const char *guid, const char *line)
{
    char path[320], tmp[330];
    gamepad_user_db(path, sizeof path);
    char dir[320];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if(sl) { *sl = 0; mkdir(dir, 0755); }
    snprintf(tmp, sizeof tmp, "%s.novo", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if(!out) { if(in) fclose(in); return false; }
    char l[1024];
    while(in && fgets(l, sizeof l, in))
        if(strncasecmp(l, guid, 32)) fputs(l, out);
    if(in) fclose(in);
    if(line) fprintf(out, "%s\n", line);
    bool ok = fclose(out) == 0 && rename(tmp, path) == 0;
    gamepad_remap();
    return ok;
}

static void reg_save(lv_event_t *e)
{
    (void)e;
    char line[1024];
    if(padmap_format(&reg, line, sizeof line) < 0 || !user_db_put(reg.guid, line)) {
        lv_label_set_text(reg_status, LV_SYMBOL_WARNING);
        return;
    }
    if(settings()->notify) osd_show(&bf_icon_games, C_SCC, tr(S_SCC_REG_SAVED), reg.name);
    show_tab(TAB_TEST);
}

static void reg_forget(lv_event_t *e)
{
    (void)e;
    gp_info_t g;
    if(!gamepad_info(sel, &g)) return;
    user_db_put(g.guid, NULL);
    show_tab(TAB_PADS);
}

static void reg_side_buttons(lv_obj_t *card, bool back, bool skip)
{
    lv_obj_t *br = bf_plain(card);
    lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(br, BF_SP_S, 0);
    if(back) {
        lv_obj_t *bk = bf_button(br, LV_SYMBOL_LEFT, tr(S_SCC_REG_BACK), 0);
        lv_obj_set_size(bk, 140, 48);
        lv_obj_add_event_cb(bk, reg_back, LV_EVENT_CLICKED, NULL);
        if(step <= 0) lv_obj_add_state(bk, LV_STATE_DISABLED);
    }
    if(skip) {
        lv_obj_t *sk = bf_button(br, LV_SYMBOL_NEXT, tr(S_SCC_REG_SKIP), 0);
        lv_obj_set_size(sk, 140, 48);
        lv_obj_add_event_cb(sk, reg_skip, LV_EVENT_CLICKED, NULL);
    }
}

static void reg_show(void)
{
    const bf_palette_t *p = pal();
    lv_obj_clean(body);
    pv = NULL;
    reg_prompt = reg_status = reg_bar = reg_count = NULL;
    gp_info_t g;
    if(!gamepad_info(sel, &g)) { lv_obj_t *pg = page_create(body); empty_state(pg); return; }

    /* o desenho com o botão da vez piscando e o que já foi ensinado em verde */
    lv_obj_t *left = bf_plain(body);
    lv_obj_set_size(left, 430, LV_PCT(100));
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    pv = padview_create(left, pad_width(410), sony(&g));
    lv_obj_center(pv);
    for(int k = 0; k < N_STEPS && step >= 0; k++)
        padview_done(pv, steps[k].ctl, k < step && reg.src[steps[k].ctl].kind != PS_NONE);
    if(step >= 0 && step < N_STEPS) padview_focus(pv, steps[step].ctl);

    lv_obj_t *card = bf_card(body);
    lv_obj_set_size(card, 340, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_RIGHT_MID, -BF_SP_M, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_SCC), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    char t[80];

    if(step < 0) {                                   /* introdução */
        gamepad_capture(false, NULL);
        bf_label(card, tr(S_SCC_REG_TITLE), BF_FONT_M, p->text);
        lv_obj_t *nm = bf_label(card, g.name, BF_FONT_S, lv_color_hex(0xb9a6ff));
        bf_label_ellipsis(nm, 300);
        wrap_label(card, tr(S_SCC_REG_INTRO), BF_FONT_S, p->text_dim);
        lv_obj_t *b = bf_button(card, LV_SYMBOL_PLAY, tr(S_SCC_REG_START), 1);
        lv_obj_set_size(b, LV_PCT(100), 50);
        lv_obj_add_event_cb(b, reg_begin, LV_EVENT_CLICKED, NULL);
        if(g.map_from == GPM_USER) {
            lv_obj_t *f = bf_button(card, LV_SYMBOL_TRASH, tr(S_SCC_REG_FORGET), 2);
            lv_obj_set_size(f, LV_PCT(100), 46);
            lv_obj_add_event_cb(f, reg_forget, LV_EVENT_CLICKED, NULL);
        }
        return;
    }
    gamepad_capture(true, reg_obs);
    if(step >= N_STEPS) {                            /* fim */
        int n = 0;
        for(int c = 0; c < PC__N; c++) n += reg.src[c].kind != PS_NONE;
        lv_obj_t *ok = bf_label(card, LV_SYMBOL_OK, BF_FONT_L, p->success);
        (void)ok;
        snprintf(t, sizeof t, tr(S_SCC_REG_DONE), n);
        wrap_label(card, t, BF_FONT_M, p->text);
        lv_obj_t *br = bf_plain(card);                    /* Voltar e Salvar na mesma linha */
        lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(br, BF_SP_S, 0);
        lv_obj_t *bk = bf_button(br, LV_SYMBOL_LEFT, tr(S_SCC_REG_BACK), 0);
        lv_obj_set_size(bk, 120, 50);
        lv_obj_add_event_cb(bk, reg_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *sv = bf_button(br, LV_SYMBOL_SAVE, tr(S_SCC_REG_SAVE), 1);
        lv_obj_set_height(sv, 50);
        lv_obj_set_flex_grow(sv, 1);
        lv_obj_add_event_cb(sv, reg_save, LV_EVENT_CLICKED, NULL);
        reg_status = bf_label(card, "", BF_FONT_S, p->warning);
        return;
    }
    lv_obj_t *row = bf_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bf_label(row, tr(S_SCC_REG_TITLE), BF_FONT_S, p->text_dim);
    snprintf(t, sizeof t, tr(S_SCC_REG_STEP), step + 1, N_STEPS);
    reg_count = bf_label(row, t, BF_FONT_S, lv_color_hex(0xb9a6ff));
    reg_bar = lv_bar_create(card);
    lv_obj_set_size(reg_bar, LV_PCT(100), 8);
    lv_bar_set_range(reg_bar, 0, N_STEPS);
    lv_bar_set_value(reg_bar, step, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(reg_bar, p->surface2, 0);
    lv_obj_set_style_bg_opa(reg_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reg_bar, 4, 0);
    lv_obj_set_style_bg_color(reg_bar, lv_color_hex(C_SCC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(reg_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(reg_bar, 4, LV_PART_INDICATOR);
    reg_prompt = wrap_label(card, tr(steps[step].prompt), BF_FONT_L, p->text);
    reg_status = bf_label(card, waiting_rest ? tr(S_SCC_REG_RELEASE) : "", BF_FONT_S, p->text_dim);
    lv_obj_t *h = wrap_label(card, tr(S_SCC_REG_HINT), BF_FONT_S, p->text_dim);
    lv_obj_set_style_text_opa(h, LV_OPA_80, 0);
    reg_side_buttons(card, true, true);
    /* já parado? (o observador só roda com eventos) */
    if(waiting_rest && at_rest()) {
        waiting_rest = false;
        reg_baseline();
        lv_label_set_text(reg_status, "");
    }
}

/* ============================================================== abas */

static void tab_click(lv_event_t *e) { show_tab((int)(intptr_t)lv_event_get_user_data(e)); }

static void show_tab(int t)
{
    const bf_palette_t *p = pal();
    tab = t;
    gamepad_capture(false, NULL);
    for(int i = 0; i < 3; i++) {
        bool on = i == t;
        lv_obj_set_style_bg_opa(tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tabs[i], 0), on ? lv_color_white() : p->text_dim, 0);
    }
    lv_obj_clean(body);
    pv = raw_lbl = cnt_lbl = bat_lbl = hold_bar = NULL;
    /* 30 quadros/s só no testador; nas outras abas basta notar um controle chegando */
    if(tmr) lv_timer_set_period(tmr, t == TAB_TEST ? 33 : 400);
    if(t == TAB_PADS) build_pads();
    else if(t == TAB_TEST) build_test();
    else { step = -1; reg_show(); }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if(tab == TAB_TEST && raw_lbl) tick_test();
    else if(tab == TAB_PADS) {
        char sig[200];
        pads_signature(sig, sizeof sig);
        if(strcmp(sig, pads_sig)) show_tab(TAB_PADS);        /* controle chegou ou saiu */
    } else if(tab == TAB_TEST) {
        gp_info_t g;
        if(first_pad() >= 0 && gamepad_info(sel, &g)) show_tab(TAB_TEST);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    /* abas em cima e o conteúdo no resto: a altura real (360, ou 432 no Foston Glass) */
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(root, 10, 0);
    lv_obj_set_style_pad_row(root, 8, 0);
    /* abas: controle segmentado em pílula, com folga em cima (nada encosta na barra) */
    lv_obj_t *seg = bf_plain(root);
    lv_obj_set_size(seg, 600, 46);
    lv_obj_set_style_bg_color(seg, p->surface, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_80, 0);
    lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_border_color(seg, p->border, 0);
    lv_obj_set_style_pad_all(seg, 4, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 4, 0);
    static const bf_str_t names[3] = { S_SCC_TAB_PADS, S_SCC_TAB_TEST, S_SCC_TAB_REG };
    static const char *const sym[3] = { LV_SYMBOL_LIST, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EDIT };
    for(int i = 0; i < 3; i++) {
        tabs[i] = lv_button_create(seg);
        lv_obj_remove_style_all(tabs[i]);
        lv_obj_set_height(tabs[i], LV_PCT(100));
        lv_obj_set_flex_grow(tabs[i], 1);
        lv_obj_set_style_radius(tabs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(C_SCC), 0);
        lv_obj_set_style_bg_grad_color(tabs[i], lv_color_hex(0x9b6ef3), 0);
        lv_obj_set_style_bg_grad_dir(tabs[i], LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_30, LV_STATE_PRESSED);
        char t[64];
        snprintf(t, sizeof t, "%s  %s", sym[i], tr(names[i]));
        lv_obj_t *l = bf_label(tabs[i], t, BF_FONT_M, p->text_dim);
        lv_obj_center(l);
        lv_obj_add_event_cb(tabs[i], tab_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    body = bf_plain(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_update_layout(root);
    sel = 0;
    tmr = lv_timer_create(tick, 400, NULL);
    show_tab(TAB_PADS);
}

static void destroy(void)
{
    gamepad_capture(false, NULL);
    if(tmr) { lv_timer_delete(tmr); tmr = NULL; }
    root = body = pv = raw_lbl = cnt_lbl = bat_lbl = hold_bar = NULL;
    reg_prompt = reg_status = reg_bar = reg_count = NULL;
}

/* testes (--screenshot): 0..2 abre a aba; 3 começa o registro, 4 salva, 5 esquece */
void scr_scc_demo_tab(int t)
{
    if(!root) return;
    if(t <= TAB_REG) show_tab(t);
    else if(t == 3) reg_begin(NULL);
    else if(t == 4) reg_save(NULL);
    else if(t == 5) reg_forget(NULL);
}

const bf_screen_def_t scr_scc_def = { create, destroy, S_SCC_TITLE, NULL };
