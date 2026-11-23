/* scr_bluetooth.c — Bluetooth: liga/desliga, visível, procurar, aparelhos (parear,
 * conectar, desconectar, esquecer) e controle da música do celular (AVRCP). Tudo pelo
 * bfbt-agent (core/btstat): estado relido a cada segundo, comandos pelo FIFO. O pedido de
 * pareamento com código aparece em qualquer tela (bt_poll_global). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/btstat.h"
#include "../core/perfmode.h"
#include "../core/perfui.h"
#include "../widgets/page.h"
#include "../widgets/dialog.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define C_BT 0x3584e4

static lv_obj_t *sw, *st_lbl, *btn_vis, *btn_scan, *lbl_vis, *lbl_scan, *pl_card, *pl_title, *pl_sub,
                *pl_play, *list, *msg;
static lv_timer_t *tick;
static bt_info_t cur;
static char list_sig[BT_MAXDEV * 24 + 64];
static char busy_addr[18];                /* aparelho com comando em andamento */
static time_t switched_at;                /* quando o usuário ligou */
static char forget_addr[18];

static void set_text(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void enable(lv_obj_t *o, bool on)
{
    if(on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

/* ------------------------------------------------------------- ações */

static void on_switch(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if(on) switched_at = time(NULL);
    bt_cmd(on ? "on" : "off");
}

static void on_visible(lv_event_t *e) { (void)e; bt_cmd("visible %d", cur.discoverable ? 0 : 1); }
static void on_scan(lv_event_t *e) { (void)e; bt_cmd("scan %d", cur.discovering ? 0 : 1); }

static void on_player(lv_event_t *e)
{
    const char *c = lv_event_get_user_data(e);
    if(!strcmp(c, "toggle")) c = !strcmp(cur.pl_status, "playing") ? "pause" : "play";
    bt_cmd("%s", c);
}

static void forget_answer(int btn, void *ud)
{
    (void)ud;
    if(btn == 1 && forget_addr[0]) bt_cmd("forget %s", forget_addr);
}

static void on_dev_action(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= cur.ndev) return;
    const bt_dev_t *d = &cur.dev[i];
    const char *what = lv_obj_get_user_data(b);
    if(!strcmp(what, "forget")) {
        snprintf(forget_addr, sizeof forget_addr, "%s", d->addr);
        char t[96];
        snprintf(t, sizeof t, tr(S_BT_FORGET_Q), d->name);
        const char *btns[] = { tr(S_CANCEL), tr(S_BT_FORGET) };
        dialog_open(LV_SYMBOL_TRASH, pal()->danger, t, tr(S_BT_FORGET_BODY), btns, 2, -1, 1, forget_answer, NULL);
        return;
    }
    if(bt_cmd("%s %s", what, d->addr)) snprintf(busy_addr, sizeof busy_addr, "%s", d->addr);
}

/* ------------------------------------------------------------ desenho */

static lv_obj_t *small_button(lv_obj_t *parent, const char *sym, const char *txt, int kind, const char *what, int idx)
{
    lv_obj_t *b = bf_button(parent, sym, txt, kind);
    lv_obj_set_user_data(b, (void *)what);
    lv_obj_add_event_cb(b, on_dev_action, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return b;
}

static void build_list(void)
{
    const bf_palette_t *p = pal();
    lv_obj_clean(list);
    if(!cur.powered) return;
    if(!cur.ndev) {
        lv_obj_t *n = bf_label(list, tr(S_BT_NODEV), BF_FONT_S, p->text_dim);
        lv_obj_set_width(n, LV_PCT(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        return;
    }
    for(int i = 0; i < cur.ndev; i++) {
        const bt_dev_t *d = &cur.dev[i];
        lv_obj_t *row = bf_card(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, BF_SP_M, 0);
        lv_obj_set_style_pad_ver(row, BF_SP_S, 0);
        bool audio = !strncmp(d->icon, "audio", 5);
        page_round_icon(row, audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH,
                        d->connected ? lv_color_hex(C_BT) : p->text_dim, 44);
        lv_obj_t *col = bf_plain(row);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *nm = bf_label(col, d->name, BF_FONT_M, p->text);
        bf_label_ellipsis(nm, 300);
        bool busy = !strcmp(busy_addr, d->addr);
        const char *st = busy ? tr(S_BT_BUSY) : d->connected ? tr(S_BT_CONNECTED) : d->paired ? tr(S_BT_PAIRED)
                                                                                           : tr(S_BT_FOUND);
        bf_label(col, st, BF_FONT_S, d->connected ? lv_color_hex(C_BT) : p->text_dim);
        lv_obj_t *b;
        if(!d->paired) b = small_button(row, NULL, tr(S_BT_PAIR), 1, "pair", i);
        else if(d->connected) b = small_button(row, NULL, tr(S_BT_DISCONNECT), 0, "disconnect", i);
        else b = small_button(row, NULL, tr(S_BT_CONNECT), 1, "connect", i);
        enable(b, !busy);
        if(d->paired) small_button(row, LV_SYMBOL_TRASH, NULL, 2, "forget", i);
    }
}

static void refresh(void)
{
    bt_read(&cur);
    const bf_palette_t *p = pal();
    char b[256];

    /* o comando em andamento acabou? */
    if(busy_addr[0] && strcmp(cur.res_state, "andamento")) {
        if(!strcmp(cur.res_state, "erro")) {
            const char *err = strrchr(cur.res_err, '.');
            snprintf(b, sizeof b, tr(S_BT_FAILED), err ? err + 1 : cur.res_err);
            set_text(msg, b);
        } else set_text(msg, "");
        busy_addr[0] = 0;
    }

    bool on = cur.enabled || cur.adapter;          /* ligado pelo app ou por fora (S35bluetooth) */
    if(lv_obj_has_state(sw, LV_STATE_CHECKED) != on) {
        if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    const char *name = cur.name[0] ? cur.name : "BigFoston";
    if(!on) set_text(st_lbl, tr(S_BT_ST_OFF));
    else if(!cur.powered) set_text(st_lbl, switched_at && time(NULL) - switched_at > 20 ? tr(S_BT_ST_FAIL) : tr(S_BT_ST_STARTING));
    else {
        snprintf(b, sizeof b, tr(cur.discoverable ? S_BT_ST_VISIBLE : S_BT_ST_ON), name);
        set_text(st_lbl, b);
    }
    lv_obj_set_style_text_color(st_lbl, cur.powered ? lv_color_hex(C_BT) : p->text_dim, 0);
    enable(btn_vis, cur.powered);
    enable(btn_scan, cur.powered);
    set_text(lbl_vis, tr(cur.discoverable ? S_BT_HIDE : S_BT_VISIBLE));
    set_text(lbl_scan, tr(cur.discovering ? S_BT_SCANNING : S_BT_SCAN));

    if(cur.player && cur.powered) {
        lv_obj_remove_flag(pl_card, LV_OBJ_FLAG_HIDDEN);
        set_text(pl_title, cur.pl_title[0] ? cur.pl_title : "—");
        snprintf(b, sizeof b, tr(S_BT_MUSIC), cur.pl_dev);
        if(cur.pl_artist[0]) snprintf(b, sizeof b, "%s · %s", cur.pl_artist, cur.pl_dev);
        set_text(pl_sub, b);
        set_text(pl_play, !strcmp(cur.pl_status, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    } else lv_obj_add_flag(pl_card, LV_OBJ_FLAG_HIDDEN);

    /* a lista só é refeita quando muda (evita piscar e perder o toque) */
    char sig[sizeof list_sig];
    int k = snprintf(sig, sizeof sig, "%d%s;", cur.powered, busy_addr);
    for(int i = 0; i < cur.ndev && k < (int)sizeof sig - 32; i++)
        k += snprintf(sig + k, sizeof sig - (size_t)k, "%s%d%d%.8s;", cur.dev[i].addr, cur.dev[i].paired,
                      cur.dev[i].connected, cur.dev[i].name);
    if(strcmp(sig, list_sig)) {
        snprintf(list_sig, sizeof list_sig, "%s", sig);
        build_list();
    }
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

static lv_obj_t *action_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, lv_obj_t **lbl)
{
    lv_obj_t *b = bf_button(parent, sym, "", 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    *lbl = lv_obj_get_child(b, -1);          /* o texto é o último filho do botão */
    return b;
}

/* ---------------------------------------------------------------- pausado pelo modo performance */

static void create(lv_obj_t *parent, int arg);
static lv_obj_t *paused_pg, *paused_parent;
static int paused_wait;

/* o Bluetooth voltou (ou o modo desligou e passou tempo bastante): refaz a página de verdade */
static void paused_check(lv_timer_t *t)
{
    (void)t;
    if(perfmode_paused() & PM_SVC_BT) { paused_wait = 0; return; }
    bt_info_t b;
    bt_read(&b);
    if(!b.available && ++paused_wait < 10) return;
    lv_timer_delete(tick);
    tick = NULL;
    lv_obj_t *par = paused_parent;
    lv_obj_delete(paused_pg);
    paused_pg = NULL;
    create(par, 0);
}

static void paused_off(lv_event_t *e) { (void)e; perfui_disable(); }

static void paused_page(lv_obj_t *parent, lv_obj_t *pg)
{
    const bf_palette_t *p = pal();
    page_header(pg, LV_SYMBOL_BLUETOOTH, lv_color_hex(C_BT), tr(S_APP_BLUETOOTH), tr(S_BT_SUB));
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(c, lv_color_hex(0xe66100), 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    char title[96];
    snprintf(title, sizeof title, LV_SYMBOL_CHARGE "  %s", tr(S_PERFM_PAUSED));
    bf_label(c, title, BF_FONT_M, lv_color_hex(0xe66100));
    lv_obj_t *m = bf_label(c, tr(S_PERFM_PAUSED_LONG), BF_FONT_S, p->text_dim);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_t *b = bf_button(c, LV_SYMBOL_CHARGE, tr(S_PERFM_NOW_OFF), 1);
    lv_obj_set_height(b, 52);
    lv_obj_add_event_cb(b, paused_off, LV_EVENT_CLICKED, NULL);
    paused_pg = pg;
    paused_parent = parent;
    paused_wait = 0;
    tick = lv_timer_create(paused_check, 1000, NULL);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    list_sig[0] = busy_addr[0] = 0;
    switched_at = 0;
    lv_obj_t *pg = page_create(parent);
    bt_read(&cur);
    if(!cur.available && (perfmode_paused() & PM_SVC_BT)) {
        paused_page(parent, pg);
        return;
    }
    if(!cur.available) {
        page_header(pg, LV_SYMBOL_BLUETOOTH, lv_color_hex(C_BT), tr(S_APP_BLUETOOTH), tr(S_BT_SUB));
        page_unavailable(pg, tr(S_BT_NOAGENT));
        return;
    }
    sw = page_switch_row(pg, tr(S_APP_BLUETOOTH), cur.enabled || cur.adapter, on_switch, NULL);
    st_lbl = bf_label(pg, "", BF_FONT_S, p->text_dim);
    lv_obj_set_width(st_lbl, LV_PCT(100));
    lv_label_set_long_mode(st_lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t *acts = bf_plain(pg);
    lv_obj_set_size(acts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(acts, BF_SP_M, 0);
    btn_vis = action_button(acts, LV_SYMBOL_EYE_OPEN, on_visible, &lbl_vis);
    btn_scan = action_button(acts, LV_SYMBOL_REFRESH, on_scan, &lbl_scan);
    lv_obj_set_flex_grow(btn_vis, 1);
    lv_obj_set_flex_grow(btn_scan, 1);

    /* música do celular conectado */
    pl_card = bf_card(pg);
    lv_obj_set_size(pl_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pl_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pl_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pl_card, BF_SP_M, 0);
    page_round_icon(pl_card, LV_SYMBOL_AUDIO, lv_color_hex(C_BT), 44);
    lv_obj_t *col = bf_plain(pl_card);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    pl_title = bf_label(col, "", BF_FONT_M, p->text);
    bf_label_ellipsis(pl_title, 380);
    pl_sub = bf_label(col, "", BF_FONT_S, p->text_dim);
    bf_label_ellipsis(pl_sub, 380);
    static const char *cmds[] = { "prev", "toggle", "next" };
    static const char *syms[] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for(int i = 0; i < 3; i++) {
        lv_obj_t *b = bf_button(pl_card, syms[i], NULL, i == 1 ? 1 : 0);
        lv_obj_add_event_cb(b, on_player, LV_EVENT_CLICKED, (void *)cmds[i]);
        if(i == 1) pl_play = lv_obj_get_child(b, 0);
    }
    lv_obj_add_flag(pl_card, LV_OBJ_FLAG_HIDDEN);

    msg = bf_label(pg, "", BF_FONT_S, p->warning);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

    list = bf_plain(pg);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, BF_SP_S, 0);

    refresh();
    tick = lv_timer_create(tick_cb, 1000, NULL);
}

static void destroy(void)
{
    if(tick) lv_timer_delete(tick);
    tick = NULL;
    paused_pg = paused_parent = NULL;
}

const bf_screen_def_t scr_bluetooth_def = { create, destroy, S_APP_BLUETOOTH, NULL };
