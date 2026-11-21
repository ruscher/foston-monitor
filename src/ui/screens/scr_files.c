/* scr_files.c — Arquivos.
 *
 * Locais: cartão SD (/mnt/sd), dados do shell e o sistema (/). Pastas
 * primeiro, ícone por tipo, tamanho, caminho atual e espaço livre/total do
 * local. Abre MP3 no Multimídia (a pasta vira a fila), texto num
 * visualizador monoespaçado e JPEG/SVG no visualizador de imagem.
 *
 * Gravação (core/fileops.c), só no SD com gravação liberada e na pasta de
 * dados — o sistema fica só leitura: segurar um item abre Copiar, Recortar,
 * Renomear e Apagar; a barra da pasta tem Nova pasta e Colar. Cópias e
 * exclusões rodam numa thread com progresso e Cancelar. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/cover.h"
#include "../widgets/imgview.h"
#include "../core/imgload.h"
#include "../widgets/dialog.h"
#include "../core/fileops.h"
#include "../core/osd.h"
#include "../core/sdctl.h"
#include "../core/player.h"
#include "../assets/icons/bf_icons.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

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
    static const char *const book[] = { "epub", "pdf", NULL };
    static const char *const text[] = { "txt", "log", "conf", "cfg", "ini", "sh", "desktop", "md",
                                        "csv", "json", "xml", "srt", "nmea", "sha256", "c", "h", NULL };
    if(dir) return K_DIR;
    if(ext_in(n, audio)) return K_AUDIO;
    if(img_is_image(n)) return K_IMAGE;           /* JPEG, PNG, BMP, GIF, SVG (core/imgload) */
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
    bf_scrollable(body);
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
    if(i == 0) {                                /* cartão ausente ou ejetado: nada a abrir */
        sd_state_t sd = sdctl_status();
        if(sd == SD_ABSENT || sd == SD_EJECTED || sd == SD_MOUNTING) return;
    }
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

static void item_menu(lv_event_t *e);

static lv_obj_t *row_button(lv_obj_t *parent, const char *sym, lv_color_t sym_color, const char *name,
                            const char *right, lv_event_cb_t cb, intptr_t ud, bool menu)
{
    const bf_palette_t *p = pal();
    lv_obj_t *it = lv_button_create(parent);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, LV_PCT(100), 48);
    lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
    /* SHORT_CLICKED: depois de segurar (menu) o item não abre também */
    lv_obj_add_event_cb(it, cb, menu ? LV_EVENT_SHORT_CLICKED : LV_EVENT_CLICKED, (void *)ud);
    if(menu) lv_obj_add_event_cb(it, item_menu, LV_EVENT_LONG_PRESSED, (void *)ud);
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

/* ================================================================ cartão SD */

static lv_timer_t *sd_timer, *sd_watch;
static sd_state_t sd_shown = SD_UNKNOWN;     /* estado desenhado na lista de locais */

/* na lista de locais: o cartão entrou, saiu ou terminou de montar? redesenha */
static void sd_watch_cb(lv_timer_t *t)
{
    (void)t;
    if(cwd[0] || !root || sdctl_busy()) return;
    if(sdctl_status() != sd_shown) rebuild();
}

static void sd_tick(lv_timer_t *t)
{
    (void)t;
    int rc;
    char msg[256];
    if(!sdctl_done(&rc, msg, sizeof msg)) return;
    lv_timer_delete(sd_timer);
    sd_timer = NULL;
    sd_state_t sd = sdctl_status();
    if(sd == SD_EJECTED) osd_show(&bf_icon_sdcard, 0x2ec27e, tr(S_FILES_SD_REMOVE_OK), tr(S_FILES_SD_REMOVE_SUB));
    else if(sd == SD_RW || sd == SD_RO) osd_show(&bf_icon_sdcard, 0xe0a40c, tr(S_FILES_SD_MOUNTED), msg);
    else osd_show(&bf_icon_sdcard, 0xe5484d, tr(rc == 2 ? S_FILES_SD_BUSY : S_FILES_FAILED), msg);
    if(root && !cwd[0]) rebuild();
}

static void sd_click(lv_event_t *e)
{
    bool eject = (bool)(intptr_t)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_active());   /* o toque não abre a linha também */
    if(sdctl_busy()) return;
    if(eject && player_state() != PL_STOPPED) player_stop();   /* música do cartão: solta o arquivo */
    if(!sdctl_start(eject)) return;
    osd_show(&bf_icon_sdcard, 0xe0a40c, tr(eject ? S_FILES_SD_EJECTING : S_FILES_SD_MOUNTING), NULL);
    if(!sd_timer) sd_timer = lv_timer_create(sd_tick, 200, NULL);
}

/* botão Ejetar (cartão montado) ou Montar (ejetado e ainda no slot), na linha do SD */
static void sd_button(lv_obj_t *row, sd_state_t sd)
{
    if(sd == SD_ABSENT || sd == SD_MOUNTING) return;
    bool eject = sd == SD_RW || sd == SD_RO;
    lv_obj_t *b = bf_button(row, eject ? LV_SYMBOL_EJECT : LV_SYMBOL_UPLOAD,
                            tr(eject ? S_FILES_SD_EJECT : S_FILES_SD_MOUNT), eject ? 0 : 1);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 40);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(b, sd_click, LV_EVENT_CLICKED, (void *)(intptr_t)eject);
    /* o espaço livre passa para a esquerda do botão */
    lv_obj_t *right = lv_obj_get_child(row, 2);
    if(right) lv_obj_align_to(right, b, LV_ALIGN_OUT_LEFT_MID, -BF_SP_M, 0);
    lv_obj_t *name = lv_obj_get_child(row, 1);
    if(name) bf_label_ellipsis(name, 260);
}

/* ================================================================ gravação */

static char clip[1024];          /* caminho copiado/recortado */
static bool clip_move;
static int menu_i = -1;          /* item do menu de ações */
static lv_timer_t *op_timer;
static lv_obj_t *op_bar, *op_lbl;
static lv_obj_t *name_panel, *name_ta, *name_kb;
static int name_mode;            /* 0 nova pasta, 1 renomear */

static bool entry_path(int i, char *out, size_t n)
{
    return i >= 0 && i < n_ents &&
           snprintf(out, n, "%s/%s", strcmp(cwd, "/") ? cwd : "", ents[i].name) < (int)n;
}

static void notice(bool ok, const char *title, const char *sub)
{
    osd_show(&bf_icon_files, ok ? C_FILES : 0xe5484d, title, sub);
}

/* ---- progresso das operações longas ---- */

static void op_tick(lv_timer_t *t)
{
    (void)t;
    fo_status_t s;
    fo_get(&s);
    if(s.state == FO_RUNNING) {
        if(op_bar) {
            int v = s.total > 0 ? (int)(s.done * 1000 / s.total) : 0;
            lv_bar_set_value(op_bar, v > 1000 ? 1000 : v, LV_ANIM_OFF);
        }
        if(op_lbl && strcmp(lv_label_get_text(op_lbl), s.current)) lv_label_set_text(op_lbl, s.current);
        return;
    }
    lv_timer_delete(op_timer);
    op_timer = NULL;
    op_bar = op_lbl = NULL;
    dialog_close_all();
    const char *what = tr(s.op == FO_DELETE ? S_FILES_DELETED : s.op == FO_MOVE ? S_FILES_MOVED : S_FILES_COPIED);
    if(s.state == FO_DONE) notice(true, what, NULL);
    else if(s.state == FO_CANCELLED) notice(false, tr(S_FILES_CANCELLED), NULL);
    else notice(false, tr(S_FILES_FAILED), s.err);
    if(s.state == FO_DONE && s.op == FO_MOVE) clip[0] = 0;   /* recortado: já foi */
    fo_reset();
    if(root) rebuild();
}

static void op_cancel_cb(int b, void *ud)
{
    (void)b; (void)ud;
    fo_cancel();                     /* o timer termina quando a thread parar */
}

static void op_run(fo_op_t op, const char *src, const char *dst)
{
    char err[160];
    if(!fo_start(op, src, dst, err, sizeof err)) { notice(false, tr(S_FILES_FAILED), err); return; }
    static const char *btns[1];
    btns[0] = tr(S_CANCEL);
    const char *name = strrchr(src, '/');
    name = name ? name + 1 : src;
    lv_obj_t *card = dialog_open(LV_SYMBOL_COPY, lv_color_hex(C_FILES),
                                 tr(op == FO_DELETE ? S_FILES_DELETING : op == FO_MOVE ? S_FILES_MOVING : S_FILES_COPYING),
                                 name, btns, 1, -1, -1, op_cancel_cb, NULL);
    op_bar = lv_bar_create(card);
    lv_obj_set_size(op_bar, LV_PCT(100), 12);
    lv_bar_set_range(op_bar, 0, 1000);
    lv_obj_set_style_bg_color(op_bar, lv_color_hex(C_FILES), LV_PART_INDICATOR);
    lv_obj_move_to_index(op_bar, 2);
    op_lbl = bf_label(card, "", BF_FONT_S, pal()->text_dim);
    bf_label_ellipsis(op_lbl, LV_PCT(100));
    lv_obj_move_to_index(op_lbl, 3);
    op_timer = lv_timer_create(op_tick, 200, NULL);
}

/* ---- nome (nova pasta / renomear): painel no topo + teclado ---- */

static void name_close(void)
{
    if(name_kb) { lv_obj_delete(name_kb); name_kb = NULL; }
    if(name_panel) { lv_obj_delete(name_panel); name_panel = NULL; }
    name_ta = NULL;
}

static void name_close_async(void *ud) { (void)ud; name_close(); }

static void name_ok(lv_event_t *e)
{
    (void)e;
    char err[160] = "", path[1024];
    const char *name = lv_textarea_get_text(name_ta);
    bool ok;
    if(name_mode == 0) ok = fo_mkdir(cwd, name, err, sizeof err);
    else ok = entry_path(menu_i, path, sizeof path) && fo_rename(path, name, err, sizeof err);
    if(!ok) {
        notice(false, tr(S_FILES_FAILED), err[0] ? err : tr(S_FILES_CANT_OPEN));
        return;                          /* o painel fica para corrigir o nome */
    }
    lv_async_call(name_close_async, NULL);
    notice(true, tr(name_mode == 0 ? S_FILES_FOLDER_DONE : S_FILES_RENAMED), name);
    rebuild();
}

static void name_cancel(lv_event_t *e) { (void)e; lv_async_call(name_close_async, NULL); }

static void name_open(int mode, const char *initial)
{
    const bf_palette_t *p = pal();
    name_close();
    name_mode = mode;
    name_panel = bf_card(lv_layer_top());
    lv_obj_set_size(name_panel, 760, LV_SIZE_CONTENT);
    lv_obj_align(name_panel, LV_ALIGN_TOP_MID, 0, 8);   /* cabe acima do teclado (210 px) */
    lv_obj_set_style_shadow_width(name_panel, 30, 0);
    lv_obj_set_style_shadow_opa(name_panel, LV_OPA_50, 0);
    lv_obj_set_flex_flow(name_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(name_panel, BF_SP_S, 0);
    /* título + X para fechar */
    lv_obj_t *head = bf_plain(name_panel);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = bf_label(head, tr(mode == 0 ? S_FILES_NEW_FOLDER : S_FILES_RENAME), BF_FONT_L, p->text);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_t *x = bf_button(head, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 56, 48);
    lv_obj_add_event_cb(x, name_cancel, LV_EVENT_CLICKED, NULL);
    name_ta = lv_textarea_create(name_panel);
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_max_length(name_ta, 200);
    lv_obj_set_width(name_ta, LV_PCT(100));
    lv_obj_set_style_text_font(name_ta, BF_FONT_M, 0);
    lv_textarea_set_text(name_ta, initial ? initial : "");
    bf_label(name_panel, tr(S_FILES_NAME_HINT), BF_FONT_S, p->text_dim);
    /* Cancelar / Criar (ou Renomear) */
    lv_obj_t *row = bf_plain(name_panel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    lv_obj_t *b = bf_button(row, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0);
    lv_obj_set_size(b, 200, 52);
    lv_obj_add_event_cb(b, name_cancel, LV_EVENT_CLICKED, NULL);
    b = bf_button(row, LV_SYMBOL_OK, tr(mode == 0 ? S_FILES_CREATE : S_FILES_RENAME), 1);
    lv_obj_set_size(b, 200, 52);
    lv_obj_add_event_cb(b, name_ok, LV_EVENT_CLICKED, NULL);
    name_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(name_kb, BF_SCREEN_W, 210);
    lv_obj_align(name_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(name_kb, name_ta);
    lv_obj_add_event_cb(name_kb, name_ok, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(name_kb, name_cancel, LV_EVENT_CANCEL, NULL);
}

/* ---- menu de ações de um item (segurar) ---- */

enum { A_COPY, A_CUT, A_RENAME, A_DELETE };

static void delete_cb(int b, void *ud)
{
    (void)ud;
    char path[1024];
    if(b == 1 && entry_path(menu_i, path, sizeof path)) op_run(FO_DELETE, path, NULL);
}

static void action_click(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    char path[1024];
    dialog_close_all();
    if(!entry_path(menu_i, path, sizeof path)) return;
    switch(a) {
        case A_COPY:
        case A_CUT:
            snprintf(clip, sizeof clip, "%s", path);
            clip_move = a == A_CUT;
            notice(true, tr(a == A_CUT ? S_FILES_CUT_DONE : S_FILES_COPY_DONE), ents[menu_i].name);
            rebuild();
            break;
        case A_RENAME:
            name_open(1, ents[menu_i].name);
            break;
        case A_DELETE: {
            static const char *btns[2];
            btns[0] = tr(S_CANCEL);
            btns[1] = tr(S_FILES_DELETE);
            char body[400];
            snprintf(body, sizeof body, tr(ents[menu_i].kind == K_DIR ? S_FILES_DELETE_DIR_Q : S_FILES_DELETE_Q),
                     ents[menu_i].name);
            dialog_open(LV_SYMBOL_TRASH, pal()->danger, tr(S_FILES_DELETE), body, btns, 2, -1, 1, delete_cb, NULL);
            break;
        }
    }
}

static void item_menu(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= n_ents) return;
    menu_i = i;
    lv_indev_wait_release(lv_indev_active());
    bool w = fo_writable(cwd);
    static const char *btns[1];
    btns[0] = tr(S_CANCEL);
    lv_obj_t *card = dialog_open(kind_symbol(ents[i].kind), lv_color_hex(C_FILES), ents[i].name,
                                 w ? NULL : tr(S_FILES_RO_HINT), btns, 1, -1, -1, NULL, NULL);
    lv_obj_t *grid = bf_plain(card);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BF_SP_S, 0);
    lv_obj_set_style_pad_column(grid, BF_SP_S, 0);
    lv_obj_move_to_index(grid, w ? 1 : 2);
    static const struct { const char *sym; bf_str_t txt; bool write; } acts[] = {
        { LV_SYMBOL_COPY, S_FILES_COPY, false }, { LV_SYMBOL_CUT, S_FILES_CUT, true },
        { LV_SYMBOL_EDIT, S_FILES_RENAME, true }, { LV_SYMBOL_TRASH, S_FILES_DELETE, true },
    };
    for(int a = 0; a < 4; a++) {
        if(acts[a].write && !w) continue;
        lv_obj_t *b = bf_button(grid, acts[a].sym, tr(acts[a].txt), a == A_DELETE ? 2 : 0);
        lv_obj_set_width(b, LV_PCT(49));
        lv_obj_add_event_cb(b, action_click, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    }
}

/* ---- barra da pasta: nova pasta, colar ---- */

static void new_folder_click(lv_event_t *e) { (void)e; name_open(0, ""); }

static void paste_click(lv_event_t *e)
{
    (void)e;
    if(!clip[0]) return;
    if(access(clip, F_OK)) { notice(false, tr(S_FILES_FAILED), tr(S_FILES_CLIP_GONE)); clip[0] = 0; rebuild(); return; }
    op_run(clip_move ? FO_MOVE : FO_COPY, clip, cwd);
}

static void clip_clear(lv_event_t *e) { (void)e; clip[0] = 0; rebuild(); }

static void build_action_bar(void)
{
    lv_obj_t *bar = bf_plain(root);
    lv_obj_set_size(bar, LV_PCT(100), 48);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, BF_SP_S, 0);
    lv_obj_t *b = bf_button(bar, LV_SYMBOL_PLUS, tr(S_FILES_NEW_FOLDER), 0);
    lv_obj_set_height(b, 46);
    lv_obj_add_event_cb(b, new_folder_click, LV_EVENT_CLICKED, NULL);
    if(clip[0]) {
        const char *n = strrchr(clip, '/');
        char t[300];
        snprintf(t, sizeof t, "%s \"%.200s\"", tr(clip_move ? S_FILES_MOVE_HERE : S_FILES_PASTE), n ? n + 1 : clip);
        b = bf_button(bar, LV_SYMBOL_PASTE, t, 1);
        lv_obj_set_height(b, 46);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_t *l = lv_obj_get_child(b, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_add_event_cb(b, paste_click, LV_EVENT_CLICKED, NULL);
        b = bf_button(bar, LV_SYMBOL_CLOSE, NULL, 0);
        lv_obj_set_size(b, 56, 46);
        lv_obj_add_event_cb(b, clip_clear, LV_EVENT_CLICKED, NULL);
    }
}

static lv_obj_t *list_obj;           /* lista atual (para manter a rolagem) */
static char list_dir[1024];

static void rebuild(void)
{
    const bf_palette_t *p = pal();
    /* refeita na mesma pasta (renomear, apagar, colar): volta ao mesmo ponto */
    int keep_y = list_obj && !strcmp(list_dir, cwd) ? (int)lv_obj_get_scroll_y(list_obj) : 0;
    list_obj = NULL;
    snprintf(list_dir, sizeof list_dir, "%s", cwd);
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

    bool bar = cwd[0] && fo_writable(cwd);
    if(bar) build_action_bar();
    lv_obj_t *list = bf_card(root);
    lv_obj_set_size(list, LV_PCT(100), 360 - 52 - 2 * BF_SP_S - BF_SP_XS - (bar ? 48 + BF_SP_XS : 0));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, BF_SP_XS, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    bf_scrollable(list);
    list_obj = list;

    if(!cwd[0]) {
        sd_state_t sd = sdctl_status();
        sd_shown = sd;
        for(int i = 0; i < (int)(sizeof places / sizeof places[0]); i++) {
            const char *pp = places[i].path ? places[i].path : data_dir();
            char sp[96];
            bool is_sd = i == 0;
            if(is_sd && (sd == SD_ABSENT || sd == SD_EJECTED || sd == SD_MOUNTING))   /* /mnt/sd vazio: não é o cartão */
                snprintf(sp, sizeof sp, "%s", tr(sd == SD_EJECTED ? S_FILES_SD_EJECTED :
                                                  sd == SD_MOUNTING ? S_FILES_SD_MOUNTING : S_FILES_ABSENT));
            else space_line(sp, sizeof sp, pp);
            lv_obj_t *row = row_button(list, places[i].sym, lv_color_hex(C_FILES), tr(places[i].name),
                                       sp[0] ? sp : tr(S_FILES_ABSENT), go_place, i, false);
            if(is_sd && sd != SD_UNKNOWN) sd_button(row, sd);
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
                   open_entry, i, true);
    }
    if(truncated) bf_label(list, tr(S_FILES_TOO_MANY), BF_FONT_S, p->warning);
    if(keep_y) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, keep_y, LV_ANIM_OFF);
    }
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    fo_status_t fs;
    fo_get(&fs);
    if(fs.state != FO_IDLE && fs.state != FO_RUNNING) fo_reset();   /* terminou fora da tela */
    if(fs.state == FO_RUNNING && !op_timer)                  /* ainda copiando: aviso no fim */
        op_timer = lv_timer_create(op_tick, 200, NULL);
    if(sdctl_available() && !sd_watch) sd_watch = lv_timer_create(sd_watch_cb, 1000, NULL);
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
    if(sd_timer) { lv_timer_delete(sd_timer); sd_timer = NULL; }   /* o resultado sai no próximo create */
    if(sd_watch) { lv_timer_delete(sd_watch); sd_watch = NULL; }
    name_close();
    if(op_timer) {                       /* a operação continua; o aviso vem por OSD na volta */
        lv_timer_delete(op_timer);
        op_timer = NULL;
    }
    op_bar = op_lbl = NULL;
    list_obj = NULL;
    viewer_close(NULL);
    imgview_close();
    free_ents();
    root = NULL;
}

void scr_files_demo(const char *path)
{
    struct stat st;
    if(path && !strcmp(path, "+nova-pasta")) { name_open(0, "Viagem 2026"); return; }
    if(path && !strcmp(path, "+fechar")) { name_close(); return; }
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
