/* scr_fm.c — Rádio FM: transmissor FM do FOSTON (KT0801A, bffm). Leva o som do aparelho a um
 * rádio externo (carro, casa) sintonizado na mesma frequência. O FOSTON não tem receptor AM/FM
 * (docs/fm-radio/08): não há aba de ouvir rádio.
 *
 * A transmissão é do serviço (bffm deixa o chip ligado): sair da tela não desliga. O estado
 * mostrado é o do hardware (/run/bigfoston-fm, escrito pelo bffm depois de conferir o PW_OK e a
 * frequência lida de volta do chip), nunca o do botão.
 * Comandos rodam em segundo plano (core/fmstat.c); a tela só relê o estado a cada 400 ms. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/fmstat.h"
#include "../widgets/glass.h"
#include "../widgets/dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_WHITE lv_color_hex(GL_WHITE)
#define C_DIM   lv_color_hex(GL_TEXT2)
#define C_GREEN 0x3DDC84
#define C_AMBER 0xF5B83D
#define C_RED   0xFF6B6B
#define C_GRAY  0x8A9BB5

static lv_obj_t *root, *st_dot, *st_lbl, *st_info, *f_int, *f_dec, *f_box, *slider, *tx_btn, *tx_icon, *tx_lbl, *hint;
static lv_obj_t *mode_row[2], *fav_list, *timer_lbl, *tone_lbl, *minus_b, *plus_b;
static lv_timer_t *tick;
static int shown_freq = -1;               /* frequência no mostrador */
static long apply_at;                     /* aplica a frequência no hardware depois de parar de mexer */
static fm_info_t cur;

static long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

static void set_text(lv_obj_t *l, const char *t) { if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }

/* ------------------------------------------------------------ peças de vidro */

static lv_obj_t *gcard(lv_obj_t *p)
{
    lv_obj_t *c = bf_card(p);
    lv_obj_set_style_bg_color(c, lv_color_hex(GL_BLUE), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void grad(lv_obj_t *o, uint32_t top, uint32_t bottom, lv_state_t s)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(top), s);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), s);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, s);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, s);
}

static lv_obj_t *gbtn(lv_obj_t *p, const char *sym, const char *txt)
{
    lv_obj_t *b = bf_button(p, sym, txt, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, C_WHITE, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(GL_LIGHT), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(GL_BLUE), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_min_height(b, 0, 0);
    return b;
}

static lv_obj_t *title(lv_obj_t *p, const char *t)
{
    lv_obj_t *l = bf_label(p, t, BF_FONT_S, C_DIM);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

/* ------------------------------------------------------------ frequência */

static void show_freq(int f)
{
    if(f == shown_freq) return;
    shown_freq = f;
    char a[16], b[16];
    snprintf(a, sizeof a, "%d", f / 10);
    snprintf(b, sizeof b, "%d", f % 10);
    set_text(f_int, a);
    set_text(f_dec, b);
    if(slider && lv_slider_get_value(slider) != f) lv_slider_set_value(slider, f, LV_ANIM_OFF);
}

/* nova frequência escolhida: mostra na hora; com o FM ligado, aplica 350 ms depois de parar */
static void choose(int f)
{
    if(f < FM_MIN) f = FM_MIN;
    if(f > FM_MAX) f = FM_MAX;
    fm_conf()->freq = f;
    show_freq(f);
    apply_at = ms_now() + 350;
}

static void on_step(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    choose(fm_conf()->freq + d);
}

static void on_slider(lv_event_t *e) { choose((int)lv_slider_get_value(lv_event_get_target(e))); }

/* ----- teclado numérico */

static char kp_buf[8];
static lv_obj_t *kp_lbl;
static void kp_show(void)
{
    char b[24];
    snprintf(b, sizeof b, "%s MHz", kp_buf[0] ? kp_buf : "—");
    set_text(kp_lbl, b);
}

static void kp_click(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    const char *t = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));
    if(!t) return;
    size_t n = strlen(kp_buf);
    if(!strcmp(t, LV_SYMBOL_BACKSPACE)) { if(n) kp_buf[n - 1] = 0; }
    else if(!strcmp(t, ",")) { if(n && !strchr(kp_buf, ',') && n < 3) { kp_buf[n] = ','; kp_buf[n + 1] = 0; } }
    else if(n < 5) {
        char *c = strchr(kp_buf, ',');
        if(c && strlen(c) >= 2) return;                      /* uma casa decimal (passo 0,1 MHz) */
        kp_buf[n] = t[0];
        kp_buf[n + 1] = 0;
    }
    kp_show();
}

static void kp_answer(int b, void *ud)
{
    (void)ud;
    kp_lbl = NULL;
    if(b != 1 || !kp_buf[0]) return;
    int ip = atoi(kp_buf), dp = 0;
    char *c = strchr(kp_buf, ',');
    if(c && c[1]) dp = c[1] - '0';
    int f = ip * 10 + dp;
    if(!fm_freq_valid(f)) {
        const char *btns[] = { tr(S_CLOSE) };
        dialog_open(LV_SYMBOL_WARNING, lv_color_hex(C_AMBER), tr(S_FM_INVALID), tr(S_FM_RANGE), btns, 1, 0, -1, NULL, NULL);
        return;
    }
    choose(f);
}

static void on_keypad(lv_event_t *e)
{
    (void)e;
    static const char *const map[] = { "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", ",", "0",
                                       LV_SYMBOL_BACKSPACE, "" };
    const char *btns[] = { tr(S_CANCEL), "OK" };
    kp_buf[0] = 0;
    lv_obj_t *card = dialog_open(BF_SYMBOL_TOWER, lv_color_hex(C_GREEN), tr(S_FM_TYPE_FREQ), tr(S_FM_RANGE), btns, 2, 1,
                                 -1, kp_answer, NULL);
    lv_obj_set_width(card, 420);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    kp_lbl = bf_label(card, "", BF_FONT_L, pal()->text);
    lv_obj_set_style_text_align(kp_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(kp_lbl, LV_PCT(100));
    lv_obj_move_to_index(kp_lbl, 2);
    lv_obj_t *m = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(m, map);
    lv_obj_set_size(m, LV_PCT(100), 200);
    lv_obj_set_style_text_font(m, BF_FONT_L, 0);
    lv_obj_add_event_cb(m, kp_click, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_move_to_index(m, 3);
    kp_show();
}

/* ------------------------------------------------------------ transmitir */

static void on_tx(lv_event_t *e)
{
    (void)e;
    if(fm_busy()) return;
    if(cur.state == FM_ON || cur.state == FM_STARTING) { fm_off(); return; }
    if(cur.state == FM_UNAVAILABLE) return;
    fm_conf_t *c = fm_conf();
    fm_conf_save();
    fm_on(c->freq, c->mode);
}

static void on_mode(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    fm_conf_t *c = fm_conf();
    if(c->mode == m) return;
    c->mode = m;
    fm_conf_save();
    if(cur.state == FM_ON) fm_set_mode(m);
}

static const int timers[] = { 0, 15, 30, 60, 120 };

static void timer_text(void)
{
    char b[64];
    int t = fm_conf()->timer_min;
    time_t at = fm_off_at();
    if(cur.state == FM_ON && at) {
        long left = (long)(at - time(NULL)) / 60 + 1;
        snprintf(b, sizeof b, tr(S_FM_TIMER_LEFT), left);
    } else if(!t) snprintf(b, sizeof b, "%s", tr(S_FM_TIMER_OFF));
    else if(t < 60) snprintf(b, sizeof b, tr(S_FM_TIMER_MIN), t);
    else snprintf(b, sizeof b, tr(S_FM_TIMER_H), t / 60);
    set_text(timer_lbl, b);
}

static void on_timer(lv_event_t *e)
{
    (void)e;
    fm_conf_t *c = fm_conf();
    int k = 0;
    for(int i = 0; i < 5; i++) if(timers[i] == c->timer_min) k = i;
    c->timer_min = timers[(k + 1) % 5];
    fm_conf_save();
    timer_text();
}

static void on_tone(lv_event_t *e)
{
    (void)e;
    if(fm_tone_running()) fm_tone_stop();
    else fm_tone(8);
}

/* ----- diagnóstico */

static void on_diag(lv_event_t *e)
{
    (void)e;
    char b[512], fr[16] = "—", up[32] = "—";
    if(cur.state == FM_ON) {
        fm_format(cur.freq, fr, sizeof fr);
        long s = cur.since ? (long)(time(NULL) - cur.since) : 0;
        snprintf(up, sizeof up, "%ld min", s / 60);
    }
    const char *st = cur.state == FM_ON ? tr(S_FM_ST_ON_SHORT) : cur.state == FM_STARTING ? tr(S_FM_ST_STARTING)
                   : cur.state == FM_ERROR ? tr(S_FM_ST_ERROR) : cur.state == FM_UNAVAILABLE ? tr(S_FM_ST_UNAVAIL)
                   : tr(S_FM_ST_OFF);
    snprintf(b, sizeof b, tr(S_FM_DIAG_BODY), st, fr, cur.speaker_off ? tr(S_FM_OUT_FM) : tr(S_FM_OUT_BOTH), up,
             cur.error[0] ? cur.error : fm_last_result()[0] ? fm_last_result() : "—");
    const char *btns[] = { tr(S_CLOSE) };
    dialog_open(BF_SYMBOL_INFO, lv_color_hex(GL_LIGHT), tr(S_FM_DIAG), b, btns, 1, 0, -1, NULL, NULL);
}

/* ------------------------------------------------------------ favoritos */

static void build_favs(void);
static int fav_sel;
static lv_obj_t *ren_ta, *ren_kb;

static void on_fav(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    fm_conf_t *c = fm_conf();
    if(i < 0 || i >= c->nfav) return;
    if(!fm_freq_valid(c->fav[i].freq)) {                   /* frequência salva fora da faixa: avisa */
        const char *btns[] = { tr(S_CLOSE) };
        dialog_open(LV_SYMBOL_WARNING, lv_color_hex(C_AMBER), tr(S_FM_INVALID), tr(S_FM_RANGE), btns, 1, 0, -1, NULL, NULL);
        return;
    }
    choose(c->fav[i].freq);
}

static void ren_close(void) { if(ren_kb) { lv_obj_delete(ren_kb); ren_kb = NULL; } }

static void ren_answer(int b, void *ud)
{
    (void)ud;
    fm_conf_t *c = fm_conf();
    if(b == 1 && ren_ta && fav_sel >= 0 && fav_sel < c->nfav) {
        const char *t = lv_textarea_get_text(ren_ta);
        if(t[0]) {
            snprintf(c->fav[fav_sel].name, sizeof c->fav[0].name, "%s", t);
            for(char *p = c->fav[fav_sel].name; *p; p++) if(*p == '|' || *p == '\n') *p = ' ';
            fm_conf_save();
            build_favs();
        }
    }
    ren_ta = NULL;
    ren_close();
}

static void ren_kb_done(lv_event_t *e) { (void)e; ren_close(); }

static void rename_fav(int i)
{
    fav_sel = i;
    const char *btns[] = { tr(S_CANCEL), "OK" };
    lv_obj_t *card = dialog_open(LV_SYMBOL_EDIT, lv_color_hex(GL_LIGHT), tr(S_FM_RENAME), NULL, btns, 2, 1, -1,
                                 ren_answer, NULL);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
    ren_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(ren_ta, true);
    lv_textarea_set_max_length(ren_ta, 30);
    lv_textarea_set_text(ren_ta, fm_conf()->fav[i].name);
    lv_obj_set_width(ren_ta, LV_PCT(100));
    lv_obj_set_style_text_font(ren_ta, BF_FONT_M, 0);
    lv_obj_move_to_index(ren_ta, 1);
    ren_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(ren_kb, BF_SCREEN_W, 200);
    lv_obj_align(ren_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(ren_kb, BF_FONT_M, 0);
    lv_keyboard_set_textarea(ren_kb, ren_ta);
    lv_obj_add_event_cb(ren_kb, ren_kb_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ren_kb, ren_kb_done, LV_EVENT_CANCEL, NULL);
}

static void fav_menu_answer(int b, void *ud)
{
    (void)ud;
    fm_conf_t *c = fm_conf();
    int i = fav_sel;
    if(i < 0 || i >= c->nfav) return;
    if(b == 0 && i > 0) {                                   /* para cima (o 1º é o padrão) */
        fm_fav_t t = c->fav[i - 1]; c->fav[i - 1] = c->fav[i]; c->fav[i] = t;
    } else if(b == 1) { rename_fav(i); return; }
    else if(b == 2) {
        memmove(&c->fav[i], &c->fav[i + 1], sizeof c->fav[0] * (size_t)(c->nfav - i - 1));
        c->nfav--;
    } else return;
    fm_conf_save();
    build_favs();
}

static void on_fav_long(lv_event_t *e)
{
    fav_sel = (int)(intptr_t)lv_event_get_user_data(e);
    fm_conf_t *c = fm_conf();
    if(fav_sel < 0 || fav_sel >= c->nfav) return;
    char t[64], f[16];
    fm_format(c->fav[fav_sel].freq, f, sizeof f);
    snprintf(t, sizeof t, "%s \xC2\xB7 %s MHz", c->fav[fav_sel].name, f);
    const char *btns[] = { tr(S_FM_UP), tr(S_FM_RENAME), tr(S_FM_DELETE), tr(S_CANCEL) };
    dialog_open(LV_SYMBOL_LIST, lv_color_hex(GL_LIGHT), t, tr(S_FM_FAV_HELP), btns, 4, -1, 2, fav_menu_answer, NULL);
}

static void on_fav_add(lv_event_t *e)
{
    (void)e;
    fm_conf_t *c = fm_conf();
    for(int i = 0; i < c->nfav; i++) if(c->fav[i].freq == c->freq) return;   /* já está */
    if(c->nfav >= FM_MAXFAV) {
        const char *btns[] = { tr(S_CLOSE) };
        dialog_open(LV_SYMBOL_WARNING, lv_color_hex(C_AMBER), tr(S_FM_FAVS), tr(S_FM_FAV_FULL), btns, 1, 0, -1, NULL, NULL);
        return;
    }
    fm_fav_t *v = &c->fav[c->nfav++];
    v->freq = c->freq;
    snprintf(v->name, sizeof v->name, tr(S_FM_FAV_N), c->nfav);
    fm_conf_save();
    build_favs();
}

static void build_favs(void)
{
    if(!fav_list) return;
    lv_obj_clean(fav_list);
    fm_conf_t *c = fm_conf();
    if(!c->nfav) {
        lv_obj_t *l = bf_label(fav_list, tr(S_FM_NO_FAVS), BF_FONT_S, C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }
    for(int i = 0; i < c->nfav; i++) {
        lv_obj_t *r = bf_plain(fav_list);
        lv_obj_set_size(r, LV_PCT(100), 44);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r, 10, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_bg_color(r, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_30, LV_STATE_PRESSED);
        bool ok = fm_freq_valid(c->fav[i].freq);
        lv_obj_t *n = bf_label(r, c->fav[i].name, BF_FONT_M, C_WHITE);
        lv_obj_set_flex_grow(n, 1);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        char f[24];
        fm_format(c->fav[i].freq, f, sizeof f);
        if(!ok) snprintf(f + strlen(f), sizeof f - strlen(f), " %s", LV_SYMBOL_WARNING);
        bf_label(r, f, BF_FONT_M, ok ? lv_color_hex(GL_ACTIVE) : lv_color_hex(C_AMBER));
        lv_obj_add_event_cb(r, on_fav, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_fav_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

/* ------------------------------------------------------------ estado */

static void refresh(void)
{
    fm_read(&cur);
    fm_conf_t *c = fm_conf();
    /* frequência: a do hardware quando ligado (e o usuário não está mexendo) */
    if(cur.state == FM_ON && !apply_at && !fm_busy() && cur.freq && cur.freq != c->freq) { c->freq = cur.freq; }
    show_freq(c->freq);
    if(apply_at && ms_now() >= apply_at && !fm_busy()) {
        apply_at = 0;
        fm_conf_save();
        if(cur.state == FM_ON && cur.freq != c->freq) fm_set_freq(c->freq);
    }
    char b[96], f[16];
    uint32_t col;
    const char *btn;
    const char *ic = LV_SYMBOL_POWER;
    if(fm_busy() && cur.state != FM_ON && cur.state != FM_ERROR) cur.state = FM_STARTING;
    switch(cur.state) {
    case FM_ON:
        fm_format(cur.freq, f, sizeof f);
        snprintf(b, sizeof b, tr(S_FM_ST_ON), f);
        col = C_GREEN; btn = tr(S_FM_TX_ON); break;
    case FM_STARTING: snprintf(b, sizeof b, "%s", tr(S_FM_ST_STARTING)); col = C_AMBER; btn = tr(S_FM_TX_STARTING); break;
    case FM_ERROR:
        snprintf(b, sizeof b, "%s", tr(S_FM_ST_ERROR));
        col = C_RED; btn = tr(S_FM_TX_OFF); break;
    case FM_UNAVAILABLE: snprintf(b, sizeof b, "%s", tr(S_FM_ST_UNAVAIL)); col = C_GRAY; btn = tr(S_FM_TX_UNAVAIL); ic = LV_SYMBOL_WARNING; break;
    default: snprintf(b, sizeof b, "%s", tr(S_FM_ST_OFF)); col = C_GRAY; btn = tr(S_FM_TX_OFF); break;
    }
    set_text(st_lbl, b);
    lv_obj_set_style_bg_color(st_dot, lv_color_hex(col), 0);
    set_text(tx_lbl, btn);
    set_text(tx_icon, ic);
    if(cur.state == FM_ON) grad(tx_btn, 0x5BE3A0, 0x1E9A5E, 0);
    else if(cur.state == FM_STARTING) grad(tx_btn, 0xFFD27A, 0xDB9226, 0);
    else {
        lv_obj_set_style_bg_color(tx_btn, lv_color_white(), 0);
        lv_obj_set_style_bg_grad_dir(tx_btn, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_opa(tx_btn, LV_OPA_20, 0);
    }
    if(cur.state == FM_UNAVAILABLE) lv_obj_add_state(tx_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(tx_btn, LV_STATE_DISABLED);
    /* informação à direita do estado */
    if(cur.state == FM_ON && cur.since) {
        long m = (long)(time(NULL) - cur.since) / 60;
        snprintf(b, sizeof b, tr(S_FM_UPTIME), m);
    } else snprintf(b, sizeof b, "KT0801A");
    set_text(st_info, b);
    for(int i = 0; i < 2; i++) {
        bool sel = c->mode == i;
        lv_obj_t *desc = lv_obj_get_child(lv_obj_get_child(mode_row[i], 1), 1);
        if(sel && lv_obj_get_height(root) >= 400) lv_obj_remove_flag(desc, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(mode_row[i], sel ? lv_color_hex(GL_ACTIVE) : lv_color_white(), 0);
        lv_obj_set_style_border_opa(mode_row[i], sel ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_t *dot = lv_obj_get_child(lv_obj_get_child(mode_row[i], 0), 0);
        if(dot) { if(sel) lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN); }
    }
    set_text(hint, tr(cur.state == FM_ON ? S_FM_HINT_ON : cur.state == FM_ERROR ? S_FM_HINT_ERR : S_FM_HINT));
    set_text(tone_lbl, tr(fm_tone_running() ? S_FM_TONE_STOP : S_FM_TONE));
    timer_text();
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

/* ------------------------------------------------------------ montagem */

static lv_obj_t *mode_option(lv_obj_t *p, int i, bf_str_t name, bf_str_t desc)
{
    lv_obj_t *r = bf_plain(p);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(r, 8, 0);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_t *ring = bf_plain(r);
    lv_obj_set_size(ring, 22, 22);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(GL_ACTIVE), 0);
    lv_obj_t *in = bf_plain(ring);
    lv_obj_set_size(in, 12, 12);
    lv_obj_set_style_radius(in, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(in, lv_color_hex(GL_ACTIVE), 0);
    lv_obj_set_style_bg_opa(in, LV_OPA_COVER, 0);
    lv_obj_center(in);
    lv_obj_t *c = bf_plain(r);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    bf_label(c, tr(name), BF_FONT_M, C_WHITE);
    lv_obj_t *d = bf_label(c, tr(desc), BF_FONT_S, C_DIM);
    lv_obj_set_width(d, LV_PCT(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(r, on_mode, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    mode_row[i] = r;
    return r;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(root, 10, 0);
    lv_obj_update_layout(root);
    bool tall = lv_obj_get_content_height(root) >= 400;

    /* ---------- esquerda: transmissor */
    lv_obj_t *left = gcard(root);
    lv_obj_set_size(left, 490, LV_PCT(100));
    lv_obj_set_style_pad_all(left, tall ? 18 : 12, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *top = bf_plain(left);                      /* estado */
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    st_dot = bf_plain(top);
    lv_obj_set_size(st_dot, 12, 12);
    lv_obj_set_style_radius(st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(st_dot, LV_OPA_COVER, 0);
    st_lbl = bf_label(top, "", BF_FONT_M, C_WHITE);
    lv_obj_set_flex_grow(st_lbl, 1);
    lv_label_set_long_mode(st_lbl, LV_LABEL_LONG_DOT);
    st_info = bf_label(top, "", BF_FONT_S, C_DIM);         /* toque: diagnóstico */
    lv_obj_add_flag(st_info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(st_info, 14);
    lv_obj_add_event_cb(st_info, on_diag, LV_EVENT_CLICKED, NULL);

    /* mostrador grande: 99 . 5 MHz (toque: digitar) */
    f_box = bf_plain(left);
    lv_obj_set_size(f_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(f_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(f_box, 6, 0);
    lv_obj_add_flag(f_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(f_box, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(f_box, on_keypad, LV_EVENT_CLICKED, NULL);
    f_int = bf_label(f_box, "88", BF_FONT_CLOCK, C_WHITE);
    lv_obj_t *dot = bf_plain(f_box);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_color(dot, C_WHITE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_bottom(dot, 8, 0);
    f_dec = bf_label(f_box, "1", BF_FONT_CLOCK, C_WHITE);
    lv_obj_t *mhz = bf_label(f_box, "MHz", BF_FONT_L, lv_color_hex(GL_ACTIVE));
    lv_obj_set_style_margin_bottom(mhz, 14, 0);
    lv_obj_set_style_margin_left(mhz, 6, 0);

    /* sintonia: -0,1  régua  +0,1 */
    lv_obj_t *tune = bf_plain(left);
    lv_obj_set_size(tune, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tune, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tune, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tune, 14, 0);
    minus_b = gbtn(tune, LV_SYMBOL_LEFT, "0,1");
    lv_obj_set_size(minus_b, 92, 56);
    lv_obj_add_event_cb(minus_b, on_step, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(minus_b, on_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
    slider = lv_slider_create(tune);
    lv_slider_set_range(slider, FM_MIN, FM_MAX);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 10);
    lv_obj_set_style_bg_color(slider, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(slider, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(GL_ACTIVE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 9, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 22);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    plus_b = gbtn(tune, NULL, "0,1 " LV_SYMBOL_RIGHT);
    lv_obj_set_size(plus_b, 92, 56);
    lv_obj_add_event_cb(plus_b, on_step, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_add_event_cb(plus_b, on_step, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
    if(i18n_get_lang() == LANG_EN) {                     /* 0.1 em inglês */
        lv_label_set_text(lv_obj_get_child(minus_b, 1), "0.1");
        lv_label_set_text(lv_obj_get_child(plus_b, 0), "0.1 " LV_SYMBOL_RIGHT);
    }

    /* transmitir */
    tx_btn = bf_button(left, NULL, NULL, 0);
    lv_obj_set_size(tx_btn, LV_PCT(100), tall ? 68 : 58);
    lv_obj_set_style_radius(tx_btn, 34, 0);
    lv_obj_set_style_shadow_width(tx_btn, 0, 0);
    lv_obj_set_style_border_color(tx_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(tx_btn, 1, 0);
    lv_obj_set_style_border_opa(tx_btn, LV_OPA_40, 0);
    lv_obj_set_style_opa(tx_btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_column(tx_btn, 12, 0);
    tx_icon = bf_label(tx_btn, LV_SYMBOL_POWER, BF_FONT_L, C_WHITE);
    tx_lbl = bf_label(tx_btn, "", BF_FONT_L, C_WHITE);
    lv_obj_add_event_cb(tx_btn, on_tx, LV_EVENT_CLICKED, NULL);
    hint = bf_label(left, "", BF_FONT_S, C_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    /* ---------- direita: saída, favoritos, teste e temporizador */
    lv_obj_t *right = bf_plain(root);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);

    lv_obj_t *out = gcard(right);
    lv_obj_set_size(out, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(out, 10, 0);
    lv_obj_set_flex_flow(out, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(out, 6, 0);
    title(out, tr(S_FM_OUTPUT));
    mode_option(out, 0, S_FM_OUT_FM, S_FM_OUT_FM_D);
    mode_option(out, 1, S_FM_OUT_BOTH, S_FM_OUT_BOTH_D);
    /* a descrição fica só na opção escolhida (sobra lugar para os favoritos) */

    lv_obj_t *fv = gcard(right);
    lv_obj_set_width(fv, LV_PCT(100));
    lv_obj_set_flex_grow(fv, 1);
    lv_obj_set_style_pad_all(fv, 10, 0);
    lv_obj_set_flex_flow(fv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fv, 6, 0);
    lv_obj_t *fh = bf_plain(fv);
    lv_obj_set_size(fh, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    title(fh, tr(S_FM_FAVS));
    lv_obj_t *add = gbtn(fh, LV_SYMBOL_PLUS, tr(S_FM_SAVE));
    lv_obj_set_height(add, 36);
    lv_obj_set_style_text_font(add, BF_FONT_S, 0);
    lv_obj_add_event_cb(add, on_fav_add, LV_EVENT_CLICKED, NULL);
    fav_list = bf_plain(fv);
    lv_obj_set_width(fav_list, LV_PCT(100));
    lv_obj_set_flex_grow(fav_list, 1);
    lv_obj_set_flex_flow(fav_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fav_list, 4, 0);
    bf_scrollable(fav_list);

    lv_obj_t *bot = bf_plain(right);
    lv_obj_set_size(bot, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bot, 8, 0);
    lv_obj_t *tb = gbtn(bot, LV_SYMBOL_AUDIO, NULL);
    lv_obj_set_size(tb, 104, 48);
    tone_lbl = bf_label(tb, "", BF_FONT_S, C_WHITE);
    lv_obj_add_event_cb(tb, on_tone, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tm = gbtn(bot, BF_SYMBOL_CLOCK, NULL);
    lv_obj_set_height(tm, 48);
    lv_obj_set_flex_grow(tm, 1);
    timer_lbl = bf_label(tm, "", BF_FONT_S, C_WHITE);
    lv_obj_add_event_cb(tm, on_timer, LV_EVENT_CLICKED, NULL);

    shown_freq = -1;
    apply_at = 0;
    build_favs();
    refresh();
    tick = lv_timer_create(tick_cb, 400, NULL);
}

static void destroy(void)
{
    if(tick) lv_timer_delete(tick);
    tick = NULL;
    if(apply_at) {                                       /* saiu mexendo: aplica a última escolha */
        apply_at = 0;
        fm_conf_save();
        fm_info_t i;
        fm_read(&i);
        if(i.state == FM_ON && i.freq != fm_conf()->freq) fm_set_freq(fm_conf()->freq);
    }
    ren_close();
    root = fav_list = NULL;
}

const bf_screen_def_t scr_fm_def = { create, destroy, S_SET_FM, NULL };

/* --screenshot: 0 normal, 1 digitar a frequência, 2 diagnóstico */
void scr_fm_demo(int step)
{
    if(!root) return;
    if(step == 1) on_keypad(NULL);
    else if(step == 2) on_diag(NULL);
    else refresh();
}
