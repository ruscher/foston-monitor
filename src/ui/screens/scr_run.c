/* scr_run.c — Run: executar aplicativos e comandos.
 *
 * Campo de busca/comando + teclado na tela (teclas >= 64 px), abas
 * Apps / Favoritos / Histórico e painel de saída (stdout+stderr, código de saída).
 * Sem "sh -c" o texto vira argv direto (sem shell); com "sh -c" roda /bin/sh -c.
 * Histórico e favoritos: $BIGFOSTON_DATA/history.txt e favorites.txt
 * (linhas que começam com "$ " foram executadas em modo shell). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/runner.h"
#include "../core/desktop.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { TAB_APPS, TAB_FAVS, TAB_HIST, TAB__N };
#define OUT_MAX 6144

static lv_obj_t *ta, *btn_sh, *btn_fav, *list, *tabs[TAB__N], *outcard, *out_lbl, *out_scroll,
       *out_status, *btn_stop, *kb, *body;
static int tab;
static bf_app_entry_t *apps;
static int n_apps;
static bf_strlist_t hist, favs;
static char outbuf[OUT_MAX];
static size_t outlen;
static bool alive;   /* tela aberta (callbacks do runner podem chegar depois) */

static void rebuild_list(void);
static void show_output(bool on);

/* ---------- utilidades ---------- */

static bool shell_mode(void) { return lv_obj_has_state(btn_sh, LV_STATE_CHECKED); }

static void toast(const char *msg, lv_color_t color)
{
    lv_obj_t *t = bf_pill(lv_layer_top(), msg, color);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, pal()->surface2, 0);
    lv_obj_set_style_text_font(t, BF_FONT_M, 0);
    lv_obj_set_style_pad_hor(t, 20, 0);
    lv_obj_set_style_pad_ver(t, 10, 0);
    lv_obj_set_style_max_width(t, 720, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -(BF_BOTBAR_H + 12));
    lv_obj_delete_delayed(t, 3000);
}

/* "$ cmd" => modo shell */
static const char *entry_text(const char *s, bool *is_sh)
{
    if(!strncmp(s, "$ ", 2)) { *is_sh = true; return s + 2; }
    *is_sh = false;
    return s;
}

static void current_key(char *buf, size_t n)
{
    const char *t = lv_textarea_get_text(ta);
    if(shell_mode()) snprintf(buf, n, "$ %s", t);
    else snprintf(buf, n, "%s", t);
}

static void sync_fav_btn(void)
{
    char k[300];
    current_key(k, sizeof k);
    if(lv_textarea_get_text(ta)[0] && strlist_contains(&favs, k)) lv_obj_add_state(btn_fav, LV_STATE_CHECKED);
    else lv_obj_remove_state(btn_fav, LV_STATE_CHECKED);
}

/* ---------- saída ---------- */

/* Anexa texto filtrando escapes ANSI e caracteres de controle. */
static void out_append(const char *d, int len)
{
    static int esc;  /* 0 normal, 1 viu ESC, 2 dentro de CSI */
    for(int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)d[i];
        if(esc == 1) { esc = c == '[' ? 2 : 0; continue; }
        if(esc == 2) { if(c >= 0x40 && c <= 0x7e) esc = 0; continue; }
        if(c == 0x1b) { esc = 1; continue; }
        if(c == '\t') c = ' ';
        if(c < 0x20 && c != '\n') continue;
        if(outlen >= OUT_MAX - 1) {
            /* descarta a metade mais antiga, recomeçando numa linha inteira */
            size_t cut = OUT_MAX / 2;
            while(cut < outlen && outbuf[cut] != '\n') cut++;
            memmove(outbuf, outbuf + cut, outlen - cut);
            outlen -= cut;
        }
        outbuf[outlen++] = (char)c;
    }
    outbuf[outlen] = 0;
}

static void out_refresh(void)
{
    if(!alive) return;
    lv_label_set_text_static(out_lbl, outbuf);
    lv_obj_update_layout(out_scroll);
    lv_obj_scroll_to_y(out_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void on_out(const char *d, int len, void *ud)
{
    (void)ud;
    out_append(d, len);
    out_refresh();
}

static void on_done(int code, int sig, void *ud)
{
    (void)ud;
    if(!alive) return;
    const bf_palette_t *p = pal();
    if(sig) {
        lv_label_set_text_fmt(out_status, "%s %d", tr(S_RUN_SIGNAL), sig);
        lv_obj_set_style_text_color(out_status, p->warning, 0);
    }
    else {
        lv_label_set_text_fmt(out_status, "%s: %d", tr(S_RUN_EXIT), code);
        lv_obj_set_style_text_color(out_status, code == 0 ? p->success : p->danger, 0);
    }
    lv_obj_add_flag(btn_stop, LV_OBJ_FLAG_HIDDEN);
}

static void start_capture_ui(const char *title)
{
    outlen = 0;
    outbuf[0] = 0;
    out_append("> ", 2);
    out_append(title, (int)strlen(title));
    out_append("\n", 1);
    show_output(true);
    out_refresh();
    lv_label_set_text(out_status, tr(S_RUN_RUNNING));
    lv_obj_set_style_text_color(out_status, pal()->accent, 0);
    lv_obj_remove_flag(btn_stop, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- execução ---------- */

static void exec_text(const char *text, bool sh)
{
    char msg[200];
    if(!text[0]) return;
    if(runner_busy()) { toast(tr(S_RUN_RUNNING), pal()->warning); return; }
    int rc;
    start_capture_ui(text);
    if(sh) rc = runner_shell(text, on_out, on_done, NULL, msg, sizeof msg);
    else {
        static char buf[512];
        char *argv[32];
        snprintf(buf, sizeof buf, "%s", text);
        if(runner_split(buf, argv, 32) == 0) return;
        rc = runner_exec(argv, true, on_out, on_done, NULL, msg, sizeof msg);
    }
    if(rc < 0) {
        out_append(msg, (int)strlen(msg));
        out_append("\n", 1);
        out_refresh();
        lv_label_set_text_fmt(out_status, "%s", tr(S_RUN_FAILED));
        lv_obj_set_style_text_color(out_status, pal()->danger, 0);
        lv_obj_add_flag(btn_stop, LV_OBJ_FLAG_HIDDEN);
        if(rc == -EPERM) toast(tr(S_RUN_ROOT), pal()->danger);
        return;
    }
    char key[520];
    snprintf(key, sizeof key, "%s%s", sh ? "$ " : "", text);
    strlist_push_front(&hist, key);
    strlist_save(&hist, "history.txt");
    if(tab == TAB_HIST) rebuild_list();
}

static void run_app(const bf_app_entry_t *a)
{
    if(a->terminal) { exec_text(a->exec, false); return; }
    static char buf[256];
    char *argv[32], msg[200];
    snprintf(buf, sizeof buf, "%s", a->exec);
    if(runner_split(buf, argv, 32) == 0) return;
    int rc = runner_exec(argv, false, NULL, NULL, NULL, msg, sizeof msg);
    if(rc < 0) toast(msg, pal()->danger);
    else toast(tr(S_RUN_LAUNCHED), pal()->success);
}

/* ---------- teclado ---------- */

static void kb_hide(void)
{
    if(kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if(ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
}

static void kb_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_READY) {
        kb_hide();
        exec_text(lv_textarea_get_text(ta), shell_mode());
    }
    else if(c == LV_EVENT_CANCEL) kb_hide();
}

static void kb_show(void)
{
    if(!kb) {
        kb = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(kb, BF_SCREEN_W, 296);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_text_font(kb, BF_FONT_M, 0);
        lv_obj_set_style_pad_all(kb, 6, 0);
        lv_obj_set_style_pad_gap(kb, 6, 0);
        lv_obj_set_style_bg_color(kb, pal()->surface, 0);
        lv_obj_set_style_radius(kb, 0, 0);
        lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(kb, 2, 0);
        lv_obj_set_style_border_color(kb, pal()->accent, 0);
        lv_obj_set_style_radius(kb, BF_RADIUS_S, LV_PART_ITEMS);
        lv_obj_add_event_cb(kb, kb_event, LV_EVENT_ALL, NULL);
    }
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

/* ---------- lista ---------- */

static void item_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(tab == TAB_APPS) { if(i < n_apps) run_app(&apps[i]); return; }
    bf_strlist_t *l = tab == TAB_FAVS ? &favs : &hist;
    if(i >= l->n) return;
    bool sh;
    const char *t = entry_text(l->item[i], &sh);
    lv_textarea_set_text(ta, t);
    if(sh) lv_obj_add_state(btn_sh, LV_STATE_CHECKED);
    else lv_obj_remove_state(btn_sh, LV_STATE_CHECKED);
    sync_fav_btn();
}

static lv_obj_t *list_row(const char *title, const char *sub, bool mono_title, int idx)
{
    const bf_palette_t *p = pal();
    lv_obj_t *b = lv_button_create(list);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, 64, 0);
    lv_obj_set_style_pad_hor(b, BF_SP_M, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_radius(b, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, p->border, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_50, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_t *t = bf_label(b, title, mono_title ? BF_FONT_MONO : BF_FONT_M, p->text);
    bf_label_ellipsis(t, LV_PCT(100));
    if(sub) {
        lv_obj_t *s = bf_label(b, sub, BF_FONT_S, p->text_dim);
        bf_label_ellipsis(s, LV_PCT(100));
    }
    lv_obj_add_event_cb(b, item_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return b;
}

static void rebuild_list(void)
{
    lv_obj_clean(list);
    int shown = 0;
    if(tab == TAB_APPS) {
        const char *flt = lv_textarea_get_text(ta);
        for(int i = 0; i < n_apps; i++) {
            if(flt[0] && !strcasestr(apps[i].name, flt) && !strcasestr(apps[i].exec, flt)) continue;
            char sub[300];
            snprintf(sub, sizeof sub, "%s%s%s", apps[i].comment[0] ? apps[i].comment : apps[i].exec,
                     apps[i].terminal ? "  " LV_SYMBOL_BULLET "  " : "", apps[i].terminal ? BF_SYMBOL_TERMINAL : "");
            list_row(apps[i].name, sub, false, i);
            shown++;
        }
        if(!n_apps) {
            lv_obj_t *l = bf_label(list, tr(S_RUN_EMPTY_APPS), BF_FONT_S, pal()->text_dim);
            lv_obj_set_style_pad_all(l, BF_SP_M, 0);
            return;
        }
    }
    else {
        bf_strlist_t *l = tab == TAB_FAVS ? &favs : &hist;
        for(int i = 0; i < l->n; i++) { list_row(l->item[i], NULL, true, i); shown++; }
    }
    if(!shown) {
        lv_obj_t *l = bf_label(list, tr(S_RUN_EMPTY_LIST), BF_FONT_S, pal()->text_dim);
        lv_obj_set_style_pad_all(l, BF_SP_M, 0);
    }
}

static void tab_click(lv_event_t *e)
{
    tab = (int)(intptr_t)lv_event_get_user_data(e);
    for(int i = 0; i < TAB__N; i++) {
        if(i == tab) lv_obj_add_state(tabs[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(tabs[i], LV_STATE_CHECKED);
    }
    show_output(false);
    rebuild_list();
}

/* ---------- eventos da barra de comando ---------- */

static void ta_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_CLICKED || c == LV_EVENT_FOCUSED) kb_show();
    else if(c == LV_EVENT_VALUE_CHANGED) {
        sync_fav_btn();
        if(tab == TAB_APPS && !lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN)) rebuild_list();
    }
}

static void exec_click(lv_event_t *e)
{
    (void)e;
    kb_hide();
    exec_text(lv_textarea_get_text(ta), shell_mode());
}

static void sh_click(lv_event_t *e) { (void)e; sync_fav_btn(); }

static void fav_click(lv_event_t *e)
{
    (void)e;
    if(!lv_textarea_get_text(ta)[0]) { lv_obj_remove_state(btn_fav, LV_STATE_CHECKED); return; }
    char k[300];
    current_key(k, sizeof k);
    if(!strlist_remove(&favs, k)) strlist_push_front(&favs, k);
    strlist_save(&favs, "favorites.txt");
    sync_fav_btn();
    if(tab == TAB_FAVS) rebuild_list();
}

static void stop_click(lv_event_t *e) { (void)e; runner_stop(); }
static void close_out_click(lv_event_t *e) { (void)e; show_output(false); }

static void show_output(bool on)
{
    if(on) { lv_obj_remove_flag(outcard, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(outcard, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN); }
}

/* ---------- criação ---------- */

static void create(lv_obj_t *parent, int arg)
{
    const bf_palette_t *p = pal();
    alive = true;
    strlist_load(&hist, "history.txt");
    strlist_load(&favs, "favorites.txt");
    n_apps = desktop_scan(&apps);

    lv_obj_t *pg = bf_plain(parent);
    lv_obj_set_size(pg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(pg, BF_SP_L, 0);
    lv_obj_set_style_pad_ver(pg, BF_SP_M, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pg, BF_SP_M, 0);

    /* barra de comando */
    lv_obj_t *cmd = bf_plain(pg);
    lv_obj_set_size(cmd, LV_PCT(100), 64);
    lv_obj_set_flex_flow(cmd, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cmd, BF_SP_S, 0);
    ta = lv_textarea_create(cmd);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, tr(S_RUN_PLACEHOLDER));
    lv_textarea_set_max_length(ta, 250);
    lv_obj_set_height(ta, 64);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_text_font(ta, BF_FONT_M, 0);
    lv_obj_set_style_pad_ver(ta, 18, 0);
    lv_obj_set_style_radius(ta, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(ta, p->surface, 0);
    lv_obj_set_style_border_color(ta, p->border, 0);
    lv_obj_set_style_border_color(ta, p->accent, LV_STATE_FOCUSED);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ta, ta_event, LV_EVENT_ALL, NULL);

    btn_sh = bf_button(cmd, BF_SYMBOL_TERMINAL, tr(S_RUN_SHELL), 0);
    lv_obj_add_flag(btn_sh, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_text_font(btn_sh, BF_FONT_S, 0);
    lv_obj_add_event_cb(btn_sh, sh_click, LV_EVENT_VALUE_CHANGED, NULL);
    btn_fav = bf_button(cmd, BF_SYMBOL_STAR, NULL, 0);
    lv_obj_set_width(btn_fav, 64);
    lv_obj_set_style_pad_hor(btn_fav, 0, 0);
    lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(btn_fav, p->warning, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn_fav, fav_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bx = bf_button(cmd, LV_SYMBOL_PLAY, tr(S_RUN_EXEC), 1);
    lv_obj_add_event_cb(bx, exec_click, LV_EVENT_CLICKED, NULL);

    /* corpo: abas à esquerda, lista/saída à direita */
    body = bf_plain(pg);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, BF_SP_M, 0);

    lv_obj_t *tabcol = bf_plain(body);
    lv_obj_set_size(tabcol, 168, LV_PCT(100));
    lv_obj_set_flex_flow(tabcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tabcol, BF_SP_S, 0);
    static const char *const tsym[TAB__N] = { LV_SYMBOL_LIST, BF_SYMBOL_STAR, BF_SYMBOL_CLOCK };
    static const bf_str_t ttxt[TAB__N] = { S_RUN_APPS, S_RUN_FAVS, S_RUN_HISTORY };
    for(int i = 0; i < TAB__N; i++) {
        tabs[i] = bf_button(tabcol, tsym[i], tr(ttxt[i]), 0);
        lv_obj_set_width(tabs[i], LV_PCT(100));
        lv_obj_set_flex_align(tabs[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(tabs[i], tab_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_add_state(tabs[tab], LV_STATE_CHECKED);

    lv_obj_t *right = bf_card(body);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_all(right, 4, 0);

    list = bf_plain(right);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    bf_scrollable(list);

    /* painel de saída */
    outcard = bf_plain(right);
    lv_obj_set_size(outcard, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(outcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(outcard, 6, 0);
    lv_obj_set_style_pad_row(outcard, 4, 0);
    lv_obj_t *oh = bf_plain(outcard);
    lv_obj_set_size(oh, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(oh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(oh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(oh, BF_SP_M, 0);
    lv_obj_set_style_pad_left(oh, 6, 0);
    bf_label(oh, tr(S_RUN_OUTPUT), BF_FONT_M, p->text);
    out_status = bf_label(oh, "", BF_FONT_S, p->text_dim);
    lv_obj_set_flex_grow(out_status, 1);
    btn_stop = bf_button(oh, LV_SYMBOL_STOP, tr(S_RUN_STOP), 2);
    lv_obj_t *bc = bf_button(oh, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_add_event_cb(btn_stop, stop_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bc, close_out_click, LV_EVENT_CLICKED, NULL);
    out_scroll = bf_plain(outcard);
    lv_obj_set_width(out_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(out_scroll, 1);
    lv_obj_set_style_bg_color(out_scroll, p->dark ? lv_color_hex(0x12151b) : lv_color_hex(0xf6f8fb), 0);
    lv_obj_set_style_bg_opa(out_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(out_scroll, BF_RADIUS_S, 0);
    lv_obj_set_style_pad_all(out_scroll, BF_SP_S, 0);
    bf_scrollable(out_scroll);
    out_lbl = bf_label(out_scroll, "", BF_FONT_MONO, p->dark ? lv_color_hex(0xc8f5d0) : p->text);
    lv_obj_set_width(out_lbl, LV_PCT(100));
    lv_label_set_long_mode(out_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(outcard, LV_OBJ_FLAG_HIDDEN);

    if(arg == RUN_ARG_TERMINAL) lv_obj_add_state(btn_sh, LV_STATE_CHECKED);
    rebuild_list();
    if(arg == RUN_ARG_TERMINAL) kb_show();
}

static void destroy(void)
{
    alive = false;
    runner_forget();
    if(kb) { lv_obj_delete(kb); kb = NULL; }
    free(apps);
    apps = NULL;
    n_apps = 0;
    strlist_free(&hist);
    strlist_free(&favs);
    ta = NULL;
}

/* ---------- ganchos de captura de tela ---------- */

void scr_run_demo_keyboard(void)
{
    if(!ta) return;
    lv_textarea_set_text(ta, "uname -a");
    kb_show();
}

void scr_run_demo_command(const char *cmd)
{
    if(!ta) return;
    lv_obj_add_state(btn_sh, LV_STATE_CHECKED);
    lv_textarea_set_text(ta, cmd);
    exec_text(cmd, true);
}

const bf_screen_def_t scr_run_def = { create, destroy, S_APP_RUN, NULL };
