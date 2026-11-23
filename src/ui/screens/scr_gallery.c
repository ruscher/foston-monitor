/* scr_gallery.c — Galeria: miniaturas das imagens do cartão SD e da pasta de
 * dados (JPEG, PNG, BMP, GIF, SVG — core/imgload), em grade com rolagem.
 *
 * As miniaturas são feitas numa thread (decodificar uma foto leva de dezenas
 * a centenas de ms no ARM) e só para a parte visível da grade; ficam num cache
 * em <dados>/thumbs (quando a pasta aceita gravação), então a segunda vez é
 * imediata. Toque abre o visualizador (widgets/imgview: um toque mostra zoom,
 * giro e ◀ barra ▶ para trocar de foto; deslizar também troca). */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/app.h"
#include "../core/imgload.h"
#include "../core/fileops.h"
#include "../widgets/imgview.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_IMGS   600
#define THUMB      120
#define GAP        8
#define COLS       6
#define KEEP_MAX   180          /* miniaturas na RAM (120x120x2 = 28 KB cada) */
#define C_GALLERY  0xd6409f

typedef struct { char *path; time_t mtime; off_t size; dev_t dev; ino_t ino; } gimg_t;

static gimg_t imgs[MAX_IMGS];
static int n_imgs;
static bool truncated;

/* ---- interface (thread do LVGL) ---- */
static lv_obj_t *root, *grid, *cells[MAX_IMGS];
static lv_image_dsc_t thumbs[MAX_IMGS];
static uint8_t tstate[MAX_IMGS];   /* 0 nada, 1 pedido, 2 na tela, 3 falhou */
static int n_loaded, view_idx = -1;
static lv_timer_t *tick;

/* ---- thread de miniaturas ---- */
static pthread_t worker;
static bool worker_up, quit;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int want[COLS * 8], n_want;           /* pedidos, em ordem de prioridade */
static struct { int idx; lv_image_dsc_t d; bool ok; } done[32];
static int n_done, busy_idx = -1;
static char cache_dir[512];
static bool cache_ok;

/* ---------------------------------------------------------------- busca */

static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if(!d) return;
    struct dirent *e;
    char p[1024];
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if(stat(p, &st)) continue;
        if(S_ISDIR(st.st_mode)) {
            if(depth < 5 && strcmp(e->d_name, "thumbs") && strcmp(e->d_name, "bookcache")) scan(p, depth + 1);
            continue;
        }
        if(!S_ISREG(st.st_mode) || !img_is_image(e->d_name)) continue;
        bool dup = false;              /* o mesmo arquivo por outro caminho (mount --bind) */
        for(int i = 0; i < n_imgs && !dup; i++) dup = imgs[i].dev == st.st_dev && imgs[i].ino == st.st_ino;
        if(dup) continue;
        if(n_imgs >= MAX_IMGS) { truncated = true; break; }
        imgs[n_imgs++] = (gimg_t){ strdup(p), st.st_mtime, st.st_size, st.st_dev, st.st_ino };
    }
    closedir(d);
}

static int cmp_date(const void *a, const void *b)
{
    const gimg_t *x = a, *y = b;
    if(x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;     /* recentes primeiro */
    return strcasecmp(x->path, y->path);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(base_name(((const gimg_t *)a)->path), base_name(((const gimg_t *)b)->path));
}

static void free_list(void)
{
    for(int i = 0; i < n_imgs; i++) free(imgs[i].path);
    n_imgs = 0;
    truncated = false;
}

/* ---------------------------------------------------------- cache em disco */

static void cache_name(char *out, size_t n, const gimg_t *g)
{
    uint32_t h = 2166136261u;                       /* FNV-1a do caminho, data e tamanho */
    for(const char *s = g->path; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ (uint32_t)g->mtime) * 16777619u;
    h = (h ^ (uint32_t)g->size) * 16777619u;
    snprintf(out, n, "%s/%08x.565", cache_dir, h);
}

static bool cache_read(const gimg_t *g, lv_image_dsc_t *d)
{
    char f[600];
    cache_name(f, sizeof f, g);
    FILE *fp = fopen(f, "rb");
    if(!fp) return false;
    uint16_t wh[2];
    bool ok = false;
    if(fread(wh, 2, 2, fp) == 2 && wh[0] && wh[1] && wh[0] <= THUMB && wh[1] <= THUMB) {
        size_t n = (size_t)wh[0] * wh[1] * 2;
        uint8_t *px = malloc(n);
        if(px && fread(px, 1, n, fp) == n) {
            memset(d, 0, sizeof *d);
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_RGB565;
            d->header.w = wh[0];
            d->header.h = wh[1];
            d->header.stride = wh[0] * 2u;
            d->data = px;
            d->data_size = (uint32_t)n;
            ok = true;
        }
        else free(px);
    }
    fclose(fp);
    return ok;
}

static void cache_write(const gimg_t *g, const lv_image_dsc_t *d)
{
    char f[600], tmp[620];
    cache_name(f, sizeof f, g);
    snprintf(tmp, sizeof tmp, "%s.tmp", f);
    FILE *fp = fopen(tmp, "wb");
    if(!fp) return;
    uint16_t wh[2] = { (uint16_t)d->header.w, (uint16_t)d->header.h };
    bool ok = fwrite(wh, 2, 2, fp) == 2 && fwrite(d->data, 1, d->data_size, fp) == d->data_size;
    if(fclose(fp) || !ok || rename(tmp, f)) unlink(tmp);
}

/* ------------------------------------------------------ thread de miniaturas */

static void *worker_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mx);
    while(!quit) {
        if(!n_want || n_done >= (int)(sizeof done / sizeof done[0])) { pthread_cond_wait(&cv, &mx); continue; }
        int idx = want[0];
        memmove(want, want + 1, sizeof want[0] * (size_t)--n_want);
        busy_idx = idx;
        gimg_t g = imgs[idx];
        char *path = strdup(g.path);
        pthread_mutex_unlock(&mx);

        lv_image_dsc_t d;
        memset(&d, 0, sizeof d);
        g.path = path;
        bool ok = cache_ok && cache_read(&g, &d);
        if(!ok) {
            char err[96];
            ok = img_load_fit(path, THUMB, THUMB, lv_color_hex(0x222630), &d, err, sizeof err);
            if(!ok) fprintf(stderr, "galeria: %s: %s\n", path, err);
            else if(cache_ok) cache_write(&g, &d);
        }
        free(path);

        pthread_mutex_lock(&mx);
        busy_idx = -1;
        done[n_done].idx = idx;
        done[n_done].d = d;
        done[n_done].ok = ok;
        n_done++;
    }
    pthread_mutex_unlock(&mx);
    return NULL;
}

static void worker_stop(void)
{
    if(!worker_up) return;
    pthread_mutex_lock(&mx);
    quit = true;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mx);
    pthread_join(worker, NULL);
    worker_up = false;
    for(int i = 0; i < n_done; i++) {
        if(done[i].ok) free((void *)done[i].d.data);
    }
    n_done = n_want = 0;
}

static void worker_start(void)
{
    if(worker_up) return;
    quit = false;
    n_done = n_want = 0;
    worker_up = pthread_create(&worker, NULL, worker_main, NULL) == 0;
}

/* ------------------------------------------------------------- interface */

static void drop_thumb(int i)
{
    lv_obj_t *im = lv_obj_get_child(cells[i], 0);
    lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
    img_free(&thumbs[i]);
    tstate[i] = 0;
    n_loaded--;
}

static void open_index(int i);

static void open_async(void *ud)
{
    if(imgview_is_open()) open_index((int)(intptr_t)ud);   /* fechada no meio tempo: não reabre */
}

static void nav(int index, void *ud)
{
    (void)ud;
    if(index < 0 || index >= n_imgs || index == view_idx) return;
    lv_async_call(open_async, (void *)(intptr_t)index);   /* fora do evento do próprio visualizador */
}

static void open_index(int i)
{
    if(i < 0 || i >= n_imgs || !root) return;             /* a tela pode ter fechado antes do async */
    view_idx = i;
    imgview_set_nav(nav, NULL, i, n_imgs);
    if(!imgview_open(imgs[i].path)) imgview_close();
    if(grid && cells[i]) lv_obj_scroll_to_view(cells[i], LV_ANIM_OFF);
}

static int pick;                          /* GALLERY_PICK_WALLPAPER: tocar escolhe o papel de parede */

static void choose_wallpaper(int i)
{
    bf_settings_t *s = settings();
    snprintf(s->wall_image, sizeof s->wall_image, "%s", imgs[i].path);
    s->wallpaper = BF_WALL_IMAGE;
    settings_save();
    nav_back();
    app_rebuild();                        /* o tema recalcula o papel de parede */
}

static void cell_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(pick && i >= 0 && i < n_imgs) choose_wallpaper(i);
    else open_index(i);
}

/* pede as miniaturas da parte visível, recebe as prontas, libera as distantes */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if(!grid || !n_imgs) return;
    int row_h = THUMB + GAP, sy = (int)lv_obj_get_scroll_y(grid);
    int first = sy / row_h * COLS, last = ((sy + lv_obj_get_height(grid)) / row_h + 2) * COLS;
    if(first < 0) first = 0;
    if(last > n_imgs) last = n_imgs;

    pthread_mutex_lock(&mx);
    for(int k = 0; k < n_done; k++) {
        int i = done[k].idx;
        if(i >= n_imgs || tstate[i] != 1) { if(done[k].ok) free((void *)done[k].d.data); continue; }
        if(done[k].ok) {
            thumbs[i] = done[k].d;
            lv_obj_t *im = lv_obj_get_child(cells[i], 0);
            lv_image_set_src(im, &thumbs[i]);
            lv_obj_remove_flag(im, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lv_obj_get_child(cells[i], 1), LV_OBJ_FLAG_HIDDEN);
            tstate[i] = 2;
            n_loaded++;
        }
        else {
            tstate[i] = 3;
            lv_label_set_text(lv_obj_get_child(cells[i], 1), LV_SYMBOL_WARNING);
        }
    }
    n_done = 0;
    /* fila nova: só o que está visível (e a linha seguinte) */
    for(int k = 0; k < n_want; k++) if(tstate[want[k]] == 1) tstate[want[k]] = 0;
    n_want = 0;
    for(int i = first; i < last && n_want < (int)(sizeof want / sizeof want[0]); i++) {
        if(tstate[i] == 0 && i != busy_idx) { want[n_want++] = i; tstate[i] = 1; }
        else if(i == busy_idx && tstate[i] == 0) tstate[i] = 1;
    }
    if(n_want) pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mx);

    /* RAM: tira as mais distantes da tela */
    for(int i = 0; n_loaded > KEEP_MAX && i < n_imgs; i++) {
        int far = i < first - COLS * 6 || i > last + COLS * 6;
        if(far && tstate[i] == 2) drop_thumb(i);
    }
}

static void build_grid(void);

static void sort_click(lv_event_t *e)
{
    (void)e;
    settings()->gallery_sort = !settings()->gallery_sort;
    settings_save();
    build_grid();
}

static void build_grid(void)
{
    const bf_palette_t *p = pal();
    worker_stop();
    for(int i = 0; i < n_imgs; i++) img_free(&thumbs[i]);
    memset(tstate, 0, sizeof tstate);
    n_loaded = 0;
    qsort(imgs, (size_t)n_imgs, sizeof imgs[0], settings()->gallery_sort ? cmp_name : cmp_date);
    lv_obj_clean(root);
    grid = NULL;
    memset(cells, 0, sizeof cells);

    lv_obj_t *hdr = bf_plain(root);
    lv_obj_set_size(hdr, LV_PCT(100), 48);
    char b[96];
    if(pick) snprintf(b, sizeof b, "%s", tr(S_GAL_PICK_WALL));
    else snprintf(b, sizeof b, tr(S_GAL_COUNT), n_imgs);
    lv_obj_t *t = bf_label(hdr, b, BF_FONT_M, p->text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    if(truncated) {
        lv_obj_t *w = bf_label(hdr, tr(S_GAL_TOO_MANY), BF_FONT_S, p->warning);
        lv_obj_align_to(w, t, LV_ALIGN_OUT_RIGHT_MID, BF_SP_M, 0);
    }
    lv_obj_t *sb = bf_button(hdr, LV_SYMBOL_SHUFFLE, tr(settings()->gallery_sort ? S_GAL_SORT_NAME : S_GAL_SORT_DATE), 0);
    lv_obj_set_height(sb, 44);
    lv_obj_align(sb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sb, sort_click, LV_EVENT_CLICKED, NULL);

    if(!n_imgs) {
        lv_obj_t *box = bf_card(root);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(box, BF_SP_XL, 0);
        lv_obj_t *l = bf_label(box, tr(S_GAL_EMPTY), BF_FONT_M, p->text_dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }

    grid = bf_plain(root);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    lv_obj_set_style_pad_right(grid, 10, 0);       /* espaço da barra de rolagem */
    bf_scrollable(grid);
    for(int i = 0; i < n_imgs; i++) {
        lv_obj_t *c = lv_button_create(grid);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, THUMB, THUMB);
        lv_obj_set_style_radius(c, BF_RADIUS_S, 0);
        lv_obj_set_style_clip_corner(c, true, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x222630), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(C_GALLERY), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(c, 3, LV_STATE_PRESSED);
        lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(c, cell_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *im = lv_image_create(c);           /* filho 0: miniatura */
        lv_obj_center(im);
        lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *ph = bf_label(c, LV_SYMBOL_IMAGE, BF_FONT_M, lv_color_hex(0x6c7383));   /* filho 1 */
        lv_obj_center(ph);
        cells[i] = c;
    }
    worker_start();
}

static void create(lv_obj_t *parent, int arg)
{
    pick = arg == GALLERY_PICK_WALLPAPER;
    char dir[512];
    free_list();
    if(access("/mnt/sd", F_OK) == 0) scan("/mnt/sd", 0);
    snprintf(dir, sizeof dir, "%s", data_dir());
    scan(dir, 0);
    data_path(cache_dir, sizeof cache_dir, "thumbs");
    /* a pasta de dados precisa aceitar gravação (cartão com gravação liberada) */
    cache_ok = fo_writable(data_dir()) && (mkdir(cache_dir, 0755) == 0 || access(cache_dir, W_OK) == 0);

    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(root, BF_SP_M, 0);
    lv_obj_set_style_pad_ver(root, BF_SP_S, 0);
    lv_obj_set_style_pad_row(root, BF_SP_XS, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    build_grid();
    tick = lv_timer_create(tick_cb, 60, NULL);
}

static void destroy(void)
{
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    imgview_close();
    worker_stop();
    for(int i = 0; i < n_imgs; i++) img_free(&thumbs[i]);
    memset(tstate, 0, sizeof tstate);
    memset(cells, 0, sizeof cells);
    n_loaded = 0;
    root = grid = NULL;
    view_idx = -1;
    free_list();
}

/* capturas de tela: espera as miniaturas e abre a primeira imagem */
void scr_gallery_demo(int step)
{
    if(step == 0) {
        for(int k = 0; k < 200 && grid; k++) {      /* até ~10 s */
            tick_cb(NULL);
            bool all = true;
            int lim = n_imgs < COLS * 3 ? n_imgs : COLS * 3;
            for(int i = 0; i < lim; i++) if(tstate[i] < 2) all = false;
            if(all) break;
            usleep(50000);
        }
        tick_cb(NULL);
    }
    else if(step == 1) {                           /* a maior foto estática (GIF animado não gira) */
        int best = 0, w, h;
        long big = -1;
        for(int i = 0; i < n_imgs; i++) {
            img_kind_t k = img_kind_of(imgs[i].path);
            if(k != IMG_GIF && k != IMG_SVG && img_probe(imgs[i].path, &w, &h) && (long)w * h > big)
                big = (long)w * h, best = i;
        }
        open_index(best);
    }
    else if(step == 3) imgview_show_controls(true);
    else if(step == 4) imgview_rotate(1);
    else if(step == 5) { imgview_rotate(-1); imgview_zoom(1); imgview_zoom(1); }
    else if(step == 6) nav_back();                 /* Voltar do sistema: fecha só a imagem */
    else imgview_close();
}

const bf_screen_def_t scr_gallery_def = { create, destroy, S_APP_GALLERY, NULL };
