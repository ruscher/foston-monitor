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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define C_PHONE 0x2ec27e
#define MAX_ROWS 60

enum { TAB_DIAL, TAB_CONTACTS, TAB_RECENTS, TAB_MSGS, NTABS };

static lv_obj_t *root, *tabbar, *tab_btn[NTABS], *body, *status_lbl, *status2_lbl, *call_panel;
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
    lv_obj_set_style_text_color(num_lbl, number[0] ? pal()->text : pal()->text_dim, 0);
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

static lv_obj_t *keypad(lv_obj_t *parent, lv_event_cb_t cb, int w, int h)
{
    static const char *const keys[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
    lv_obj_t *g = bf_plain(parent);
    lv_obj_set_size(g, 3 * w + 2 * BF_SP_S, 4 * h + 3 * BF_SP_S);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, BF_SP_S, 0);
    for(int i = 0; i < 12; i++) {
        lv_obj_t *b = bf_button(g, NULL, keys[i], 0);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_min_height(b, 0, 0);
        lv_obj_set_style_pad_ver(b, 0, 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if(l) lv_obj_set_style_text_font(l, BF_FONT_L, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)keys[i]);
    }
    return g;
}

static void build_dial(void)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, BF_SP_XL, 0);
    lv_obj_update_layout(body);
    int kh = (lv_obj_get_content_height(body) - 3 * BF_SP_S) / 4;
    if(kh > 60) kh = 60;
    if(kh < 40) kh = 40;
    keypad(body, on_key, 104, kh);
    lv_obj_t *right = bf_card(body);             /* cartão: o texto fica legível sobre o papel de parede */
    lv_obj_set_size(right, 360, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, BF_SP_M, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = bf_plain(right);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    num_lbl = bf_label(row, "", BF_FONT_L, pal()->text);
    lv_obj_set_flex_grow(num_lbl, 1);
    lv_label_set_long_mode(num_lbl, LV_LABEL_LONG_DOT);
    lv_obj_t *bs = bf_button(row, LV_SYMBOL_BACKSPACE, NULL, 0);
    lv_obj_set_size(bs, 72, 56);
    lv_obj_add_event_cb(bs, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bs, on_clear, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *call = bf_button(right, LV_SYMBOL_CALL, tr(S_PH_CALL), 1);
    lv_obj_set_size(call, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(call, lv_color_hex(C_PHONE), 0);
    lv_obj_add_event_cb(call, on_call, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hint = bf_label(right, tr(S_PH_REDIAL_HINT), BF_FONT_S, pal()->text_dim);
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

static void sync_text(char *b, size_t n)
{
    if(!strcmp(cur.sync, "andamento")) { snprintf(b, n, "%s", tr(S_PH_IMPORTING)); return; }
    const char *d = cur.data[0] ? cur.data : ph_last_data();
    char path[160];
    snprintf(path, sizeof path, "%s/sincronizado", d);
    FILE *f = d[0] ? fopen(path, "r") : NULL;
    long t = 0;
    if(f) { if(fscanf(f, "%ld", &t) != 1) t = 0; fclose(f); }
    if(!t) { snprintf(b, n, "%s", tr(S_PH_NODATA)); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    char w[32];
    strftime(w, sizeof w, "%d/%m %H:%M", &tm);
    snprintf(b, n, tr(S_PH_SYNCED), w);
}

/* barra de cima das listas: busca (contatos), situação da importação, Importar, Apagar */
/* aviso dentro da lista (num cartão, legível sobre o papel de parede) */
static void list_note(const char *txt)
{
    lv_obj_t *c = bf_card(list);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *l = bf_label(c, txt, BF_FONT_M, pal()->text_dim);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

static void list_header(bool search)
{
    lv_obj_t *row = bf_card(body);
    lv_obj_set_style_pad_all(row, BF_SP_XS, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    if(search) {
        search_ta = lv_textarea_create(row);
        lv_textarea_set_one_line(search_ta, true);
        lv_textarea_set_max_length(search_ta, 40);
        lv_textarea_set_placeholder_text(search_ta, tr(S_PH_SEARCH));
        lv_obj_set_style_text_font(search_ta, BF_FONT_M, 0);
        lv_obj_set_height(search_ta, 52);
        lv_obj_set_flex_grow(search_ta, 1);
    }
    sync_lbl = bf_label(row, "", BF_FONT_S, pal()->text_dim);
    if(search) lv_obj_set_width(sync_lbl, 120);
    else lv_obj_set_flex_grow(sync_lbl, 1);
    lv_label_set_long_mode(sync_lbl, LV_LABEL_LONG_WRAP);
    char b[200];
    sync_text(b, sizeof b);
    lv_label_set_text(sync_lbl, b);
    if(search) lv_obj_set_style_text_align(sync_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *imp = bf_button(row, LV_SYMBOL_DOWNLOAD, search ? NULL : tr(S_PH_IMPORT), 1);
    lv_obj_set_height(imp, 52);
    if(search) lv_obj_set_width(imp, 64);
    lv_obj_add_event_cb(imp, on_import, LV_EVENT_CLICKED, NULL);
    lv_obj_t *er = bf_button(row, LV_SYMBOL_TRASH, NULL, 2);
    lv_obj_set_size(er, 64, 52);
    lv_obj_add_event_cb(er, on_erase, LV_EVENT_CLICKED, NULL);
    list = bf_plain(body);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    bf_scrollable(list);
}

static lv_obj_t *list_row(const char *sym, lv_color_t c, const char *title, const char *sub, const char *right)
{
    lv_obj_t *row = bf_card(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_set_style_pad_ver(row, BF_SP_S, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    page_round_icon(row, sym, c, 40);
    lv_obj_t *colm = bf_plain(row);
    lv_obj_set_height(colm, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(colm, 1);
    lv_obj_set_flex_flow(colm, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = bf_label(colm, title, BF_FONT_M, pal()->text);
    bf_label_ellipsis(t, 520);
    if(sub && sub[0]) {
        lv_obj_t *s = bf_label(colm, sub, BF_FONT_S, pal()->text_dim);
        lv_obj_set_width(s, 560);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_height(s, 40, 0);
    }
    if(right && right[0]) bf_label(row, right, BF_FONT_S, pal()->text_dim);
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
        lv_obj_t *r = list_row(LV_SYMBOL_CALL, lv_color_hex(C_PHONE), name[0] ? name : first, first, right);
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
}

static void on_search_focus(lv_event_t *e)
{
    (void)e;
    if(search_kb) return;
    search_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(search_kb, BF_SCREEN_W, 210);
    lv_obj_align(search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(search_kb, search_ta);
    lv_obj_add_event_cb(search_kb, kb_close, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(search_kb, kb_close, LV_EVENT_CANCEL, NULL);
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
    if(!contacts.n) {
        list_note(tr(S_PH_NODATA));
    }
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
        const char *sym = kind[0] == 'F' ? LV_SYMBOL_UPLOAD : kind[0] == 'P' ? LV_SYMBOL_CLOSE : LV_SYMBOL_DOWNLOAD;
        lv_color_t c = kind[0] == 'P' ? pal()->danger : kind[0] == 'F' ? lv_color_hex(BF_BLUE) : lv_color_hex(C_PHONE);
        const char *sub = kind[0] == 'P' ? tr(S_PH_MISSED) : kind[0] == 'F' ? tr(S_PH_OUTGOING) : tr(S_PH_RECEIVED);
        char subl[96];
        snprintf(subl, sizeof subl, "%s%s%s", sub, name[0] ? " · " : "", name[0] ? num : "");
        lv_obj_t *r = list_row(sym, c, name[0] ? name : num[0] ? num : tr(S_PH_UNKNOWN), subl, w);
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

static table_t msgs, sent;

static void build_msgs(void)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, BF_SP_S, 0);
    list_header(false);
    table_load(&msgs, "mensagens.tsv");
    table_load(&sent, "enviadas.tsv");
    if(!msgs.n && !sent.n) {
        /* sem mensagens: explica por quê (o Android não liberou o MAP, ou nada importado) */
        bool denied = strstr(cur.sync, "indisponivel") || strstr(cur.sync, "negado");
        const char *d = cur.data[0] ? cur.data : ph_last_data();
        char path[160];
        snprintf(path, sizeof path, "%s/sincronizado", d);
        bool synced = d[0] && !access(path, R_OK);
        list_note(tr(denied || (synced && !msgs.buf) ? S_PH_MSG_DENIED : synced ? S_PH_NOMSG : S_PH_NODATA));
        return;
    }
    /* recebidas e enviadas, mais recentes primeiro (a data AAAAMMDDTHHMMSS ordena como texto) */
    int a = 0, b = 0, shown = 0;
    while(shown < MAX_ROWS && (a < msgs.n || b < sent.n)) {
        char wa[24] = "", wb[24] = "";
        if(a < msgs.n) col(msgs.row[a], 1, wa, sizeof wa);
        if(b < sent.n) col(sent.row[b], 1, wb, sizeof wb);
        bool from_in = b >= sent.n || (a < msgs.n && strcmp(wa, wb) >= 0);
        const char *r = from_in ? msgs.row[a++] : sent.row[b++];
        char rd[4], when[24], name[96], addr[64], text[320], w[24], title[200];
        col(r, 0, rd, sizeof rd);
        col(r, 1, when, sizeof when);
        col(r, 2, name, sizeof name);
        col(r, 3, addr, sizeof addr);
        col(r, 4, text, sizeof text);
        fmt_when(when, w, sizeof w);
        const char *who = name[0] ? name : addr[0] ? addr : tr(S_PH_UNKNOWN);
        if(from_in) snprintf(title, sizeof title, "%s", who);
        else snprintf(title, sizeof title, tr(S_PH_TO), who);
        lv_color_t c = !from_in ? lv_color_hex(BF_BLUE) : rd[0] == '0' ? lv_color_hex(BF_AMBER) : pal()->text_dim;
        lv_obj_t *row = list_row(LV_SYMBOL_ENVELOPE, c, title, text, w);
        if(addr[0]) {
            char *p = (char *)r;
            for(int k = 0; k < 3 && p; k++) { p = strchr(p, '\t'); if(p) p++; }
            if(p) { char *t = strchr(p, '\t'); if(t) *t = 0; lv_obj_add_event_cb(row, on_recent, LV_EVENT_CLICKED, p); }
        }
        shown++;
    }
}

static void build_tab(void)
{
    kb_close(NULL);
    lv_obj_clean(body);
    search_ta = list = more_lbl = sync_lbl = num_lbl = NULL;
    lv_obj_set_style_pad_column(body, 0, 0);
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
    if(t == tab) return;
    tab = t;
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
    lv_obj_t *b = bf_button(col, sym, NULL, 0);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_min_height(b, 0, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PHONE), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
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
    lv_obj_t *l = bf_label(col, tr(label), BF_FONT_S, pal()->text_dim);
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
    call_panel = bf_card(root);
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
    page_round_icon(left, LV_SYMBOL_CALL, lv_color_hex(C_PHONE), 64);
    cp_name = bf_label(left, "", BF_FONT_L, p->text);
    bf_label_ellipsis(cp_name, 320);
    lv_obj_set_style_text_align(cp_name, LV_TEXT_ALIGN_CENTER, 0);
    cp_num = bf_label(left, "", BF_FONT_M, p->text_dim);
    cp_state = bf_label(left, "", BF_FONT_M, lv_color_hex(C_PHONE));
    cp_other = bf_label(left, "", BF_FONT_S, p->warning);     /* a outra chamada (em espera) */
    bf_label_ellipsis(cp_other, 320);
    lv_obj_add_flag(cp_other, LV_OBJ_FLAG_HIDDEN);
    cp_rec = bf_label(left, "\xE2\x80\xA2 ", BF_FONT_M, p->danger);
    lv_obj_add_flag(cp_rec, LV_OBJ_FLAG_HIDDEN);
    cp_note = bf_label(left, tr(S_PH_VOICE_NOTE), BF_FONT_S, p->text_dim);
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
    lv_obj_t *kc = bf_button(cp_dtmf, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(kc, 48, 48);
    lv_obj_add_event_cb(kc, on_sub_close, LV_EVENT_CLICKED, NULL);
    cp_volrow = sub_row(right);
    lv_obj_t *vm = bf_button(cp_volrow, LV_SYMBOL_MINUS, NULL, 0);
    lv_obj_set_size(vm, 52, 52);
    lv_obj_add_event_cb(vm, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    cp_vol = lv_slider_create(cp_volrow);
    lv_slider_set_range(cp_vol, 1, 10);
    lv_obj_set_width(cp_vol, 130);
    lv_obj_set_style_bg_color(cp_vol, lv_color_hex(C_PHONE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cp_vol, lv_color_hex(C_PHONE), LV_PART_KNOB);
    lv_obj_add_event_cb(cp_vol, on_vol, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *vp = bf_button(cp_volrow, LV_SYMBOL_PLUS, NULL, 0);
    lv_obj_set_size(vp, 52, 52);
    lv_obj_add_event_cb(vp, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *vc = bf_button(cp_volrow, LV_SYMBOL_OK, NULL, 0);
    lv_obj_set_size(vc, 52, 52);
    lv_obj_add_event_cb(vc, on_sub_close, LV_EVENT_CLICKED, NULL);
    show_only(cp_grid);

    /* falar pelo celular | FOSTON */
    cp_vrow = sub_row(right);
    bf_label(cp_vrow, tr(S_PH_TALK_ON), BF_FONT_S, p->text_dim);
    cp_vphone = bf_button(cp_vrow, LV_SYMBOL_CALL, tr(S_PH_TALK_PHONE), 0);
    cp_vfoston = bf_button(cp_vrow, LV_SYMBOL_AUDIO, "FOSTON", 0);
    for(lv_obj_t *b = cp_vphone; b; b = b == cp_vphone ? cp_vfoston : NULL) {
        lv_obj_set_size(b, 124, 44);
        lv_obj_set_style_min_height(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_PHONE), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, on_voice, LV_EVENT_CLICKED, b == cp_vfoston ? (void *)1 : NULL);
    }

    /* atender / desligar */
    lv_obj_t *btns = sub_row(right);
    cp_answer = bf_button(btns, LV_SYMBOL_CALL, tr(S_PH_ANSWER), 1);
    lv_obj_set_size(cp_answer, 170, 56);
    lv_obj_set_style_bg_color(cp_answer, lv_color_hex(C_PHONE), 0);
    lv_obj_add_event_cb(cp_answer, on_answer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hang = bf_button(btns, LV_SYMBOL_CLOSE, tr(S_PH_HANGUP), 2);
    lv_obj_set_size(hang, 170, 56);
    lv_obj_add_event_cb(hang, on_hangup, LV_EVENT_CLICKED, NULL);
    cp_keys = hang;                                /* compatibilidade: não usado */

    /* "voltar à ligação" (com a ligação minimizada para fazer outra) */
    cp_back = bf_button(root, LV_SYMBOL_CALL, tr(S_PH_BACK_TO_CALL), 1);
    lv_obj_add_flag(cp_back, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(cp_back, lv_color_hex(C_PHONE), 0);
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
    /* ao lado das abas: o celular; sinal e bateria dele */
    if(!cur.available) snprintf(b, sizeof b, "%s", tr(S_PH_NOSERVICE));
    else if(!cur.link) snprintf(b, sizeof b, "%s", tr(S_PH_NOPHONE));
    else {
        snprintf(b, sizeof b, "%s", cur.dev);
        /* operadora, antena (sinal) e bateria do celular, pelo HFP */
        size_t n2 = (size_t)snprintf(b2, sizeof b2, "%s%s", cur.op, cur.op[0] ? "  " : "");
        if(cur.signal >= 0 && cur.battery >= 0)
            snprintf(b2 + n2, sizeof b2 - n2, BF_SYMBOL_TOWER " %d/5  %s %d/5", cur.signal,
                     cur.battery >= 4 ? LV_SYMBOL_BATTERY_FULL : cur.battery >= 3 ? LV_SYMBOL_BATTERY_3 :
                     cur.battery >= 2 ? LV_SYMBOL_BATTERY_2 : cur.battery >= 1 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY,
                     cur.battery);
    }
    set_text(status_lbl, b);
    set_text(status2_lbl, b2);
    lv_obj_set_style_text_color(status_lbl, cur.link ? pal()->text_dim : pal()->warning, 0);
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
        if(finished || !strcmp(cur.sync, "idle")) { table_free(&contacts); if(tab != TAB_DIAL) build_tab(); }
        else if(sync_lbl) { sync_text(b, sizeof b); set_text(sync_lbl, b); }
    }
    update_call();
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
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
    static const char *const syms[NTABS] = { LV_SYMBOL_CALL, LV_SYMBOL_LIST, BF_SYMBOL_CLOCK, LV_SYMBOL_ENVELOPE };
    const bf_str_t names[NTABS] = { S_PH_TAB_DIAL, S_PH_TAB_CONTACTS, S_PH_TAB_RECENTS, S_PH_TAB_MSGS };
    for(int i = 0; i < NTABS; i++) {
        tab_btn[i] = bf_button(tabbar, syms[i], tr(names[i]), 0);
        lv_obj_set_height(tab_btn[i], 52);
        lv_obj_set_style_pad_hor(tab_btn[i], BF_SP_S, 0);
        for(uint32_t k = 0; k < lv_obj_get_child_count(tab_btn[i]); k++)   /* abas compactas: sobra lugar */
            lv_obj_set_style_text_font(lv_obj_get_child(tab_btn[i], (int32_t)k), BF_FONT_S, 0);
        lv_obj_set_style_bg_color(tab_btn[i], lv_color_hex(C_PHONE), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(tab_btn[i], lv_color_white(), LV_STATE_CHECKED);
        lv_obj_add_event_cb(tab_btn[i], on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *st = bf_card(tabbar);
    lv_obj_set_height(st, 52);
    lv_obj_set_style_min_width(st, 0, 0);
    lv_obj_set_style_pad_ver(st, 4, 0);
    lv_obj_set_style_pad_hor(st, BF_SP_S, 0);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(st, 1);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    status_lbl = bf_label(st, "", BF_FONT_S, p->text);
    status2_lbl = bf_label(st, "", BF_FONT_S, p->text_dim);
    for(lv_obj_t *l = status_lbl; l; l = l == status_lbl ? status2_lbl : NULL) {
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }

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
    if(t < 0 || t >= NTABS || !root) return;
    tab = t;
    build_tab();
}
