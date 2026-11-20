/* scr_files.c — Arquivos: navegador somente leitura.
 *
 * Locais: cartão SD (/mnt/sd, montado só leitura), dados do shell e o
 * sistema (/). Pastas primeiro, ícone por tipo, tamanho, caminho atual e
 * espaço livre/total do local. Abre MP3 no Multimídia (a pasta vira a fila),
 * texto num visualizador monoespaçado e JPEG num visualizador de imagem
 * reduzida para a tela. Não apaga, renomeia nem grava nada. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/cover.h"
#include "../widgets/imgview.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define MAX_ENTRIES 400
#define TEXT_MAX    (16 * 1024)   /* rótulo LVGL com texto longo fica lento no ARM */
#define IMG_MAX     (8 * 1024 * 1024)
#define C_FILES     0xe0a40c

typedef enum { K_DIR, K_AUDIO, K_IMAGE, K_BOOK, K_TEXT, K_OTHER } kind_t;

typedef struct {
    char *name;
    kind_t kind;
    long long size;
} entry_t;

static char cwd[1024] = "";          /* "" = escolher o local */
static entry_t ents[MAX_ENTRIES];
static int n_ents;
static bool truncated;
static lv_obj_t *root, *viewer;
static lv_image_dsc_t view_img;

static const struct { const char *sym; bf_str_t name; const char *path; } places[] = {
    { LV_SYMBOL_SD_CARD, S_FILES_SD, "/mnt/sd" },
    { LV_SYMBOL_HOME, S_FILES_DATA, NULL },        /* data_dir() */
    { LV_SYMBOL_DRIVE, S_FILES_SYSTEM, "/" },
};

/* ---------------- tipos e tamanhos ---------------- */

static bool ext_in(const char *n, const char *const *list)
{
    const char *dot = strrchr(n, '.');
    if(!dot) return false;
    for(int i = 0; list[i]; i++) if(!strcasecmp(dot + 1, list[i])) return true;
    return false;
}

static kind_t kind_of(const char *n, bool dir)
{
    static const char *const audio[] = { "mp3", NULL };
    static const char *const image[] = { "jpg", "jpeg", "svg", NULL };
    static const char *const book[] = { "epub", "pdf", NULL };
    static const char *const text[] = { "txt", "log", "conf", "cfg", "ini", "sh", "desktop", "md",
                                        "csv", "json", "xml", "srt", "nmea", "sha256", "c", "h", NULL };
    if(dir) return K_DIR;
    if(ext_in(n, audio)) return K_AUDIO;
    if(ext_in(n, image)) return K_IMAGE;
    if(ext_in(n, book)) return K_BOOK;
    if(ext_in(n, text)) return K_TEXT;
    return K_OTHER;
}

static const char *kind_symbol(kind_t k)
{
    switch(k) {
    case K_DIR:   return LV_SYMBOL_DIRECTORY;
    case K_AUDIO: return LV_SYMBOL_AUDIO;
    case K_IMAGE: return LV_SYMBOL_IMAGE;
    case K_BOOK:  return LV_SYMBOL_FILE;
    case K_TEXT:  return LV_SYMBOL_FILE;
    default:      return LV_SYMBOL_FILE;
    }
}

static void fmt_size(char *out, size_t n, long long b)
{
    if(b < 1024) snprintf(out, n, "%lld B", b);
    else if(b < 1024 * 1024) snprintf(out, n, "%lld KB", (b + 512) / 1024);
    else if(b < 1024LL * 1024 * 1024) snprintf(out, n, "%lld,%lld MB", b >> 20, ((b & 0xfffff) * 10) >> 20);
    else snprintf(out, n, "%lld,%lld GB", b >> 30, ((b & 0x3fffffff) * 10) >> 30);
}

/* ---------------- leitura da pasta ---------------- */

static void free_ents(void)
{
    for(int i = 0; i < n_ents; i++) free(ents[i].name);
    n_ents = 0;
    truncated = false;
}

static int cmp_ent(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if((x->kind == K_DIR) != (y->kind == K_DIR)) return x->kind == K_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool read_dir(const char *path)
{
    free_ents();
    DIR *d = opendir(path);
    if(!d) return false;
    struct dirent *e;
    char p[1024];
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if(n_ents == MAX_ENTRIES) { truncated = true; break; }
        struct stat st;
        bool have = snprintf(p, sizeof p, "%s/%s", strcmp(path, "/") ? path : "", e->d_name) < (int)sizeof p &&
                    !stat(p, &st);
        bool dir = have && S_ISDIR(st.st_mode);
        ents[n_ents].name = strdup(e->d_name);
        ents[n_ents].kind = kind_of(e->d_name, dir);
        ents[n_ents].size = dir ? -1 : have ? (long long)st.st_size : 0;
        n_ents++;
    }
    closedir(d);
    qsort(ents, n_ents, sizeof ents[0], cmp_ent);
    return true;
}

/* ---------------- visualizadores ---------------- */

static void viewer_close(lv_event_t *e)
{
    (void)e;
    if(viewer) { lv_obj_delete(viewer); viewer = NULL; }
    jpeg_free(&view_img);
}

static lv_obj_t *viewer_frame(const char *title)
{
    const bf_palette_t *p = pal();
    viewer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(viewer);
    lv_obj_set_size(viewer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer, p->bg, 0);
    lv_obj_set_style_bg_opa(viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(viewer);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(bar, p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *t = bf_label(bar, title, BF_FONT_M, p->text);
    bf_label_ellipsis(t, 640);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, BF_SP_L, 0);
    lv_obj_t *x = bf_button(bar, LV_SYMBOL_CLOSE, tr(S_CLOSE), 0);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, -BF_SP_S, 0);
    lv_obj_add_event_cb(x, viewer_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_obj_create(viewer);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), 480 - 52);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 52);
    return body;
}

static void *read_file(const char *path, size_t max, size_t *len, bool *cut)
{
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    char *buf = malloc(max + 1);
    if(!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, max, f);
    *cut = fgetc(f) != EOF;
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}

/* texto para o rótulo: UTF-8 válido, tabs como espaços, sem controles */
static void sanitize(char *s, size_t n)
{
    size_t i = 0;
    while(i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        bool ok = len && i + len <= n;
        for(int k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + k] & 0xc0) == 0x80;
        if(!ok) { s[i++] = '?'; continue; }
        if(len == 1 && c == '\t') s[i] = ' ';
        else if(len == 1 && c < 0x20 && c != '\n') s[i] = c == '\r' ? ' ' : '?';
        i += len;
    }
}

static void open_text(const char *path, const char *name)
{
    size_t len = 0;
    bool cut = false;
    char *txt = read_file(path, TEXT_MAX, &len, &cut);
    lv_obj_t *body = viewer_frame(name);
    lv_obj_set_style_pad_all(body, BF_SP_M, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    if(!txt) { bf_label(body, tr(S_FILES_CANT_OPEN), BF_FONT_M, pal()->warning); return; }
    if(memchr(txt, 0, len)) { free(txt); bf_label(body, tr(S_FILES_BINARY), BF_FONT_M, pal()->text_dim); return; }
    sanitize(txt, len);
    lv_obj_t *l = bf_label(body, txt, BF_FONT_MONO, pal()->text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    free(txt);
    if(cut) {
        lv_obj_t *c = bf_label(body, tr(S_FILES_TRUNCATED), BF_FONT_S, pal()->warning);
        lv_obj_set_style_pad_top(c, BF_SP_M, 0);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    }
}

static void open_image(const char *path, const char *name)
{
    size_t len = 0;
    bool cut = false;
    uint8_t *data = read_file(path, IMG_MAX, &len, &cut);
    lv_obj_t *body = viewer_frame(name);
    lv_obj_set_style_bg_color(body, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    bool ok = data && !cut && jpeg_fit(data, len, 800, 480 - 52, &view_img);
    free(data);
    if(!ok) {
        lv_obj_t *l = bf_label(body, tr(S_FILES_BAD_IMAGE), BF_FONT_M, pal()->text_dim);
        lv_obj_center(l);
        return;
    }
    lv_obj_t *img = lv_image_create(body);
    lv_image_set_src(img, &view_img);
    lv_obj_center(img);
}

/* ---------------- lista ---------------- */

static void rebuild(void);

static void go_place(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(cwd, sizeof cwd, "%s", places[i].path ? places[i].path : data_dir());
    rebuild();
}

static void go_up(lv_event_t *e)
{
    (void)e;
    for(int i = 0; i < (int)(sizeof places / sizeof places[0]); i++) {
        const char *pp = places[i].path ? places[i].path : data_dir();
        if(!strcmp(cwd, pp)) { cwd[0] = 0; rebuild(); return; }   /* raiz do local: volta aos locais */
    }
    char *s = strrchr(cwd, '/');
    if(!s || s == cwd) snprintf(cwd, sizeof cwd, "/");
    else *s = 0;
    rebuild();
}

static void open_entry(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= n_ents) return;
    char path[1024];
    if(snprintf(path, sizeof path, "%s/%s", strcmp(cwd, "/") ? cwd : "", ents[i].name) >= (int)sizeof path)
        return;                                    /* caminho longo demais: não trunca */
    switch(ents[i].kind) {
    case K_DIR:
        memcpy(cwd, path, sizeof cwd);
        rebuild();
        break;
    case K_AUDIO: music_play_file(path); break;
    case K_IMAGE: if(!imgview_open(path)) open_image(path, ents[i].name); break;
    case K_BOOK:  book_open_file(path); break;
    default:      open_text(path, ents[i].name); break;    /* texto ou desconhecido: tenta como texto */
    }
}

static lv_obj_t *row_button(lv_obj_t *parent, const char *sym, lv_color_t sym_color, const char *name,
                            const char *right, lv_event_cb_t cb, intptr_t ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *it = lv_button_create(parent);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, LV_PCT(100), 48);
    lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
    lv_obj_add_event_cb(it, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *ic = bf_label(it, sym, BF_FONT_M, sym_color);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = bf_label(it, name, BF_FONT_M, p->text);
    bf_label_ellipsis(l, right ? 560 : 680);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
    if(right) {
        lv_obj_t *r = bf_label(it, right, BF_FONT_S, p->text_dim);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return it;
}

/* caminho numa linha: se não couber, mostra o fim com "…" na frente */
static void set_path_label(lv_obj_t *l, const char *path, int32_t width)
{
    const lv_font_t *f = BF_FONT_M;
    lv_point_t sz;
    const char *t = path;
    char buf[1100];
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    while(sz.x > width && *t) {
        t++;
        while((*t & 0xc0) == 0x80) t++;            /* não corta no meio de um caractere UTF-8 */
        snprintf(buf, sizeof buf, "…%s", t);
        lv_text_get_size(&sz, buf, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    lv_label_set_text(l, t == path ? path : buf);
}

static void space_line(char *out, size_t n, const char *path)
{
    struct statvfs v;
    if(statvfs(path, &v)) { out[0] = 0; return; }
    char fr[24], tot[24];
    fmt_size(fr, sizeof fr, (long long)v.f_bavail * v.f_frsize);
    fmt_size(tot, sizeof tot, (long long)v.f_blocks * v.f_frsize);
    snprintf(out, n, "%s %s %s%s", fr, tr(S_FILES_FREE_OF), tot,
             (v.f_flag & ST_RDONLY) ? tr(S_FILES_RO) : "");
}

static void rebuild(void)
{
    const bf_palette_t *p = pal();
    lv_obj_clean(root);

    /* cabeçalho: subir + caminho + espaço */
    lv_obj_t *hdr = bf_plain(root);
    lv_obj_set_size(hdr, LV_PCT(100), 52);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    if(cwd[0]) {
        lv_obj_t *up = bf_button(hdr, LV_SYMBOL_UP, NULL, 0);
        lv_obj_set_size(up, 64, 48);
        lv_obj_align(up, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(up, go_up, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *t = bf_label(hdr, "", BF_FONT_M, p->text);
    if(cwd[0]) set_path_label(t, cwd, 440);
    else lv_label_set_text(t, tr(S_FILES_PLACES));
    lv_obj_align(t, LV_ALIGN_LEFT_MID, cwd[0] ? 76 : 4, 0);
    if(cwd[0]) {
        char sp[96];
        space_line(sp, sizeof sp, cwd);
        lv_obj_t *s = bf_label(hdr, sp, BF_FONT_S, p->text_dim);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    lv_obj_t *list = bf_card(root);
    lv_obj_set_size(list, LV_PCT(100), 360 - 52 - 2 * BF_SP_S - BF_SP_XS);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, BF_SP_XS, 0);
    lv_obj_set_style_pad_row(list, 2, 0);

    if(!cwd[0]) {
        for(int i = 0; i < (int)(sizeof places / sizeof places[0]); i++) {
            const char *pp = places[i].path ? places[i].path : data_dir();
            char sp[96];
            space_line(sp, sizeof sp, pp);
            row_button(list, places[i].sym, lv_color_hex(C_FILES), tr(places[i].name),
                       sp[0] ? sp : tr(S_FILES_ABSENT), go_place, i);
        }
        return;
    }
    if(!read_dir(cwd)) {
        bf_label(list, tr(S_FILES_CANT_OPEN), BF_FONT_M, p->warning);
        return;
    }
    if(!n_ents) bf_label(list, tr(S_FILES_EMPTY), BF_FONT_M, p->text_dim);
    for(int i = 0; i < n_ents; i++) {
        char sz[24] = "";
        if(ents[i].kind != K_DIR) fmt_size(sz, sizeof sz, ents[i].size);
        lv_color_t c = ents[i].kind == K_DIR ? lv_color_hex(C_FILES) :
                       ents[i].kind == K_AUDIO ? lv_color_hex(0xe5534b) :
                       ents[i].kind == K_IMAGE ? p->accent : p->text_dim;
        row_button(list, kind_symbol(ents[i].kind), c, ents[i].name, ents[i].kind == K_DIR ? NULL : sz,
                   open_entry, i);
    }
    if(truncated) bf_label(list, tr(S_FILES_TOO_MANY), BF_FONT_S, p->warning);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, BF_SP_S, 0);
    lv_obj_set_style_pad_row(root, BF_SP_XS, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    rebuild();
}

static void destroy(void)
{
    viewer_close(NULL);
    imgview_close();
    free_ents();
    root = NULL;
}

void scr_files_demo(const char *path)
{
    struct stat st;
    if(path && !stat(path, &st) && S_ISREG(st.st_mode)) {   /* arquivo: abre o visualizador */
        const char *n = strrchr(path, '/');
        n = n ? n + 1 : path;
        if(kind_of(n, false) == K_IMAGE) { if(!imgview_open(path)) open_image(path, n); }
        else open_text(path, n);
        return;
    }
    viewer_close(NULL);
    snprintf(cwd, sizeof cwd, "%s", path ? path : "");
    if(root) rebuild();
}

const bf_screen_def_t scr_files_def = { create, destroy, S_APP_FILES, NULL };
