/* scr_book.c — Livros: biblioteca e leitor de TXT/EPUB.
 *
 * Biblioteca: *.txt e *.epub em /mnt/sd e <dados>/books (até 3 níveis),
 * com o progresso de cada um. Leitor: página por página medida com a fonte
 * real (lv_text_get_size + busca binária, quebrando em palavra), toque à
 * direita avança e à esquerda volta, três tamanhos de letra e três temas
 * (escuro, sépia, claro). A posição é salva em <dados>/books.conf a cada
 * página e reaberta no mesmo ponto. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/book.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_BOOKS  200
#define PAGE_MAX   6000          /* bytes no máximo por página */
#define HIST       512
#define C_BOOK     0xc98b2c

typedef struct { char *path; size_t pos, len; } shelf_t;

static shelf_t shelf[MAX_BOOKS];
static int n_shelf;

static book_t bk;
static char cur_path[512];
static size_t page_start, page_end;
static size_t hist[HIST];
static int n_hist;
static int font_idx = 1, theme_idx;

static lv_obj_t *root, *page_lbl, *foot_lbl, *reader, *tools;

static const lv_font_t *font(void)
{
    return font_idx == 0 ? BF_FONT_S : font_idx == 1 ? BF_FONT_M : BF_FONT_L;
}

/* ---------------- posições salvas ---------------- */

static void conf_path(char *out, size_t n) { data_path(out, n, "books.conf"); }

static size_t saved_pos(const char *path, size_t *len)
{
    char p[300], line[700];
    conf_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    size_t pos = 0;
    if(len) *len = 0;
    if(!f) return 0;
    while(fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if(!t2) continue;
        t2[strcspn(t2, "\n")] = 0;
        if(!strcmp(t2 + 1, path)) {
            pos = strtoul(line, NULL, 10);
            if(len) *len = strtoul(t1 + 1, NULL, 10);
        }
    }
    fclose(f);
    return pos;
}

static void save_pos(void)
{
    char p[300], tmp[310], line[700];
    conf_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w");
    if(!out) { if(in) fclose(in); return; }
    fprintf(out, "%zu\t%zu\t%s\n", page_start, bk.len, cur_path);
    while(in && fgets(line, sizeof line, in)) {
        char *t2 = strchr(line, '\t');
        t2 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if(!t2) continue;
        char name[600];
        snprintf(name, sizeof name, "%s", t2 + 1);
        name[strcspn(name, "\n")] = 0;
        if(strcmp(name, cur_path)) fputs(line, out);
    }
    if(in) fclose(in);
    fclose(out);
    rename(tmp, p);
}

/* ---------------- paginação ---------------- */

static int text_w, text_h;

static bool fits(size_t start, size_t n)
{
    static char buf[PAGE_MAX + 1];
    memcpy(buf, bk.text + start, n);
    buf[n] = 0;
    lv_point_t sz;
    lv_text_get_size(&sz, buf, font(), 0, 4, text_w, LV_TEXT_FLAG_NONE);
    return sz.y <= text_h;
}

/* fim da página que começa em `start` (busca binária, termina em palavra) */
static size_t page_from(size_t start)
{
    size_t left = bk.len - start, hi = left < PAGE_MAX ? left : PAGE_MAX, lo = 0;
    while(lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while(mid > lo && ((unsigned char)bk.text[start + mid] & 0xc0) == 0x80) mid--;   /* não corta UTF-8 */
        if(mid == lo) { lo = hi; break; }
        if(fits(start, mid)) lo = mid; else hi = mid - 1;
    }
    size_t end = start + lo;
    if(end < bk.len) {
        size_t w = end;
        while(w > start && w + 60 > end && bk.text[w - 1] != ' ' && bk.text[w - 1] != '\n') w--;
        if(w > start && w + 60 > end) end = w;
    }
    if(end == start && start < bk.len) end = start + 1;
    return end;
}

/* início da página que termina em `end` (para voltar sem histórico) */
static size_t page_before(size_t end)
{
    size_t lo = end > PAGE_MAX ? end - PAGE_MAX : 0, hi = end;
    while(lo < hi) {                     /* menor início cuja página alcança `end` */
        size_t mid = (lo + hi) / 2;
        if(page_from(mid) >= end) hi = mid; else lo = mid + 1;
    }
    while(lo > 0 && ((unsigned char)bk.text[lo] & 0xc0) == 0x80) lo--;
    return lo < end ? lo : (end ? end - 1 : 0);
}

static void show_page(void)
{
    page_end = page_from(page_start);
    char *t = strndup(bk.text + page_start, page_end - page_start);
    lv_label_set_text(page_lbl, t ? t : "");
    free(t);
    char f[96];
    int pct = bk.len ? (int)((unsigned long long)page_end * 100 / bk.len) : 100;
    snprintf(f, sizeof f, "%d%%", pct);
    lv_label_set_text(foot_lbl, f);
    save_pos();
}

static void next_page(void)
{
    if(page_end >= bk.len) return;
    if(n_hist == HIST) { memmove(hist, hist + 1, sizeof hist[0] * (HIST - 1)); n_hist--; }
    hist[n_hist++] = page_start;
    page_start = page_end;
    show_page();
}

static void prev_page(void)
{
    if(!page_start) return;
    page_start = n_hist ? hist[--n_hist] : page_before(page_start);
    show_page();
}

/* ---------------- leitor ---------------- */

static void apply_theme(void)
{
    static const uint32_t bg[3] = { 0, 0xf4ecd8, 0xfafafa }, fg[3] = { 0, 0x5b4636, 0x1a1a1a };
    lv_color_t b = theme_idx ? lv_color_hex(bg[theme_idx]) : pal()->bg;
    lv_color_t f = theme_idx ? lv_color_hex(fg[theme_idx]) : pal()->text;
    lv_obj_set_style_bg_color(reader, b, 0);
    lv_obj_set_style_text_color(page_lbl, f, 0);
    lv_obj_set_style_text_color(foot_lbl, lv_color_mix(f, b, 140), 0);
}

static void reader_click(lv_event_t *e)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    int x = pt.x - a.x1, w = lv_area_get_width(&a);
    if(x < w / 3) prev_page();
    else if(x > w * 2 / 3) next_page();
    else if(lv_obj_has_flag(tools, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(tools, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(tools, LV_OBJ_FLAG_HIDDEN);
}

static void relayout(void)
{
    lv_obj_set_style_text_font(page_lbl, font(), 0);
    lv_obj_set_style_text_line_space(page_lbl, 4, 0);
    n_hist = 0;
    show_page();
}

static void font_click(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int f = font_idx + d;
    if(f < 0 || f > 2) return;
    font_idx = f;
    relayout();
}

static void theme_click(lv_event_t *e)
{
    (void)e;
    theme_idx = (theme_idx + 1) % 3;
    apply_theme();
}

static void back_to_shelf(lv_event_t *e);

static void open_book(const char *path)
{
    char err[160];
    book_free(&bk);
    lv_obj_clean(root);
    if(!book_load(path, &bk, err, sizeof err)) {
        const bf_palette_t *p = pal();
        lv_obj_t *c = bf_card(root);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, BF_SP_XL, 0);
        lv_obj_t *l = bf_label(c, "", BF_FONT_M, p->warning);
        lv_label_set_text_fmt(l, "%s\n%s", tr(S_BOOK_CANT_OPEN), err);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_t *b = bf_button(root, LV_SYMBOL_LEFT, tr(S_BOOK_SHELF), 0);
        lv_obj_add_event_cb(b, back_to_shelf, LV_EVENT_CLICKED, NULL);
        return;
    }
    snprintf(cur_path, sizeof cur_path, "%s", path);
    page_start = saved_pos(path, NULL);
    if(page_start >= bk.len) page_start = 0;
    while(page_start > 0 && ((unsigned char)bk.text[page_start] & 0xc0) == 0x80) page_start--;
    n_hist = 0;

    reader = lv_obj_create(root);
    lv_obj_remove_style_all(reader);
    lv_obj_set_size(reader, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(reader, LV_OPA_COVER, 0);
    lv_obj_add_flag(reader, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(reader, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reader, reader_click, LV_EVENT_CLICKED, NULL);

    text_w = BF_SCREEN_W - 2 * 28;
    text_h = BF_CONTENT_H - 16 - 26;
    page_lbl = lv_label_create(reader);
    lv_label_set_long_mode(page_lbl, LV_LABEL_LONG_WRAP);   /* quebra igual à medida da paginação */
    lv_obj_set_size(page_lbl, text_w, text_h);
    lv_obj_set_pos(page_lbl, 28, 12);
    foot_lbl = lv_label_create(reader);
    lv_obj_set_style_text_font(foot_lbl, BF_FONT_S, 0);
    lv_obj_align(foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* ferramentas (toque no meio da página mostra/esconde) */
    tools = bf_card(reader);
    lv_obj_set_size(tools, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(tools, BF_SP_S, 0);
    lv_obj_set_style_pad_column(tools, BF_SP_S, 0);
    lv_obj_align(tools, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_t *b;
    b = bf_button(tools, LV_SYMBOL_LEFT, tr(S_BOOK_SHELF), 0);
    lv_obj_add_event_cb(b, back_to_shelf, LV_EVENT_CLICKED, NULL);
    b = bf_button(tools, NULL, "A-", 0);
    lv_obj_add_event_cb(b, font_click, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    b = bf_button(tools, NULL, "A+", 0);
    lv_obj_add_event_cb(b, font_click, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    b = bf_button(tools, BF_SYMBOL_MOON, tr(S_BOOK_THEME), 0);
    lv_obj_add_event_cb(b, theme_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(tools, LV_OBJ_FLAG_HIDDEN);

    apply_theme();
    relayout();
}

/* ---------------- biblioteca ---------------- */

static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if(!d) return;
    struct dirent *e;
    char p[512];
    while(n_shelf < MAX_BOOKS && (e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        if(snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if(stat(p, &st)) continue;
        if(S_ISDIR(st.st_mode)) { if(depth < 3) scan(p, depth + 1); }
        else if(S_ISREG(st.st_mode) && book_is_supported(e->d_name)) {
            shelf[n_shelf].path = strdup(p);
            shelf[n_shelf].pos = saved_pos(p, &shelf[n_shelf].len);
            n_shelf++;
        }
    }
    closedir(d);
}

static int cmp_shelf(const void *a, const void *b)
{
    return strcasecmp(((const shelf_t *)a)->path, ((const shelf_t *)b)->path);
}

static void pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i >= 0 && i < n_shelf) open_book(shelf[i].path);
}

static void build_shelf(void)
{
    const bf_palette_t *p = pal();
    char books[300];
    for(int i = 0; i < n_shelf; i++) free(shelf[i].path);
    n_shelf = 0;
    scan("/mnt/sd", 0);
    data_path(books, sizeof books, "books");
    scan(books, 0);
    qsort(shelf, n_shelf, sizeof shelf[0], cmp_shelf);

    lv_obj_clean(root);
    reader = NULL;
    if(!n_shelf) {
        lv_obj_t *c = bf_card(root);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, BF_SP_XL, 0);
        lv_obj_t *l = bf_label(c, tr(S_BOOK_EMPTY), BF_FONT_M, p->text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }
    lv_obj_t *list = bf_card(root);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, BF_SP_XS, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    for(int i = 0; i < n_shelf; i++) {
        lv_obj_t *it = lv_button_create(list);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, LV_PCT(100), 52);
        lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
        lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
        lv_obj_add_event_cb(it, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        bool epub = strcasestr(shelf[i].path, ".epub") != NULL;
        lv_obj_t *ic = bf_label(it, epub ? LV_SYMBOL_FILE : LV_SYMBOL_LIST, BF_FONT_M, lv_color_hex(C_BOOK));
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
        const char *n = strrchr(shelf[i].path, '/');
        lv_obj_t *l = bf_label(it, n ? n + 1 : shelf[i].path, BF_FONT_M, p->text);
        bf_label_ellipsis(l, 600);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
        if(shelf[i].len) {
            char pc[16];
            snprintf(pc, sizeof pc, "%d%%", (int)((unsigned long long)shelf[i].pos * 100 / shelf[i].len));
            lv_obj_t *r = bf_label(it, pc, BF_FONT_S, p->text_dim);
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

static void back_to_shelf(lv_event_t *e)
{
    (void)e;
    book_free(&bk);
    build_shelf();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if(bk.text) open_book(cur_path);         /* volta ao livro aberto */
    else build_shelf();
}

static void destroy(void)
{
    root = page_lbl = foot_lbl = reader = tools = NULL;
}

void scr_book_demo(const char *path)
{
    if(path) open_book(path);
    else { book_free(&bk); build_shelf(); }
}

const bf_screen_def_t scr_book_def = { create, destroy, S_APP_BOOK, NULL };
