/* scr_map.c — Big GPS: mapa offline em tela cheia (docs/big-gps).
 *
 * O mapa (.bgm no cartão) é desenhado pelo motor de biggps/ numa thread própria, em
 * buffer duplo: a interface continua respondendo ao toque durante os 50–300 ms de um
 * quadro. Arrastar move a imagem na hora e pede quadros novos; os rótulos (nomes de
 * lugares e de vias) e o marcador da posição são desenhados por cima pelo LVGL.
 * A posição vem do bfgps (core/gpsstat), uma vez por segundo. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/gpsstat.h"
#include "../core/settings.h"
#include "../biggps/bgmap.h"
#include "../biggps/bgrender.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAP_W   800
#define MAP_H   480
#define MAXLBL  96
#define BTN     56
#define BIGGPS_DIR_DEFAULT "/mnt/sd/BigFoston/big-gps"
#define C_POS   0x1a73e8

typedef struct {
    int16_t x, y, angle;
    uint8_t kind, prio;
    char text[56];
} lbl_t;

typedef struct {               /* rótulo já posicionado (sem sobreposição) */
    lv_area_t a;
    const lv_font_t *font;
    char text[56];
} placed_t;

/* ------------------------------------------------ thread de renderização */
static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, stop, has_req, done;
    bgr_view_t req, done_view;
    uint16_t *buf[2];
    int front;                 /* a interface mostra buf[front] */
    lbl_t lbl[2][MAXLBL];
    int nlbl[2];
    unsigned ms;
    bgm_pack_t *pack;
} R = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void *worker(void *arg)
{
    (void)arg;
    static bgr_label_t tmp[MAXLBL];
    pthread_mutex_lock(&R.mu);
    for(;;) {
        while(!R.stop && (!R.has_req || R.done)) pthread_cond_wait(&R.cv, &R.mu);
        if(R.stop) break;
        bgr_view_t v = R.req;
        R.has_req = 0;
        int back = !R.front;
        pthread_mutex_unlock(&R.mu);

        rast_surface_t s = { R.buf[back], MAP_W, MAP_H, MAP_W };
        bgr_stats_t st;
        int n = bgr_render(R.pack, &v, &s, tmp, MAXLBL, &st);
        for(int i = 0; i < n; i++) {
            lbl_t *l = &R.lbl[back][i];
            l->x = tmp[i].x; l->y = tmp[i].y; l->angle = tmp[i].angle;
            l->kind = tmp[i].kind; l->prio = tmp[i].prio;
            snprintf(l->text, sizeof l->text, "%s", tmp[i].text ? tmp[i].text : "");
        }

        pthread_mutex_lock(&R.mu);
        R.nlbl[back] = n;
        R.done_view = v;
        R.ms = (st.us_fetch + st.us_draw) / 1000;
        R.done = 1;
        fprintf(stderr, "biggps: quadro z%.1f em %u ms (%u tiles, %u feições)\n", v.zoom256 / 256.0, R.ms, st.tiles, st.feats);
    }
    pthread_mutex_unlock(&R.mu);
    return NULL;
}

/* ------------------------------------------------------------ estado da tela */
static lv_obj_t *ov, *img, *layer, *info_lbl, *scale_lbl, *scale_bar, *follow_btn, *rot_lbl, *msg;
static lv_timer_t *tick;
static lv_image_dsc_t dsc[2];
static bgr_view_t cur;         /* vista pedida (alvo) */
static bgr_view_t shown;       /* vista do buffer na tela */
static int have_shown;
static bool follow = true, heading_up;
static bool have_pos, pos_live;
static double pos_lat, pos_lon, pos_course, pos_speed;
static placed_t placed[MAXLBL];
static int nplaced;
static int off_x, off_y;       /* deslocamento da imagem enquanto o quadro novo não chega */
static lv_point_t drag_start;
static bgr_view_t drag_view;
static bool dragging;
static unsigned gps_ms;

static void anchor(int *ax, int *ay)
{
    *ax = MAP_W / 2;
    *ay = follow && heading_up ? MAP_H * 2 / 3 : MAP_H / 2;   /* à frente aparece mais */
}

static void request(void)
{
    if(!R.started) return;
    anchor(&cur.ox, &cur.oy);
    cur.night = theme_dark_now();
    pthread_mutex_lock(&R.mu);
    R.req = cur;
    R.has_req = 1;
    pthread_cond_signal(&R.cv);
    pthread_mutex_unlock(&R.mu);
}

static void update_marker(void);

/* desloca a imagem atual para mostrar 'cur' enquanto o quadro novo é desenhado */
static void update_offset(void)
{
    if(!have_shown) return;
    off_x = off_y = 0;
    if(shown.zoom256 == cur.zoom256 && shown.rot == cur.rot && shown.ox == cur.ox && shown.oy == cur.oy) {
        rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
        int sx, sy;
        bgr_world_to_screen(&shown, &s, cur.cx, cur.cy, &sx, &sy);
        off_x = shown.ox - sx;
        off_y = shown.oy - sy;
    }
    lv_obj_set_pos(img, off_x, off_y);
    lv_obj_invalidate(layer);
    update_marker();
}

/* ---------------------------------------------------------------- rótulos */
static bool overlaps(const lv_area_t *a)
{
    for(int i = 0; i < nplaced; i++) {
        const lv_area_t *b = &placed[i].a;
        if(a->x1 <= b->x2 + 4 && a->x2 + 4 >= b->x1 && a->y1 <= b->y2 + 2 && a->y2 + 2 >= b->y1) return true;
    }
    return false;
}

static void layout_labels(const lbl_t *l, int n)
{
    /* áreas dos controles (voltar, coluna da direita, faixas de baixo) não recebem rótulos */
    static const lv_area_t reserved[] = {
        { 0, 0, 80, 80 }, { MAP_W - 84, 0, MAP_W, 12 + 5 * (BTN + 10) },
        { 0, MAP_H - 52, 300, MAP_H }, { MAP_W - 170, MAP_H - 76, MAP_W, MAP_H },
    };
    nplaced = 0;
    for(unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        placed[nplaced].a = reserved[i];
        placed[nplaced].font = NULL;
        placed[nplaced].text[0] = 0;
        nplaced++;
    }
    if(have_pos) {                                   /* nem sobre o marcador da posição */
        uint32_t wx, wy;
        int sx, sy;
        rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
        bgr_lonlat_to_world(pos_lon, pos_lat, &wx, &wy);
        bgr_world_to_screen(&shown, &s, wx, wy, &sx, &sy);
        placed[nplaced].a = (lv_area_t){ sx - 26, sy - 26, sx + 26, sy + 26 };
        placed[nplaced].font = NULL;
        placed[nplaced].text[0] = 0;
        nplaced++;
    }
    for(int i = 0; i < n && nplaced < MAXLBL; i++) {
        bool place = l[i].kind >= BGK_P_CITY;
        if(!l[i].text[0]) continue;
        if(!place && (l[i].angle > 25 || l[i].angle < -25)) continue;   /* texto girado: a fazer */
        bool dup = false;
        for(int k = 0; k < nplaced && !dup; k++)
            dup = placed[k].font && !strcmp(placed[k].text, l[i].text);
        if(dup) continue;
        const lv_font_t *f = (l[i].kind == BGK_P_CITY || l[i].kind == BGK_P_TOWN || l[i].kind == BGK_P_SUBURB) ? BF_FONT_M : BF_FONT_S;
        lv_point_t sz;
        lv_text_get_size(&sz, l[i].text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t a = { l[i].x - sz.x / 2, l[i].y - sz.y / 2, l[i].x + sz.x / 2, l[i].y + sz.y / 2 };
        if(a.x1 < 2 || a.y1 < 2 || a.x2 > MAP_W - 2 || a.y2 > MAP_H - 2 || overlaps(&a)) continue;
        placed[nplaced].a = a;
        placed[nplaced].font = f;
        snprintf(placed[nplaced].text, sizeof placed[nplaced].text, "%s", l[i].text);
        nplaced++;
    }
}

static void draw_text_halo(lv_layer_t *ly, const placed_t *p, int dx, int dy, lv_color_t fg, lv_color_t halo)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = p->font;
    d.text = p->text;
    d.color = halo;
    static const int8_t o[8][2] = { {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1} };
    for(int k = 0; k < 8; k++) {
        lv_area_t a = p->a;
        lv_area_move(&a, dx + o[k][0], dy + o[k][1]);
        lv_draw_label(ly, &d, &a);
    }
    d.color = fg;
    lv_area_t a = p->a;
    lv_area_move(&a, dx, dy);
    lv_draw_label(ly, &d, &a);
}

/* Marcador da posição: bitmaps ARGB8888 gerados uma vez (supersampling 4x4) e girados
 * pelo LVGL; seta no rumo quando anda, ponto quando parado. */
#define MK 48
static uint32_t mk_arrow[MK * MK], mk_dot[MK * MK], mk_stale[MK * MK];
static lv_image_dsc_t mk_dsc[3];
static lv_obj_t *mark;

static bool in_poly(const float *p, int n, float x, float y)
{
    bool in = false;
    for(int i = 0, j = n - 1; i < n; j = i++)
        if(((p[2 * i + 1] > y) != (p[2 * j + 1] > y))
           && x < (p[2 * j] - p[2 * i]) * (y - p[2 * i + 1]) / (p[2 * j + 1] - p[2 * i + 1]) + p[2 * i])
            in = !in;
    return in;
}

static void mk_build(void)
{
    static const float outer[] = { 24, 3, 40, 43, 24, 34, 8, 43 };
    float inner[8];
    for(int i = 0; i < 4; i++) {                       /* encolhe em volta do centro (24, 30) */
        inner[2 * i] = 24 + (outer[2 * i] - 24) * 0.68f;
        inner[2 * i + 1] = 30 + (outer[2 * i + 1] - 30) * 0.68f;
    }
    for(int y = 0; y < MK; y++)
        for(int x = 0; x < MK; x++) {
            int wa = 0, ba = 0, wd = 0, bd = 0;
            for(int sy = 0; sy < 4; sy++)
                for(int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                    if(in_poly(inner, 4, fx, fy)) ba++;
                    else if(in_poly(outer, 4, fx, fy)) wa++;
                    float dx = fx - 24, dy = fy - 24, d2 = dx * dx + dy * dy;
                    if(d2 < 8.5f * 8.5f) bd++;
                    else if(d2 < 12.0f * 12.0f) wd++;
                }
            /* cor = mistura branco/azul pela cobertura; alfa = cobertura total */
            uint32_t blue = C_POS, grey = 0x8a8f98;
            #define MIX(w, b, col) ((w) + (b) ? (uint32_t)(((w) + (b)) * 255 / 16) << 24 | \
                (((((col) >> 16 & 255) * (b) + 255 * (w)) / ((w) + (b))) << 16) | \
                (((((col) >> 8 & 255) * (b) + 255 * (w)) / ((w) + (b))) << 8) | \
                ((((col) & 255) * (b) + 255 * (w)) / ((w) + (b))) : 0)
            mk_arrow[y * MK + x] = MIX(wa, ba, blue);
            mk_dot[y * MK + x] = MIX(wd, bd, blue);
            mk_stale[y * MK + x] = MIX(wd, bd, grey);
            #undef MIX
        }
    uint32_t *bufs[3] = { mk_arrow, mk_dot, mk_stale };
    for(int i = 0; i < 3; i++) {
        memset(&mk_dsc[i], 0, sizeof mk_dsc[i]);
        mk_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        mk_dsc[i].header.cf = LV_COLOR_FORMAT_ARGB8888;
        mk_dsc[i].header.w = MK;
        mk_dsc[i].header.h = MK;
        mk_dsc[i].header.stride = MK * 4;
        mk_dsc[i].data = (const uint8_t *)bufs[i];
        mk_dsc[i].data_size = sizeof mk_arrow;
    }
}

static void update_marker(void)
{
    if(!mark) return;
    if(!have_pos || !have_shown) { lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN); return; }
    uint32_t wx, wy;
    int sx, sy;
    rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
    bgr_lonlat_to_world(pos_lon, pos_lat, &wx, &wy);
    bgr_world_to_screen(&shown, &s, wx, wy, &sx, &sy);
    sx += off_x;
    sy += off_y;
    if(sx < -MK || sy < -MK || sx > MAP_W + MK || sy > MAP_H + MK) { lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN); return; }
    bool moving = pos_live && pos_speed >= 3.0;
    const lv_image_dsc_t *d = moving ? &mk_dsc[0] : pos_live ? &mk_dsc[1] : &mk_dsc[2];
    if(lv_image_get_src(mark) != d) lv_image_set_src(mark, d);
    if(moving) {
        lv_image_set_pivot(mark, 24, 30);
        lv_image_set_rotation(mark, (int32_t)lround(fmod(pos_course + shown.rot + 720.0, 360.0) * 10));
        lv_obj_set_pos(mark, sx - 24, sy - 30);
    } else {
        lv_image_set_rotation(mark, 0);
        lv_obj_set_pos(mark, sx - 24, sy - 24);
    }
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
}

static void layer_draw(lv_event_t *e)
{
    lv_layer_t *ly = lv_event_get_layer(e);
    if(!have_shown) return;
    bool night = shown.night;
    lv_color_t fg = night ? lv_color_hex(0xE8E8E8) : lv_color_hex(0x202020);
    lv_color_t halo = night ? lv_color_hex(0x101318) : lv_color_white();
    for(int i = 0; i < nplaced; i++)
        if(placed[i].font) draw_text_halo(ly, &placed[i], off_x, off_y, fg, halo);
}

/* ------------------------------------------------------------ barra/escala */
static void update_scale(void)
{
    double mpp = bgr_metres_per_pixel(&cur);
    static const double nice[] = { 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000 };
    double m = nice[0];
    for(unsigned i = 0; i < sizeof nice / sizeof nice[0]; i++)
        if(nice[i] / mpp <= 120) m = nice[i];
    int px = (int)(m / mpp);
    char t[24];
    if(m >= 1000) snprintf(t, sizeof t, "%g km", m / 1000);
    else snprintf(t, sizeof t, "%g m", m);
    lv_label_set_text(scale_lbl, t);
    lv_obj_set_width(scale_bar, px < 10 ? 10 : px);
}

static void update_info(const gps_info_t *g)
{
    char t[96];
    if(!g->running) snprintf(t, sizeof t, LV_SYMBOL_GPS "  %s", tr(gps_enabled() ? S_GPS_NOT_RUNNING : S_GPS_DISABLED));
    else if(g->state < GPS_FIX2D) {
        if(have_pos) snprintf(t, sizeof t, LV_SYMBOL_GPS "  %s · %s", tr(S_GPS_SEARCHING), tr(S_MAP_LAST));
        else snprintf(t, sizeof t, LV_SYMBOL_GPS "  %s", tr(S_GPS_SEARCHING));
    } else snprintf(t, sizeof t, LV_SYMBOL_GPS "  %.0f km/h · %d sat.", g->speed_kmh, g->used);
    if(strcmp(lv_label_get_text(info_lbl), t)) lv_label_set_text(info_lbl, t);
}

/* ------------------------------------------------------- última posição */
static void last_save(void)
{
    if(!have_pos && !have_shown) return;
    char path[512];
    data_path(path, sizeof path, "biggps-last");
    FILE *f = fopen(path, "w");
    if(!f) return;
    double lon, lat;
    if(have_pos) { lon = pos_lon; lat = pos_lat; }
    else bgr_world_to_lonlat(cur.cx, cur.cy, &lon, &lat);
    fprintf(f, "%.7f %.7f %d %d\n", lat, lon, cur.zoom256, have_pos ? 1 : 0);
    fclose(f);
}

static bool last_load(double *lat, double *lon, int *zoom, int *was_pos)
{
    char path[512];
    data_path(path, sizeof path, "biggps-last");
    FILE *f = fopen(path, "r");
    if(!f) return false;
    bool ok = fscanf(f, "%lf %lf %d %d", lat, lon, zoom, was_pos) == 4
              && fabs(*lat) <= 85 && fabs(*lon) <= 180 && *zoom >= BGR_ZOOM_MIN && *zoom <= BGR_ZOOM_MAX;
    fclose(f);
    return ok;
}

/* -------------------------------------------------------------- eventos */
static void gps_update(void)
{
    gps_info_t g;
    gps_read(&g);
    update_info(&g);
    pos_live = g.running && g.state >= GPS_FIX2D;
    if(!pos_live) return;
    have_pos = true;
    pos_lat = g.lat;
    pos_lon = g.lon;
    pos_speed = g.speed_kmh;
    if(g.speed_kmh >= 3.0) pos_course = g.course;
    if(follow) {
        bgr_lonlat_to_world(pos_lon, pos_lat, &cur.cx, &cur.cy);
        cur.rot = heading_up ? -(int)lround(pos_course) : 0;
        request();
    }
    update_marker();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    int swapped = 0;
    pthread_mutex_lock(&R.mu);
    if(R.done) {
        R.front = !R.front;
        R.done = 0;
        shown = R.done_view;
        have_shown = 1;
        layout_labels(R.lbl[R.front], R.nlbl[R.front]);
        pthread_cond_signal(&R.cv);
        swapped = 1;
    }
    pthread_mutex_unlock(&R.mu);
    if(swapped) {
        lv_image_set_src(img, &dsc[R.front]);
        update_offset();
        lv_obj_invalidate(img);
    }
    gps_ms += 100;
    if(gps_ms >= 1000) {
        gps_ms = 0;
        gps_update();
    }
}

static void set_follow(bool on)
{
    follow = on;
    if(follow_btn) {
        lv_obj_set_style_bg_color(follow_btn, on ? lv_color_hex(C_POS) : pal()->surface, 0);
        lv_obj_set_style_text_color(follow_btn, on ? lv_color_white() : pal()->text, 0);
    }
}

static void press_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if(!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if(c == LV_EVENT_PRESSED) {
        drag_start = p;
        drag_view = cur;
        dragging = false;
    } else if(c == LV_EVENT_PRESSING) {
        int dx = p.x - drag_start.x, dy = p.y - drag_start.y;
        if(!dragging && dx * dx + dy * dy < 64) return;
        if(!dragging) {
            dragging = true;
            set_follow(false);
        }
        rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
        bgr_view_t v = drag_view;
        anchor(&v.ox, &v.oy);
        bgr_screen_to_world(&v, &s, v.ox - dx, v.oy - dy, &cur.cx, &cur.cy);
        update_offset();
        request();
    } else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        dragging = false;
    }
}

static void zoom_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int z = (cur.zoom256 + 128) / 256 * 256 + d * 256;
    if(z < BGR_ZOOM_MIN) z = BGR_ZOOM_MIN;
    if(z > BGR_ZOOM_MAX) z = BGR_ZOOM_MAX;
    if(z == cur.zoom256) return;
    cur.zoom256 = z;
    update_scale();
    request();
}

static void follow_cb(lv_event_t *e)
{
    (void)e;
    set_follow(true);
    if(have_pos) {
        bgr_lonlat_to_world(pos_lon, pos_lat, &cur.cx, &cur.cy);
        cur.rot = heading_up ? -(int)lround(pos_course) : 0;
        request();
    }
}

static void rot_cb(lv_event_t *e)
{
    (void)e;
    heading_up = !heading_up;
    lv_label_set_text(rot_lbl, heading_up ? LV_SYMBOL_UP : "N");
    cur.rot = heading_up && follow ? -(int)lround(pos_course) : 0;
    request();
}

static void close_cb(lv_event_t *e) { (void)e; nav_back(); }
static void sats_cb(lv_event_t *e) { (void)e; nav_go(SCR_GPS, 0); }

/* --------------------------------------------------------------- criação */
static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN, BTN);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, pal()->surface, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(b, pal()->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 10, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_text_color(b, pal()->text, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, BF_FONT_M, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static lv_obj_t *chip(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, pal()->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_80, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* primeiro .bgm em <dir>/mapas ou <dir> (ordem alfabética) */
static bool find_map(char *out, size_t n)
{
    const char *base = getenv("BIGFOSTON_BIGGPS");
    if(!base || !*base) base = BIGGPS_DIR_DEFAULT;
    char dirs[2][512];
    snprintf(dirs[0], sizeof dirs[0], "%s/mapas", base);
    snprintf(dirs[1], sizeof dirs[1], "%s", base);
    for(int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        if(!dp) continue;
        char best[256] = "";
        struct dirent *de;
        while((de = readdir(dp))) {
            size_t l = strlen(de->d_name);
            if(l > 4 && !strcasecmp(de->d_name + l - 4, ".bgm") && de->d_name[0] != '.'
               && (!best[0] || strcmp(de->d_name, best) < 0))
                if(snprintf(best, sizeof best, "%s", de->d_name) >= (int)sizeof best) best[0] = 0;
        }
        closedir(dp);
        if(best[0]) return snprintf(out, n, "%s/%s", dirs[d], best) < (int)n;
    }
    return false;
}

static void show_message(const char *title, const char *sub)
{
    msg = bf_card(ov);
    lv_obj_set_size(msg, 520, LV_SIZE_CONTENT);
    lv_obj_center(msg);
    lv_obj_set_flex_flow(msg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(msg, BF_SP_S, 0);
    bf_label(msg, title, BF_FONT_M, pal()->text);
    lv_obj_t *s = bf_label(msg, sub, BF_FONT_S, pal()->text_dim);
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    (void)parent;
    const bf_palette_t *p = pal();
    bool night = theme_dark_now();

    ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, MAP_W, MAP_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(night ? 0x1A1D23 : 0xF2EFE9), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(ov, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ov, press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ov, press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ov, press_cb, LV_EVENT_PRESS_LOST, NULL);

    img = lv_image_create(ov);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    layer = lv_obj_create(ov);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, MAP_W, MAP_H);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(layer, layer_draw, LV_EVENT_DRAW_MAIN, NULL);
    if(!mk_dsc[0].data) mk_build();
    mark = lv_image_create(ov);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);

    /* controles */
    lv_obj_t *b = round_btn(ov, LV_SYMBOL_LEFT, close_cb, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 12, 12);
    b = round_btn(ov, LV_SYMBOL_PLUS, zoom_cb, 1);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12);
    b = round_btn(ov, LV_SYMBOL_MINUS, zoom_cb, -1);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12 + BTN + 10);
    b = round_btn(ov, "N", rot_cb, 0);
    rot_lbl = lv_obj_get_child(b, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12 + 2 * (BTN + 10));
    follow_btn = round_btn(ov, LV_SYMBOL_GPS, follow_cb, 0);
    lv_obj_align(follow_btn, LV_ALIGN_TOP_RIGHT, -12, 12 + 3 * (BTN + 10));
    b = round_btn(ov, LV_SYMBOL_BARS, sats_cb, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12 + 4 * (BTN + 10));

    lv_obj_t *c = chip(ov);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    info_lbl = bf_label(c, "", BF_FONT_S, p->text);

    lv_obj_t *sc = chip(ov);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(sc, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    scale_lbl = bf_label(sc, "", BF_FONT_S, p->text);
    scale_bar = lv_obj_create(sc);
    lv_obj_remove_style_all(scale_bar);
    lv_obj_set_size(scale_bar, 60, 4);
    lv_obj_set_style_bg_color(scale_bar, p->text, 0);
    lv_obj_set_style_bg_opa(scale_bar, LV_OPA_COVER, 0);
    /* atribuição exigida pela ODbL: sempre visível */
    lv_obj_t *attr = bf_label(sc, "\xC2\xA9 OpenStreetMap", BF_FONT_S, p->text_dim);
    (void)attr;

    set_follow(follow);
    lv_label_set_text(rot_lbl, heading_up ? LV_SYMBOL_UP : "N");

    char path[1024], err[256];
    if(!find_map(path, sizeof path)) {
        show_message(tr(S_MAP_NOMAP), tr(S_MAP_NOMAP_SUB));
        gps_update();
        tick = lv_timer_create(tick_cb, 100, NULL);
        return;
    }
    R.pack = bgm_open(path, err, sizeof err);
    if(!R.pack) {
        show_message(tr(S_MAP_NOMAP), err);
        gps_update();
        tick = lv_timer_create(tick_cb, 100, NULL);
        return;
    }
    bgm_set_cache_limit(R.pack, 8u << 20);
    fprintf(stderr, "biggps: mapa %s\n", path);
    for(int i = 0; i < 2; i++) {
        R.buf[i] = malloc((size_t)MAP_W * MAP_H * 2);
        memset(&dsc[i], 0, sizeof dsc[i]);
        dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        dsc[i].header.w = MAP_W;
        dsc[i].header.h = MAP_H;
        dsc[i].header.stride = MAP_W * 2;
        dsc[i].data = (const uint8_t *)R.buf[i];
        dsc[i].data_size = MAP_W * MAP_H * 2;
    }
    if(!R.buf[0] || !R.buf[1]) {
        show_message(tr(S_MAP_NOMAP), "sem memória");
        return;
    }

    /* vista inicial: GPS, última posição salva ou o centro do mapa */
    double lat, lon;
    int zoom, was_pos;
    memset(&cur, 0, sizeof cur);
    cur.zoom256 = 15 * 256;
    if(last_load(&lat, &lon, &zoom, &was_pos)) {
        bgr_lonlat_to_world(lon, lat, &cur.cx, &cur.cy);
        cur.zoom256 = zoom;
        if(was_pos) { have_pos = true; pos_lat = lat; pos_lon = lon; }
    } else {
        const bgm_header_t *h = bgm_header(R.pack);
        cur.cx = h->bbox[0] / 2 + h->bbox[2] / 2;
        cur.cy = h->bbox[1] / 2 + h->bbox[3] / 2;
        cur.zoom256 = 11 * 256;
    }
    R.stop = R.has_req = R.done = 0;
    R.front = 0;
    int rc = pthread_create(&R.th, NULL, worker, NULL);
    if(rc == 0) R.started = 1;
    else fprintf(stderr, "biggps: pthread_create: %s\n", strerror(rc));
    update_scale();
    gps_update();
    request();
    tick = lv_timer_create(tick_cb, 100, NULL);
}

static void destroy(void)
{
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    if(R.started) {
        pthread_mutex_lock(&R.mu);
        R.stop = 1;
        pthread_cond_signal(&R.cv);
        pthread_mutex_unlock(&R.mu);
        pthread_join(R.th, NULL);
        R.started = 0;
    }
    last_save();
    if(ov) { lv_obj_delete(ov); ov = NULL; }
    if(R.pack) { bgm_close(R.pack); R.pack = NULL; }
    for(int i = 0; i < 2; i++) { free(R.buf[i]); R.buf[i] = NULL; }
    img = layer = mark = info_lbl = scale_lbl = scale_bar = follow_btn = rot_lbl = msg = NULL;
    have_shown = 0;
    nplaced = 0;
}

const bf_screen_def_t scr_map_def = { create, destroy, S_APP_GPS, NULL };
