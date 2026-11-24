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

static void build_pads(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(body);
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
        lv_obj_set_style_pad_column(card, BF_SP_M, 0);
        gp_badge(card, 64, C_SCC);
        lv_obj_t *col = bf_plain(card);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_t *nm = bf_label(col, g.name, BF_FONT_M, p->text);
        bf_label_ellipsis(nm, 380);
        char b[200];
        snprintf(b, sizeof b, "%s %s", g.bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_USB,
                 g.bt ? (g.addr[0] ? g.addr : "Bluetooth") : "USB");
        bf_label(col, b, BF_FONT_S, p->text_dim);
        snprintf(b, sizeof b, "%s: %s  ·  %s", tr(S_SCC_MAP), g.map_name, from_text(g.map_from));
        lv_obj_t *ml = wrap_label(col, b, BF_FONT_S, g.map_from == GPM_DEFAULT ? p->warning : lv_color_hex(0x9f86ff));
        (void)ml;
        lv_obj_t *gl = bf_label(col, g.guid, BF_FONT_MONO, p->text_dim);
        lv_obj_set_style_text_opa(gl, LV_OPA_70, 0);
        lv_obj_t *bc = bf_plain(card);
        lv_obj_set_size(bc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(bc, BF_SP_S, 0);
        lv_obj_t *bt = bf_button(bc, LV_SYMBOL_EYE_OPEN, tr(S_SCC_TAB_TEST), 1);
        lv_obj_set_size(bt, 170, 48);
        lv_obj_add_event_cb(bt, test_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *br = bf_button(bc, LV_SYMBOL_EDIT, tr(S_SCC_TAB_REG), 0);
        lv_obj_set_size(br, 170, 48);
        lv_obj_add_event_cb(br, reg_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if(!shown) empty_state(pg);
    int db, user;
    db_counts(&db, &user);
    char b[200];
    snprintf(b, sizeof b, tr(S_SCC_DB), db, user);
    lv_obj_t *n = wrap_label(pg, b, BF_FONT_S, p->text_dim);
    lv_obj_set_style_pad_top(n, BF_SP_S, 0);
}

/* ============================================================== Testar */

typedef struct { lv_obj_t *o; int ctl; uint32_t on; bool was; } part_t;
static part_t parts[PC__NBTN];
static int n_parts;
static lv_obj_t *stick[2], *dot[2], *trig[2], *trig_fill[2], *raw_lbl, *cnt_lbl;
static char raw_txt[96];
static uint32_t b_since;                /* B segurado desde (sair do testador) */
static int16_t last_ax[PC__NAXIS] = { 9999 };

static lv_obj_t *blob(lv_obj_t *par, int x, int y, int w, int h, int r)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *part(lv_obj_t *par, int ctl, int x, int y, int w, int h, int r, uint32_t on, const char *txt)
{
    const bf_palette_t *p = pal();
    lv_obj_t *o = blob(par, x, y, w, h, r);
    lv_obj_set_style_bg_color(o, p->surface, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(on), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_60, 0);
    if(txt) {
        lv_obj_t *l = bf_label(o, txt, BF_FONT_S, lv_color_hex(on));
        lv_obj_center(l);
    }
    if(n_parts < PC__NBTN) parts[n_parts++] = (part_t){ o, ctl, on, false };
    return o;
}

/* as formas do PlayStation desenhadas com linhas (a fonte não tem △ ○ ✕ □) */
static void shape(lv_obj_t *o, int kind, uint32_t col)
{
    lv_color_t c = lv_color_hex(col);
    if(kind == 1) {                                  /* bola */
        lv_obj_t *r = blob(o, 0, 0, 16, 16, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_border_color(r, c, 0);
        lv_obj_center(r);
        return;
    }
    if(kind == 3) {                                  /* quadrado */
        lv_obj_t *r = blob(o, 0, 0, 15, 15, 1);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_border_color(r, c, 0);
        lv_obj_center(r);
        return;
    }
    static lv_point_precise_t cross1[2] = { { 0, 0 }, { 14, 14 } }, cross2[2] = { { 14, 0 }, { 0, 14 } };
    static lv_point_precise_t tri[4] = { { 8, 0 }, { 16, 14 }, { 0, 14 }, { 8, 0 } };
    for(int k = 0; k < (kind == 0 ? 2 : 1); k++) {
        lv_obj_t *l = lv_line_create(o);
        lv_line_set_points(l, kind == 2 ? tri : k ? cross2 : cross1, kind == 2 ? 4 : 2);
        lv_obj_set_style_line_width(l, 2, 0);
        lv_obj_set_style_line_color(l, c, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_center(l);
    }
}

static void test_obs(int slot, uint16_t type, uint16_t code, int32_t value)
{
    if(slot != sel) return;
    const pm_bits_t *b = gamepad_bits(slot);
    char src[16] = "";
    if(b && type == EV_KEY && pm_button_index(b, code) >= 0) snprintf(src, sizeof src, "b%d", pm_button_index(b, code));
    else if(b && type == EV_ABS && pm_hat_index(b, code) >= 0) snprintf(src, sizeof src, "h%d", pm_hat_index(b, code));
    else if(b && type == EV_ABS && pm_axis_index(b, code) >= 0) snprintf(src, sizeof src, "a%d", pm_axis_index(b, code));
    snprintf(raw_txt, sizeof raw_txt, "%s 0x%03x (%s) = %d", type == EV_KEY ? "EV_KEY" : "EV_ABS", code, src, (int)value);
}

static void build_test(void)
{
    const bf_palette_t *p = pal();
    int s = first_pad();
    if(s < 0) { lv_obj_t *pg = page_create(body); empty_state(pg); return; }
    gp_info_t g;
    gamepad_info(s, &g);
    bool ps = sony(&g);
    n_parts = 0;
    memset(last_ax, 0x7f, sizeof last_ax);
    raw_txt[0] = 0;
    b_since = 0;
    gamepad_capture(true, test_obs);

    lv_obj_t *pad = bf_plain(body);
    lv_obj_set_size(pad, 640, 236);
    lv_obj_align(pad, LV_ALIGN_TOP_MID, 0, -6);
    lv_color_t shell = lv_color_mix(p->surface2, p->text_dim, 220);
    /* silhueta: corpo e as duas pegadas */
    for(int k = 0; k < 2; k++) {
        lv_obj_t *gr = blob(pad, k ? 418 : 102, 92, 120, 136, 60);
        lv_obj_set_style_bg_color(gr, shell, 0);
        lv_obj_set_style_transform_pivot_x(gr, 60, 0);
        lv_obj_set_style_transform_pivot_y(gr, 68, 0);
        lv_obj_set_style_transform_rotation(gr, k ? -200 : 200, 0);
    }
    lv_obj_t *bd = blob(pad, 82, 34, 476, 150, 74);
    lv_obj_set_style_bg_color(bd, shell, 0);
    lv_obj_set_style_shadow_width(bd, 24, 0);
    lv_obj_set_style_shadow_color(bd, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(bd, LV_OPA_30, 0);

    uint32_t acc = 0x9f86ff;
    /* ombros */
    part(pad, PC_LSHOULDER, 120, 4, 112, 26, 13, acc, ps ? "L1" : "LB");
    part(pad, PC_RSHOULDER, 408, 4, 112, 26, 13, acc, ps ? "R1" : "RB");
    /* gatilhos: barras que enchem */
    for(int k = 0; k < 2; k++) {
        trig[k] = blob(pad, k ? 600 : 18, 40, 22, 130, 11);
        lv_obj_set_style_bg_color(trig[k], p->surface, 0);
        lv_obj_set_style_border_width(trig[k], 2, 0);
        lv_obj_set_style_border_color(trig[k], lv_color_hex(acc), 0);
        lv_obj_set_style_border_opa(trig[k], LV_OPA_60, 0);
        lv_obj_set_style_clip_corner(trig[k], true, 0);
        trig_fill[k] = blob(trig[k], 0, 0, 22, 0, 0);
        lv_obj_set_style_bg_color(trig_fill[k], lv_color_hex(acc), 0);
        lv_obj_align(trig_fill[k], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t *tl = bf_label(pad, ps ? (k ? "R2" : "L2") : (k ? "RT" : "LT"), BF_FONT_S, p->text_dim);
        lv_obj_set_pos(tl, k ? 600 : 18, 174);
    }
    /* direcional */
    static const int dp[4][2] = { { 0, -30 }, { 0, 30 }, { -30, 0 }, { 30, 0 } };
    static const char *const arrows[4] = { LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    for(int d = 0; d < 4; d++) part(pad, PC_DPUP + d, 170 + dp[d][0] - 14, 92 + dp[d][1] - 14, 28, 28, 6, 0xdfe3ea, arrows[d]);
    /* botões de frente: Y em cima, A embaixo, X à esquerda, B à direita */
    static const int fb[4][3] = { { PC_Y, 0, -32 }, { PC_A, 0, 32 }, { PC_X, -32, 0 }, { PC_B, 32, 0 } };
    static const uint32_t pscol[4] = { 0x40e2a0, 0x7cb2e8, 0xff69f8, 0xff6666 };   /* △ ✕ □ ○ */
    static const uint32_t xcol[4] = { 0xf4c430, 0x6fcf4a, 0x4aa3ff, 0xff5a52 };    /* Y A X B */
    static const char *const xl[4] = { "Y", "A", "X", "B" };
    static const int pskind[4] = { 2, 0, 3, 1 };
    for(int k = 0; k < 4; k++) {
        lv_obj_t *o = part(pad, fb[k][0], 470 + fb[k][1] - 17, 92 + fb[k][2] - 17, 34, 34, LV_RADIUS_CIRCLE,
                           ps ? pscol[k] : xcol[k], ps ? NULL : xl[k]);
        if(ps) shape(o, pskind[k], pscol[k]);
    }
    /* centro: Back, Guia, Start (e o touchpad no PlayStation) */
    if(ps) part(pad, PC_TOUCHPAD, 262, 44, 116, 52, 10, 0xdfe3ea, NULL);
    part(pad, PC_BACK, ps ? 226 : 262, ps ? 48 : 70, 30, 18, 9, 0xdfe3ea, NULL);
    part(pad, PC_START, ps ? 384 : 348, ps ? 48 : 70, 30, 18, 9, 0xdfe3ea, NULL);
    part(pad, PC_GUIDE, 302, ps ? 104 : 100, 36, 36, LV_RADIUS_CIRCLE, 0xdfe3ea, ps ? "PS" : NULL);
    /* analógicos: anel e o ponto que anda; o anel acende ao afundar (L3/R3) */
    for(int k = 0; k < 2; k++) {
        stick[k] = part(pad, k ? PC_RSTICK : PC_LSTICK, (k ? 400 : 240) - 38, 152 - 38, 76, 76, LV_RADIUS_CIRCLE,
                        acc, NULL);
        dot[k] = blob(stick[k], 0, 0, 30, 30, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(dot[k], lv_color_hex(0xdfe3ea), 0);
        lv_obj_set_style_shadow_width(dot[k], 10, 0);
        lv_obj_set_style_shadow_opa(dot[k], LV_OPA_50, 0);
        lv_obj_center(dot[k]);
    }

    lv_obj_t *info = bf_plain(body);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    raw_lbl = bf_label(info, "", BF_FONT_MONO, p->text);
    cnt_lbl = bf_label(info, tr(S_SCC_TEST_HINT), BF_FONT_S, p->text_dim);
    lv_obj_set_width(cnt_lbl, LV_PCT(100));
    lv_label_set_long_mode(cnt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(cnt_lbl, LV_TEXT_ALIGN_CENTER, 0);
}

/* o que está desenhado dentro da peça (rótulo, forma) contrasta com o fundo aceso */
static void part_ink(lv_obj_t *o, bool on, uint32_t col)
{
    lv_color_t c = lv_color_hex(col);
    bool light = (((col >> 16) & 0xff) * 3 + ((col >> 8) & 0xff) * 6 + (col & 0xff)) / 10 > 170;
    lv_color_t ink = on ? (light ? lv_color_hex(0x1a1d26) : lv_color_white()) : c;
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *ch = lv_obj_get_child(o, (int32_t)i);
        if(ch == dot[0] || ch == dot[1]) continue;
        if(lv_obj_check_type(ch, &lv_line_class)) lv_obj_set_style_line_color(ch, ink, 0);
        else if(lv_obj_check_type(ch, &lv_label_class)) lv_obj_set_style_text_color(ch, ink, 0);
        else lv_obj_set_style_border_color(ch, ink, 0);
    }
}

static void tick_test(void)
{
    gp_info_t g;
    if(!gamepad_info(sel, &g)) { show_tab(TAB_TEST); return; }   /* saiu: mostra como parear */
    const bf_palette_t *p = pal();
    for(int i = 0; i < n_parts; i++) {
        bool on = g.btn[parts[i].ctl];
        if(on == parts[i].was) continue;
        parts[i].was = on;
        lv_obj_set_style_bg_color(parts[i].o, on ? lv_color_hex(parts[i].on) : p->surface, 0);
        lv_obj_set_style_border_opa(parts[i].o, on ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_set_style_shadow_width(parts[i].o, on ? 16 : 0, 0);
        lv_obj_set_style_shadow_color(parts[i].o, lv_color_hex(parts[i].on), 0);
        lv_obj_set_style_shadow_opa(parts[i].o, LV_OPA_70, 0);
        part_ink(parts[i].o, on, parts[i].on);
    }
    for(int k = 0; k < 2; k++) {
        int16_t x = g.ax[(k ? PC_RIGHTX : PC_LEFTX) - PC__NBTN], y = g.ax[(k ? PC_RIGHTY : PC_LEFTY) - PC__NBTN];
        int ix = (k ? PC_RIGHTX : PC_LEFTX) - PC__NBTN, iy = ix + 1;
        if(x != last_ax[ix] || y != last_ax[iy]) {
            last_ax[ix] = x;
            last_ax[iy] = y;
            lv_obj_align(dot[k], LV_ALIGN_CENTER, x * 23 / 1000, y * 23 / 1000);
        }
        int it = (k ? PC_RTRIGGER : PC_LTRIGGER) - PC__NBTN;
        if(g.ax[it] != last_ax[it]) {
            last_ax[it] = g.ax[it];
            lv_obj_set_height(trig_fill[k], 126 * g.ax[it] / 1000);
            lv_obj_align(trig_fill[k], LV_ALIGN_BOTTOM_MID, 0, 0);
        }
    }
    char c[200], ev[48];
    snprintf(ev, sizeof ev, tr(S_SCC_EVENTS), (unsigned)g.events);
    snprintf(c, sizeof c, "%s  ·  %s", raw_txt[0] ? raw_txt : "-", ev);
    if(strcmp(lv_label_get_text(raw_lbl), c)) lv_label_set_text(raw_lbl, c);
    /* B segurado 2 s: sai do testador */
    uint32_t now = lv_tick_get();
    if(!g.btn[PC_B]) b_since = 0;
    else if(!b_since) b_since = now ? now : 1;
    else if(now - b_since > 2000) show_tab(TAB_PADS);
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

static void reg_show(void)
{
    const bf_palette_t *p = pal();
    lv_obj_clean(body);
    reg_prompt = reg_status = reg_bar = reg_count = NULL;
    lv_obj_t *pg = page_create(body);
    gp_info_t g;
    if(!gamepad_info(sel, &g)) { empty_state(pg); return; }
    lv_obj_t *card = bf_card(pg);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    lv_obj_t *row = bf_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *nm = bf_label(row, g.name, BF_FONT_M, p->text);
    bf_label_ellipsis(nm, 520);
    if(step < 0) {                                   /* introdução */
        wrap_label(card, tr(S_SCC_REG_INTRO), BF_FONT_S, p->text_dim);
        lv_obj_t *br = bf_plain(card);
        lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(br, BF_SP_S, 0);
        lv_obj_t *b = bf_button(br, LV_SYMBOL_PLAY, tr(S_SCC_REG_START), 1);
        lv_obj_set_size(b, 220, 52);
        lv_obj_add_event_cb(b, reg_begin, LV_EVENT_CLICKED, NULL);
        if(g.map_from == GPM_USER) {
            lv_obj_t *f = bf_button(br, LV_SYMBOL_TRASH, tr(S_SCC_REG_FORGET), 2);
            lv_obj_set_size(f, 260, 52);
            lv_obj_add_event_cb(f, reg_forget, LV_EVENT_CLICKED, NULL);
        }
        gamepad_capture(false, NULL);
        return;
    }
    gamepad_capture(true, reg_obs);
    char t[80];
    if(step >= N_STEPS) {                            /* fim */
        int n = 0;
        for(int c = 0; c < PC__N; c++) n += reg.src[c].kind != PS_NONE;
        snprintf(t, sizeof t, tr(S_SCC_REG_DONE), n);
        bf_label(card, LV_SYMBOL_OK, BF_FONT_L, p->success);
        bf_label(card, t, BF_FONT_L, p->text);
        lv_obj_t *br = bf_plain(card);
        lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(br, BF_SP_S, 0);
        lv_obj_t *bk = bf_button(br, LV_SYMBOL_LEFT, tr(S_SCC_REG_BACK), 0);
        lv_obj_set_size(bk, 180, 52);
        lv_obj_add_event_cb(bk, reg_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *sv = bf_button(br, LV_SYMBOL_SAVE, tr(S_SCC_REG_SAVE), 1);
        lv_obj_set_size(sv, 220, 52);
        lv_obj_add_event_cb(sv, reg_save, LV_EVENT_CLICKED, NULL);
        reg_status = bf_label(card, "", BF_FONT_S, p->warning);
        return;
    }
    snprintf(t, sizeof t, tr(S_SCC_REG_STEP), step + 1, N_STEPS);
    reg_count = bf_label(row, t, BF_FONT_S, p->text_dim);
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
    reg_prompt = bf_label(card, tr(steps[step].prompt), BF_FONT_L, p->text);
    lv_obj_set_style_pad_ver(reg_prompt, BF_SP_S, 0);
    reg_status = bf_label(card, waiting_rest ? tr(S_SCC_REG_RELEASE) : "", BF_FONT_S, p->text_dim);
    lv_obj_t *br = bf_plain(card);
    lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(br, BF_SP_S, 0);
    lv_obj_t *bk = bf_button(br, LV_SYMBOL_LEFT, tr(S_SCC_REG_BACK), 0);
    lv_obj_set_size(bk, 180, 52);
    lv_obj_add_event_cb(bk, reg_back, LV_EVENT_CLICKED, NULL);
    if(!step) lv_obj_add_state(bk, LV_STATE_DISABLED);
    lv_obj_t *sk = bf_button(br, LV_SYMBOL_NEXT, tr(S_SCC_REG_SKIP), 0);
    lv_obj_set_size(sk, 180, 52);
    lv_obj_add_event_cb(sk, reg_skip, LV_EVENT_CLICKED, NULL);
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
        lv_obj_set_style_bg_color(tabs[i], on ? lv_color_hex(C_SCC) : p->surface, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tabs[i], 0), on ? lv_color_white() : p->text, 0);
    }
    lv_obj_clean(body);
    raw_lbl = cnt_lbl = NULL;
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
    lv_obj_t *bar = bf_plain(root);
    lv_obj_set_size(bar, LV_PCT(100), 56);
    lv_obj_set_style_pad_top(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, BF_SP_S, 0);
    static const bf_str_t names[3] = { S_SCC_TAB_PADS, S_SCC_TAB_TEST, S_SCC_TAB_REG };
    static const char *const sym[3] = { LV_SYMBOL_LIST, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EDIT };
    for(int i = 0; i < 3; i++) {
        tabs[i] = bf_button(bar, NULL, "", 0);
        lv_obj_set_size(tabs[i], 220, 44);
        lv_obj_set_style_radius(tabs[i], 22, 0);
        lv_obj_t *l = lv_obj_get_child(tabs[i], 0);
        char t[64];
        snprintf(t, sizeof t, "%s  %s", sym[i], tr(names[i]));
        lv_label_set_text(l, t);
        lv_obj_add_event_cb(tabs[i], tab_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    (void)p;
    body = bf_plain(root);
    lv_obj_set_size(body, LV_PCT(100), BF_CONTENT_H - 60);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 58);
    sel = 0;
    tmr = lv_timer_create(tick, 400, NULL);
    show_tab(TAB_PADS);
}

static void destroy(void)
{
    gamepad_capture(false, NULL);
    if(tmr) { lv_timer_delete(tmr); tmr = NULL; }
    root = body = raw_lbl = cnt_lbl = NULL;
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
