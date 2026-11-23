/* scr_phone.c — Telefone: discador, contatos, recentes e mensagens do celular conectado por
 * Bluetooth, e o painel da ligação (atender, desligar, teclas). Tudo pelo bfphone
 * (core/phonestat): estado relido a cada 500 ms, comandos pelo FIFO.
 *
 * Os dados (agenda, histórico, mensagens) só existem depois que o usuário toca em "Importar do
 * celular": o bfphone os grava no cartão (BigFoston/telefone/<aparelho>/, arquivos .tsv no formato do
 * src/bt/bfpbap.c); "Apagar dados" os remove. Nada disso vai para logs.
 *
 * A voz da ligação fica no celular (o FOSTON ainda não toca a voz: docs/phone). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/phonestat.h"
#include "../widgets/page.h"
#include "../widgets/dialog.h"
#include "../widgets/glass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define C_PHONE 0x2ec27e
#define MAX_ROWS 60

enum { TAB_DIAL, TAB_CONTACTS, TAB_RECENTS, TAB_MSGS, NTABS };

static lv_obj_t *root, *tabbar, *tab_btn[NTABS], *body, *status_lbl, *status2_lbl, *call_panel, *st_bars, *st_bat;
static lv_obj_t *num_lbl, *search_ta, *search_kb, *list, *more_lbl, *sync_lbl;
static lv_obj_t *cp_name, *cp_num, *cp_state, *cp_answer, *cp_keys, *cp_dtmf, *cp_note, *cp_vphone, *cp_vfoston;
static lv_timer_t *tick;
static ph_info_t cur;
static int tab;
static char number[40];
static char sync_seen[200];
static char call_sig[256];

/* ------------------------------------------------------------ dados */

typedef struct { char *buf; char **row; int n; } table_t;
static table_t contacts;
static bool erase_pending;            /* "Apagar" pedido: listas vazias até o bfphone apagar */

static void table_free(table_t *t)
{
    free(t->buf);
    free(t->row);
    memset(t, 0, sizeof *t);
}

/* carrega um TSV inteiro (até 8 MB) e indexa as linhas */
static bool table_load(table_t *t, const char *name)
{
    table_free(t);
    const char *d = cur.data[0] ? cur.data : ph_last_data();
    if(!d[0] || erase_pending) return false;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", d, name);
    FILE *f = fopen(path, "r");
    if(!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if(sz <= 0 || sz > (8L << 20)) { fclose(f); return false; }
    t->buf = malloc((size_t)sz + 1);
    if(!t->buf) { fclose(f); return false; }
    size_t got = fread(t->buf, 1, (size_t)sz, f);
    fclose(f);
    t->buf[got] = 0;
    int lines = 0;
    for(size_t i = 0; i < got; i++) if(t->buf[i] == '\n') lines++;
    t->row = malloc(sizeof(char *) * (size_t)(lines + 1));
    if(!t->row) { table_free(t); return false; }
    for(char *s = t->buf, *e; *s; s = e + 1) {
        e = strchr(s, '\n');
        if(!e) { t->row[t->n++] = s; break; }
        *e = 0;
        if(*s) t->row[t->n++] = s;
    }
    return true;
}

/* campo n (0 = primeiro) de uma linha TSV, copiado */
static void col(const char *row, int n, char *out, size_t on)
{
    for(int i = 0; i < n && row; i++) { row = strchr(row, '\t'); if(row) row++; }
    out[0] = 0;
    if(!row) return;
    size_t k = 0;
    while(row[k] && row[k] != '\t' && k + 1 < on) { out[k] = row[k]; k++; }
    out[k] = 0;
}

/* "20261122T140312" -> "22/11 14:03" */
static void fmt_when(const char *w, char *out, size_t n)
{
    if(strlen(w) >= 13 && w[8] == 'T')
        snprintf(out, n, "%.2s/%.2s %.2s:%.2s", w + 6, w + 4, w + 9, w + 11);
    else snprintf(out, n, "%s", w);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void dial(const char *num)
{
    if(!num || !num[0]) return;
    ph_cmd("dial %s", num);
}

/* ---------------------------------------------------------- discador */

static void num_update(void)
{
    set_text(num_lbl, number[0] ? number : tr(S_PH_TYPE_NUMBER));
    lv_obj_set_style_text_color(num_lbl, lv_color_hex(number[0] ? GL_WHITE : GL_TEXT2), 0);
}

static void on_key(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(number);
    if(n + 1 < sizeof number) { number[n] = k[0]; number[n + 1] = 0; }
    num_update();
}

static void on_back(lv_event_t *e) { (void)e; size_t n = strlen(number); if(n) number[n - 1] = 0; num_update(); }
static void on_clear(lv_event_t *e) { if(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) { number[0] = 0; num_update(); } }

static void on_call(lv_event_t *e)
{
    (void)e;
    if(number[0]) dial(number);
    else ph_cmd("redial");
}

/* ------------------------------------------------------------ visual (vidro) */

/* O Telefone usa as peças do Foston Glass: cartões translúcidos sobre o papel de parede, texto
 * claro, ícones em degradê. Só cor sólida, degradê vertical e transparência (sem desfoque). */
#define T_WHITE lv_color_hex(GL_WHITE)
#define T_DIM   lv_color_hex(GL_TEXT2)

typedef struct { uint32_t top, bottom; } grad_t;
static const grad_t G_GREEN = { 0x5BE3A0, 0x1E9A5E }, G_BLUE = { GL_LIGHT, GL_BLUE },
                    G_LAV = { GL_LAVENDER, GL_LAVBLUE }, G_RED = { 0xFF8E8E, 0xD04545 },
                    G_AMBER = { 0xFFD27A, 0xDB9226 }, G_SKY = { GL_SKY, 0x3E86C9 };

static lv_obj_t *gcard(lv_obj_t *parent)
{
    lv_obj_t *c = bf_card(parent);
    lv_obj_set_style_bg_color(c, lv_color_hex(GL_BLUE), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, 0);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    return c;
}

static void grad_bg(lv_obj_t *o, grad_t g, lv_state_t s)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(g.top), s);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(g.bottom), s);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, s);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, s);
}

/* botão de vidro; kind 1 = verde (ligar/enviar), 2 = vermelho; marcado = azul claro */
static lv_obj_t *gbtn(lv_obj_t *parent, const char *sym, const char *txt, int kind)
{
    lv_obj_t *b = bf_button(parent, sym, txt, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, T_WHITE, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    if(kind == 1) grad_bg(b, G_GREEN, 0);
    else if(kind == 2) grad_bg(b, G_RED, 0);
    else {
        lv_obj_set_style_bg_color(b, lv_color_hex(GL_LIGHT), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(GL_BLUE), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    }
    lv_obj_set_style_bg_opa(b, kind ? LV_OPA_70 : LV_OPA_60, LV_STATE_PRESSED);
    grad_bg(b, G_SKY, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, T_WHITE, LV_STATE_CHECKED);
    return b;
}

/* ícone redondo em degradê (listas) */
static lv_obj_t *gicon(lv_obj_t *parent, const char *sym, grad_t g, int size)
{
    lv_obj_t *t = glass_tile(parent, size, size / 3, g.top, g.bottom, NULL, sym, BF_FONT_M);
    return t;
}

/* iniciais do nome num círculo colorido (contatos e conversas) */
static lv_obj_t *avatar(lv_obj_t *parent, const char *name, int size)
{
    static const grad_t pal5[] = { { 0x7FC8F8, 0x2F6FB8 }, { 0xB9A6F2, 0x6D5BC9 }, { 0x6FE3B4, 0x1F9A6E },
                                   { 0xFFB98A, 0xD96A3B }, { 0xF59AC0, 0xC24C82 } };
    unsigned h = 0;
    for(const char *c = name; *c; c++) h = h * 31 + (unsigned char)*c;
    char ini[8] = "";
    int k = 0, words = 0;
    for(const char *c = name; *c && words < 2; c++) {
        bool start = c == name || c[-1] == ' ';
        if(!start || *c == ' ') continue;
        unsigned char u = (unsigned char)*c;
        if(u >= 0x80) {                                  /* letra acentuada: copia a sequência UTF-8 */
            int n = (u & 0xe0) == 0xc0 ? 2 : (u & 0xf0) == 0xe0 ? 3 : 1;
            for(int i = 0; i < n && c[i] && k < 6; i++) ini[k++] = c[i];
        } else if((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) ini[k++] = (char)(u >= 'a' ? u - 32 : u);
        else if(u >= '0' && u <= '9' && !words) { ini[k++] = '#'; words = 2; break; }
        else continue;
        words++;
    }
    ini[k] = 0;
    lv_obj_t *a = glass_tile(parent, size, LV_RADIUS_CIRCLE, pal5[h % 5].top, pal5[h % 5].bottom, NULL,
                             ini[0] ? ini : LV_SYMBOL_CALL, BF_FONT_M);
    return a;
}

/* ---------------------------------------------------------- teclado de texto (pt-BR) */

static const char *const kb_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xA7", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "?", "\n",
    LV_SYMBOL_KEYBOARD, "1#", "\xC3\xA1\xC3\xA3", " ", LV_SYMBOL_OK, ""
};
static const char *const kb_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x87", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "!", "\n",
    LV_SYMBOL_KEYBOARD, "1#", "\xC3\xA1\xC3\xA3", " ", LV_SYMBOL_OK, ""
};
static const char *const kb_acc[] = {
    "\xC3\xA1", "\xC3\xA0", "\xC3\xA2", "\xC3\xA3", "\xC3\xA9", "\xC3\xAA", "\xC3\xAD", "\xC3\xB3", "\xC3\xB4", "\xC3\xB5", "\n",
    "\xC3\xBA", "\xC3\xBC", "\xC3\xA7", "\xC3\x81", "\xC3\x83", "\xC3\x89", "\xC3\x8D", "\xC3\x93", "\xC3\x95", "\xC3\x87", "\n",
    "abc", "-", "'", "\"", ":", ";", "(", ")", "@", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, "1#", " ", LV_SYMBOL_OK, ""
};
#define KF LV_KEYBOARD_CTRL_BUTTON_FLAGS
static const lv_buttonmatrix_ctrl_t kb_ctrl_txt[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 4, KF | 4, KF | 4, 18, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_acc[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 6, 4, 4, 4, 4, 4, 4, 4, 4, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
    KF | 4, KF | 4, 22, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
};

static lv_obj_t *search_kb_ta;
static void kb_close(lv_event_t *e);

/* o botão "áã" troca para os acentos; o resto é o tratamento normal do LVGL */
static void kb_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *t = lv_buttonmatrix_get_button_text(kb, id);
    if(t && !strcmp(t, "\xC3\xA1\xC3\xA3")) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1); return; }
    lv_keyboard_def_event_cb(e);
}

static lv_obj_t *kb_open(lv_obj_t *ta, lv_event_cb_t on_done)
{
    lv_obj_t *kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(kb, BF_SCREEN_W, 200);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, BF_FONT_M, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(GL_BLUE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, T_WHITE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1E4878), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(GL_LIGHT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lc, kb_ctrl_txt);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_uc, kb_ctrl_txt);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_acc, kb_ctrl_acc);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_done, LV_EVENT_CANCEL, NULL);
    return kb;
}

/* campo de texto de vidro */
static lv_obj_t *gfield(lv_obj_t *parent, const char *placeholder, int max)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, BF_FONT_M, 0);
    lv_obj_set_style_text_color(ta, T_WHITE, 0);
    lv_obj_set_style_text_color(ta, T_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_70, 0);
    lv_obj_set_style_border_color(ta, lv_color_white(), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_30, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(GL_ACTIVE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(ta, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(GL_ACTIVE), LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta, lv_color_hex(GL_ACTIVE), LV_PART_CURSOR | LV_STATE_FOCUSED);
    return ta;
}

/* ---------------------------------------------------------- discador */

static const char *const key_letters[12] = { "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", "" };

static lv_obj_t *keypad(lv_obj_t *parent, lv_event_cb_t cb, int w, int h)
{
    static const char *const keys[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    lv_obj_t *g = bf_plain(parent);
    lv_obj_set_size(g, 3 * w + 2 * BF_SP_S, 4 * h + 3 * BF_SP_S);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, BF_SP_S, 0);
    bool letters = h >= 48;
    for(int i = 0; i < 12; i++) {
        lv_obj_t *b = gbtn(g, NULL, NULL, 0);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_min_height(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_pad_row(b, 0, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        bf_label(b, keys[i], letters ? BF_FONT_L : BF_FONT_M, T_WHITE);
        if(letters && key_letters[i][0]) bf_label(b, key_letters[i], BF_FONT_S, T_DIM);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)keys[i]);
    }
    return g;
}

static void build_dial(void)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, BF_SP_L, 0);
    lv_obj_update_layout(body);
    int kh = (lv_obj_get_content_height(body) - 3 * BF_SP_S) / 4;
    if(kh > 76) kh = 76;
    if(kh < 40) kh = 40;
    keypad(body, on_key, 112, kh);
    lv_obj_t *right = gcard(body);                 /* cartão: legível sobre o papel de parede */
    lv_obj_set_size(right, 380, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(right, BF_SP_L, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, BF_SP_M, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = bf_plain(right);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    num_lbl = bf_label(row, "", BF_FONT_L, T_WHITE);
    lv_obj_set_flex_grow(num_lbl, 1);
    lv_label_set_long_mode(num_lbl, LV_LABEL_LONG_DOT);
    lv_obj_t *bs = gbtn(row, LV_SYMBOL_BACKSPACE, NULL, 0);
    lv_obj_set_size(bs, 68, 52);
    lv_obj_add_event_cb(bs, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bs, on_clear, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *line = bf_plain(right);              /* fio fino sob o número */
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_30, 0);
    lv_obj_t *call = gbtn(right, LV_SYMBOL_CALL, tr(S_PH_CALL), 1);
    lv_obj_set_size(call, LV_PCT(100), 68);
    lv_obj_set_style_radius(call, 34, 0);
    lv_obj_set_style_text_font(call, BF_FONT_L, 0);
    lv_obj_add_event_cb(call, on_call, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hint = bf_label(right, tr(S_PH_REDIAL_HINT), BF_FONT_S, T_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    num_update();
}

/* ---------------------------------------------------------- listas */

static void import_answer(int btn, void *ud)
{
    (void)ud;
    if(btn == 1) ph_cmd("sync");
}

static void on_import(lv_event_t *e)
{
    (void)e;
    const char *btns[] = { tr(S_CANCEL), tr(S_PH_IMPORT) };
    dialog_open(LV_SYMBOL_DOWNLOAD, lv_color_hex(C_PHONE), tr(S_PH_IMPORT_Q), tr(S_PH_IMPORT_BODY), btns, 2, 1, -1,
                import_answer, NULL);
}

static void build_tab(void);

static void erase_answer(int btn, void *ud)
{
    (void)ud;
    if(btn != 1) return;
    if(!ph_cmd("erase")) return;
    erase_pending = true;
    build_tab();                          /* solta as linhas antes de liberar a agenda */
    table_free(&contacts);
}

static void on_erase(lv_event_t *e)
{
    (void)e;
    if(!strcmp(cur.sync, "andamento")) return;      /* o bfphone não apaga durante a importação */
    const char *btns[] = { tr(S_CANCEL), tr(S_PH_ERASE) };
    dialog_open(LV_SYMBOL_TRASH, pal()->danger, tr(S_PH_ERASE_Q), tr(S_PH_ERASE_BODY), btns, 2, -1, 1,
                erase_answer, NULL);
}

static const bf_str_t auto_name[4] = { S_PH_AUTO_OFF, S_PH_AUTO_NEW, S_PH_AUTO_RECENT, S_PH_AUTO_ALL };
static const bf_str_t auto_desc[4] = { S_PH_AUTO_OFF_D, S_PH_AUTO_NEW_D, S_PH_AUTO_RECENT_D, S_PH_AUTO_ALL_D };

static void sync_text(char *b, size_t n)
{
    if(!strcmp(cur.sync, "andamento")) { snprintf(b, n, "%s", tr(S_PH_IMPORTING)); return; }
    const char *d = cur.data[0] ? cur.data : ph_last_data();
    char path[160];
    snprintf(path, sizeof path, "%s/sincronizado", d);
    FILE *f = d[0] ? fopen(path, "r") : NULL;
    long t = 0;
    if(f) { if(fscanf(f, "%ld", &t) != 1) t = 0; fclose(f); }
    size_t k = 0;
    if(!t) k = (size_t)snprintf(b, n, "%s", tr(S_PH_NODATA));
    else {
        time_t tt = (time_t)t;
        struct tm tm;
        localtime_r(&tt, &tm);
        char w[32];
        strftime(w, sizeof w, "%d/%m %H:%M", &tm);
        k = (size_t)snprintf(b, n, tr(S_PH_SYNCED), w);
    }
    if(tab == TAB_MSGS && k < n) {                /* nas mensagens: o modo automático */
        int m = cur.msg_auto >= 0 && cur.msg_auto <= 3 ? cur.msg_auto : 0;
        char a[96];
        snprintf(a, sizeof a, tr(S_PH_AUTO_FMT), tr(auto_name[m]));
        snprintf(b + k, n - k, "\n%s%s", a, cur.notify ? " \xE2\x80\xA2 " : "");
        if(cur.notify) { size_t l = strlen(b); snprintf(b + l, n - l, "%s", tr(S_PH_NOTIFY_ON)); }
    }
}

/* aviso dentro da lista (num cartão, legível sobre o papel de parede) */
static lv_obj_t *list_note(const char *txt)
{
    lv_obj_t *c = gcard(list);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, BF_SP_L, 0);
    lv_obj_t *l = bf_label(c, txt, BF_FONT_M, T_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return c;
}

/* ----- importação automática das mensagens (escolha) */

static lv_obj_t *auto_rows[4];

static void auto_mark(int m)
{
    for(int i = 0; i < 4; i++) {
        if(!auto_rows[i]) continue;
        lv_obj_t *dot = lv_obj_get_child(auto_rows[i], 0);
        lv_obj_t *in = dot ? lv_obj_get_child(dot, 0) : NULL;
        if(in) { if(i == m) lv_obj_remove_flag(in, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(in, LV_OBJ_FLAG_HIDDEN); }
        lv_obj_set_style_border_color(auto_rows[i], i == m ? lv_color_hex(GL_ACTIVE) : lv_color_white(), 0);
        lv_obj_set_style_border_opa(auto_rows[i], i == m ? LV_OPA_COVER : LV_OPA_20, 0);
    }
}

static void on_auto_pick(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if(!ph_cmd("msgauto %d", m)) return;
    cur.msg_auto = m;
    auto_mark(m);
    if(sync_lbl) { char b[200]; sync_text(b, sizeof b); set_text(sync_lbl, b); }
}

static void auto_rows_gone(lv_event_t *e) { (void)e; memset(auto_rows, 0, sizeof auto_rows); }

static void on_auto(lv_event_t *e)
{
    (void)e;
    const char *btns[] = { tr(S_CLOSE) };
    lv_obj_t *card = dialog_open(LV_SYMBOL_REFRESH, lv_color_hex(C_PHONE), tr(S_PH_MSG_AUTO), NULL, btns, 1, 0, -1,
                                 NULL, NULL);
    lv_obj_set_width(card, 600);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    lv_obj_set_style_pad_all(card, BF_SP_L, 0);
    lv_obj_add_event_cb(card, auto_rows_gone, LV_EVENT_DELETE, NULL);
    for(int i = 0; i < 4; i++) {
        lv_obj_t *r = gcard(card);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(r, 6, 0);
        lv_obj_set_style_pad_hor(r, BF_SP_M, 0);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, BF_SP_M, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(r, LV_OPA_90, LV_STATE_PRESSED);
        lv_obj_t *dot = lv_obj_create(r);                   /* botão de opção */
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 26, 26);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(GL_ACTIVE), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *in = lv_obj_create(dot);
        lv_obj_remove_style_all(in);
        lv_obj_set_size(in, 14, 14);
        lv_obj_set_style_radius(in, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(in, lv_color_hex(GL_ACTIVE), 0);
        lv_obj_set_style_bg_opa(in, LV_OPA_COVER, 0);
        lv_obj_center(in);
        lv_obj_t *c = bf_plain(r);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        bf_label(c, tr(auto_name[i]), BF_FONT_M, T_WHITE);
        lv_obj_t *d = bf_label(c, tr(auto_desc[i]), BF_FONT_S, T_DIM);
        lv_obj_set_width(d, LV_PCT(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_add_event_cb(r, on_auto_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_move_to_index(r, (int32_t)(1 + i));          /* entre o título e o Fechar */
        auto_rows[i] = r;
    }
    auto_mark(cur.msg_auto);
}

/* barra de cima das listas: busca (contatos), situação da importação, Importar, Apagar */
static void list_header(bool search)
{
    lv_obj_t *row = gcard(body);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_pad_left(row, BF_SP_M, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    if(search) {
        search_ta = gfield(row, tr(S_PH_SEARCH), 40);
        lv_textarea_set_one_line(search_ta, true);
        lv_obj_set_height(search_ta, 48);
        lv_obj_set_flex_grow(search_ta, 1);
    }
    sync_lbl = bf_label(row, "", BF_FONT_S, T_DIM);
    if(search) lv_obj_set_width(sync_lbl, 130);
    else lv_obj_set_flex_grow(sync_lbl, 1);
    lv_label_set_long_mode(sync_lbl, LV_LABEL_LONG_WRAP);
    char b[200];
    sync_text(b, sizeof b);
    lv_label_set_text(sync_lbl, b);
    if(search) lv_obj_set_style_text_align(sync_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *imp = gbtn(row, LV_SYMBOL_DOWNLOAD, search ? NULL : tr(S_PH_IMPORT), 0);
    lv_obj_set_height(imp, 48);
    grad_bg(imp, G_LAV, 0);
    if(search) lv_obj_set_width(imp, 60);
    lv_obj_add_event_cb(imp, on_import, LV_EVENT_CLICKED, NULL);
    if(tab == TAB_MSGS) {
        lv_obj_t *au = gbtn(row, LV_SYMBOL_SETTINGS, NULL, 0);
        lv_obj_set_size(au, 60, 48);
        lv_obj_add_event_cb(au, on_auto, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *er = gbtn(row, LV_SYMBOL_TRASH, NULL, 2);
    lv_obj_set_size(er, 60, 48);
    lv_obj_set_style_bg_opa(er, LV_OPA_80, 0);
    lv_obj_add_event_cb(er, on_erase, LV_EVENT_CLICKED, NULL);
    list = bf_plain(body);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    bf_scrollable(list);
}

/* linha de lista: ícone (lead já criado pelo chamador, ou símbolo em degradê), título, detalhe, à direita */
static lv_obj_t *list_row2(const char *title, const char *sub, const char *right)
{
    lv_obj_t *row = gcard(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_pad_hor(row, BF_SP_M, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_90, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(GL_ACTIVE), LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *colm = bf_plain(row);
    lv_obj_set_height(colm, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(colm, 1);
    lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(colm, 2, 0);
    lv_obj_t *t = bf_label(colm, title, BF_FONT_M, T_WHITE);
    bf_label_ellipsis(t, 500);
    if(sub && sub[0]) {
        lv_obj_t *s = bf_label(colm, sub, BF_FONT_S, T_DIM);
        lv_obj_set_width(s, 540);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_height(s, 40, 0);
    }
    if(right && right[0]) bf_label(row, right, BF_FONT_S, T_DIM);
    return row;
}

static lv_obj_t *list_row(const char *sym, grad_t g, const char *title, const char *sub, const char *right)
{
    lv_obj_t *row = list_row2(title, sub, right);
    lv_obj_t *ic = gicon(row, sym, g, 44);
    lv_obj_move_to_index(ic, 0);
    return row;
}

/* números de um contato: "cel:123;casa:456" */
static char pick[4][48];
static int npick;

static void pick_answer(int btn, void *ud)
{
    (void)ud;
    if(btn >= 0 && btn < npick) dial(pick[btn]);
}

static void on_contact(lv_event_t *e)
{
    const char *r = lv_event_get_user_data(e);
    char name[96], tels[700];
    col(r, 0, name, sizeof name);
    col(r, 1, tels, sizeof tels);
    npick = 0;
    for(char *t = strtok(tels, ";"); t && npick < 3; t = strtok(NULL, ";")) {
        char *c = strchr(t, ':');
        snprintf(pick[npick++], sizeof pick[0], "%s", c ? c + 1 : t);
    }
    if(!npick) return;
    if(npick == 1) { dial(pick[0]); return; }
    const char *btns[4];
    for(int i = 0; i < npick; i++) btns[i] = pick[i];
    btns[npick] = tr(S_CANCEL);
    dialog_open(LV_SYMBOL_CALL, lv_color_hex(C_PHONE), name, tr(S_PH_CHOOSE), btns, npick + 1, 0, -1, pick_answer, NULL);
}

static bool match(const char *row, const char *q)
{
    if(!q[0]) return true;
    char name[96], tels[700];
    col(row, 0, name, sizeof name);
    if(strcasestr(name, q)) return true;
    if(q[0] >= '0' && q[0] <= '9') { col(row, 1, tels, sizeof tels); return strstr(tels, q) != NULL; }
    return false;
}

static void fill_contacts(void)
{
    lv_obj_clean(list);
    if(!contacts.buf && !table_load(&contacts, "contatos.tsv")) return;
    const char *q = search_ta ? lv_textarea_get_text(search_ta) : "";
    int shown = 0, total = 0;
    for(int i = 0; i < contacts.n; i++) {
        if(!match(contacts.row[i], q)) continue;
        total++;
        if(shown >= MAX_ROWS) continue;
        char name[96], tels[700], first[64] = "";
        col(contacts.row[i], 0, name, sizeof name);
        col(contacts.row[i], 1, tels, sizeof tels);
        char *c = strchr(tels, ':');
        if(c) { snprintf(first, sizeof first, "%s", c + 1); char *s = strchr(first, ';'); if(s) *s = 0; }
        int ntel = 0;
        for(char *p = tels; *p; p++) if(*p == ':') ntel++;
        char right[16] = "";
        if(ntel > 1) snprintf(right, sizeof right, "+%d", ntel - 1);
        lv_obj_t *r = list_row2(name[0] ? name : first, first, right);
        lv_obj_move_to_index(avatar(r, name[0] ? name : first, 44), 0);
        gicon(r, LV_SYMBOL_CALL, G_GREEN, 40);                     /* ligar: o toque é da linha */
        lv_obj_add_event_cb(r, on_contact, LV_EVENT_CLICKED, contacts.row[i]);
        shown++;
    }
    if(total > shown) {
        char b[96];
        snprintf(b, sizeof b, tr(S_PH_MORE), shown, total);
        list_note(b);
    }
}

static void on_search(lv_event_t *e) { (void)e; fill_contacts(); }

static void kb_close(lv_event_t *e)
{
    (void)e;
    if(search_kb) { lv_obj_delete(search_kb); search_kb = NULL; }
    if(search_kb_ta) { lv_obj_remove_state(search_kb_ta, LV_STATE_FOCUSED); search_kb_ta = NULL; }
}

static void on_search_focus(lv_event_t *e)
{
    (void)e;
    if(search_kb) return;
    search_kb = kb_open(search_ta, kb_close);
    search_kb_ta = search_ta;
}

static void build_contacts(void)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, BF_SP_S, 0);
    list_header(true);
    lv_obj_add_event_cb(search_ta, on_search, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(search_ta, on_search_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(search_ta, on_search_focus, LV_EVENT_CLICKED, NULL);
    fill_contacts();
    if(!contacts.n) list_note(tr(S_PH_NODATA));
}

static void on_recent(lv_event_t *e)
{
    const char *num = lv_event_get_user_data(e);
    dial(num);
}

static table_t recents;

static void build_recents(void)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, BF_SP_S, 0);
    list_header(false);
    if(!table_load(&recents, "historico.tsv") || !recents.n) {
        list_note(tr(recents.buf ? S_PH_NOHIST : S_PH_NODATA));
        return;
    }
    for(int i = 0; i < recents.n && i < 100; i++) {
        char kind[4], name[96], num[48], when[24], w[24];
        col(recents.row[i], 0, kind, sizeof kind);
        col(recents.row[i], 1, name, sizeof name);
        col(recents.row[i], 2, num, sizeof num);
        col(recents.row[i], 3, when, sizeof when);
        fmt_when(when, w, sizeof w);
        /* recebida: seta para baixo-esquerda; feita: para cima; perdida: em vermelho */
        const char *sym = kind[0] == 'F' ? LV_SYMBOL_UPLOAD : kind[0] == 'P' ? LV_SYMBOL_CLOSE : LV_SYMBOL_DOWNLOAD;
        grad_t g = kind[0] == 'P' ? G_RED : kind[0] == 'F' ? G_BLUE : G_GREEN;
        const char *sub = kind[0] == 'P' ? tr(S_PH_MISSED) : kind[0] == 'F' ? tr(S_PH_OUTGOING) : tr(S_PH_RECEIVED);
        char subl[96];
        snprintf(subl, sizeof subl, "%s%s%s", sub, name[0] ? " \xC2\xB7 " : "", name[0] ? num : "");
        lv_obj_t *r = list_row(sym, g, name[0] ? name : num[0] ? num : tr(S_PH_UNKNOWN), subl, w);
        if(kind[0] == 'P') {                                  /* perdida: o nome em vermelho claro */
            lv_obj_t *colm = lv_obj_get_child(r, 1);
            lv_obj_t *t = colm ? lv_obj_get_child(colm, 0) : NULL;
            if(t) lv_obj_set_style_text_color(t, lv_color_hex(0xFFB0B0), 0);
        }
        /* o ponteiro aponta para dentro de recents.buf: col() copia, mas o número fica na linha */
        char *p = recents.row[i];
        for(int k = 0; k < 2 && p; k++) { p = strchr(p, '\t'); if(p) p++; }
        if(p && num[0]) {
            char *t = strchr(p, '\t');
            if(t) *t = 0;                     /* a linha vira "...\tnúmero"; a data já foi lida */
            lv_obj_add_event_cb(r, on_recent, LV_EVENT_CLICKED, p);
        }
    }
}

/* ---------------------------------------------------------- mensagens */

/* Recebidas (mensagens.tsv) e enviadas (enviadas.tsv) agrupadas por conversa (os 8 últimos
 * dígitos do número); tocar abre a conversa em balões, com resposta pelo MAP (bfphone "sms"). */
static table_t msgs, sent;

typedef struct {
    char key[12], name[96], addr[64], when[24], text[200];
    int unread, count;
} conv_t;
#define MAX_CONV 120
static conv_t *convs;
static int nconv;
static int conv_open = -1;                 /* conversa aberta (índice em convs) */
static lv_obj_t *reply_ta, *reply_kb, *conv_list, *send_lbl, *send_btn;
static char sms_seen[64];
/* enviadas agora (ainda não reimportadas): aparecem no fim da conversa */
typedef struct { char key[12], text[200], when[24]; } pend_t;
static pend_t pend[8];
static int npend;

static void addr_key(const char *addr, char *out, size_t n)
{
    char d[64];
    size_t k = 0;
    for(const char *c = addr; *c && k + 1 < sizeof d; c++) if(*c >= '0' && *c <= '9') d[k++] = *c;
    d[k] = 0;
    /* sem algarismos: o próprio endereço, cortado no tamanho da chave (intencional) */
    snprintf(out, n, "%.*s", (int)(n - 1), k > 8 ? d + k - 8 : d[0] ? d : addr);
}

static void conv_add(const char *row, bool in)
{
    char rd[4], when[24], name[96], addr[64], text[200], key[12];
    col(row, 0, rd, sizeof rd);
    col(row, 1, when, sizeof when);
    col(row, 2, name, sizeof name);
    col(row, 3, addr, sizeof addr);
    col(row, 4, text, sizeof text);
    if(!addr[0] && !name[0]) return;
    addr_key(addr[0] ? addr : name, key, sizeof key);
    conv_t *c = NULL;
    for(int i = 0; i < nconv; i++) if(!strcmp(convs[i].key, key)) { c = &convs[i]; break; }
    if(!c) {
        if(nconv >= MAX_CONV) return;
        c = &convs[nconv++];
        memset(c, 0, sizeof *c);
        snprintf(c->key, sizeof c->key, "%s", key);
    }
    c->count++;
    if(in && rd[0] == '0') c->unread++;
    if(name[0] && (in || !c->name[0])) snprintf(c->name, sizeof c->name, "%s", name);
    if(addr[0] && !c->addr[0]) snprintf(c->addr, sizeof c->addr, "%s", addr);
    if(strcmp(when, c->when) > 0) {
        snprintf(c->when, sizeof c->when, "%s", when);
        snprintf(c->text, sizeof c->text, "%s%s", in ? "" : tr(S_PH_YOU_PREFIX), text);
    }
}

static int conv_cmp(const void *a, const void *b) { return strcmp(((const conv_t *)b)->when, ((const conv_t *)a)->when); }

static void conv_index(void)
{
    if(!convs) convs = malloc(sizeof *convs * MAX_CONV);
    nconv = 0;
    if(!convs) return;
    for(int i = 0; i < msgs.n; i++) conv_add(msgs.row[i], true);
    for(int i = 0; i < sent.n; i++) conv_add(sent.row[i], false);
    qsort(convs, (size_t)nconv, sizeof *convs, conv_cmp);
}

static void build_msgs(void);

static void on_conv(lv_event_t *e)
{
    conv_open = (int)(intptr_t)lv_event_get_user_data(e);
    build_tab();
}

static void reply_kb_close(lv_event_t *e)
{
    (void)e;
    if(reply_kb) { lv_obj_delete(reply_kb); reply_kb = NULL; }
    if(conv_list) { lv_obj_remove_flag(conv_list, LV_OBJ_FLAG_HIDDEN); lv_obj_scroll_to_y(conv_list, LV_COORD_MAX, LV_ANIM_OFF); }
    if(reply_ta) lv_obj_remove_state(reply_ta, LV_STATE_FOCUSED);
}

static void on_reply_focus(lv_event_t *e)
{
    (void)e;
    if(reply_kb) return;
    /* o teclado cobre a metade de baixo: esconde os balões e o campo sobe para logo abaixo do topo */
    if(conv_list) lv_obj_add_flag(conv_list, LV_OBJ_FLAG_HIDDEN);
    reply_kb = kb_open(reply_ta, reply_kb_close);
}

static void bubble(lv_obj_t *parent, const char *text, const char *when, bool in, const char *note)
{
    lv_obj_t *wrap = bf_plain(parent);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, in ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          in ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(wrap, 2, 0);
    lv_obj_t *b = bf_plain(wrap);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, 520, 0);
    lv_obj_set_style_pad_hor(b, BF_SP_M, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_set_style_radius(b, 18, 0);
    if(in) {
        lv_obj_set_style_bg_color(b, lv_color_hex(GL_BLUE), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(GL_DEEP), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_border_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    } else grad_bg(b, G_GREEN, 0);
    lv_obj_t *l = bf_label(b, text, BF_FONT_M, T_WHITE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 520 - 2 * BF_SP_M, 0);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    char w[64];
    fmt_when(when, w, sizeof w);
    if(note) { size_t k = strlen(w); snprintf(w + k, sizeof w - k, "%s%s", w[0] ? "  " : "", note); }
    bf_label(wrap, w, BF_FONT_S, T_DIM);
}

static void on_conv_back(lv_event_t *e) { (void)e; conv_open = -1; reply_kb_close(NULL); build_tab(); }

static void on_conv_call(lv_event_t *e)
{
    (void)e;
    if(conv_open >= 0 && conv_open < nconv) dial(convs[conv_open].addr);
}

static void send_state(void)
{
    if(!send_lbl) return;
    const char *s = cur.sms;
    if(!strcmp(s, "andamento")) set_text(send_lbl, tr(S_PH_SENDING));
    else if(!strncmp(s, "ok", 2)) set_text(send_lbl, tr(S_PH_SENT));
    else if(!strncmp(s, "erro", 4)) set_text(send_lbl, tr(S_PH_SEND_FAIL));
    else set_text(send_lbl, "");
    lv_obj_set_style_text_color(send_lbl, !strncmp(s, "erro", 4) ? lv_color_hex(0xFFB0B0) : T_DIM, 0);
}

static void on_send(lv_event_t *e)
{
    (void)e;
    if(conv_open < 0 || conv_open >= nconv || !reply_ta) return;
    const char *t = lv_textarea_get_text(reply_ta);
    if(!t[0] || !convs[conv_open].addr[0]) return;
    if(!strcmp(cur.sms, "andamento")) return;
    if(!ph_sms(convs[conv_open].addr, t)) { set_text(send_lbl, tr(S_PH_SEND_FAIL)); return; }
    if(npend == (int)(sizeof pend / sizeof pend[0])) { memmove(pend, pend + 1, sizeof pend[0] * (size_t)(npend - 1)); npend--; }
    pend_t *p = &pend[npend++];
    snprintf(p->key, sizeof p->key, "%s", convs[conv_open].key);
    snprintf(p->text, sizeof p->text, "%s", t);
    time_t n = time(NULL);
    struct tm tm;
    localtime_r(&n, &tm);
    strftime(p->when, sizeof p->when, "%Y%m%dT%H%M%S", &tm);
    snprintf(cur.sms, sizeof cur.sms, "andamento");
    snprintf(sms_seen, sizeof sms_seen, "andamento");
    lv_textarea_set_text(reply_ta, "");
    reply_kb_close(NULL);
    build_tab();
}

typedef struct { const char *row; bool in; char when[24]; } msgref_t;
static int ref_cmp(const void *a, const void *b) { return strcmp(((const msgref_t *)a)->when, ((const msgref_t *)b)->when); }

static void build_conv(void)
{
    conv_t *c = &convs[conv_open];
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, BF_SP_S, 0);
    /* topo: voltar, quem, ligar */
    lv_obj_t *top = gcard(body);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top, 6, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, BF_SP_M, 0);
    lv_obj_t *bk = gbtn(top, LV_SYMBOL_LEFT, NULL, 0);
    lv_obj_set_size(bk, 60, 48);
    lv_obj_add_event_cb(bk, on_conv_back, LV_EVENT_CLICKED, NULL);
    avatar(top, c->name[0] ? c->name : c->addr, 40);
    lv_obj_t *who = bf_plain(top);
    lv_obj_set_height(who, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(who, 1);
    lv_obj_set_flex_flow(who, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *nl = bf_label(who, c->name[0] ? c->name : c->addr[0] ? c->addr : tr(S_PH_UNKNOWN), BF_FONT_M, T_WHITE);
    bf_label_ellipsis(nl, 420);
    send_lbl = bf_label(who, c->name[0] ? c->addr : "", BF_FONT_S, T_DIM);
    bf_label_ellipsis(send_lbl, 420);
    if(c->addr[0]) {
        lv_obj_t *cl = gbtn(top, LV_SYMBOL_CALL, NULL, 1);
        lv_obj_set_size(cl, 60, 48);
        lv_obj_add_event_cb(cl, on_conv_call, LV_EVENT_CLICKED, NULL);
    }
    /* balões, do mais antigo ao mais novo (os 60 últimos) */
    conv_list = bf_plain(body);
    lv_obj_set_width(conv_list, LV_PCT(100));
    lv_obj_set_flex_grow(conv_list, 1);
    lv_obj_set_flex_flow(conv_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(conv_list, 6, 0);
    lv_obj_set_style_pad_hor(conv_list, BF_SP_S, 0);
    bf_scrollable(conv_list);
    int n = 0;
    msgref_t *refs = malloc(sizeof *refs * (size_t)(msgs.n + sent.n + 1));
    if(refs) {
        char key[12], addr[64], name[96];
        for(int k = 0; k < 2; k++) {
            table_t *t = k ? &sent : &msgs;
            for(int i = 0; i < t->n; i++) {
                col(t->row[i], 3, addr, sizeof addr);
                if(!addr[0]) col(t->row[i], 2, name, sizeof name);
                addr_key(addr[0] ? addr : name, key, sizeof key);
                if(strcmp(key, c->key)) continue;
                refs[n].row = t->row[i];
                refs[n].in = !k;
                col(t->row[i], 1, refs[n].when, sizeof refs[n].when);
                n++;
            }
        }
        qsort(refs, (size_t)n, sizeof *refs, ref_cmp);
        for(int i = n > 60 ? n - 60 : 0; i < n; i++) {
            char text[320];
            col(refs[i].row, 4, text, sizeof text);
            bubble(conv_list, text, refs[i].when, refs[i].in, NULL);
        }
        free(refs);
    }
    for(int i = 0; i < npend; i++)
        if(!strcmp(pend[i].key, c->key))
            bubble(conv_list, pend[i].text, pend[i].when, false, NULL);
    /* resposta */
    lv_obj_t *rb = gcard(body);
    lv_obj_set_size(rb, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(rb, 6, 0);
    lv_obj_set_flex_flow(rb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rb, BF_SP_S, 0);
    reply_ta = gfield(rb, tr(S_PH_REPLY_HINT), 600);
    lv_obj_set_flex_grow(reply_ta, 1);
    lv_obj_set_height(reply_ta, 52);
    lv_obj_add_event_cb(reply_ta, on_reply_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(reply_ta, on_reply_focus, LV_EVENT_CLICKED, NULL);
    send_btn = gbtn(rb, LV_SYMBOL_UPLOAD, tr(S_PH_SEND), 1);
    lv_obj_set_size(send_btn, 140, 52);
    lv_obj_add_event_cb(send_btn, on_send, LV_EVENT_CLICKED, NULL);
    if(!c->addr[0]) lv_obj_add_state(send_btn, LV_STATE_DISABLED);
    if(strcmp(cur.sms, "idle")) send_state();     /* resultado do último envio no lugar do número */
    lv_obj_update_layout(conv_list);
    lv_obj_scroll_to_y(conv_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void build_msgs(void)
{
    table_load(&msgs, "mensagens.tsv");
    table_load(&sent, "enviadas.tsv");
    conv_index();
    if(conv_open >= nconv) conv_open = -1;
    if(conv_open >= 0) { build_conv(); return; }
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, BF_SP_S, 0);
    list_header(false);
    if(!nconv) {
        /* sem mensagens: explica por quê (o Android não liberou o MAP, ou nada importado) */
        bool denied = strstr(cur.sync, "indisponivel") || strstr(cur.sync, "negado") || strstr(cur.sync, "obex -1");
        const char *d = cur.data[0] ? cur.data : ph_last_data();
        char path[160];
        snprintf(path, sizeof path, "%s/sincronizado", d);
        bool synced = d[0] && !access(path, R_OK);
        bf_str_t s = denied || (synced && !msgs.buf) ? S_PH_MSG_DENIED : synced ? S_PH_NOMSG : S_PH_NODATA;
        lv_obj_t *n = list_note(tr(s));
        if(s == S_PH_MSG_DENIED) lv_obj_move_to_index(gicon(n, LV_SYMBOL_WARNING, G_AMBER, 40), 0);
        if(s == S_PH_MSG_DENIED) {
            lv_obj_set_flex_flow(n, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(n, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_column(n, BF_SP_M, 0);
            lv_obj_t *l = lv_obj_get_child(n, 1);
            if(l) { lv_obj_set_width(l, LV_SIZE_CONTENT); lv_obj_set_flex_grow(l, 1); }
        }
        return;
    }
    for(int i = 0; i < nconv && i < MAX_ROWS; i++) {
        conv_t *c = &convs[i];
        char w[24], right[40];
        fmt_when(c->when, w, sizeof w);
        snprintf(right, sizeof right, "%s", w);
        lv_obj_t *r = list_row2(c->name[0] ? c->name : c->addr[0] ? c->addr : tr(S_PH_UNKNOWN), c->text, right);
        lv_obj_move_to_index(avatar(r, c->name[0] ? c->name : c->addr, 44), 0);
        if(c->unread) {                                   /* bolinha com as não lidas */
            char u[12];
            snprintf(u, sizeof u, "%d", c->unread > 99 ? 99 : c->unread);
            glass_tile(r, 28, LV_RADIUS_CIRCLE, G_AMBER.top, G_AMBER.bottom, NULL, u, BF_FONT_S);
            lv_obj_t *colm = lv_obj_get_child(r, 1);
            lv_obj_t *t = colm ? lv_obj_get_child(colm, 0) : NULL;
            if(t) lv_obj_set_style_text_color(t, lv_color_hex(GL_ACTIVE), 0);
        }
        lv_obj_add_event_cb(r, on_conv, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void build_tab(void)
{
    kb_close(NULL);
    if(reply_kb) { lv_obj_delete(reply_kb); reply_kb = NULL; }
    lv_obj_clean(body);
    search_ta = list = more_lbl = sync_lbl = num_lbl = reply_ta = conv_list = send_lbl = send_btn = NULL;
    lv_obj_set_style_pad_column(body, 0, 0);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if(tab != TAB_MSGS) conv_open = -1;
    switch(tab) {
    case TAB_DIAL: build_dial(); break;
    case TAB_CONTACTS: build_contacts(); break;
    case TAB_RECENTS: build_recents(); break;
    case TAB_MSGS: build_msgs(); break;
    }
    for(int i = 0; i < NTABS; i++) {
        if(i == tab) lv_obj_add_state(tab_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(tab_btn[i], LV_STATE_CHECKED);
    }
}

static void on_tab(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if(t == tab && !(t == TAB_MSGS && conv_open >= 0)) return;
    tab = t;
    conv_open = -1;
    build_tab();
}

/* ------------------------------------------------------ painel da ligação */

enum { ACT_MUTE, ACT_KEYS, ACT_VOL, ACT_NEW, ACT_HOLD, ACT_REC, NACT };
static lv_obj_t *cp_other;
static lv_obj_t *act_btn[NACT], *act_lbl[NACT], *cp_grid, *cp_volrow, *cp_vol, *cp_vrow, *cp_rec, *cp_back;
static bool minimized;                    /* "Nova chamada": painel escondido, discador aberto */
static char min_sig[256];

static const ph_call_t *main_call(void)
{
    const ph_call_t *m = NULL;
    for(int i = 0; i < cur.ncall; i++) {                  /* a recebendo e a ativa têm prioridade */
        const ph_call_t *c = &cur.call[i];
        if(c->stat == PH_INCOMING) return c;
        if(!m || c->stat == PH_ACTIVE) m = c;
    }
    return m;
}

static void on_answer(lv_event_t *e) { (void)e; ph_cmd("answer"); }
static void on_hangup(lv_event_t *e) { (void)e; ph_cmd("hangup"); }
static void on_dtmf(lv_event_t *e) { ph_cmd("dtmf %s", (const char *)lv_event_get_user_data(e)); }

/* falar pelo celular ou pelo FOSTON (alto-falante e microfone do aparelho) */
static void on_voice(lv_event_t *e)
{
    bool foston = lv_event_get_user_data(e) != NULL;
    if(foston && !cur.voice_foston) {
        const char *btns[] = { tr(S_CLOSE) };
        dialog_open(BF_SYMBOL_INFO, lv_color_hex(C_PHONE), tr(S_PH_VOICE_FOSTON), tr(S_PH_VOICE_SOON), btns, 1, 0, -1,
                    NULL, NULL);
        return;
    }
    ph_cmd("voice %s", foston ? "foston" : "phone");
}

static void show_only(lv_obj_t *o)       /* grade, teclado ou volume: um de cada vez */
{
    lv_obj_t *all[] = { cp_grid, cp_dtmf, cp_volrow };
    for(int i = 0; i < 3; i++) {
        if(all[i] == o) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_vol(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int v = cur.vol ? cur.vol : 5;
    if(t == cp_vol) v = lv_slider_get_value(cp_vol);
    else v += (intptr_t)lv_event_get_user_data(e);
    if(v < 1) v = 1;
    if(v > 10) v = 10;
    lv_slider_set_value(cp_vol, v, LV_ANIM_OFF);
    cur.vol = v;
    ph_cmd("vol %d", v);
}

static void on_act(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    switch(a) {
    case ACT_MUTE: ph_cmd("mute %d", cur.mute ? 0 : 1); break;
    case ACT_KEYS: show_only(cp_dtmf); break;
    case ACT_VOL: lv_slider_set_value(cp_vol, cur.vol ? cur.vol : 5, LV_ANIM_OFF); show_only(cp_volrow); break;
    case ACT_HOLD: ph_cmd("hold"); break;
    case ACT_REC: ph_cmd("record %d", cur.rec ? 0 : 1); break;
    case ACT_NEW:                                  /* com duas chamadas: juntar */
        if(cur.ncall >= 2) { ph_cmd("merge"); break; }
        /* discador por cima da ligação */
        minimized = true;
        snprintf(min_sig, sizeof min_sig, "%s", call_sig);
        lv_obj_add_flag(call_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cp_back, LV_OBJ_FLAG_HIDDEN);
        number[0] = 0;
        tab = TAB_DIAL;
        build_tab();
        break;
    }
}

static void on_back_to_call(lv_event_t *e)
{
    (void)e;
    minimized = false;
    lv_obj_add_flag(cp_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(call_panel, LV_OBJ_FLAG_HIDDEN);
}

static void on_sub_close(lv_event_t *e) { (void)e; show_only(cp_grid); }

/* botão redondo com ícone e legenda */
static lv_obj_t *action(lv_obj_t *grid, int a, const char *sym, bf_str_t label)
{
    lv_obj_t *col = bf_plain(grid);
    lv_obj_set_size(col, 112, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t *b = gbtn(col, sym, NULL, 0);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_min_height(b, 0, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, on_act, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    if(!sym) {                                     /* Gravar: ponto vermelho desenhado */
        lv_obj_t *dot = lv_obj_create(b);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 20, 20);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, pal()->danger, 0);
        lv_obj_center(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *l = bf_label(col, tr(label), BF_FONT_S, T_DIM);
    lv_obj_set_width(l, 112);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    act_btn[a] = b;
    act_lbl[a] = l;
    return b;
}

static void act_set(int a, bool enabled, bool checked)
{
    lv_obj_t *b = act_btn[a];
    if(enabled) lv_obj_remove_state(b, LV_STATE_DISABLED);
    else lv_obj_add_state(b, LV_STATE_DISABLED);
    if(checked) lv_obj_add_state(b, LV_STATE_CHECKED);
    else lv_obj_remove_state(b, LV_STATE_CHECKED);
    lv_obj_set_style_opa(act_btn[a], enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(act_lbl[a], enabled ? LV_OPA_COVER : LV_OPA_40, 0);
}

/* linha com o botão de fechar (volta à grade) */
static lv_obj_t *sub_row(lv_obj_t *parent)
{
    lv_obj_t *r = bf_plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, BF_SP_S, 0);
    return r;
}

static void build_call_panel(void)
{
    const bf_palette_t *p = pal();
    call_panel = gcard(root);
    lv_obj_set_style_bg_opa(call_panel, LV_OPA_COVER, 0);           /* cobre as abas por baixo */
    lv_obj_set_style_bg_color(call_panel, lv_color_hex(0x1D4A7C), 0);
    lv_obj_set_style_bg_grad_color(call_panel, lv_color_hex(0x0E2747), 0);
    lv_obj_set_style_radius(call_panel, 24, 0);
    lv_obj_set_size(call_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(call_panel, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(call_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(call_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(call_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(call_panel, BF_SP_S, 0);

    /* esquerda: quem, estado, gravação, onde está a voz */
    lv_obj_t *left = bf_plain(call_panel);
    lv_obj_set_size(left, 330, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, BF_SP_S, 0);
    glass_tile(left, 72, LV_RADIUS_CIRCLE, G_GREEN.top, G_GREEN.bottom, NULL, LV_SYMBOL_CALL, BF_FONT_L);
    cp_name = bf_label(left, "", BF_FONT_L, T_WHITE);
    bf_label_ellipsis(cp_name, 320);
    lv_obj_set_style_text_align(cp_name, LV_TEXT_ALIGN_CENTER, 0);
    cp_num = bf_label(left, "", BF_FONT_M, T_DIM);
    cp_state = bf_label(left, "", BF_FONT_M, lv_color_hex(G_GREEN.top));
    cp_other = bf_label(left, "", BF_FONT_S, p->warning);     /* a outra chamada (em espera) */
    bf_label_ellipsis(cp_other, 320);
    lv_obj_add_flag(cp_other, LV_OBJ_FLAG_HIDDEN);
    cp_rec = bf_label(left, "\xE2\x80\xA2 ", BF_FONT_M, p->danger);
    lv_obj_add_flag(cp_rec, LV_OBJ_FLAG_HIDDEN);
    cp_note = bf_label(left, tr(S_PH_VOICE_NOTE), BF_FONT_S, T_DIM);
    lv_obj_set_width(cp_note, 320);
    lv_label_set_long_mode(cp_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(cp_note, LV_TEXT_ALIGN_CENTER, 0);

    /* direita: ações, falar pelo, atender/desligar */
    lv_obj_t *right = bf_plain(call_panel);
    lv_obj_set_size(right, 380, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right, BF_SP_S, 0);

    cp_grid = bf_plain(right);
    lv_obj_set_size(cp_grid, 3 * 112 + 2 * BF_SP_S, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cp_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(cp_grid, BF_SP_S, 0);
    lv_obj_set_style_pad_row(cp_grid, BF_SP_S, 0);
    action(cp_grid, ACT_MUTE, LV_SYMBOL_MUTE, S_PH_ACT_MUTE);
    action(cp_grid, ACT_KEYS, LV_SYMBOL_KEYBOARD, S_PH_KEYPAD);
    action(cp_grid, ACT_VOL, LV_SYMBOL_VOLUME_MAX, S_PH_ACT_VOLUME);
    action(cp_grid, ACT_NEW, LV_SYMBOL_PLUS, S_PH_ACT_NEW);
    action(cp_grid, ACT_HOLD, LV_SYMBOL_PAUSE, S_PH_ACT_HOLD);
    action(cp_grid, ACT_REC, NULL, S_PH_ACT_REC);

    /* teclado (DTMF) e volume, no lugar da grade */
    cp_dtmf = bf_plain(right);
    lv_obj_set_size(cp_dtmf, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cp_dtmf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cp_dtmf, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cp_dtmf, BF_SP_S, 0);
    keypad(cp_dtmf, on_dtmf, 84, 36);
    lv_obj_t *kc = gbtn(cp_dtmf, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(kc, 48, 48);
    lv_obj_add_event_cb(kc, on_sub_close, LV_EVENT_CLICKED, NULL);
    cp_volrow = sub_row(right);
    lv_obj_t *vm = gbtn(cp_volrow, LV_SYMBOL_MINUS, NULL, 0);
    lv_obj_set_size(vm, 52, 52);
    lv_obj_add_event_cb(vm, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    cp_vol = lv_slider_create(cp_volrow);
    lv_slider_set_range(cp_vol, 1, 10);
    lv_obj_set_width(cp_vol, 130);
    lv_obj_set_style_bg_color(cp_vol, lv_color_hex(GL_ACTIVE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cp_vol, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(cp_vol, on_vol, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *vp = gbtn(cp_volrow, LV_SYMBOL_PLUS, NULL, 0);
    lv_obj_set_size(vp, 52, 52);
    lv_obj_add_event_cb(vp, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *vc = gbtn(cp_volrow, LV_SYMBOL_OK, NULL, 0);
    lv_obj_set_size(vc, 52, 52);
    lv_obj_add_event_cb(vc, on_sub_close, LV_EVENT_CLICKED, NULL);
    show_only(cp_grid);

    /* falar pelo celular | FOSTON */
    cp_vrow = sub_row(right);
    bf_label(cp_vrow, tr(S_PH_TALK_ON), BF_FONT_S, T_DIM);
    cp_vphone = gbtn(cp_vrow, LV_SYMBOL_CALL, tr(S_PH_TALK_PHONE), 0);
    cp_vfoston = gbtn(cp_vrow, LV_SYMBOL_AUDIO, "FOSTON", 0);
    for(lv_obj_t *b = cp_vphone; b; b = b == cp_vphone ? cp_vfoston : NULL) {
        lv_obj_set_size(b, 124, 44);
        lv_obj_set_style_min_height(b, 0, 0);
        lv_obj_add_event_cb(b, on_voice, LV_EVENT_CLICKED, b == cp_vfoston ? (void *)1 : NULL);
    }

    /* atender / desligar */
    lv_obj_t *btns = sub_row(right);
    cp_answer = gbtn(btns, LV_SYMBOL_CALL, tr(S_PH_ANSWER), 1);
    lv_obj_set_size(cp_answer, 170, 56);
    lv_obj_set_style_radius(cp_answer, 28, 0);
    lv_obj_add_event_cb(cp_answer, on_answer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hang = gbtn(btns, LV_SYMBOL_CLOSE, tr(S_PH_HANGUP), 2);
    lv_obj_set_size(hang, 170, 56);
    lv_obj_set_style_radius(hang, 28, 0);
    lv_obj_add_event_cb(hang, on_hangup, LV_EVENT_CLICKED, NULL);
    cp_keys = hang;                                /* compatibilidade: não usado */

    /* "voltar à ligação" (com a ligação minimizada para fazer outra) */
    cp_back = gbtn(root, LV_SYMBOL_CALL, tr(S_PH_BACK_TO_CALL), 1);
    lv_obj_add_flag(cp_back, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(cp_back, 26, 0);
    lv_obj_set_size(cp_back, 230, 52);
    lv_obj_align(cp_back, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(cp_back, on_back_to_call, LV_EVENT_CLICKED, NULL);
}

static void update_call(void)
{
    const ph_call_t *c = main_call();
    char sig[256];
    snprintf(sig, sizeof sig, "%d", cur.ncall);
    for(int i = 0; i < cur.ncall; i++) {
        size_t n = strlen(sig);
        snprintf(sig + n, sizeof sig - n, "|%d%d%s", cur.call[i].idx, cur.call[i].stat, cur.call[i].num);
    }
    bool changed = strcmp(sig, call_sig) != 0;
    snprintf(call_sig, sizeof call_sig, "%s", sig);
    if(!c) {
        minimized = false;
        lv_obj_add_flag(cp_back, LV_OBJ_FLAG_HIDDEN);
        if(!lv_obj_has_flag(call_panel, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(call_panel, LV_OBJ_FLAG_HIDDEN);
            show_only(cp_grid);
            if(tab == TAB_RECENTS) build_tab();           /* a ligação entra no histórico do celular */
        }
        return;
    }
    /* minimizada: volta sozinha quando as chamadas mudam (a nova chamada aparece) */
    if(minimized && strcmp(sig, min_sig)) { minimized = false; lv_obj_add_flag(cp_back, LV_OBJ_FLAG_HIDDEN); }
    if(minimized) return;
    if(changed || lv_obj_has_flag(call_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(call_panel, LV_OBJ_FLAG_HIDDEN);
        kb_close(NULL);
        set_text(cp_name, c->name[0] ? c->name : c->num[0] ? c->num : tr(S_PH_UNKNOWN));
        set_text(cp_num, c->name[0] ? c->num : "");
    }
    bool ringing = c->stat == PH_INCOMING || c->stat == PH_WAITING;
    if(ringing) { lv_obj_remove_flag(cp_answer, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(cp_grid, LV_OBJ_FLAG_HIDDEN); }
    else {
        lv_obj_add_flag(cp_answer, LV_OBJ_FLAG_HIDDEN);
        if(lv_obj_has_flag(cp_dtmf, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(cp_volrow, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(cp_grid, LV_OBJ_FLAG_HIDDEN);
    }
    bool active = c->stat == PH_ACTIVE, held = c->stat == PH_HELD;
    if(!active && !lv_obj_has_flag(cp_dtmf, LV_OBJ_FLAG_HIDDEN)) show_only(cp_grid);
    act_set(ACT_MUTE, cur.voice_on, cur.mute);
    act_set(ACT_KEYS, active, false);
    act_set(ACT_VOL, true, false);
    /* duas chamadas: Juntar (conferência) e Alternar; e quem é a outra */
    bool two = cur.ncall >= 2;
    static bool was_two;
    if(two != was_two) {
        was_two = two;
        lv_obj_t *il = lv_obj_get_child(act_btn[ACT_NEW], 0), *hl = lv_obj_get_child(act_btn[ACT_HOLD], 0);
        if(il) lv_label_set_text(il, two ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_PLUS);
        if(hl) lv_label_set_text(hl, two ? LV_SYMBOL_REFRESH : LV_SYMBOL_PAUSE);
        lv_label_set_text(act_lbl[ACT_NEW], tr(two ? S_PH_ACT_MERGE : S_PH_ACT_NEW));
        lv_label_set_text(act_lbl[ACT_HOLD], tr(two ? S_PH_ACT_SWAP : S_PH_ACT_HOLD));
    }
    const ph_call_t *o = NULL;
    for(int i = 0; i < cur.ncall; i++) if(&cur.call[i] != c) o = &cur.call[i];
    if(o) {
        char t[160];
        bool conf = o->stat == PH_ACTIVE && c->stat == PH_ACTIVE;
        snprintf(t, sizeof t, "%s: %s", tr(conf ? S_PH_IN_CONF : o->stat == PH_HELD ? S_PH_ST_HELD : S_PH_OTHER_CALL),
                 o->name[0] ? o->name : o->num[0] ? o->num : tr(S_PH_UNKNOWN));
        set_text(cp_other, t);
        lv_obj_remove_flag(cp_other, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(cp_other, LV_OBJ_FLAG_HIDDEN);
    bool any_held = false;
    for(int i = 0; i < cur.ncall; i++) if(cur.call[i].stat == PH_HELD) any_held = true;
    act_set(ACT_NEW, active || held, false);
    act_set(ACT_HOLD, active || held, held && !two);
    if(two) act_set(ACT_NEW, any_held || cur.ncall >= 2, false);
    act_set(ACT_REC, cur.voice_on, cur.rec);
    if(cur.rec) {
        lv_obj_remove_flag(cp_rec, LV_OBJ_FLAG_HIDDEN);
        char r[64];
        snprintf(r, sizeof r, "\xE2\x80\xA2 %s", tr(S_PH_RECORDING));
        set_text(cp_rec, r);
    } else lv_obj_add_flag(cp_rec, LV_OBJ_FLAG_HIDDEN);
    /* escolha da voz: marca a opção atual */
    bool on_foston = !cur.voice_phone && cur.voice_foston;     /* escolhido (o DSP liga com o canal de voz) */
    if(on_foston) { lv_obj_add_state(cp_vfoston, LV_STATE_CHECKED); lv_obj_remove_state(cp_vphone, LV_STATE_CHECKED); }
    else { lv_obj_add_state(cp_vphone, LV_STATE_CHECKED); lv_obj_remove_state(cp_vfoston, LV_STATE_CHECKED); }
    set_text(cp_note, tr(on_foston ? S_PH_VOICE_ON_FOSTON : S_PH_VOICE_NOTE));
    char b[64];
    switch(c->stat) {
    case PH_ACTIVE: {
        long s = cur.since ? (long)(time(NULL) - cur.since) : 0;
        if(s < 0) s = 0;
        if(s > 359999) s = 359999;
        char d[40];
        snprintf(d, sizeof d, "%02ld:%02ld", s / 60, s % 60);
        snprintf(b, sizeof b, tr(S_PH_ST_ACTIVE), d);
        break;
    }
    case PH_HELD: snprintf(b, sizeof b, "%s", tr(S_PH_ST_HELD)); break;
    case PH_DIALING: snprintf(b, sizeof b, "%s", tr(S_PH_ST_DIALING)); break;
    case PH_ALERTING: snprintf(b, sizeof b, "%s", tr(S_PH_ST_ALERTING)); break;
    case PH_WAITING: snprintf(b, sizeof b, "%s", tr(S_PH_ST_WAITING)); break;
    default: snprintf(b, sizeof b, "%s", tr(S_PH_ST_INCOMING)); break;
    }
    set_text(cp_state, b);
}

/* ------------------------------------------------------------ estado */

static void refresh(void)
{
    ph_read(&cur);
    char b[200], b2[96] = "";
    /* ao lado das abas: o celular, a operadora; antena e bateria dele */
    if(!cur.available) snprintf(b, sizeof b, "%s", tr(S_PH_NOSERVICE));
    else if(!cur.link) snprintf(b, sizeof b, "%s", tr(S_PH_NOPHONE));
    else {
        snprintf(b, sizeof b, "%s", cur.dev);
        snprintf(b2, sizeof b2, "%s", cur.op);
    }
    set_text(status_lbl, b);
    set_text(status2_lbl, b2);
    lv_obj_set_style_text_color(status_lbl, cur.link ? lv_color_hex(GL_WHITE) : pal()->warning, 0);
    glass_bars_set(st_bars, cur.link && cur.signal >= 0 ? (cur.signal * 4 + 4) / 5 : -1);
    char bat[32] = "";
    if(cur.link && cur.battery >= 0)
        snprintf(bat, sizeof bat, "%s", cur.battery >= 4 ? LV_SYMBOL_BATTERY_FULL : cur.battery >= 3 ? LV_SYMBOL_BATTERY_3 :
                 cur.battery >= 2 ? LV_SYMBOL_BATTERY_2 : cur.battery >= 1 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY);
    set_text(st_bat, bat);
    static int auto_sig = -1;                     /* modo automático ou avisos mudaram: atualiza o texto */
    int asig = cur.msg_auto * 2 + cur.notify;
    if(asig != auto_sig) {
        auto_sig = asig;
        if(sync_lbl && strcmp(cur.sync, "andamento")) { sync_text(b, sizeof b); set_text(sync_lbl, b); }
    }
    /* envio de SMS: mostra o resultado; a enviada entra na conversa na próxima importação */
    if(strcmp(cur.sms, sms_seen)) { snprintf(sms_seen, sizeof sms_seen, "%s", cur.sms); send_state(); }
    /* o bfphone apagou os dados: volta a ler (vazio) */
    if(erase_pending) {
        const char *d = cur.data[0] ? cur.data : ph_last_data();
        char path[160];
        snprintf(path, sizeof path, "%s/sincronizado", d);
        if(!d[0] || access(path, F_OK)) { erase_pending = false; if(tab != TAB_DIAL) build_tab(); }
    }
    /* a importação terminou: relê as listas */
    if(strcmp(cur.sync, sync_seen)) {
        bool finished = strcmp(sync_seen, "andamento") == 0;
        snprintf(sync_seen, sizeof sync_seen, "%s", cur.sync);
        if(finished && strstr(cur.sync, "ok")) npend = 0;          /* as enviadas já vieram do celular */
        /* relê as listas; com o teclado aberto (escrevendo) espera fechar */
        if((finished || !strcmp(cur.sync, "idle")) && !reply_kb && !search_kb) {
            table_free(&contacts);
            if(tab != TAB_DIAL) build_tab();
        } else if(finished) sync_seen[0] = 0;
        else if(sync_lbl) { sync_text(b, sizeof b); set_text(sync_lbl, b); }
    }
    update_call();
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, BF_SP_M, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, BF_SP_S, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    tabbar = bf_plain(root);
    lv_obj_set_size(tabbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabbar, BF_SP_S, 0);
    /* abas num trilho de vidro; a escolhida acende em azul claro */
    lv_obj_t *seg = gcard(tabbar);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, 56);
    lv_obj_set_style_pad_all(seg, 4, 0);
    lv_obj_set_style_radius(seg, 28, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 4, 0);
    static const char *const syms[NTABS] = { LV_SYMBOL_CALL, LV_SYMBOL_LIST, BF_SYMBOL_CLOCK, LV_SYMBOL_ENVELOPE };
    const bf_str_t names[NTABS] = { S_PH_TAB_DIAL, S_PH_TAB_CONTACTS, S_PH_TAB_RECENTS, S_PH_TAB_MSGS };
    for(int i = 0; i < NTABS; i++) {
        tab_btn[i] = gbtn(seg, syms[i], tr(names[i]), 0);
        lv_obj_set_height(tab_btn[i], 48);
        lv_obj_set_style_min_height(tab_btn[i], 0, 0);
        lv_obj_set_style_radius(tab_btn[i], 24, 0);
        lv_obj_set_style_pad_hor(tab_btn[i], 12, 0);
        lv_obj_set_style_bg_opa(tab_btn[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tab_btn[i], 0, 0);
        lv_obj_set_style_border_width(tab_btn[i], 1, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(tab_btn[i], lv_color_hex(GL_TEXT2), 0);
        for(uint32_t k = 0; k < lv_obj_get_child_count(tab_btn[i]); k++)   /* abas compactas: sobra lugar */
            lv_obj_set_style_text_font(lv_obj_get_child(tab_btn[i], (int32_t)k), BF_FONT_S, 0);
        lv_obj_add_event_cb(tab_btn[i], on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* o celular: nome e operadora, antena e bateria dele (HFP) */
    lv_obj_t *st = gcard(tabbar);
    lv_obj_set_height(st, 56);
    lv_obj_set_style_min_width(st, 0, 0);
    lv_obj_set_style_pad_ver(st, 4, 0);
    lv_obj_set_style_pad_hor(st, BF_SP_M, 0);
    lv_obj_set_style_radius(st, 28, 0);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(st, 1);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, BF_SP_S, 0);
    lv_obj_t *stc = bf_plain(st);
    lv_obj_set_height(stc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(stc, 1);
    lv_obj_set_flex_flow(stc, LV_FLEX_FLOW_COLUMN);
    status_lbl = bf_label(stc, "", BF_FONT_S, lv_color_hex(GL_WHITE));
    status2_lbl = bf_label(stc, "", BF_FONT_S, lv_color_hex(GL_TEXT2));
    for(lv_obj_t *l = status_lbl; l; l = l == status_lbl ? status2_lbl : NULL) {
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    st_bars = glass_bars(st);
    st_bat = bf_label(st, "", BF_FONT_S, lv_color_hex(GL_TEXT2));

    body = bf_plain(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);

    build_call_panel();
    ph_read(&cur);
    snprintf(sync_seen, sizeof sync_seen, "%s", cur.sync);
    call_sig[0] = 0;
    build_tab();
    refresh();
    tick = lv_timer_create(tick_cb, 500, NULL);
}

static void destroy(void)
{
    if(tick) lv_timer_delete(tick);
    tick = NULL;
    kb_close(NULL);
    table_free(&contacts);
    table_free(&recents);
    table_free(&msgs);
    table_free(&sent);
    if(reply_kb) { lv_obj_delete(reply_kb); reply_kb = NULL; }
    free(convs);
    convs = NULL;
    nconv = 0;
    conv_open = -1;
    root = NULL;
}

const bf_screen_def_t scr_phone_def = { create, destroy, S_APP_PHONE, NULL };

/* modo --screenshot: abre uma aba */
void scr_phone_demo(int t)
{
    if(t == 10 && cp_vfoston) { lv_obj_send_event(cp_vfoston, LV_EVENT_CLICKED, NULL); return; }
    if(t == 11 && act_btn[ACT_VOL]) { lv_obj_send_event(act_btn[ACT_VOL], LV_EVENT_CLICKED, NULL); return; }
    if(t == 12 && act_btn[ACT_KEYS]) { lv_obj_send_event(act_btn[ACT_KEYS], LV_EVENT_CLICKED, NULL); return; }
    if(t == 13 && cp_grid) { show_only(cp_grid); return; }
    if(t == 14 && tab == TAB_MSGS && nconv) { conv_open = 0; build_tab(); return; }
    if(t == 15 && tab == TAB_MSGS) { on_auto(NULL); return; }
    if(t == 16 && reply_ta) { lv_textarea_set_text(reply_ta, "Já estou chegando, obrigado!"); on_reply_focus(NULL); return; }
    if(t < 0 || t >= NTABS || !root) return;
    tab = t;
    build_tab();
}
