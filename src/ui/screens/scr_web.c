/* scr_web.c — Internet (BigFoston Web).
 *
 * A página é desenhada pelo motor bfweb (NetSurf 3.11, src/web/frontend) noutro
 * processo, sem privilégios e com memória limitada (core/webproc); esta tela mostra a
 * imagem da página sem cópia e cuida do resto com a cara do sistema:
 *
 *   barra   ◀ ▶ ⟳/✕ · endereço (cadeado + domínio; toque edita com sugestões) · ★ · ☰
 *   página  arrastar rola (com inércia), toque clica, toque longo abre o menu do link;
 *           campo de texto da página abre o teclado pt-BR (widgets/kbd) sem cobrir o campo
 *   início  pesquisa, WebApps, favoritos e visitados recentemente
 *   menu    favoritos, histórico, downloads, criar WebApp, zoom, economizar dados,
 *           JavaScript, mecanismo de pesquisa, memória, sobre
 *
 * Ao sair do app o motor é encerrado (a RAM volta para o GPS e os outros apps); a
 * página aberta reabre ao voltar. Favoritos, histórico e WebApps: core/webdata. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/osd.h"
#include "../core/webdata.h"
#include "../core/webproc.h"
#include "../core/webdl.h"
#include "../widgets/kbd.h"
#include "../widgets/page.h"
#include "../widgets/imgview.h"
#include "../assets/icons/bf_icons.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define TB_H       52
#define KB_H       200
#define C_WEB      0x3584e4
#define DRAG_START 10           /* px até virar arrasto */
#define PANEL_W    440

/* ---------------- estado ---------------- */

static lv_obj_t *root, *toolbar, *b_back, *b_fwd, *b_reload, *b_star, *addr_btn, *addr_lbl, *addr_lock;
static lv_obj_t *progress, *page, *start, *panel, *edit_row, *edit_ta, *edit_sugg, *kb, *notice, *scroll_ind;
static lv_obj_t *ctx_menu;
static int ph;                              /* altura da página (área de conteúdo - barra) */
static char cur_url[WEB_URL_MAX], cur_title[WEB_TITLE_MAX];
static char last_url[WEB_URL_MAX];          /* reabre ao voltar ao app (vale entre telas) */
static char pending_go[WEB_URL_MAX];        /* pedido antes do READY */
static bool loading, engine_ready, stopping, page_kb;
static int sec_state, zoom_pct = 100;
static int scroll_y, extent_h;

/* toque na página */
static lv_point_t t_start, t_last;
static bool dragging, long_sent;
static int acc_x, acc_y;                    /* rolagem a enviar */
static float vel_y, vel_x;                  /* px por quadro, para a inércia */
static uint32_t t_move;
static lv_timer_t *scroll_t, *ind_t;
static char feat_link[WEB_URL_MAX], feat_obj[WEB_URL_MAX];

static void show_start(bool on);
static void go_url(const char *url);
static void engine_start(const char *url);
static void panel_close(void);
static void edit_close(void);
static void page_kb_hide(void);

/* ---------------- utilidades ---------------- */

static void toast(const char *title, const char *sub)
{
    osd_show(&bf_icon_web, C_WEB, title, sub);
}

static lv_obj_t *tool(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = bf_button(parent, sym, NULL, 0);
    lv_obj_set_size(b, 50, 44);
    lv_obj_set_style_pad_hor(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void set_enabled(lv_obj_t *o, bool on)
{
    if(on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
    lv_obj_set_style_opa(o, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

static bool is_about(const char *u) { return !u[0] || !strncasecmp(u, "about:", 6); }

/* há rota padrão? (rede USB com compartilhamento, Bluetooth PAN...) */
static bool have_route(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    if(!f) return true;                       /* sem /proc (simulador): não atrapalha */
    char line[256];
    bool any = false;
    while(fgets(line, sizeof line, f)) {
        char ifn[32];
        unsigned long dst;
        if(sscanf(line, "%31s %lx", ifn, &dst) == 2 && dst == 0) any = true;
    }
    fclose(f);
    return any;
}

static bool clock_ok(void) { return time(NULL) > 1700000000; }   /* depois de nov/2023 */

/* ---------------- barra ---------------- */

static void update_bar(void)
{
    if(!toolbar) return;
    char host[128];
    if(is_about(cur_url)) snprintf(host, sizeof host, "%s", tr(S_WEB_ADDRESS));
    else web_host(cur_url, host, sizeof host);
    lv_label_set_text(addr_lbl, host);
    lv_obj_set_style_text_color(addr_lbl, is_about(cur_url) ? pal()->text_dim : pal()->text, 0);
    /* cadeado só em HTTPS sem problemas; aviso em HTTP */
    if(sec_state == 6) { lv_label_set_text(addr_lock, BF_SYMBOL_LOCK); lv_obj_set_style_text_color(addr_lock, pal()->success, 0); }
    else if(sec_state == 3 || sec_state == 4 || sec_state == 5) { lv_label_set_text(addr_lock, LV_SYMBOL_WARNING); lv_obj_set_style_text_color(addr_lock, pal()->warning, 0); }
    else { lv_label_set_text(addr_lock, BF_SYMBOL_SEARCH); lv_obj_set_style_text_color(addr_lock, pal()->text_dim, 0); }
    lv_label_set_text(lv_obj_get_child(b_reload, 0), loading ? LV_SYMBOL_CLOSE : LV_SYMBOL_REFRESH);
    bool marked = !is_about(cur_url) && web_mark_find(cur_url) >= 0;
    lv_obj_set_style_text_color(lv_obj_get_child(b_star, 0), marked ? lv_color_hex(0xf5a524) : pal()->text, 0);
    set_enabled(b_star, !is_about(cur_url));
    if(loading) lv_obj_remove_flag(progress, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
}

static void back_click(lv_event_t *e) { (void)e; if(start && !lv_obj_has_flag(start, LV_OBJ_FLAG_HIDDEN) && !is_about(cur_url)) { show_start(false); return; } webproc_send("BACK"); }
static void fwd_click(lv_event_t *e) { (void)e; webproc_send("FWD"); }

static void reload_click(lv_event_t *e)
{
    (void)e;
    if(loading) webproc_send("STOP");
    else if(!webproc_running() && last_url[0]) engine_start(last_url);
    else webproc_send("RELOAD");
}

static void star_click(lv_event_t *e)
{
    (void)e;
    if(is_about(cur_url)) return;
    int i = web_mark_find(cur_url);
    if(i >= 0) { web_mark_remove(i); toast(tr(S_WEB_REMOVED), cur_title); }
    else if(web_mark_add(cur_title[0] ? cur_title : cur_url, cur_url)) toast(tr(S_WEB_ADDED), cur_title);
    update_bar();
}

/* ---------------- motor ---------------- */

static void progress_anim(void *o, int32_t v) { lv_obj_set_x(o, v); }

static void ind_hide(lv_timer_t *t) { (void)t; if(scroll_ind) lv_obj_add_flag(scroll_ind, LV_OBJ_FLAG_HIDDEN); ind_t = NULL; }

/* indicador de posição na borda direita (some sozinho) */
static void show_scroll_ind(void)
{
    if(!scroll_ind || extent_h <= ph) return;
    int h = ph * ph / extent_h;
    if(h < 24) h = 24;
    int y = (int)((long)scroll_y * (ph - h) / (extent_h - ph));
    lv_obj_set_size(scroll_ind, 5, h);
    lv_obj_set_pos(scroll_ind, BF_SCREEN_W - 8, TB_H + y);
    lv_obj_remove_flag(scroll_ind, LV_OBJ_FLAG_HIDDEN);
    if(ind_t) lv_timer_reset(ind_t);
    else { ind_t = lv_timer_create(ind_hide, 900, NULL); lv_timer_set_repeat_count(ind_t, 1); }
}

static void notice_hide(void)
{
    if(notice) { lv_obj_delete(notice); notice = NULL; }
}

static void retry_click(lv_event_t *e) { (void)e; notice_hide(); engine_start(last_url[0] ? last_url : NULL); }

/* aviso no meio da página (motor caiu, motor ausente) */
static void show_notice(const char *msg, bool retry)
{
    notice_hide();
    notice = bf_card(root);
    lv_obj_set_size(notice, 560, LV_SIZE_CONTENT);
    lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, TB_H + 30);
    lv_obj_set_flex_flow(notice, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(notice, BF_SP_L, 0);
    lv_obj_set_style_pad_row(notice, BF_SP_M, 0);
    lv_obj_t *l = bf_label(notice, msg, BF_FONT_M, pal()->text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    if(retry) {
        lv_obj_t *b = bf_button(notice, LV_SYMBOL_REFRESH, tr(S_WEB_RELOAD_PAGE), 1);
        lv_obj_add_event_cb(b, retry_click, LV_EVENT_CLICKED, NULL);
    }
}

static void caret_visible(int y, int h)
{
    /* o teclado (layer_top, embaixo) não pode cobrir o campo */
    lv_area_t a;
    lv_obj_get_coords(page, &a);
    int kb_top = BF_SCREEN_H - KB_H;
    int bottom = a.y1 + y + h;
    if(bottom > kb_top - 12) webproc_send("SCROLL 0 %d", bottom - (kb_top - 12) + 40);
    else if(a.y1 + y < a.y1) webproc_send("SCROLL 0 %d", y - 20);
}

static void page_kb_key(kbd_key_t k, const char *text, void *ud)
{
    (void)ud;
    switch(k) {
    case KBD_TEXT: webproc_send("TEXT %s", text); break;
    case KBD_BACKSPACE: webproc_send("KEY backspace"); break;
    case KBD_ENTER: webproc_send("KEY enter"); page_kb_hide(); break;
    case KBD_LEFT: webproc_send("KEY left"); break;
    case KBD_RIGHT: webproc_send("KEY right"); break;
    case KBD_CLOSE: page_kb_hide(); break;
    }
}

static lv_obj_t *make_kb(lv_obj_t *ta)
{
    lv_obj_t *k = kbd_create(lv_layer_top(), ta);
    lv_obj_set_size(k, BF_SCREEN_W, KB_H);
    lv_obj_align(k, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(k, BF_FONT_M, 0);
    lv_obj_set_style_pad_all(k, 4, 0);
    lv_obj_set_style_pad_gap(k, 4, 0);
    lv_obj_set_style_bg_color(k, pal()->surface, 0);
    lv_obj_set_style_radius(k, 0, 0);
    lv_obj_set_style_border_side(k, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(k, 2, 0);
    lv_obj_set_style_border_color(k, lv_color_hex(C_WEB), 0);
    lv_obj_set_style_radius(k, BF_RADIUS_S, LV_PART_ITEMS);
    return k;
}

static void page_kb_show(void)
{
    if(kb) return;
    kb = make_kb(NULL);
    kbd_set_sink(kb, page_kb_key, NULL);
    page_kb = true;
}

static void page_kb_hide(void)
{
    if(kb && page_kb) { lv_obj_delete(kb); kb = NULL; }
    page_kb = false;
}

static void ctx_close(void)
{
    if(ctx_menu) { lv_obj_delete(ctx_menu); ctx_menu = NULL; }
}

static void ctx_click(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    char l[WEB_URL_MAX], o[WEB_URL_MAX];
    snprintf(l, sizeof l, "%s", feat_link);
    snprintf(o, sizeof o, "%s", feat_obj);
    ctx_close();
    switch(what) {
    case 0: go_url(l); break;
    case 1: webproc_send("DOWNLOAD %s", l); break;
    case 2: webproc_send("DOWNLOAD %s", o); break;
    case 3: if(web_mark_add(l, l)) toast(tr(S_WEB_ADDED), l); break;
    }
}

static void ctx_bg_click(lv_event_t *e) { if(lv_event_get_target(e) == lv_event_get_current_target(e)) ctx_close(); }

static void ctx_row(lv_obj_t *box, const char *sym, const char *txt, int what)
{
    lv_obj_t *b = bf_button(box, sym, txt, 0);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, ctx_click, LV_EVENT_CLICKED, (void *)(intptr_t)what);
}

/* menu do toque longo (link e/ou imagem) */
static void ctx_open(int x, int y, const char *link, const char *obj)
{
    ctx_close();
    if(!link[0] && !obj[0]) return;
    snprintf(feat_link, sizeof feat_link, "%s", link);
    snprintf(feat_obj, sizeof feat_obj, "%s", obj);
    ctx_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ctx_menu);
    lv_obj_set_size(ctx_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(ctx_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ctx_menu, ctx_bg_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *box = bf_card(ctx_menu);
    lv_obj_set_width(box, 330);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, BF_SP_S, 0);
    lv_obj_set_style_pad_row(box, BF_SP_XS, 0);
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_50, 0);
    lv_obj_t *t = bf_label(box, link[0] ? link : obj, BF_FONT_S, pal()->text_dim);
    bf_label_ellipsis(t, 310);
    if(link[0]) {
        ctx_row(box, LV_SYMBOL_RIGHT, tr(S_WEB_OPEN_LINK), 0);
        ctx_row(box, LV_SYMBOL_DOWNLOAD, tr(S_WEB_DL_LINK), 1);
        ctx_row(box, BF_SYMBOL_STAR, tr(S_WEB_MARK_LINK), 3);
    }
    if(obj[0]) ctx_row(box, BF_SYMBOL_IMAGE, tr(S_WEB_DL_IMAGE), 2);
    lv_obj_update_layout(box);
    lv_area_t a;
    lv_obj_get_coords(page, &a);
    int bx = a.x1 + x - 165, by = a.y1 + y + 12, bh = lv_obj_get_height(box);
    if(bx < 8) bx = 8;
    if(bx > BF_SCREEN_W - 338) bx = BF_SCREEN_W - 338;
    if(by + bh > BF_SCREEN_H - 8) by = a.y1 + y - bh - 12;
    if(by < 8) by = 8;
    lv_obj_set_pos(box, bx, by);
}

/* uma linha de evento do motor */
static void on_event(const char *l, void *ud)
{
    (void)ud;
    int a, b, c, d;
    if(!strncmp(l, "R ", 2) && sscanf(l + 2, "%d %d %d %d", &a, &b, &c, &d) == 4) {
        if(!page) return;
        lv_area_t ar;
        lv_obj_get_coords(page, &ar);
        lv_area_t r = { ar.x1 + a, ar.y1 + b, ar.x1 + c - 1, ar.y1 + d - 1 };
        lv_obj_invalidate_area(page, &r);
    }
    else if(!strncmp(l, "READY", 5)) {
        engine_ready = true;
        if(zoom_pct != 100) webproc_send("ZOOM %d", zoom_pct);
        if(pending_go[0]) { webproc_send("GO %s", pending_go); pending_go[0] = 0; }
    }
    else if(!strncmp(l, "URL ", 4)) {
        snprintf(cur_url, sizeof cur_url, "%s", l + 4);
        if(!is_about(cur_url)) {
            snprintf(last_url, sizeof last_url, "%s", cur_url);
            show_start(false);
        }
        update_bar();
    }
    else if(!strncmp(l, "TITLE ", 6)) {
        snprintf(cur_title, sizeof cur_title, "%s", l + 6);
        web_history_set_title(cur_url, cur_title);
    }
    else if(!strncmp(l, "LOAD ", 5)) {
        loading = l[5] == '1';
        if(!loading && !is_about(cur_url)) web_history_add(cur_title, cur_url);
        update_bar();
    }
    else if(!strncmp(l, "NAV ", 4) && sscanf(l + 4, "%d %d", &a, &b) == 2) {
        set_enabled(b_back, a || (start && !lv_obj_has_flag(start, LV_OBJ_FLAG_HIDDEN) && !is_about(cur_url)));
        set_enabled(b_fwd, b);
    }
    else if(!strncmp(l, "SEC ", 4)) { sec_state = atoi(l + 4); update_bar(); }
    else if(!strncmp(l, "SCROLL ", 7) && sscanf(l + 7, "%d %d %d %d", &a, &b, &c, &d) == 4) {
        scroll_y = b;
        extent_h = d;
        if(dragging || fabsf(vel_y) > 0.5f) show_scroll_ind();
    }
    else if(!strncmp(l, "CARET 1 ", 8) && sscanf(l + 8, "%d %d %d", &a, &b, &c) == 3) {
        page_kb_show();
        caret_visible(b, c);
    }
    else if(!strcmp(l, "CARET 0")) page_kb_hide();
    else if(!strncmp(l, "FEAT ", 5)) {
        const char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if(t2 && sscanf(l + 5, "%d %d", &a, &b) == 2) {
            char link[WEB_URL_MAX];
            snprintf(link, sizeof link, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
            ctx_open(a, b, link, t2 + 1);
        }
    }
    else if(!strncmp(l, "DLREQ ", 6)) {
        char name[256];
        if(webdl_request(l + 6, name, sizeof name)) toast(tr(S_WEB_DL_START), name);
        else toast(tr(S_WEB_DL_FAIL), name);
    }
    else if(!strncmp(l, "LAUNCH ", 7)) toast(tr(S_WEB_LAUNCH), l + 7);
    else if(!strncmp(l, "ZOOM ", 5)) zoom_pct = atoi(l + 5);
    else if(!strncmp(l, "EXIT", 4)) {
        engine_ready = false;
        loading = false;
        page_kb_hide();
        if(root) update_bar();
        if(!stopping && root) show_notice(tr(S_WEB_CRASH), true);
    }
}

static void engine_start(const char *url)
{
    notice_hide();
    if(!webproc_available()) { show_notice(tr(S_WEB_NO_ENGINE), false); return; }
    webproc_opts_t o = { BF_SCREEN_W, ph, settings()->web_js, !settings()->web_no_images, NULL };
    char err[200];
    engine_ready = false;
    stopping = false;
    pending_go[0] = 0;
    if(url && *url) snprintf(pending_go, sizeof pending_go, "%s", url);
    if(!webproc_start(&o, on_event, NULL, err, sizeof err)) { show_notice(err, true); return; }
    if(page) lv_image_set_src(page, webproc_image());
}

static void engine_stop(void)
{
    stopping = true;
    webproc_stop();
    engine_ready = false;
}

static void go_url(const char *url)
{
    if(!url || !*url) return;
    if(!web_scheme_allowed(url)) { toast(tr(S_WEB_BAD_URL), url); return; }
    bool net = !strncasecmp(url, "http", 4);
    if(net && !have_route()) toast(tr(S_WEB_NO_NET), NULL);
    else if(!strncasecmp(url, "https:", 6) && !clock_ok()) toast(tr(S_WEB_CLOCK), NULL);
    panel_close();
    show_start(false);
    snprintf(last_url, sizeof last_url, "%s", url);
    if(!webproc_running()) { engine_start(url); return; }
    if(!engine_ready) { snprintf(pending_go, sizeof pending_go, "%s", url); return; }
    webproc_send("GO %s", url);
}

/* ---------------- toque na página ---------------- */

static void page_point(lv_event_t *e, lv_point_t *p)
{
    lv_indev_get_point(lv_indev_active(), p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    p->x -= a.x1;
    p->y -= a.y1;
}

/* manda a rolagem acumulada (até ~30 por segundo) e faz a inércia depois do toque */
static void scroll_tick(lv_timer_t *t)
{
    (void)t;
    if(!dragging) {
        if(fabsf(vel_y) < 0.6f && fabsf(vel_x) < 0.6f) { vel_x = vel_y = 0; return; }
        acc_x += (int)vel_x;
        acc_y += (int)vel_y;
        vel_x *= 0.92f;
        vel_y *= 0.92f;
    }
    if(acc_x || acc_y) {
        webproc_send("SCROLL %d %d", acc_x, acc_y);
        acc_x = acc_y = 0;
    }
}

static void page_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    if(c == LV_EVENT_PRESSED) {
        page_point(e, &p);
        t_start = t_last = p;
        dragging = long_sent = false;
        vel_x = vel_y = 0;
        acc_x = acc_y = 0;
        t_move = lv_tick_get();
    }
    else if(c == LV_EVENT_PRESSING) {
        page_point(e, &p);
        int dx = p.x - t_start.x, dy = p.y - t_start.y;
        if(!dragging && dx * dx + dy * dy > DRAG_START * DRAG_START) dragging = true;
        if(dragging) {
            int mx = t_last.x - p.x, my = t_last.y - p.y;
            acc_x += mx;
            acc_y += my;
            uint32_t dt = lv_tick_elaps(t_move);
            if(dt > 0) {                                /* velocidade por quadro de 30 ms */
                float k = 30.0f / (float)(dt < 5 ? 5 : dt);
                vel_x = vel_x * 0.5f + mx * k * 0.5f;
                vel_y = vel_y * 0.5f + my * k * 0.5f;
            }
            t_move = lv_tick_get();
            t_last = p;
        }
    }
    else if(c == LV_EVENT_LONG_PRESSED) {
        if(!dragging) {
            long_sent = true;
            webproc_send("LONG %d %d", t_start.x, t_start.y);
        }
    }
    else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if(!dragging && !long_sent && c == LV_EVENT_RELEASED) webproc_send("CLICK %d %d", t_start.x, t_start.y);
        if(dragging && lv_tick_elaps(t_move) > 80) vel_x = vel_y = 0;   /* parou antes de soltar */
        if(fabsf(vel_x) < 3) vel_x = 0;                  /* inércia só para gestos claros */
        dragging = false;
    }
}

/* ---------------- início ---------------- */

static void edit_open(void);

static void start_search(lv_event_t *e) { (void)e; edit_open(); }

static void item_go(lv_event_t *e)
{
    const char *u = lv_event_get_user_data(e);
    go_url(u);
}

/* azulejo com a inicial do nome */
static lv_obj_t *tile(lv_obj_t *parent, const char *name, const char *url, unsigned color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 112, 104);
    lv_obj_set_style_radius(b, BF_RADIUS_M, 0);
    lv_obj_set_style_bg_color(b, pal()->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, item_go, LV_EVENT_CLICKED, (void *)url);
    lv_obj_t *ic = lv_obj_create(b);
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 56, 56);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(ic, 16, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(ic, lv_color_mix(lv_color_hex(color), lv_color_black(), 170), 0);
    lv_obj_set_style_bg_grad_dir(ic, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    char ini[8] = "?";
    char host[64];
    web_host(url, host, sizeof host);
    /* sem título (ou o título é o próprio endereço): mostra o domínio */
    const char *src = name && *name && !strstr(name, "://") ? name : host;
    /* primeira letra (UTF-8 inteiro), em maiúscula se for ASCII */
    int n = 1;
    unsigned char c0 = (unsigned char)src[0];
    if(c0 >= 0xc0) n = c0 >= 0xf0 ? 4 : c0 >= 0xe0 ? 3 : 2;
    snprintf(ini, sizeof ini, "%.*s", n, src);
    if(n == 1) ini[0] = (char)toupper(c0);
    lv_obj_t *l = bf_label(ic, ini, BF_FONT_L, lv_color_white());
    lv_obj_center(l);
    lv_obj_t *t = bf_label(b, src, BF_FONT_S, pal()->text);
    bf_label_ellipsis(t, 104);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -6);
    return b;
}

static void section(lv_obj_t *p, const char *title)
{
    lv_obj_t *l = bf_label(p, title, BF_FONT_S, pal()->text_dim);
    lv_obj_set_style_pad_top(l, BF_SP_S, 0);
}

static void build_start(void)
{
    if(start) lv_obj_clean(start);
    else {
        start = bf_plain(root);
        lv_obj_set_size(start, BF_SCREEN_W, ph);
        lv_obj_set_pos(start, 0, TB_H);
        lv_obj_set_style_bg_color(start, pal()->bg, 0);
        lv_obj_set_style_bg_opa(start, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(start, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(start, BF_SP_XL, 0);
        lv_obj_set_style_pad_ver(start, BF_SP_M, 0);
        lv_obj_set_style_pad_row(start, BF_SP_S, 0);
        bf_scrollable(start);
    }
    lv_obj_t *s = lv_button_create(start);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, LV_PCT(100), 56);
    lv_obj_set_style_radius(s, 28, 0);
    lv_obj_set_style_bg_color(s, pal()->surface, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s, lv_color_hex(C_WEB), 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_pad_hor(s, BF_SP_L, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s, BF_SP_M, 0);
    lv_obj_add_event_cb(s, start_search, LV_EVENT_CLICKED, NULL);
    bf_label(s, BF_SYMBOL_SEARCH, BF_FONT_M, lv_color_hex(C_WEB));
    bf_label(s, tr(S_WEB_ADDRESS), BF_FONT_M, pal()->text_dim);

    if(web_apps_count()) {
        section(start, tr(S_WEB_WEBAPPS));
        lv_obj_t *row = bf_plain(start);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(row, BF_SP_S, 0);
        for(int i = 0; i < web_apps_count(); i++) tile(row, web_app(i)->name, web_app(i)->url, web_app(i)->color);
    }
    section(start, tr(S_WEB_MARKS));
    if(!web_marks_count()) bf_label(start, tr(S_WEB_NO_MARKS), BF_FONT_S, pal()->text_dim);
    else {
        lv_obj_t *row = bf_plain(start);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(row, BF_SP_S, 0);
        lv_obj_set_style_pad_row(row, BF_SP_S, 0);
        static const unsigned hues[] = { 0x3584e4, 0x9b6ad6, 0x2ec27e, 0xe5534b, 0xe0a40c, 0x1fb5cc };
        for(int i = 0; i < web_marks_count() && i < 18; i++) {
            unsigned h = 5381;
            for(const char *c = web_mark(i)->url; *c; c++) h = h * 33 + (unsigned char)*c;
            tile(row, web_mark(i)->title, web_mark(i)->url, hues[h % 6]);
        }
    }
    if(web_history_count()) {
        section(start, tr(S_WEB_RECENT));
        for(int i = 0; i < web_history_count() && i < 6; i++) {
            const web_item_t *h = web_history(i);
            lv_obj_t *b = lv_button_create(start);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, LV_PCT(100), 44);
            lv_obj_set_style_radius(b, BF_RADIUS_S, 0);
            lv_obj_set_style_bg_color(b, pal()->surface2, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_pad_hor(b, BF_SP_S, 0);
            lv_obj_add_event_cb(b, item_go, LV_EVENT_CLICKED, (void *)h->url);
            lv_obj_t *t = bf_label(b, h->title, BF_FONT_S, pal()->text);
            bf_label_ellipsis(t, 470);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
            char host[64];
            web_host(h->url, host, sizeof host);
            lv_obj_t *r = bf_label(b, host, BF_FONT_S, pal()->text_dim);
            bf_label_ellipsis(r, 220);
            lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

static void show_start(bool on)
{
    if(on) {
        build_start();
        lv_obj_remove_flag(start, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(start);
        if(scroll_ind) lv_obj_move_foreground(scroll_ind);
    }
    else if(start) lv_obj_add_flag(start, LV_OBJ_FLAG_HIDDEN);
    if(b_back) set_enabled(b_back, on ? !is_about(cur_url) : true);
}

/* ---------------- edição do endereço ---------------- */

static bool edit_fresh;

/* texto com o cursor no fim e a rolagem certa (o campo de uma linha guarda a rolagem
 * do texto anterior até o próximo layout) */
static void edit_set_text(const char *t)
{
    lv_textarea_set_text(edit_ta, t);
    lv_obj_update_layout(edit_ta);
    lv_obj_scroll_to_x(edit_ta, 0, LV_ANIM_OFF);
    lv_textarea_set_cursor_pos(edit_ta, LV_TEXTAREA_CURSOR_LAST);
}

static void edit_go(const char *text)
{
    char url[WEB_URL_MAX];
    if(!web_normalize(text, settings()->web_search, url, sizeof url)) {
        if(text && *text) toast(tr(S_WEB_BAD_URL), text);
        return;
    }
    edit_close();
    go_url(url);
}

static void sugg_click(lv_event_t *e) { edit_go(lv_event_get_user_data(e)); }

/* favoritos e histórico que contêm o texto (sem diferenciar maiúsculas) */
static void edit_suggest(void)
{
    if(!edit_sugg) return;
    lv_obj_clean(edit_sugg);
    const char *q = lv_textarea_get_text(edit_ta);
    int shown = 0, max = (BF_SCREEN_H - KB_H - (lv_obj_get_y(root) + BF_TOPBAR_H) - TB_H) / 44;
    if(max < 1) max = 1;
    for(int pass = 0; pass < 2 && shown < max; pass++) {
        int n = pass ? web_history_count() : web_marks_count();
        for(int i = 0; i < n && shown < max; i++) {
            const web_item_t *it = pass ? web_history(i) : web_mark(i);
            if(*q && !strcasestr(it->url, q) && !strcasestr(it->title, q)) continue;
            if(!*q && pass == 0) continue;                    /* vazio: só os recentes */
            lv_obj_t *b = lv_button_create(edit_sugg);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, LV_PCT(100), 44);
            lv_obj_set_style_bg_color(b, pal()->surface2, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_pad_hor(b, BF_SP_M, 0);
            lv_obj_add_event_cb(b, sugg_click, LV_EVENT_CLICKED, (void *)it->url);
            lv_obj_t *ic = bf_label(b, pass ? LV_SYMBOL_LOOP : BF_SYMBOL_STAR, BF_FONT_S, pal()->text_dim);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_t *t = bf_label(b, it->title, BF_FONT_S, pal()->text);
            bf_label_ellipsis(t, 420);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 30, 0);
            char host[64];
            web_host(it->url, host, sizeof host);
            lv_obj_t *r = bf_label(b, host, BF_FONT_S, pal()->text_dim);
            bf_label_ellipsis(r, 250);
            lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
            shown++;
        }
    }
}

static void edit_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_INSERT && edit_fresh) {
        edit_fresh = false;
        lv_textarea_set_text(edit_ta, "");              /* a inserção segue no campo vazio */
    }
    else if(c == LV_EVENT_VALUE_CHANGED) edit_suggest();
}

static void kb_done(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_READY) {
        char t[WEB_URL_MAX];
        snprintf(t, sizeof t, "%s", lv_textarea_get_text(edit_ta));
        edit_go(t);
    }
    else edit_close();
}

static void edit_cancel(lv_event_t *e) { (void)e; edit_close(); }

static void edit_close(void)
{
    if(kb && !page_kb) { lv_obj_delete(kb); kb = NULL; }
    if(edit_row) { lv_obj_delete(edit_row); edit_row = NULL; }
    if(edit_sugg) { lv_obj_delete(edit_sugg); edit_sugg = NULL; }
    edit_ta = NULL;
}

static void edit_open(void)
{
    panel_close();
    page_kb_hide();
    edit_close();
    const bf_palette_t *p = pal();
    edit_row = bf_plain(root);
    lv_obj_set_size(edit_row, BF_SCREEN_W, TB_H);
    lv_obj_set_pos(edit_row, 0, 0);
    lv_obj_set_style_bg_color(edit_row, p->surface, 0);
    lv_obj_set_style_bg_opa(edit_row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(edit_row, BF_SP_S, 0);
    lv_obj_set_style_pad_column(edit_row, BF_SP_S, 0);
    lv_obj_set_flex_flow(edit_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(edit_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    edit_ta = lv_textarea_create(edit_row);
    lv_textarea_set_one_line(edit_ta, true);
    lv_textarea_set_max_length(edit_ta, WEB_URL_MAX - 1);
    lv_textarea_set_placeholder_text(edit_ta, tr(S_WEB_ADDRESS));
    lv_obj_set_style_text_font(edit_ta, BF_FONT_M, 0);
    lv_obj_set_height(edit_ta, 44);
    lv_obj_set_style_pad_ver(edit_ta, 7, 0);            /* linha de 25 px: 44 - 2x7 - 2x2 de borda = 26 */
    lv_obj_set_style_pad_hor(edit_ta, BF_SP_M, 0);
    lv_obj_set_style_radius(edit_ta, 22, 0);
    lv_obj_set_style_bg_color(edit_ta, p->surface2, 0);
    lv_obj_set_style_bg_opa(edit_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(edit_ta, p->text, 0);
    lv_obj_set_style_border_width(edit_ta, 2, 0);
    lv_obj_set_style_border_color(edit_ta, lv_color_hex(C_WEB), 0);
    lv_obj_set_scrollbar_mode(edit_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_grow(edit_ta, 1);
    lv_obj_set_style_border_color(edit_ta, lv_color_hex(C_WEB), LV_STATE_FOCUSED);
    lv_obj_add_state(edit_ta, LV_STATE_FOCUSED);
    edit_fresh = false;
    if(!is_about(cur_url)) {
        edit_set_text(cur_url);
        edit_fresh = true;                              /* a primeira tecla substitui a URL */
    }
    lv_obj_add_event_cb(edit_ta, edit_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(edit_ta, edit_event, LV_EVENT_INSERT, NULL);
    lv_obj_t *x = bf_button(edit_row, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 50, 44);
    lv_obj_add_event_cb(x, edit_cancel, LV_EVENT_CLICKED, NULL);

    edit_sugg = bf_plain(root);
    lv_obj_set_pos(edit_sugg, 0, TB_H);
    lv_obj_set_size(edit_sugg, BF_SCREEN_W, ph);
    lv_obj_set_style_bg_color(edit_sugg, p->bg, 0);
    lv_obj_set_style_bg_opa(edit_sugg, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(edit_sugg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(edit_sugg, BF_SP_M, 0);
    edit_suggest();

    kb = make_kb(edit_ta);
    lv_obj_add_event_cb(kb, kb_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_done, LV_EVENT_CANCEL, NULL);
    page_kb = false;
}

static void addr_click(lv_event_t *e) { (void)e; edit_open(); }

/* ---------------- painel lateral (menu, favoritos, histórico, downloads) ---------------- */

static void panel_close(void)
{
    if(panel) { lv_obj_delete(panel); panel = NULL; }
    webdl_set_listener(NULL, NULL);
}

static void panel_close_click(lv_event_t *e) { (void)e; panel_close(); }

static lv_obj_t *panel_open(const char *title)
{
    panel_close();
    edit_close();
    const bf_palette_t *p = pal();
    panel = bf_card(root);
    lv_obj_set_size(panel, PANEL_W, ph);
    lv_obj_set_pos(panel, BF_SCREEN_W - PANEL_W, TB_H);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, BF_SP_S, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_50, 0);
    bf_scrollable(panel);
    lv_obj_t *hdr = bf_plain(panel);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    lv_obj_t *t = bf_label(hdr, title, BF_FONT_M, p->text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, BF_SP_S, 0);
    lv_obj_t *x = bf_button(hdr, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 48, 40);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(x, panel_close_click, LV_EVENT_CLICKED, NULL);
    return panel;
}

static lv_obj_t *panel_row(const char *sym, const char *left, const char *right, lv_event_cb_t cb, void *ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *it = lv_button_create(panel);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, LV_PCT(100), 48);
    lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
    if(cb) lv_obj_add_event_cb(it, cb, LV_EVENT_CLICKED, ud);
    int x = 0;
    if(sym) {
        lv_obj_t *s = bf_label(it, sym, BF_FONT_M, lv_color_hex(C_WEB));
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);
        x = 36;
    }
    lv_obj_t *l = bf_label(it, left, BF_FONT_S, p->text);
    bf_label_ellipsis(l, right ? 250 : 360 - x);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    if(right) {
        lv_obj_t *r = bf_label(it, right, BF_FONT_S, p->text_dim);
        bf_label_ellipsis(r, 120);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return it;
}

static void open_marks(void);
static void open_history(void);
static void open_downloads(void);
static void open_menu(void);

static void mark_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        web_mark_remove(i);
        lv_indev_wait_release(lv_indev_active());
        lv_async_call((lv_async_cb_t)open_marks, NULL);    /* não recria o painel dentro do evento dele */
        update_bar();
        return;
    }
    const web_item_t *m = web_mark(i);
    if(m) { char u[WEB_URL_MAX]; snprintf(u, sizeof u, "%s", m->url); go_url(u); }
}

static void open_marks(void)
{
    if(!root) return;
    panel_open(tr(S_WEB_MARKS));
    if(!web_marks_count()) { bf_label(panel, tr(S_WEB_NO_MARKS), BF_FONT_S, pal()->text_dim); return; }
    for(int i = 0; i < web_marks_count(); i++) {
        char host[64];
        web_host(web_mark(i)->url, host, sizeof host);
        lv_obj_t *r = panel_row(BF_SYMBOL_STAR, web_mark(i)->title, host, mark_pick, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, mark_pick, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    bf_label(panel, tr(S_WEB_DELETE_HINT), BF_FONT_S, pal()->text_dim);
}

static void hist_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        web_history_remove(i);
        lv_indev_wait_release(lv_indev_active());
        lv_async_call((lv_async_cb_t)open_history, NULL);
        return;
    }
    const web_item_t *h = web_history(i);
    if(h) { char u[WEB_URL_MAX]; snprintf(u, sizeof u, "%s", h->url); go_url(u); }
}

static void hist_clear(lv_event_t *e) { (void)e; web_history_clear(); lv_async_call((lv_async_cb_t)open_history, NULL); }

static void hist_toggle(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    web_history_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void open_history(void)
{
    if(!root) return;
    panel_open(tr(S_WEB_HISTORY));
    page_switch_row(panel, tr(S_WEB_KEEP_HIST), web_history_enabled(), hist_toggle, NULL);
    if(!web_history_count()) { bf_label(panel, tr(S_WEB_NO_HIST), BF_FONT_S, pal()->text_dim); return; }
    panel_row(LV_SYMBOL_TRASH, tr(S_WEB_CLEAR_HIST), NULL, hist_clear, NULL);
    for(int i = 0; i < web_history_count() && i < 100; i++) {
        const web_item_t *h = web_history(i);
        char when[24];
        struct tm tm;
        localtime_r(&h->when, &tm);
        strftime(when, sizeof when, "%d/%m %H:%M", &tm);
        lv_obj_t *r = panel_row(NULL, h->title, when, hist_pick, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, hist_pick, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    bf_label(panel, tr(S_WEB_DELETE_HINT), BF_FONT_S, pal()->text_dim);
}

/* --- downloads --- */

static void dl_action(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int id = code >> 2, what = code & 3;
    if(what == 0) webdl_pause(id);
    else if(what == 1) webdl_resume(id);
    else if(what == 2) webdl_cancel(id);
    else {
        const webdl_t *d = webdl_find(id);
        if(d && d->state == WEBDL_DONE) {
            char p[WEB_URL_MAX];
            snprintf(p, sizeof p, "%s", d->path);
            panel_close();
            webdl_open_file(p);
        }
    }
}

static void fmt_size(char *out, size_t n, long long b)
{
    if(b < 0) snprintf(out, n, "?");
    else if(b < 1024 * 1024) snprintf(out, n, "%lld KB", (b + 512) / 1024);
    else snprintf(out, n, "%lld,%lld MB", b >> 20, ((b & 0xfffff) * 10) >> 20);
}

static void dl_refresh(void *ud);

static void open_downloads(void)
{
    if(!root) return;
    panel_open(tr(S_WEB_DOWNLOADS));
    const bf_palette_t *p = pal();
    int n = webdl_count();
    if(!n) bf_label(panel, tr(S_WEB_NO_DL), BF_FONT_S, p->text_dim);
    for(int i = n - 1; i >= 0; i--) {                    /* mais recentes em cima */
        const webdl_t *d = webdl_at(i);
        lv_obj_t *c = bf_plain(panel);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(c, 6, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 4, 0);
        lv_obj_t *t = bf_label(c, d->name, BF_FONT_S, p->text);
        bf_label_ellipsis(t, PANEL_W - 30);
        char a[32], b[32], s[256];
        fmt_size(a, sizeof a, d->done);
        fmt_size(b, sizeof b, d->total > 0 ? d->total : -1);
        const char *st = d->state == WEBDL_DONE ? tr(S_WEB_DL_DONE) : d->state == WEBDL_FAILED ? d->error :
                         d->state == WEBDL_PAUSED ? tr(S_WEB_PAUSE) : "";
        if(d->state == WEBDL_RUNNING && d->speed > 0) {
            char sp[32];
            fmt_size(sp, sizeof sp, d->speed);
            long eta = d->total > 0 && d->speed > 0 ? (long)((d->total - d->done) / d->speed) : -1;
            if(eta >= 0) snprintf(s, sizeof s, "%s / %s  ·  %s/s  ·  %ld:%02ld", a, b, sp, eta / 60, eta % 60);
            else snprintf(s, sizeof s, "%s  ·  %s/s", a, sp);
        }
        else snprintf(s, sizeof s, "%s / %s  ·  %s", a, b, st);
        lv_obj_t *info = bf_label(c, s, BF_FONT_S, d->state == WEBDL_FAILED ? p->warning : p->text_dim);
        bf_label_ellipsis(info, PANEL_W - 30);
        if(d->state != WEBDL_DONE) {
            lv_obj_t *bar = lv_bar_create(c);
            lv_obj_set_size(bar, LV_PCT(100), 6);
            lv_bar_set_value(bar, d->total > 0 ? (int)(d->done * 100 / d->total) : 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, p->surface2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(C_WEB), LV_PART_INDICATOR);
        }
        lv_obj_t *row = bf_plain(c);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, BF_SP_S, 0);
        struct { int what; const char *sym; bf_str_t txt; bool show; } bt[] = {
            { 3, LV_SYMBOL_EYE_OPEN, S_WEB_OPEN, d->state == WEBDL_DONE },
            { 0, LV_SYMBOL_PAUSE, S_WEB_PAUSE, d->state == WEBDL_RUNNING },
            { 1, LV_SYMBOL_PLAY, S_WEB_RESUME, d->state == WEBDL_PAUSED || d->state == WEBDL_FAILED },
            { 2, LV_SYMBOL_CLOSE, S_WEB_CANCEL, d->state != WEBDL_DONE },
        };
        for(size_t k = 0; k < sizeof bt / sizeof bt[0]; k++) {
            if(!bt[k].show) continue;
            lv_obj_t *x = bf_button(row, bt[k].sym, tr(bt[k].txt), bt[k].what == 3 ? 1 : 0);
            lv_obj_set_height(x, 40);
            lv_obj_add_event_cb(x, dl_action, LV_EVENT_CLICKED, (void *)(intptr_t)(d->id << 2 | bt[k].what));
        }
    }
    webdl_set_listener(dl_refresh, NULL);
}

static void dl_refresh_async(void *ud) { (void)ud; if(panel) open_downloads(); }
static void dl_refresh(void *ud) { (void)ud; lv_async_call(dl_refresh_async, NULL); }

/* --- menu --- */

static void restart_engine(void)
{
    char u[WEB_URL_MAX];
    snprintf(u, sizeof u, "%s", is_about(cur_url) ? "" : cur_url);
    engine_stop();
    engine_start(u[0] ? u : NULL);
}

static void menu_pick(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    switch(w) {
    case 0: panel_close(); show_start(true); break;
    case 1: lv_async_call((lv_async_cb_t)open_marks, NULL); break;
    case 2: lv_async_call((lv_async_cb_t)open_history, NULL); break;
    case 3: lv_async_call((lv_async_cb_t)open_downloads, NULL); break;
    case 4:
        if(!is_about(cur_url)) {
            char name[48];
            const char *src = cur_title[0] ? cur_title : cur_url;
            size_t l = strlen(src);
            if(l > sizeof name - 1) {                     /* corta sem partir um caractere UTF-8 */
                l = sizeof name - 1;
                while(l && ((unsigned char)src[l] & 0xc0) == 0x80) l--;
            }
            memcpy(name, src, l);
            name[l] = 0;
            /* nome curto: até o primeiro separador comum de títulos */
            char *cut = strstr(name, " – ");
            if(!cut) cut = strstr(name, " - ");
            if(!cut) cut = strstr(name, " | ");
            if(cut && cut != name) *cut = 0;
            if(web_app_add(name, cur_url)) toast(tr(S_WEB_APP_ADDED), name);
        }
        panel_close();
        break;
    case 5: settings()->web_search = (settings()->web_search + 1) % web_search_count(); settings_save(); lv_async_call((lv_async_cb_t)open_menu, NULL); break;
    case 6: toast(tr(S_WEB_ABOUT), tr(S_WEB_ABOUT_TXT)); break;
    }
}

static void zoom_click(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int z = d ? zoom_pct + d * 10 : 100;
    if(z < 50) z = 50;
    if(z > 200) z = 200;
    zoom_pct = z;
    settings()->web_zoom = z;
    settings_save();
    webproc_send("ZOOM %d", z);
    lv_async_call((lv_async_cb_t)open_menu, NULL);
}

static void opt_toggle(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if(w == 0) settings()->web_no_images = on;
    else settings()->web_js = on;
    settings_save();
    restart_engine();                                 /* opções do motor valem ao iniciar */
}

static void open_menu(void)
{
    if(!root) return;
    panel_open(tr(S_APP_WEB));
    panel_row(LV_SYMBOL_HOME, tr(S_WEB_START), NULL, menu_pick, (void *)0);
    panel_row(BF_SYMBOL_STAR, tr(S_WEB_MARKS), NULL, menu_pick, (void *)1);
    panel_row(LV_SYMBOL_LOOP, tr(S_WEB_HISTORY), NULL, menu_pick, (void *)2);
    char n[16];
    snprintf(n, sizeof n, "%d", webdl_active());
    panel_row(LV_SYMBOL_DOWNLOAD, tr(S_WEB_DOWNLOADS), webdl_active() ? n : NULL, menu_pick, (void *)3);
    if(!is_about(cur_url)) panel_row(LV_SYMBOL_PLUS, tr(S_WEB_CREATE_APP), NULL, menu_pick, (void *)4);
    /* zoom: − 100% + */
    lv_obj_t *z = bf_plain(panel);
    lv_obj_set_size(z, LV_PCT(100), 52);
    lv_obj_set_flex_flow(z, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(z, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(z, BF_SP_M, 0);
    lv_obj_set_style_pad_column(z, BF_SP_S, 0);
    lv_obj_t *zl = bf_label(z, tr(S_WEB_ZOOM), BF_FONT_S, pal()->text);
    lv_obj_set_flex_grow(zl, 1);
    lv_obj_t *b = bf_button(z, LV_SYMBOL_MINUS, NULL, 0);
    lv_obj_set_size(b, 52, 42);
    lv_obj_add_event_cb(b, zoom_click, LV_EVENT_CLICKED, (void *)-1);
    char zp[16];
    snprintf(zp, sizeof zp, "%d%%", zoom_pct);
    b = bf_button(z, NULL, zp, 0);
    lv_obj_set_size(b, 76, 42);
    lv_obj_add_event_cb(b, zoom_click, LV_EVENT_CLICKED, (void *)0);
    b = bf_button(z, LV_SYMBOL_PLUS, NULL, 0);
    lv_obj_set_size(b, 52, 42);
    lv_obj_add_event_cb(b, zoom_click, LV_EVENT_CLICKED, (void *)1);
    page_switch_row(panel, tr(S_WEB_SAVE_DATA), settings()->web_no_images, opt_toggle, (void *)0);
    page_switch_row(panel, tr(S_WEB_JS), settings()->web_js, opt_toggle, (void *)1);
    panel_row(BF_SYMBOL_SEARCH, tr(S_WEB_ENGINE), web_search_name(settings()->web_search), menu_pick, (void *)5);
    char mem[32];
    snprintf(mem, sizeof mem, "%ld MB", (webproc_rss_kb() + 512) / 1024);
    panel_row(BF_SYMBOL_HDD, tr(S_WEB_MEMORY), webproc_running() ? mem : "-", NULL, NULL);
    panel_row(BF_SYMBOL_INFO, tr(S_WEB_ABOUT), "NetSurf 3.11", menu_pick, (void *)6);
}

static void menu_click(lv_event_t *e) { (void)e; if(panel) panel_close(); else open_menu(); }

/* ---------------- tela ---------------- */

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    ph = BF_CONTENT_H - TB_H;
    zoom_pct = settings()->web_zoom ? settings()->web_zoom : 100;
    web_data_load();
    webdl_init();

    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_bg_color(root, p->bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    page = lv_image_create(root);
    lv_obj_set_pos(page, 0, TB_H);
    lv_obj_set_size(page, BF_SCREEN_W, ph);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(page, page_event, LV_EVENT_ALL, NULL);

    toolbar = bf_plain(root);
    lv_obj_set_size(toolbar, BF_SCREEN_W, TB_H);
    lv_obj_set_style_bg_color(toolbar, p->surface, 0);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(toolbar, BF_SP_S, 0);
    lv_obj_set_style_pad_column(toolbar, BF_SP_XS, 0);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    b_back = tool(toolbar, LV_SYMBOL_LEFT, back_click);
    b_fwd = tool(toolbar, LV_SYMBOL_RIGHT, fwd_click);
    b_reload = tool(toolbar, LV_SYMBOL_REFRESH, reload_click);
    addr_btn = lv_button_create(toolbar);
    lv_obj_remove_style_all(addr_btn);
    lv_obj_set_height(addr_btn, 40);
    lv_obj_set_flex_grow(addr_btn, 1);
    lv_obj_set_style_radius(addr_btn, 20, 0);
    lv_obj_set_style_bg_color(addr_btn, p->surface2, 0);
    lv_obj_set_style_bg_opa(addr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(addr_btn, lv_color_mix(p->surface2, lv_color_hex(C_WEB), 200), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(addr_btn, BF_SP_M, 0);
    lv_obj_set_flex_flow(addr_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(addr_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(addr_btn, BF_SP_S, 0);
    lv_obj_add_event_cb(addr_btn, addr_click, LV_EVENT_CLICKED, NULL);
    addr_lock = bf_label(addr_btn, BF_SYMBOL_SEARCH, BF_FONT_S, p->text_dim);
    addr_lbl = bf_label(addr_btn, "", BF_FONT_M, p->text);
    lv_label_set_long_mode(addr_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(addr_lbl, 1);
    b_star = tool(toolbar, BF_SYMBOL_STAR, star_click);
    tool(toolbar, LV_SYMBOL_BARS, menu_click);

    /* carregando: faixa que corre embaixo da barra */
    lv_obj_t *track = bf_plain(root);
    lv_obj_set_size(track, BF_SCREEN_W, 3);
    lv_obj_set_pos(track, 0, TB_H - 3);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(track, true, 0);
    progress = lv_obj_create(track);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, 180, 3);
    lv_obj_set_style_bg_color(progress, lv_color_hex(C_WEB), 0);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, progress);
    lv_anim_set_exec_cb(&a, progress_anim);
    lv_anim_set_values(&a, -180, BF_SCREEN_W);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);

    scroll_ind = lv_obj_create(root);
    lv_obj_remove_style_all(scroll_ind);
    lv_obj_set_style_radius(scroll_ind, 3, 0);
    lv_obj_set_style_bg_color(scroll_ind, lv_color_hex(0x7a8090), 0);
    lv_obj_set_style_bg_opa(scroll_ind, LV_OPA_80, 0);
    lv_obj_remove_flag(scroll_ind, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(scroll_ind, LV_OBJ_FLAG_HIDDEN);

    scroll_t = lv_timer_create(scroll_tick, 30, NULL);
    cur_url[0] = cur_title[0] = 0;
    sec_state = 0;
    loading = false;
    set_enabled(b_back, false);
    set_enabled(b_fwd, false);
    update_bar();
    if(last_url[0]) engine_start(last_url);          /* volta ao app: reabre a página */
    else {
        show_start(true);
        engine_start(NULL);                          /* já carrega o motor (~1 s) */
    }
}

static void destroy(void)
{
    if(scroll_t) { lv_timer_delete(scroll_t); scroll_t = NULL; }
    if(ind_t) { lv_timer_delete(ind_t); ind_t = NULL; }
    ctx_close();
    page_kb_hide();
    edit_close();
    if(kb) { lv_obj_delete(kb); kb = NULL; }
    webdl_set_listener(NULL, NULL);
    engine_stop();                                    /* a RAM volta para o resto do sistema */
    web_data_flush();
    root = toolbar = b_back = b_fwd = b_reload = b_star = addr_btn = addr_lbl = addr_lock = NULL;
    progress = page = start = panel = edit_row = edit_ta = edit_sugg = notice = scroll_ind = NULL;
    page_kb = false;
}

/* --screenshot */
void scr_web_demo(const char *what)
{
    if(!what) return;
    if(!strcmp(what, "+start")) show_start(true);
    else if(!strcmp(what, "+menu")) open_menu();
    else if(!strcmp(what, "+marks")) open_marks();
    else if(!strcmp(what, "+history")) open_history();
    else if(!strcmp(what, "+downloads")) open_downloads();
    else if(!strcmp(what, "+edit")) edit_open();
    else if(!strcmp(what, "+close")) { panel_close(); edit_close(); }
    else if(!strncmp(what, "+type ", 6)) { if(edit_ta) { edit_fresh = false; edit_set_text(what + 6); } }
    else if(!strncmp(what, "+scroll ", 8)) webproc_send("SCROLL 0 %d", atoi(what + 8));
    else if(!strncmp(what, "+mark ", 6) || !strncmp(what, "+app ", 5)) {   /* "+mark título\turl" */
        char t[WEB_URL_MAX + WEB_TITLE_MAX];
        snprintf(t, sizeof t, "%s", what + (what[1] == 'm' ? 6 : 5));
        char *tab = strchr(t, '\t');
        if(!tab) return;
        *tab = 0;
        if(what[1] == 'm') web_mark_add(t, tab + 1);
        else web_app_add(t, tab + 1);
    }
    else go_url(what);
}

/* espera o motor (screenshot e testes): processa eventos até LOAD 0 ou o tempo */
const bf_screen_def_t scr_web_def = { create, destroy, S_APP_WEB, NULL };
