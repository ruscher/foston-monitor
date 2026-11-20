/* scr_book.c — Livros: biblioteca e leitor (TXT, EPUB, PDF em texto, SVG).
 *
 * Biblioteca: grade de capas (capa JPEG do EPUB, carregada aos poucos, ou
 * uma capa gerada), tipo do arquivo e barra de progresso de cada livro.
 * Leitor: páginas medidas com a fonte real (lv_text_get_size + busca
 * binária, quebra em palavra), paginação completa em segundo plano (~25 ms
 * por tique) com cache por livro e layout, rodapé com capítulo e
 * "Pág. N de M". Toque no meio mostra os controles: biblioteca, sumário,
 * marcadores, busca, barra deslizante de páginas com ◀ ▶, tamanho da letra
 * (A−/A+ é o zoom do texto), espaçamento, margens e tema. Deslizar ou tocar
 * nas bordas vira a página. SVG abre no visualizador com zoom e arrasto.
 * Posição salva em <dados>/books.conf; marcadores em bookmarks.conf. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/book.h"
#include "../core/cover.h"
#include "../widgets/imgview.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_BOOKS   200
#define PAGE_MAX    6000
#define C_BOOK      0xc98b2c
#define FOOT_H      24
#define TOP_PAD     10
#define CARD_W      138
#define COVER_W     104
#define COVER_H     136

/* ---------------- estado ---------------- */

typedef struct {
    char *path;
    book_kind_t kind;
    size_t pos, len;
    lv_image_dsc_t cover;
    bool cover_tried;
    lv_obj_t *cover_obj;          /* contêiner da capa na grade (NULL fora da tela) */
} shelf_t;

static shelf_t shelf[MAX_BOOKS];
static int n_shelf;
static lv_timer_t *cover_timer;
static int cover_next;

static book_t bk;
static char cur_path[512];
static char pending[512];       /* arquivo pedido pelo Arquivos */
static size_t page_start, page_end;

static size_t *pages;            /* início de cada página (paginação completa) */
static int n_pages, cap_pages;
static size_t pag_pos;
static bool pag_done;
static lv_timer_t *pag_timer;
static uint32_t pag_t0;          /* medição: início da contagem de páginas */
static int text_w, text_h, line_space;

static lv_obj_t *root, *reader, *page_lbl, *foot_l, *foot_r, *top_bar, *bot_bar, *slider, *slider_lbl;
static lv_obj_t *panel;          /* sumário/marcadores/busca */
static lv_obj_t *search_ta, *search_kb, *search_msg;

static const lv_font_t *font(void)
{
    int f = settings()->book_font;
    return f == 0 ? BF_FONT_S : f == 1 ? BF_FONT_M : BF_FONT_L;
}

/* ---------------- posições e marcadores (arquivos de texto) ---------------- */

static size_t saved_pos(const char *path, size_t *len)
{
    char p[300], line[700];
    data_path(p, sizeof p, "books.conf");
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

/* reescreve um arquivo de linhas "...\tcaminho", mantendo as aceitas por keep */
static void rewrite_conf(const char *name, const char *first_lines, bool (*keep)(const char *line))
{
    char p[300], tmp[320], line[1200];
    data_path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w");
    if(!out) { if(in) fclose(in); return; }
    if(first_lines) fputs(first_lines, out);
    while(in && fgets(line, sizeof line, in)) if(keep(line)) fputs(line, out);
    if(in) fclose(in);
    fclose(out);
    rename(tmp, p);
}

/* último campo (caminho) de uma linha com tabs */
static const char *line_path(const char *line, char *buf, size_t n)
{
    const char *t = strrchr(line, '\t');
    snprintf(buf, n, "%s", t ? t + 1 : "");
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

static bool keep_other_book(const char *line)
{
    char b[700];
    return strcmp(line_path(line, b, sizeof b), cur_path) != 0;
}

static void save_pos(void)
{
    char l[700];
    snprintf(l, sizeof l, "%zu\t%zu\t%s\n", page_start, bk.len, cur_path);
    rewrite_conf("books.conf", l, keep_other_book);
}

/* marcadores: "offset\ttrecho\tcaminho" */
typedef struct { size_t off; char snip[80]; } mark_t;

static int load_marks(mark_t *m, int max)
{
    char p[300], line[1200];
    data_path(p, sizeof p, "bookmarks.conf");
    FILE *f = fopen(p, "r");
    int n = 0;
    if(!f) return 0;
    while(n < max && fgets(line, sizeof line, f)) {
        char b[700];
        if(strcmp(line_path(line, b, sizeof b), cur_path)) continue;
        char *t1 = strchr(line, '\t'), *t2 = strrchr(line, '\t');
        if(!t1 || t2 <= t1) continue;
        m[n].off = strtoul(line, NULL, 10);
        snprintf(m[n].snip, sizeof m[n].snip, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
        n++;
    }
    fclose(f);
    return n;
}

static size_t mark_drop_off;

static bool keep_not_dropped(const char *line)
{
    char b[700];
    if(strcmp(line_path(line, b, sizeof b), cur_path)) return true;
    return (size_t)strtoul(line, NULL, 10) != mark_drop_off;
}

/* ---------------- paginação ---------------- */

static bool fits(size_t start, size_t n)
{
    static char buf[PAGE_MAX + 1];
    memcpy(buf, bk.text + start, n);
    buf[n] = 0;
    lv_point_t sz;
    lv_text_get_size(&sz, buf, font(), 0, line_space, text_w, LV_TEXT_FLAG_NONE);
    return sz.y <= text_h;
}

static size_t page_from(size_t start)
{
    size_t left = bk.len - start, hi = left < PAGE_MAX ? left : PAGE_MAX, lo = 0;
    while(lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while(mid > lo && ((unsigned char)bk.text[start + mid] & 0xc0) == 0x80) mid--;
        if(mid == lo) { lo = hi; break; }
        if(fits(start, mid)) lo = mid; else hi = mid - 1;
    }
    size_t end = start + lo;
    if(end < bk.len) {
        size_t w = end;
        while(w > start && w + 60 > end && bk.text[w - 1] != ' ' && bk.text[w - 1] != '\n') w--;
        if(w > start && w + 60 > end) end = w;
    }
    if(end == start && start < bk.len) {
        end = start + 1;
        while(end < bk.len && ((unsigned char)bk.text[end] & 0xc0) == 0x80) end++;
    }
    return end;
}

static size_t page_before(size_t end)
{
    size_t lo = end > PAGE_MAX ? end - PAGE_MAX : 0, hi = end;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        if(page_from(mid) >= end) hi = mid; else lo = mid + 1;
    }
    while(lo > 0 && ((unsigned char)bk.text[lo] & 0xc0) == 0x80) lo--;
    return lo < end ? lo : (end ? end - 1 : 0);
}

/* índice da página que contém `off` (só com a paginação completa) */
static int page_index(size_t off)
{
    int lo = 0, hi = n_pages - 1;
    while(lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if(pages[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* chave do cache: caminho + tamanho + layout */
static void cache_path(char *out, size_t n)
{
    uint32_t h = 2166136261u;
    for(const char *s = cur_path; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    char dir[300];
    data_path(dir, sizeof dir, "bookcache");
    mkdir(dir, 0755);
    snprintf(out, n, "%s/%08x-%zu-%d-%d-%d.pg", dir, (unsigned)h, bk.len, settings()->book_font, line_space, text_w);
}

static bool cache_load(void)
{
    char p[400];
    cache_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if(!f) return false;
    int n = 0;
    bool ok = fread(&n, sizeof n, 1, f) == 1 && n > 0 && n < 10000000;
    if(ok) {
        size_t *pg = malloc(sizeof *pg * (size_t)n);
        ok = pg && fread(pg, sizeof *pg, (size_t)n, f) == (size_t)n && pg[0] == 0;
        for(int i = 1; ok && i < n; i++) ok = pg[i] > pg[i - 1] && pg[i] < bk.len;
        if(ok) { free(pages); pages = pg; n_pages = cap_pages = n; }
        else free(pg);
    }
    fclose(f);
    return ok;
}

static void cache_save(void)
{
    char p[400];
    cache_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if(!f) return;
    fwrite(&n_pages, sizeof n_pages, 1, f);
    fwrite(pages, sizeof *pages, (size_t)n_pages, f);
    fclose(f);
}

static void update_footer(void);
static void show_page(void);

static void pag_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t t0 = lv_tick_get();
    while(pag_pos < bk.len && lv_tick_elaps(t0) < 25) {
        if(n_pages == cap_pages) {
            int c = cap_pages ? cap_pages * 2 : 256;
            size_t *np = realloc(pages, sizeof *np * (size_t)c);
            if(!np) { pag_pos = bk.len; break; }
            pages = np;
            cap_pages = c;
        }
        pages[n_pages++] = pag_pos;
        pag_pos = page_from(pag_pos);
    }
    if(pag_pos >= bk.len) {
        pag_done = true;
        if(pag_timer) { lv_timer_delete(pag_timer); pag_timer = NULL; }
        fprintf(stderr, "livros: %d páginas de %zu bytes em %u ms\n", n_pages, bk.len, lv_tick_elaps(pag_t0));
        cache_save();
        if(page_lbl) show_page();        /* encaixa a posição na página calculada */
    }
    update_footer();
}

static void start_pagination(void)
{
    if(pag_timer) { lv_timer_delete(pag_timer); pag_timer = NULL; }
    n_pages = 0;
    pag_pos = 0;
    pag_done = false;
    int m = settings()->book_margin, s = settings()->book_space;
    int margin = m == 0 ? 16 : m == 1 ? 32 : 64;
    line_space = s == 0 ? 2 : s == 1 ? 6 : 12;
    text_w = BF_SCREEN_W - 2 * margin;
    text_h = BF_CONTENT_H - TOP_PAD - FOOT_H - 2;
    if(page_lbl) {
        lv_obj_set_style_text_font(page_lbl, font(), 0);
        lv_obj_set_style_text_line_space(page_lbl, line_space, 0);
        lv_obj_set_size(page_lbl, text_w, text_h);
        lv_obj_set_pos(page_lbl, margin, TOP_PAD);
    }
    if(cache_load()) { pag_done = true; return; }
    pag_t0 = lv_tick_get();
    pag_timer = lv_timer_create(pag_tick, 30, NULL);
}

/* ---------------- leitor ---------------- */

static void update_footer(void)
{
    if(!foot_r) return;
    char r[96], l[160];
    int pct = bk.len ? (int)((unsigned long long)page_end * 100 / bk.len) : 100;
    if(pag_done && n_pages) {
        int i = page_index(page_start);
        snprintf(r, sizeof r, "%s %d %s %d  ·  %d%%", tr(S_BOOK_PAGE), i + 1, tr(S_BOOK_OF), n_pages, pct);
        if(slider) {
            int mx = n_pages > 1 ? n_pages : 2;
            if(lv_slider_get_max_value(slider) != mx) lv_slider_set_range(slider, 1, mx);
            lv_slider_set_value(slider, i + 1, LV_ANIM_OFF);
            lv_obj_remove_state(slider, LV_STATE_DISABLED);
            snprintf(l, sizeof l, "%d / %d", i + 1, n_pages);
            lv_label_set_text(slider_lbl, l);
        }
    }
    else {
        int pp = bk.len ? (int)((unsigned long long)pag_pos * 100 / bk.len) : 0;
        snprintf(r, sizeof r, "%d%%  ·  %s %d%%", pct, tr(S_BOOK_PAGINATING), pp);
        if(slider) {
            lv_obj_add_state(slider, LV_STATE_DISABLED);
            lv_label_set_text(slider_lbl, "…");
        }
    }
    lv_label_set_text(foot_r, r);
    int c = book_chapter_at(&bk, page_start);
    if(bk.kind == BK_PDF) snprintf(l, sizeof l, "PDF  ·  %s", c >= 0 ? bk.chapters[c].title : "");
    else snprintf(l, sizeof l, "%s", c >= 0 ? bk.chapters[c].title : bk.title);
    lv_label_set_text(foot_l, l);
}

static void show_page(void)
{
    if(!page_lbl) return;
    if(pag_done && n_pages) {
        int i = page_index(page_start);
        page_start = pages[i];
        page_end = i + 1 < n_pages ? pages[i + 1] : bk.len;
    }
    else page_end = page_from(page_start);
    char *t = strndup(bk.text + page_start, page_end - page_start);
    lv_label_set_text(page_lbl, t ? t : "");
    free(t);
    update_footer();
    save_pos();
}

static void goto_offset(size_t off)
{
    if(off >= bk.len) off = bk.len ? bk.len - 1 : 0;
    page_start = off;
    if(!pag_done)      /* sem a paginação completa: começa a página numa palavra */
        while(page_start > 0 && bk.text[page_start - 1] != '\n' && bk.text[page_start - 1] != ' ' && off - page_start < 200)
            page_start--;
    show_page();
}

static void next_page(void)
{
    if(page_end >= bk.len) return;
    page_start = page_end;
    show_page();
}

static void prev_page(void)
{
    if(!page_start) return;
    if(pag_done && n_pages) { int i = page_index(page_start); page_start = pages[i > 0 ? i - 1 : 0]; }
    else page_start = page_before(page_start);
    show_page();
}

static void apply_theme(void)
{
    static const uint32_t bg[3] = { 0, 0xf4ecd8, 0xfafafa }, fg[3] = { 0, 0x4a3a2a, 0x1a1a1a };
    int t = settings()->book_theme;
    lv_color_t b = t ? lv_color_hex(bg[t]) : pal()->bg;
    lv_color_t f = t ? lv_color_hex(fg[t]) : pal()->text;
    lv_obj_set_style_bg_color(reader, b, 0);
    lv_obj_set_style_text_color(page_lbl, f, 0);
    lv_color_t dim = lv_color_mix(f, b, 130);
    lv_obj_set_style_text_color(foot_l, dim, 0);
    lv_obj_set_style_text_color(foot_r, dim, 0);
}

static void toggle_controls(bool show)
{
    if(!top_bar) return;
    if(show) { lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(bot_bar, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(top_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(bot_bar, LV_OBJ_FLAG_HIDDEN); }
}

static void reader_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_GESTURE) {
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if(d == LV_DIR_LEFT) next_page();
        else if(d == LV_DIR_RIGHT) prev_page();
        lv_indev_wait_release(lv_indev_active());
        return;
    }
    if(c != LV_EVENT_CLICKED) return;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(reader, &a);
    int x = pt.x - a.x1, w = lv_area_get_width(&a);
    if(x < w / 4) prev_page();
    else if(x > w * 3 / 4) next_page();
    else toggle_controls(lv_obj_has_flag(top_bar, LV_OBJ_FLAG_HIDDEN));
}

static void relayout(void)
{
    size_t keep = page_start;
    start_pagination();
    goto_offset(keep);
}

static void font_click(lv_event_t *e)
{
    int f = settings()->book_font + (int)(intptr_t)lv_event_get_user_data(e);
    if(f < 0 || f > 2) return;
    settings()->book_font = f;
    settings_save();
    relayout();
}

static void space_click(lv_event_t *e)
{
    (void)e;
    settings()->book_space = (settings()->book_space + 1) % 3;
    settings_save();
    relayout();
}

static void margin_click(lv_event_t *e)
{
    (void)e;
    settings()->book_margin = (settings()->book_margin + 1) % 3;
    settings_save();
    relayout();
}

static void theme_click(lv_event_t *e)
{
    (void)e;
    settings()->book_theme = (settings()->book_theme + 1) % 3;
    settings_save();
    apply_theme();
}

static void step_click(lv_event_t *e)
{
    if((intptr_t)lv_event_get_user_data(e) > 0) next_page(); else prev_page();
}

static void slider_event(lv_event_t *e)
{
    if(!pag_done || !n_pages) return;
    int v = (int)lv_slider_get_value(slider);
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        char l[32];
        snprintf(l, sizeof l, "%d / %d", v, n_pages);
        lv_label_set_text(slider_lbl, l);
    }
    else if(v >= 1 && v <= n_pages) {     /* soltou: vai para a página */
        page_start = pages[v - 1];
        show_page();
    }
}

/* ---------------- painéis: sumário, marcadores, busca ---------------- */

static void panel_close(lv_event_t *e)
{
    (void)e;
    if(search_kb) { lv_obj_delete(search_kb); search_kb = NULL; }
    if(panel) { lv_obj_delete(panel); panel = NULL; }
    search_ta = search_msg = NULL;
}

static void panel_open(const char *title)
{
    const bf_palette_t *p = pal();
    panel_close(NULL);
    panel = bf_card(reader);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(panel, 460, LV_PCT(100));
    lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, BF_SP_S, 0);
    lv_obj_set_style_pad_row(panel, 2, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_50, 0);
    lv_obj_t *hdr = bf_plain(panel);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    lv_obj_t *t = bf_label(hdr, title, BF_FONT_M, p->text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, BF_SP_S, 0);
    lv_obj_t *x = bf_button(hdr, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 48, 40);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(x, panel_close, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *panel_row(const char *left, const char *right, lv_event_cb_t cb, intptr_t ud, bool hl)
{
    const bf_palette_t *p = pal();
    lv_obj_t *it = lv_button_create(panel);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, LV_PCT(100), 44);
    lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
    lv_obj_add_event_cb(it, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *l = bf_label(it, left, BF_FONT_S, hl ? lv_color_hex(C_BOOK) : p->text);
    bf_label_ellipsis(l, right ? 330 : 400);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if(right) {
        lv_obj_t *r = bf_label(it, right, BF_FONT_S, p->text_dim);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return it;
}

static void toc_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    panel_close(NULL);
    if(i >= 0 && i < bk.n_chapters) goto_offset(bk.chapters[i].off);
}

static void open_toc(lv_event_t *e)
{
    (void)e;
    panel_open(tr(S_BOOK_TOC));
    if(!bk.n_chapters) { bf_label(panel, tr(S_BOOK_NO_TOC), BF_FONT_S, pal()->text_dim); return; }
    int cur = book_chapter_at(&bk, page_start);
    lv_obj_t *sel = NULL;
    for(int i = 0; i < bk.n_chapters; i++) {
        char r[24] = "";
        if(pag_done && n_pages) snprintf(r, sizeof r, "%d", page_index(bk.chapters[i].off) + 1);
        lv_obj_t *it = panel_row(bk.chapters[i].title, r[0] ? r : NULL, toc_pick, i, i == cur);
        if(i == cur) sel = it;
    }
    if(sel) lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
}

static mark_t marks[64];
static int n_marks;
static void open_marks(lv_event_t *e);

static void mark_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= n_marks) return;
    if(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {     /* toque longo apaga */
        mark_drop_off = marks[i].off;
        rewrite_conf("bookmarks.conf", NULL, keep_not_dropped);
        lv_indev_wait_release(lv_indev_active());
        lv_async_call((lv_async_cb_t)open_marks, NULL);     /* não recria o painel dentro do evento dele */
        return;
    }
    size_t off = marks[i].off;
    panel_close(NULL);
    goto_offset(off);
}

static void mark_add(lv_event_t *e)
{
    (void)e;
    char snip[64];
    size_t s = page_start;
    while(s < bk.len && (bk.text[s] == '\n' || bk.text[s] == ' ')) s++;
    size_t l = 0;
    while(s + l < bk.len && l < 50 && bk.text[s + l] != '\n') l++;
    while(l > 0 && s + l < bk.len && ((unsigned char)bk.text[s + l] & 0xc0) == 0x80) l--;
    snprintf(snip, sizeof snip, "%.*s", (int)l, bk.text + s);
    for(char *q = snip; *q; q++) if(*q == '\t') *q = ' ';
    char p[300];
    data_path(p, sizeof p, "bookmarks.conf");
    FILE *f = fopen(p, "a");
    if(f) {
        fprintf(f, "%zu\t%s\t%s\n", page_start, snip, cur_path);
        fclose(f);
    }
    lv_async_call((lv_async_cb_t)open_marks, NULL);
}

static void open_marks(lv_event_t *e)
{
    (void)e;
    if(!reader) return;
    panel_open(tr(S_BOOK_MARKS));
    lv_obj_t *add = bf_button(panel, LV_SYMBOL_PLUS, tr(S_BOOK_ADD_MARK), 1);
    lv_obj_set_width(add, LV_PCT(100));
    lv_obj_add_event_cb(add, mark_add, LV_EVENT_CLICKED, NULL);
    n_marks = load_marks(marks, 64);
    if(!n_marks) { bf_label(panel, tr(S_BOOK_NO_MARKS), BF_FONT_S, pal()->text_dim); return; }
    for(int i = 0; i < n_marks; i++) {
        char r[24] = "";
        if(pag_done && n_pages) snprintf(r, sizeof r, "%d", page_index(marks[i].off) + 1);
        lv_obj_t *it = panel_row(marks[i].snip[0] ? marks[i].snip : "…", r[0] ? r : NULL, mark_pick, i, false);
        lv_obj_add_event_cb(it, mark_pick, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    bf_label(panel, tr(S_BOOK_MARK_HINT), BF_FONT_S, pal()->text_dim);
}

/* busca simples, sem diferenciar maiúsculas em ASCII; continua do início */
static void do_search(lv_event_t *e)
{
    (void)e;
    if(!search_ta) return;
    const char *q = lv_textarea_get_text(search_ta);
    size_t ql = strlen(q);
    if(!ql) return;
    const char *hit = NULL;
    for(int pass = 0; pass < 2 && !hit; pass++) {
        size_t from = pass ? 0 : page_end, to = pass ? page_end : bk.len;
        for(size_t i = from; i + ql <= to; i++)
            if(!strncasecmp(bk.text + i, q, ql)) { hit = bk.text + i; break; }
    }
    if(!hit) { lv_label_set_text(search_msg, tr(S_BOOK_NOT_FOUND)); return; }
    size_t off = (size_t)(hit - bk.text);
    panel_close(NULL);
    goto_offset(off);
}

static void search_cancel(lv_event_t *e)
{
    (void)e;
    lv_async_call((lv_async_cb_t)panel_close, NULL);
}

static void open_search(lv_event_t *e)
{
    (void)e;
    panel_open(tr(S_BOOK_SEARCH));
    lv_obj_set_height(panel, 140);
    search_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(search_ta, true);
    lv_obj_set_width(search_ta, LV_PCT(100));
    lv_obj_set_style_text_font(search_ta, BF_FONT_M, 0);
    search_msg = bf_label(panel, "", BF_FONT_S, pal()->warning);
    search_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(search_kb, BF_SCREEN_W, 210);
    lv_obj_align(search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(search_kb, search_ta);
    lv_obj_add_event_cb(search_kb, do_search, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(search_kb, search_cancel, LV_EVENT_CANCEL, NULL);
}

/* ---------------- abrir livro ---------------- */

static void back_to_shelf(lv_event_t *e);
static void build_shelf(void);

static lv_obj_t *tool(lv_obj_t *parent, const char *sym, const char *txt, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = bf_button(parent, sym, txt, 0);
    lv_obj_set_height(b, 44);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static void bar_style(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(o, pal()->surface, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(o, BF_SP_S, 0);
    lv_obj_set_style_pad_ver(o, BF_SP_XS, 0);
    lv_obj_set_style_pad_column(o, BF_SP_S, 0);
    lv_obj_set_style_pad_row(o, BF_SP_XS, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void toast(const char *msg)
{
    lv_obj_t *l = bf_label(lv_layer_top(), msg, BF_FONT_M, pal()->warning);
    lv_obj_set_style_bg_color(l, pal()->surface, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(l, BF_SP_M, 0);
    lv_obj_center(l);
    lv_obj_delete_delayed(l, 2500);
}

static void open_book(const char *path)
{
    char err[200];
    if(book_kind(path) == BK_SVG) {
        if(!imgview_open(path)) toast(tr(S_FILES_BAD_IMAGE));
        return;
    }
    if(pag_timer) { lv_timer_delete(pag_timer); pag_timer = NULL; }
    panel_close(NULL);
    lv_obj_clean(root);
    reader = page_lbl = foot_l = foot_r = top_bar = bot_bar = slider = slider_lbl = NULL;
    bool loaded = bk.text && !strcmp(path, cur_path);      /* já carregado: só remonta a tela */
    if(!loaded) {
        book_free(&bk);
        free(pages);
        pages = NULL;
        n_pages = cap_pages = 0;
    }
    if(!loaded && !book_load(path, &bk, err, sizeof err)) {
        cur_path[0] = 0;
        const bf_palette_t *p = pal();
        lv_obj_t *c = bf_card(root);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, BF_SP_XL, 0);
        lv_obj_t *l = bf_label(c, "", BF_FONT_M, p->warning);
        lv_label_set_text_fmt(l, "%s\n%s", tr(S_BOOK_CANT_OPEN), err);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_t *b = bf_button(root, LV_SYMBOL_LEFT, tr(S_BOOK_SHELF), 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, BF_SP_M, -BF_SP_M);
        lv_obj_add_event_cb(b, back_to_shelf, LV_EVENT_CLICKED, NULL);
        return;
    }
    if(path != cur_path) snprintf(cur_path, sizeof cur_path, "%s", path);
    page_start = saved_pos(path, NULL);
    if(page_start >= bk.len) page_start = 0;
    while(page_start > 0 && ((unsigned char)bk.text[page_start] & 0xc0) == 0x80) page_start--;

    reader = lv_obj_create(root);
    lv_obj_remove_style_all(reader);
    lv_obj_set_size(reader, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(reader, LV_OPA_COVER, 0);
    lv_obj_add_flag(reader, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(reader, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(reader, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(reader, reader_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(reader, reader_event, LV_EVENT_GESTURE, NULL);

    page_lbl = lv_label_create(reader);
    lv_label_set_long_mode(page_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(page_lbl, LV_OBJ_FLAG_CLICKABLE);
    foot_l = lv_label_create(reader);
    lv_obj_set_style_text_font(foot_l, BF_FONT_S, 0);
    lv_label_set_long_mode(foot_l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(foot_l, 370, 18);
    lv_obj_align(foot_l, LV_ALIGN_BOTTOM_LEFT, 16, -3);
    foot_r = lv_label_create(reader);
    lv_obj_set_style_text_font(foot_r, BF_FONT_S, 0);
    lv_obj_align(foot_r, LV_ALIGN_BOTTOM_RIGHT, -16, -3);

    /* controles (ocultos até tocar no meio) */
    top_bar = lv_obj_create(reader);
    lv_obj_remove_style_all(top_bar);
    bar_style(top_bar);
    lv_obj_set_size(top_bar, LV_PCT(100), 56);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tool(top_bar, LV_SYMBOL_LEFT, tr(S_BOOK_SHELF), back_to_shelf, 0);
    lv_obj_t *tt = bf_label(top_bar, bk.title, BF_FONT_M, pal()->text);
    bf_label_ellipsis(tt, 200);
    lv_obj_set_flex_grow(tt, 1);
    tool(top_bar, LV_SYMBOL_LIST, tr(S_BOOK_TOC), open_toc, 0);
    tool(top_bar, BF_SYMBOL_BOOKMARK, tr(S_BOOK_MARKS), open_marks, 0);
    tool(top_bar, BF_SYMBOL_SEARCH, NULL, open_search, 0);

    bot_bar = lv_obj_create(reader);
    lv_obj_remove_style_all(bot_bar);
    bar_style(bot_bar);
    lv_obj_set_size(bot_bar, LV_PCT(100), 112);
    lv_obj_align(bot_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(bot_bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bot_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tool(bot_bar, LV_SYMBOL_LEFT, NULL, step_click, -1);
    slider = lv_slider_create(bot_bar);
    lv_obj_set_size(slider, 470, 12);
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_set_style_bg_color(slider, pal()->text_dim, LV_PART_MAIN);     /* trilho visível */
    lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_BOOK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, pal()->text, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_slider_set_range(slider, 1, 2);
    lv_obj_add_event_cb(slider, slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_event, LV_EVENT_RELEASED, NULL);
    slider_lbl = bf_label(bot_bar, "…", BF_FONT_S, pal()->text);
    lv_obj_set_width(slider_lbl, 96);
    lv_obj_set_style_text_align(slider_lbl, LV_TEXT_ALIGN_CENTER, 0);
    tool(bot_bar, LV_SYMBOL_RIGHT, NULL, step_click, 1);
    tool(bot_bar, LV_SYMBOL_MINUS, "A", font_click, -1);
    tool(bot_bar, LV_SYMBOL_PLUS, "A", font_click, 1);
    tool(bot_bar, LV_SYMBOL_BARS, tr(S_BOOK_SPACING), space_click, 0);
    tool(bot_bar, BF_SYMBOL_EXPAND, tr(S_BOOK_MARGIN), margin_click, 0);
    tool(bot_bar, BF_SYMBOL_MOON, tr(S_BOOK_THEME), theme_click, 0);
    toggle_controls(false);

    apply_theme();
    size_t keep = page_start;
    start_pagination();
    page_start = keep;
    show_page();
}

/* ---------------- biblioteca ---------------- */

static void free_shelf(void)
{
    for(int i = 0; i < n_shelf; i++) {
        free(shelf[i].path);
        jpeg_free(&shelf[i].cover);
    }
    n_shelf = 0;
}

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
            shelf_t *s = &shelf[n_shelf++];
            memset(s, 0, sizeof *s);
            s->path = strdup(p);
            s->kind = book_kind(e->d_name);
            s->pos = saved_pos(p, &s->len);
        }
    }
    closedir(d);
}

static int cmp_shelf(const void *a, const void *b)
{
    const char *x = strrchr(((const shelf_t *)a)->path, '/'), *y = strrchr(((const shelf_t *)b)->path, '/');
    return strcasecmp(x ? x + 1 : "", y ? y + 1 : "");
}

static void pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i >= 0 && i < n_shelf) open_book(shelf[i].path);
}

/* capas JPEG dos EPUB: uma por tique, para não travar a grade */
static void cover_tick(lv_timer_t *t)
{
    (void)t;
    while(cover_next < n_shelf && (shelf[cover_next].kind != BK_EPUB || shelf[cover_next].cover_tried)) cover_next++;
    if(cover_next >= n_shelf || !root || reader) {
        lv_timer_delete(cover_timer);
        cover_timer = NULL;
        return;
    }
    shelf_t *s = &shelf[cover_next++];
    s->cover_tried = true;
    size_t n;
    unsigned char *j = book_cover_jpeg(s->path, &n);
    if(j && jpeg_fit(j, n, COVER_W + 20, COVER_H, &s->cover) && s->cover_obj) {
        lv_obj_clean(s->cover_obj);
        lv_obj_set_style_bg_color(s->cover_obj, lv_color_hex(0x101010), 0);
        lv_obj_t *im = lv_image_create(s->cover_obj);
        lv_image_set_src(im, &s->cover);
        lv_obj_center(im);
    }
    free(j);
}

static const char *kind_name(book_kind_t k)
{
    return k == BK_EPUB ? "EPUB" : k == BK_PDF ? "PDF" : k == BK_SVG ? "SVG" : "TXT";
}

static void build_shelf(void)
{
    const bf_palette_t *p = pal();
    char books[300];
    free_shelf();
    scan("/mnt/sd", 0);
    data_path(books, sizeof books, "books");
    scan(books, 0);
    qsort(shelf, n_shelf, sizeof shelf[0], cmp_shelf);

    lv_obj_clean(root);
    reader = page_lbl = foot_l = foot_r = top_bar = bot_bar = slider = slider_lbl = NULL;
    if(!n_shelf) {
        lv_obj_t *c = bf_card(root);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, BF_SP_XL, 0);
        lv_obj_t *l = bf_label(c, tr(S_BOOK_EMPTY), BF_FONT_M, p->text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }
    lv_obj_t *grid = bf_plain(root);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(grid, BF_SP_M, 0);
    lv_obj_set_style_pad_column(grid, BF_SP_M, 0);
    lv_obj_set_style_pad_row(grid, BF_SP_M, 0);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);

    static const uint32_t hues[] = { 0x8b5e3c, 0x2a6fd0, 0x2e8b57, 0x9b6ad6, 0xc0392b, 0x16a085, 0xd35400, 0x34495e };
    for(int i = 0; i < n_shelf; i++) {
        shelf_t *s = &shelf[i];
        lv_obj_t *card = lv_button_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, CARD_W, COVER_H + 64);
        lv_obj_set_style_radius(card, BF_RADIUS_M, 0);
        lv_obj_set_style_bg_color(card, p->surface, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_add_event_cb(card, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* capa gerada (cor pelo nome) até a capa real chegar */
        uint32_t h = 5381;
        for(const char *c = s->path; *c; c++) h = h * 33 + (uint8_t)*c;
        lv_obj_t *cv = lv_obj_create(card);
        lv_obj_remove_style_all(cv);
        lv_obj_set_size(cv, COVER_W + 20, COVER_H);
        lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(cv, BF_RADIUS_S, 0);
        lv_obj_set_style_clip_corner(cv, true, 0);
        lv_obj_set_style_bg_color(cv, lv_color_hex(hues[h % 8]), 0);
        lv_obj_set_style_bg_opa(cv, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(cv, 8, 0);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        const char *base = strrchr(s->path, '/');
        char name[120];
        snprintf(name, sizeof name, "%s", base ? base + 1 : s->path);
        char *dot = strrchr(name, '.');
        if(dot) *dot = 0;
        if(s->cover.data) {
            lv_obj_set_style_bg_color(cv, lv_color_hex(0x101010), 0);
            lv_obj_t *im = lv_image_create(cv);
            lv_image_set_src(im, &s->cover);
            lv_obj_center(im);
        }
        else {
            lv_obj_t *ct = bf_label(cv, name, BF_FONT_S, lv_color_white());
            lv_label_set_long_mode(ct, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(ct, LV_PCT(100));
        }
        s->cover_obj = cv;

        lv_obj_t *badge = bf_label(card, kind_name(s->kind), BF_FONT_S, lv_color_hex(C_BOOK));
        lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, COVER_H + 4);
        if(s->len && s->kind != BK_SVG) {
            char pc[16];
            int pct = (int)((unsigned long long)s->pos * 100 / s->len);
            snprintf(pc, sizeof pc, "%d%%", pct);
            lv_obj_t *pl = bf_label(card, pc, BF_FONT_S, p->text_dim);
            lv_obj_align(pl, LV_ALIGN_TOP_RIGHT, 0, COVER_H + 4);
            lv_obj_t *bar = lv_bar_create(card);
            lv_obj_set_size(bar, LV_PCT(100), 5);
            lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, COVER_H + 26);
            lv_bar_set_value(bar, pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, p->surface2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(C_BOOK), LV_PART_INDICATOR);
        }
        lv_obj_t *nl = bf_label(card, name, BF_FONT_S, p->text);
        bf_label_ellipsis(nl, CARD_W - 12);
        lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 0, COVER_H + 34);
    }
    cover_next = 0;
    if(!cover_timer) cover_timer = lv_timer_create(cover_tick, 60, NULL);
}

static void back_to_shelf(lv_event_t *e)
{
    (void)e;
    if(pag_timer) { lv_timer_delete(pag_timer); pag_timer = NULL; }
    panel_close(NULL);
    book_free(&bk);
    free(pages);
    pages = NULL;
    n_pages = cap_pages = 0;
    cur_path[0] = 0;
    build_shelf();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if(pending[0]) {                     /* veio do Arquivos */
        char p[512];
        snprintf(p, sizeof p, "%s", pending);
        pending[0] = 0;
        open_book(p);
    }
    else if(bk.text) open_book(cur_path); /* volta ao livro aberto, sem recarregar */
    else build_shelf();
}

static void destroy(void)
{
    if(pag_timer) { lv_timer_delete(pag_timer); pag_timer = NULL; }
    if(cover_timer) { lv_timer_delete(cover_timer); cover_timer = NULL; }
    if(search_kb) { lv_obj_delete(search_kb); search_kb = NULL; }
    imgview_close();
    for(int i = 0; i < n_shelf; i++) shelf[i].cover_obj = NULL;
    root = reader = page_lbl = foot_l = foot_r = top_bar = bot_bar = slider = slider_lbl = NULL;
    panel = search_ta = search_msg = NULL;
}

/* Arquivos: abre este arquivo no Livros (a tela abre e carrega) */

void book_open_file(const char *path)
{
    if(book_kind(path) == BK_SVG) { imgview_open(path); return; }
    snprintf(pending, sizeof pending, "%s", path);
    nav_go(SCR_BOOK, 0);
}

/* --screenshot: abre um livro e mostra as partes do leitor */
void scr_book_demo(const char *path)
{
    if(!path) { back_to_shelf(NULL); return; }
    if(!strcmp(path, "+controls")) { toggle_controls(true); return; }
    if(!strcmp(path, "+next")) { next_page(); return; }
    if(!strcmp(path, "+toc")) { open_toc(NULL); return; }
    if(!strcmp(path, "+close")) { panel_close(NULL); toggle_controls(false); return; }
    if(!strcmp(path, "+paginate")) { while(pag_timer) pag_tick(pag_timer); return; }
    if(!strcmp(path, "+covers")) { while(cover_timer) cover_tick(cover_timer); return; }
    open_book(path);
}

const bf_screen_def_t scr_book_def = { create, destroy, S_APP_BOOK, NULL };
