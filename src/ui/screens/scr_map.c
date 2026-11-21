/* scr_map.c — Big GPS: mapa offline em tela cheia (docs/big-gps).
 *
 * O mapa (.bgm no cartão) é desenhado pelo motor de biggps/ numa thread própria, em
 * buffer duplo: a interface continua respondendo ao toque durante os 50–300 ms de um
 * quadro. Arrastar move a imagem na hora e pede quadros novos; os rótulos (nomes de
 * lugares e de vias) e o marcador da posição são desenhados por cima pelo LVGL.
 * A posição vem do bfgps (core/gpsstat), uma vez por segundo.
 *
 * Navegação: toque longo marca o destino; a rota é calculada numa thread (biggps/bgrouter)
 * e desenhada no quadro do mapa; a faixa de cima mostra a próxima manobra e a de baixo o
 * que falta. "Simular" percorre a rota sem GPS (sempre identificado na tela). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/gpsstat.h"
#include "../core/settings.h"
#include "../core/voice.h"
#include "../biggps/bgmap.h"
#include "../biggps/bgrender.h"
#include "../biggps/bgrouter.h"
#include "../biggps/bgicons.h"
#include "../biggps/bgsearch.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
    const lv_font_t *font;     /* NULL = área reservada */
    char text[56];
    int angle;                 /* != 0: nome de via girado, desenhado como imagem */
} placed_t;

/* nomes de vias girados: texto com contorno num bitmap ARGB8888, girado pelo LVGL */
#define MAXROT 14
static struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint32_t *px;
    int cx, cy;                /* centro na tela (sem o deslocamento do arraste) */
} rot[MAXROT];
static int nrot;
static lv_draw_buf_t *glyph_buf;

static uint32_t utf8_next(const char *s, int *i)
{
    const unsigned char *u = (const unsigned char *)s + *i;
    uint32_t c = u[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if(n == 1) { (*i)++; return c; }
    c &= 0xFF >> (n + 1);
    for(int k = 1; k < n; k++) {
        if((u[k] & 0xC0) != 0x80) { (*i)++; return '?'; }
        c = c << 6 | (u[k] & 0x3F);
    }
    *i += n;
    return c;
}

/* texto -> ARGB8888 com contorno de 2 px; *w, *h = tamanho */
static uint32_t *text_bitmap(const char *text, const lv_font_t *f, uint32_t fg, uint32_t halo, int *w, int *h)
{
    const int pad = 2;
    int width = 0;
    for(int i = 0; text[i];) {
        uint32_t c = utf8_next(text, &i);
        int j = i;
        uint32_t nx = text[j] ? utf8_next(text, &j) : 0;
        lv_font_glyph_dsc_t g;
        if(lv_font_get_glyph_dsc(f, &g, c, nx)) width += g.adv_w;
    }
    int W = width + 2 * pad, H = lv_font_get_line_height(f) + 2 * pad;
    if(W <= 2 * pad || W > 1200) return NULL;
    uint8_t *cov = calloc((size_t)W * H, 1);
    uint32_t *px = malloc((size_t)W * H * 4);
    if(!cov || !px) { free(cov); free(px); return NULL; }
    if(!glyph_buf) glyph_buf = lv_draw_buf_create(64, 64, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    int pen = pad, base = H - pad - f->base_line;
    for(int i = 0; text[i] && glyph_buf;) {
        uint32_t c = utf8_next(text, &i);
        int j = i;
        uint32_t nx = text[j] ? utf8_next(text, &j) : 0;
        lv_font_glyph_dsc_t g;
        if(!lv_font_get_glyph_dsc(f, &g, c, nx)) continue;
        if(g.box_w && g.box_h && g.box_w <= 64 && g.box_h <= 64 && g.format == LV_FONT_GLYPH_FORMAT_A4) {
            /* LVGL 9.2 returns the draw buffer itself; the A8 pixels are in ->data */
            const uint8_t *bm = lv_font_get_glyph_bitmap(&g, glyph_buf) ? glyph_buf->data : NULL;
            uint32_t stride = lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8);   /* como o LVGL grava o glifo */
            int x0 = pen + g.ofs_x, y0 = base - g.ofs_y - g.box_h;
            for(int y = 0; bm && y < g.box_h; y++)
                for(int x = 0; x < g.box_w; x++) {
                    int X = x0 + x, Y = y0 + y;
                    if(X < 0 || Y < 0 || X >= W || Y >= H) continue;
                    uint8_t a = bm[y * stride + x];
                    if(a > cov[Y * W + X]) cov[Y * W + X] = a;
                }
            lv_font_glyph_release_draw_data(&g);
        }
        pen += g.adv_w;
    }
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++) {
            int a = cov[y * W + x], hl = 0;                 /* contorno: máximo na vizinhança de 2 px */
            for(int dy = -2; dy <= 2 && hl < 255; dy++)
                for(int dx = -2; dx <= 2; dx++) {
                    int X = x + dx, Y = y + dy;
                    if(X >= 0 && Y >= 0 && X < W && Y < H && cov[Y * W + X] > hl) hl = cov[Y * W + X];
                }
            hl = hl * 3 / 2 > 255 ? 255 : hl * 3 / 2;
            uint32_t col;
            if(a) {                                          /* texto sobre o contorno */
                uint32_t r = (((fg >> 16 & 255) * a + (halo >> 16 & 255) * (255 - a)) / 255);
                uint32_t g2 = (((fg >> 8 & 255) * a + (halo >> 8 & 255) * (255 - a)) / 255);
                uint32_t b = (((fg & 255) * a + (halo & 255) * (255 - a)) / 255);
                col = 0xFFu << 24 | r << 16 | g2 << 8 | b;
                if(hl < 255) col = (uint32_t)(a > hl ? a : hl) << 24 | (col & 0xFFFFFF);
            } else col = (uint32_t)hl << 24 | (halo & 0xFFFFFF);
            px[y * W + x] = col;
        }
    free(cov);
    *w = W;
    *h = H;
    return px;
}

static void rot_clear(void)
{
    for(int i = 0; i < nrot; i++) {
        if(rot[i].img) lv_obj_delete(rot[i].img);
        free(rot[i].px);
        rot[i].img = NULL;
        rot[i].px = NULL;
    }
    nrot = 0;
}

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
    uint32_t *route;           /* rota a desenhar (pontos x,y no mundo), dono: interface */
    int route_n;
} R = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void *worker(void *arg)
{
    (void)arg;
    static bgr_label_t tmp[MAXLBL];
    uint32_t *rcopy = NULL;
    int rcap = 0;
    pthread_mutex_lock(&R.mu);
    for(;;) {
        while(!R.stop && (!R.has_req || R.done)) pthread_cond_wait(&R.cv, &R.mu);
        if(R.stop) break;
        bgr_view_t v = R.req;
        R.has_req = 0;
        int back = !R.front;
        int rn = R.route_n;                         /* cópia da rota: a interface pode trocá-la */
        if(rn > rcap) {
            uint32_t *nb = realloc(rcopy, (size_t)rn * 2 * sizeof *nb);
            if(nb) { rcopy = nb; rcap = rn; }
            else rn = 0;
        }
        if(rn) memcpy(rcopy, R.route, (size_t)rn * 2 * sizeof *rcopy);
        pthread_mutex_unlock(&R.mu);

        rast_surface_t s = { R.buf[back], MAP_W, MAP_H, MAP_W };
        bgr_stats_t st;
        int n = bgr_render(R.pack, &v, &s, tmp, MAXLBL, &st);
        if(rn >= 2) {
            int z = v.zoom256 >> 8, w = z >= 16 ? 8 : z >= 14 ? 6 : 4;
            bgr_draw_polyline(&v, &s, rcopy, rn, w * RAST_SUB, rgb565(v.night ? 0x4c8bf5 : 0x1a73e8),
                              (w + 3) * RAST_SUB, rgb565(v.night ? 0x0d2b5e : 0x0b3d91));
        }
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
    free(rcopy);
    return NULL;
}

/* ------------------------------------------------------------ estado da tela */
static lv_obj_t *ov, *img, *layer, *info_chip, *info_lbl, *scale_lbl, *scale_bar, *follow_btn, *rot_lbl, *msg;
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

/* navegação */
static struct {
    int has_dest, waiting, active, navigating, simulate, rerouting, arrived_ms;
    int profile;
    uint32_t dx, dy;           /* destino */
    unsigned req_id;
    bgn_route_t route;
    bgn_state_t st;
    double sim_t;              /* simulação: tempo percorrido na rota (décimos de segundo) */
    int said_next, said_far, said_near;
    bool prev_heading_up;
    char dest_name[64];
} N;
static lv_obj_t *sheet, *banner, *ban_icon, *ban_dist, *ban_text, *ban_then, *botbar, *bot_lbl, *sim_chip, *dmark, *search_btn;
static void set_follow(bool on);
static lv_image_dsc_t icon_dsc;
static uint16_t icon_px[80 * 80];
static int icon_type = -1, icon_exit = -1;
static void on_fix(uint32_t x, uint32_t y, double speed, double course, bool live);
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
    for(int i = 0; i < nrot; i++)
        if(rot[i].img) lv_obj_set_pos(rot[i].img, rot[i].cx - (int)rot[i].dsc.header.w / 2 + off_x, rot[i].cy - (int)rot[i].dsc.header.h / 2 + off_y);
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
        { 0, 0, 12 + 2 * BTN + 24, 80 }, { MAP_W - 84, 0, MAP_W, 12 + 6 * (BTN + 10) },
        { 0, MAP_H - 52, 300, MAP_H }, { MAP_W - 170, MAP_H - 76, MAP_W, MAP_H },
    };
    rot_clear();
    nplaced = 0;
    for(unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        placed[nplaced].a = reserved[i];
        placed[nplaced].font = NULL;
        placed[nplaced].text[0] = 0;
        placed[nplaced].angle = 0;
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
        placed[nplaced].angle = 0;
        nplaced++;
    }
    for(int i = 0; i < n && nplaced < MAXLBL; i++) {
        bool place = l[i].kind >= BGK_P_CITY;
        if(!l[i].text[0]) continue;
        int ang = place ? 0 : l[i].angle;
        if(ang >= -8 && ang <= 8) ang = 0;                   /* quase horizontal: texto comum */
        bool dup = false;
        for(int k = 0; k < nplaced && !dup; k++)
            dup = placed[k].font && !strcmp(placed[k].text, l[i].text);
        if(dup) continue;
        const lv_font_t *f = (l[i].kind == BGK_P_CITY || l[i].kind == BGK_P_TOWN || l[i].kind == BGK_P_SUBURB) ? BF_FONT_M : BF_FONT_S;
        lv_point_t sz;
        lv_text_get_size(&sz, l[i].text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t a = { l[i].x - sz.x / 2, l[i].y - sz.y / 2, l[i].x + sz.x / 2, l[i].y + sz.y / 2 };
        if(ang) {                                            /* caixa do retângulo girado */
            double r = ang * M_PI / 180, c = fabs(cos(r)), sn = fabs(sin(r));
            int hw = (int)(sz.x * c + sz.y * sn) / 2 + 1, hh = (int)(sz.x * sn + sz.y * c) / 2 + 1;
            a = (lv_area_t){ l[i].x - hw, l[i].y - hh, l[i].x + hw, l[i].y + hh };
            if(nrot >= MAXROT) continue;
        }
        if(a.x1 < 2 || a.y1 < 2 || a.x2 > MAP_W - 2 || a.y2 > MAP_H - 2 || overlaps(&a)) continue;
        placed[nplaced].a = a;
        placed[nplaced].font = f;
        placed[nplaced].angle = ang;
        snprintf(placed[nplaced].text, sizeof placed[nplaced].text, "%s", l[i].text);
        nplaced++;
        if(ang) {
            bool night = shown.night;
            int w, h;
            uint32_t *px = text_bitmap(l[i].text, f, night ? 0xE8E8E8 : 0x202020, night ? 0x101318 : 0xFFFFFF, &w, &h);
            if(!px) { nplaced--; continue; }
            int k = nrot++;
            rot[k].px = px;
            memset(&rot[k].dsc, 0, sizeof rot[k].dsc);
            rot[k].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            rot[k].dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            rot[k].dsc.header.w = (uint32_t)w;
            rot[k].dsc.header.h = (uint32_t)h;
            rot[k].dsc.header.stride = (uint32_t)w * 4;
            rot[k].dsc.data = (const uint8_t *)px;
            rot[k].dsc.data_size = (uint32_t)(w * h * 4);
            rot[k].img = lv_image_create(ov);
            lv_obj_remove_flag(rot[k].img, LV_OBJ_FLAG_CLICKABLE);
            lv_image_set_src(rot[k].img, &rot[k].dsc);
            lv_image_set_pivot(rot[k].img, w / 2, h / 2);
            lv_image_set_rotation(rot[k].img, ang * 10);
            rot[k].cx = l[i].x;
            rot[k].cy = l[i].y;
            lv_obj_set_pos(rot[k].img, l[i].x - w / 2 + off_x, l[i].y - h / 2 + off_y);
            lv_obj_move_to_index(rot[k].img, lv_obj_get_index(layer) + 1);   /* sob os marcadores e botões */
        }
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
static uint32_t mk_arrow[MK * MK], mk_dot[MK * MK], mk_stale[MK * MK], mk_pin[MK * MK];
static lv_image_dsc_t mk_dsc[4];
#define MG 32
static uint32_t mg_px[MG * MG];                /* lupa branca (tingida pela cor do tema) */
static lv_image_dsc_t mg_dsc;
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
            /* alfinete do destino: gota vermelha com miolo branco, ponta em (24, 46) */
            int pr = 0, pw = 0;
            for(int sy = 0; sy < 4; sy++)
                for(int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4, dx = fx - 24, dy = fy - 18;
                    bool head = dx * dx + dy * dy < 13.0f * 13.0f;
                    bool tail = fy >= 18 && fy <= 46 && fabsf(dx) < 13.0f * (46 - fy) / 28;
                    if(dx * dx + dy * dy < 5.0f * 5.0f) pw++;
                    else if(head || tail) pr++;
                }
            mk_pin[y * MK + x] = MIX(pw, pr, 0xd93025u);
            mk_arrow[y * MK + x] = MIX(wa, ba, blue);
            mk_dot[y * MK + x] = MIX(wd, bd, blue);
            mk_stale[y * MK + x] = MIX(wd, bd, grey);
            #undef MIX
        }
    for(int y = 0; y < MG; y++)
        for(int x = 0; x < MG; x++) {
            int cov = 0;
            for(int sy = 0; sy < 4; sy++)
                for(int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4, dx = fx - 13, dy = fy - 13;
                    float r2 = dx * dx + dy * dy;
                    bool ring = r2 < 9.5f * 9.5f && r2 > 6.5f * 6.5f;
                    /* cabo: faixa de 3,2 px na diagonal de (20, 20) a (28, 28) */
                    float t = ((fx - 20) + (fy - 20)) / 2, px = fx - 20 - t, py = fy - 20 - t;
                    bool handle = t > -1 && t < 8.5f && px * px + py * py < 2.4f * 2.4f;
                    if(ring || handle) cov++;
                }
            mg_px[y * MG + x] = cov ? (uint32_t)(cov * 255 / 16) << 24 | 0xFFFFFF : 0;
        }
    memset(&mg_dsc, 0, sizeof mg_dsc);
    mg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    mg_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    mg_dsc.header.w = MG;
    mg_dsc.header.h = MG;
    mg_dsc.header.stride = MG * 4;
    mg_dsc.data = (const uint8_t *)mg_px;
    mg_dsc.data_size = sizeof mg_px;
    uint32_t *bufs[4] = { mk_arrow, mk_dot, mk_stale, mk_pin };
    for(int i = 0; i < 4; i++) {
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

static void update_dest_marker(void)
{
    if(!dmark) return;
    if(!N.has_dest || !have_shown) { lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN); return; }
    int sx, sy;
    rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
    bgr_world_to_screen(&shown, &s, N.dx, N.dy, &sx, &sy);
    sx += off_x;
    sy += off_y;
    if(sx < -MK || sy < -MK || sx > MAP_W + MK || sy > MAP_H + MK) { lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_set_pos(dmark, sx - 24, sy - 46);
    lv_obj_remove_flag(dmark, LV_OBJ_FLAG_HIDDEN);
}

static void update_marker(void)
{
    update_dest_marker();
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
        if(placed[i].font && !placed[i].angle) draw_text_halo(ly, &placed[i], off_x, off_y, fg, halo);
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

/* ------------------------------------------------------------- navegação */
static void request(void);

static void set_route_overlay(const uint32_t *xy, int n)
{
    uint32_t *copy = NULL;
    if(n >= 2) {
        copy = malloc((size_t)n * 2 * sizeof *copy);
        if(copy) memcpy(copy, xy, (size_t)n * 2 * sizeof *copy);
        else n = 0;
    } else n = 0;
    pthread_mutex_lock(&R.mu);
    free(R.route);
    R.route = copy;
    R.route_n = n;
    pthread_mutex_unlock(&R.mu);
}

/* enquadra a rota inteira (visão geral) */
static void fit_route(void)
{
    const bgn_route_t *r = &N.route;
    if(r->npts < 2) return;
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    for(int i = 0; i < r->npts; i++) {
        uint32_t x = r->xy[2 * i], y = r->xy[2 * i + 1];
        if(x < x0) x0 = x;
        if(x > x1) x1 = x;
        if(y < y0) y0 = y;
        if(y > y1) y1 = y;
    }
    double w = (double)(x1 - x0) + 1, h = (double)(y1 - y0) + 1;
    /* px = world * 2^(z - 24): z para caber em 560 x 280 px (folha e botões ocupam o resto) */
    double z = fmin(log2(560.0 / w), log2(280.0 / h)) + 24;
    if(z > 17) z = 17;
    if(z < 5) z = 5;
    cur.zoom256 = (int)(z * 256);
    cur.cx = x0 + (x1 - x0) / 2;
    cur.cy = y0 + (y1 - y0) / 2 + (uint32_t)(h * 0.18);   /* sobe a rota acima da folha */
    cur.rot = 0;
    follow = false;
    update_scale();
    request();
}

/* voz: eSpeak NG (core/voice), se ligada nos ajustes e instalada no cartão */
static void say(const char *text)
{
    fprintf(stderr, "biggps: voz: %s\n", text);
    if(settings()->nav_voice) voice_say(text);
}

static const char *spoken_dist(uint32_t dm, char *buf, int n)
{
    uint32_t m = dm / 10;
    if(m < 1000) {
        uint32_t step = m < 300 ? 50 : 100;
        snprintf(buf, (size_t)n, "%u metros", (m + step / 2) / step * step);
    } else if(m < 1950) {
        snprintf(buf, (size_t)n, m < 1250 ? "1 quilômetro" : m < 1750 ? "1 quilômetro e meio" : "2 quilômetros");
    } else snprintf(buf, (size_t)n, "%u quilômetros", (m + 500) / 1000);
    return buf;
}

static void lower_first(char *t)
{
    /* "Vire" -> "vire"; só ASCII (os verbos das instruções começam sem acento) */
    if(t[0] >= 'A' && t[0] <= 'Z') t[0] = (char)(t[0] - 'A' + 'a');
}

static void announce(void)
{
    const bgn_state_t *st = &N.st;
    if(!st->valid || st->next < 0 || st->next >= N.route.nman) return;
    if(st->next != N.said_next) { N.said_next = st->next; N.said_far = N.said_near = 0; }
    const bgn_man_t *m = &N.route.man[st->next];
    double v = pos_speed / 3.6;
    if(v < 5) v = 5;
    uint32_t dist = st->next_dm / 10;
    uint32_t near = (uint32_t)fmax(30, v * 6);
    uint32_t far = v > 22 ? 2000 : v > 14 ? 1000 : 300;
    char t[160], d[48], full[400];
    bgn_text(m, t, sizeof t);
    if(!N.said_near && dist <= near) {
        N.said_near = N.said_far = 1;
        /* "e depois" quando a manobra seguinte vem logo em seguida */
        if(st->next + 1 < N.route.nman && N.route.man[st->next + 1].at_dm - m->at_dm < 1500 && m->type != BGN_ARRIVE) {
            char t2[160];
            bgn_text(&N.route.man[st->next + 1], t2, sizeof t2);
            lower_first(t2);
            snprintf(full, sizeof full, "%s, e depois %s", t, t2);
            say(full);
        } else say(t);
    } else if(!N.said_far && dist <= far && dist > near + 50 && m->type != BGN_ARRIVE) {
        N.said_far = 1;
        lower_first(t);
        snprintf(full, sizeof full, "Em %s, %s", spoken_dist(st->next_dm, d, sizeof d), t);
        say(full);
    }
}

static void update_banner(void)
{
    if(!banner) return;
    const bgn_state_t *st = &N.st;
    char t[160], d[32];
    if(N.rerouting) {
        lv_label_set_text(ban_dist, "");
        lv_label_set_text(ban_text, tr(S_MAP_REROUTE));
        lv_label_set_text(ban_then, "");
        return;
    }
    if(st->next < 0 || st->next >= N.route.nman) return;
    const bgn_man_t *m = &N.route.man[st->next];
    if(m->type != icon_type || m->exit != icon_exit) {
        rast_surface_t s = { icon_px, 80, 80, 80 };
        bgn_icon(&s, m->type, m->exit, rgb565(0xffffff), rgb565(0x9fd4a8), rgb565(0x1e6e34));
        icon_type = m->type;
        icon_exit = m->exit;
        lv_image_cache_drop(&icon_dsc);
        lv_image_set_src(ban_icon, &icon_dsc);
        lv_obj_invalidate(ban_icon);
    }
    lv_label_set_text(ban_dist, m->type == BGN_ARRIVE && st->arrived ? "" : bgn_dist_text(st->next_dm, d, sizeof d));
    lv_label_set_text(ban_text, bgn_text(m, t, sizeof t));
    if(st->next + 1 < N.route.nman && N.route.man[st->next + 1].at_dm - m->at_dm < 1500) {
        char t2[160], line[200];
        snprintf(line, sizeof line, "%s: %s", tr(S_MAP_THEN), bgn_text(&N.route.man[st->next + 1], t2, sizeof t2));
        lv_label_set_text(ban_then, line);
    } else lv_label_set_text(ban_then, "");
}

static void update_botbar(void)
{
    if(!bot_lbl) return;
    char d[32], line[128];
    uint32_t rem_dm = N.navigating ? N.st.remain_dm : N.route.total_dm;
    uint32_t rem_ds = N.navigating ? N.st.remain_ds : N.route.total_ds;
    int min = (int)((rem_ds + 300) / 600);
    char tm[24];
    if(min >= 60) snprintf(tm, sizeof tm, "%d h %02d min", min / 60, min % 60);
    else snprintf(tm, sizeof tm, "%d min", min);
    int n = snprintf(line, sizeof line, "%s · %s", bgn_dist_text(rem_dm, d, sizeof d), tm);
    time_t now = time(NULL);
    struct tm lt;
    if(now > 1700000000 && localtime_r(&now, &lt)) {          /* só com relógio acertado */
        time_t eta = now + rem_ds / 10;
        localtime_r(&eta, &lt);
        if(n > 0 && n < (int)sizeof line) {
            line[n] = 0;
            char a[32];
            snprintf(a, sizeof a, tr(S_MAP_ARRIVAL), lt.tm_hour, lt.tm_min);
            snprintf(line + n, sizeof line - (size_t)n, " · %s", a);
        }
    }
    lv_label_set_text(bot_lbl, line);
}

static void close_sheet(void)
{
    if(sheet) { lv_obj_delete(sheet); sheet = NULL; }
}

static void end_navigation(void)
{
    N.navigating = N.simulate = N.rerouting = N.active = N.has_dest = N.waiting = 0;
    bgn_free(&N.route);
    memset(&N.st, 0, sizeof N.st);
    set_route_overlay(NULL, 0);
    close_sheet();
    if(banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    if(botbar) lv_obj_add_flag(botbar, LV_OBJ_FLAG_HIDDEN);
    if(sim_chip) lv_obj_add_flag(sim_chip, LV_OBJ_FLAG_HIDDEN);
    if(info_chip) lv_obj_remove_flag(info_chip, LV_OBJ_FLAG_HIDDEN);
    if(search_btn) lv_obj_remove_flag(search_btn, LV_OBJ_FLAG_HIDDEN);
    heading_up = N.prev_heading_up;
    lv_label_set_text(rot_lbl, heading_up ? LV_SYMBOL_UP : "N");
    cur.rot = 0;
    update_marker();
    request();
}

static void end_cb(lv_event_t *e) { (void)e; end_navigation(); }
static void cancel_cb(lv_event_t *e) { (void)e; end_navigation(); }

static void recent_add(const char *name, uint32_t x, uint32_t y);
static char dest_title[96];        /* nome do destino escolhido na busca (vazio: ponto marcado) */

static void start_navigation(bool simulate)
{
    close_sheet();
    recent_add(dest_title[0] ? dest_title : N.dest_name[0] ? N.dest_name : tr(S_MAP_POINT), N.dx, N.dy);
    N.navigating = 1;
    N.simulate = simulate;
    N.sim_t = 0;
    N.said_next = -1;
    N.route.seg = 0;
    N.route.off_count = 0;
    N.prev_heading_up = heading_up;
    heading_up = true;
    lv_label_set_text(rot_lbl, LV_SYMBOL_UP);
    follow = true;
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(botbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_chip, LV_OBJ_FLAG_HIDDEN);             /* a barra de baixo ocupa o lugar */
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_HIDDEN);            /* a faixa da manobra ocupa o lugar */
    if(simulate) lv_obj_remove_flag(sim_chip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sim_chip, LV_OBJ_FLAG_HIDDEN);
    cur.zoom256 = N.profile >= BGR_BIKE ? 17 * 256 : 16 * 256 + 128;
    update_scale();
    char t[160];
    if(N.route.nman) { bgn_text(&N.route.man[0], t, sizeof t); say(t); }
    if(simulate) on_fix(N.route.xy[0], N.route.xy[1], 0, -1, true);
    else if(have_pos) {
        uint32_t x, y;
        bgr_lonlat_to_world(pos_lon, pos_lat, &x, &y);
        on_fix(x, y, pos_speed, pos_course, pos_live);
    }
}

static void start_cb(lv_event_t *e) { (void)e; start_navigation(false); }
static void sim_cb(lv_event_t *e) { (void)e; start_navigation(true); }

static lv_obj_t *sheet_new(void)
{
    close_sheet();
    sheet = bf_card(ov);
    lv_obj_set_size(sheet, 620, LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sheet, BF_SP_S, 0);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);              /* não deixa o toque passar para o mapa */
    return sheet;
}

static lv_obj_t *sheet_row(lv_obj_t *parent)
{
    lv_obj_t *r = bf_plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, BF_SP_S, 0);
    return r;
}

static void sheet_message(const char *title, const char *text)
{
    lv_obj_t *sh = sheet_new();
    bf_label(sh, title, BF_FONT_M, pal()->text);
    lv_obj_t *l = bf_label(sh, text, BF_FONT_S, pal()->text_dim);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_t *row = sheet_row(sh);
    lv_obj_t *b = bf_button(row, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0);
    lv_obj_add_event_cb(b, cancel_cb, LV_EVENT_CLICKED, NULL);
}

static void route_request(int profile, bool reroute)
{
    uint32_t sx, sy;
    int heading = -1;
    if(N.simulate && N.navigating) { sx = N.st.x; sy = N.st.y; }
    else if(have_pos) {
        bgr_lonlat_to_world(pos_lon, pos_lat, &sx, &sy);
        if(pos_live && pos_speed >= 8) heading = (int)lround(pos_course);
    } else return;
    bgrt_req_t q = { ++N.req_id, profile, sx, sy, heading, N.dx, N.dy };
    N.profile = profile;
    N.waiting = 1;
    N.rerouting = reroute;
    bgrt_request(&q);
}

static void profile_cb(lv_event_t *e)
{
    int profile = (int)(intptr_t)lv_event_get_user_data(e);
    if(!have_pos) { sheet_message(tr(S_MAP_DEST), tr(S_MAP_NOPOS)); return; }
    lv_obj_t *sh = sheet_new();
    bf_label(sh, tr(S_MAP_CALC), BF_FONT_M, pal()->text);
    if(!pos_live) {
        lv_obj_t *l = bf_label(sh, tr(S_MAP_FROM_LAST), BF_FONT_S, pal()->text_dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    }
    route_request(profile, false);
}

static void fav_add(const char *name, uint32_t x, uint32_t y);

static void fav_save_cb(lv_event_t *e)
{
    fav_add(dest_title[0] ? dest_title : tr(S_MAP_POINT), N.dx, N.dy);
    lv_obj_t *b = lv_event_get_target(e);
    lv_obj_add_state(b, LV_STATE_DISABLED);
    lv_obj_t *l = lv_obj_get_child(b, lv_obj_get_child_count(b) - 1);
    if(l) lv_label_set_text(l, tr(S_MAP_SAVED));
}

static void open_dest_sheet_named(uint32_t x, uint32_t y, const char *name);

static void open_dest_sheet(uint32_t x, uint32_t y) { open_dest_sheet_named(x, y, NULL); }

static void open_dest_sheet_named(uint32_t x, uint32_t y, const char *name)
{
    if(N.navigating) return;
    snprintf(dest_title, sizeof dest_title, "%s", name ? name : "");
    bgn_free(&N.route);
    set_route_overlay(NULL, 0);
    N.active = 0;
    N.has_dest = 1;
    N.dx = x;
    N.dy = y;
    update_marker();
    lv_obj_t *sh = sheet_new();
    lv_obj_t *head = sheet_row(sh);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = bf_label(head, dest_title[0] ? dest_title : tr(S_MAP_DEST), BF_FONT_M, pal()->text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *fb = bf_button(head, LV_SYMBOL_SAVE, tr(S_MAP_SAVE_FAV), 0);
    lv_obj_set_height(fb, 40);
    lv_obj_add_event_cb(fb, fav_save_cb, LV_EVENT_CLICKED, NULL);
    bf_label(sh, tr(S_MAP_DEST_HINT), BF_FONT_S, pal()->text_dim);
    lv_obj_t *row = sheet_row(sh);
    static const bf_str_t names[BGR_PROFILES] = { S_MAP_CAR, S_MAP_MOTO, S_MAP_BIKE, S_MAP_FOOT };
    for(int i = 0; i < BGR_PROFILES; i++) {
        lv_obj_t *b = bf_button(row, NULL, tr(names[i]), i == BGR_CAR ? 1 : 0);
        lv_obj_set_height(b, 48);
        if(bgrt_have_profile(i)) lv_obj_add_event_cb(b, profile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        else lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    lv_obj_t *b = bf_button(row, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_height(b, 48);
    lv_obj_add_event_cb(b, cancel_cb, LV_EVENT_CLICKED, NULL);
}

static void route_result(bgrt_res_t *r)
{
    N.waiting = 0;
    if(r->id != N.req_id) { bgn_free(&r->route); return; }       /* resposta antiga */
    if(r->rc != BGRT_OK) {
        bgn_free(&r->route);
        if(N.rerouting) { N.rerouting = 0; N.route.off_count = 0; return; }   /* tenta de novo ao sair da rota */
        bf_str_t why = r->rc == BGRT_NOFILE ? S_MAP_NOFILE : r->rc == BGRT_NOSTART ? S_MAP_NOSTART
                     : r->rc == BGRT_NODEST ? S_MAP_NODEST : S_MAP_NOROUTE;
        sheet_message(tr(S_MAP_DEST), tr(why));
        return;
    }
    bgn_free(&N.route);
    N.route = r->route;
    snprintf(N.dest_name, sizeof N.dest_name, "%s", r->dest_name);
    N.active = 1;
    set_route_overlay(N.route.xy, N.route.npts);
    if(N.rerouting) {
        N.rerouting = 0;
        N.said_next = -1;
        say(tr(S_MAP_REROUTE));
        return;
    }
    /* visão geral com resumo */
    fit_route();
    lv_obj_t *sh = sheet_new();
    char d[32], line[160];
    int min = (int)((N.route.total_ds + 300) / 600);
    if(min >= 60) snprintf(line, sizeof line, "%s · %d h %02d min", bgn_dist_text(N.route.total_dm, d, sizeof d), min / 60, min % 60);
    else snprintf(line, sizeof line, "%s · %d min", bgn_dist_text(N.route.total_dm, d, sizeof d), min);
    bf_label(sh, line, BF_FONT_L, pal()->text);
    if(N.dest_name[0]) bf_label(sh, N.dest_name, BF_FONT_S, pal()->text_dim);
    lv_obj_t *row = sheet_row(sh);
    lv_obj_t *b = bf_button(row, LV_SYMBOL_PLAY, tr(S_MAP_START), 1);
    lv_obj_set_height(b, 48);
    lv_obj_add_event_cb(b, start_cb, LV_EVENT_CLICKED, NULL);
    if(!pos_live) lv_obj_add_state(b, LV_STATE_DISABLED);       /* sem GPS: só simulação */
    b = bf_button(row, LV_SYMBOL_SHUFFLE, tr(S_MAP_SIM), 0);
    lv_obj_set_height(b, 48);
    lv_obj_add_event_cb(b, sim_cb, LV_EVENT_CLICKED, NULL);
    b = bf_button(row, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0);
    lv_obj_set_height(b, 48);
    lv_obj_add_event_cb(b, cancel_cb, LV_EVENT_CLICKED, NULL);
}

/* uma posição (GPS ou simulação) */
static void on_fix(uint32_t x, uint32_t y, double speed, double course, bool live)
{
    have_pos = true;
    pos_live = live;
    bgr_world_to_lonlat(x, y, &pos_lon, &pos_lat);
    pos_speed = speed;
    if(speed >= 3.0 && course >= 0) pos_course = course;
    uint32_t cx = x, cy = y;
    if(N.navigating) {
        bgn_update(&N.route, x, y, speed, speed >= 3 ? pos_course : -1, &N.st);
        if(N.st.valid) { cx = N.st.x; cy = N.st.y; }           /* a câmera segue o ponto na via */
        if(N.st.off_route && !N.rerouting && !N.waiting && !N.simulate) route_request(N.profile, true);
        announce();
        update_banner();
        update_botbar();
        if(N.st.arrived && !N.arrived_ms) N.arrived_ms = 1;
        /* zoom pela velocidade: mais longe na estrada */
        int z = N.profile >= BGR_BIKE ? 17 * 256 : speed > 80 ? 15 * 256 + 128 : speed > 50 ? 16 * 256 : 16 * 256 + 128;
        if(follow && z != cur.zoom256) { cur.zoom256 = z; update_scale(); }
    }
    if(follow) {
        /* parado (posição oscilando alguns metros): só redesenha se o centro andar 2 px ou
         * o mapa girar 2 graus — poupa CPU com o GPS ligado e o aparelho quieto */
        int rot = heading_up ? -(int)lround(pos_course) : 0;
        rast_surface_t sf = { NULL, MAP_W, MAP_H, MAP_W };
        int sx, sy;
        bgr_view_t v = cur;
        anchor(&v.ox, &v.oy);
        bgr_world_to_screen(&v, &sf, cx, cy, &sx, &sy);
        int dr = abs(rot - cur.rot) % 360;
        if(dr > 180) dr = 360 - dr;
        if(abs(sx - v.ox) >= 2 || abs(sy - v.oy) >= 2 || dr >= 2 || !have_shown) {
            cur.cx = cx;
            cur.cy = cy;
            cur.rot = rot;
            request();
        }
    }
    update_marker();
}

/* simulação: avança 1 s pela rota na velocidade dos trechos */
static void sim_step(void)
{
    const bgn_route_t *r = &N.route;
    if(r->npts < 2) return;
    uint32_t total = r->cum_ds[r->npts - 1];
    N.sim_t += 10;
    if(N.sim_t > total) N.sim_t = total;
    int k = 0;
    while(k + 1 < r->npts - 1 && r->cum_ds[k + 1] < N.sim_t) k++;
    double span = (double)r->cum_ds[k + 1] - r->cum_ds[k], f = span > 0 ? (N.sim_t - r->cum_ds[k]) / span : 1;
    const uint32_t *a = &r->xy[2 * k], *b = &r->xy[2 * k + 2];
    uint32_t x = a[0] + (uint32_t)(int32_t)lround(f * (double)(int32_t)(b[0] - a[0]));
    uint32_t y = a[1] + (uint32_t)(int32_t)lround(f * (double)(int32_t)(b[1] - a[1]));
    double hd = atan2((double)(int32_t)(b[0] - a[0]), -(double)(int32_t)(b[1] - a[1])) * 180 / M_PI;
    double kmh = span > 0 ? ((double)r->cum_dm[k + 1] - r->cum_dm[k]) / span * 3.6 : 0;
    on_fix(x, y, kmh, hd < 0 ? hd + 360 : hd, true);
}

/* ----------------------------------------------------------------- busca */
#define MAXRES 20
#define MAXFAV 40
static bgs_index_t *six;
static lv_obj_t *sv, *sv_ta, *sv_list, *sv_kb;
static lv_timer_t *sv_timer;
static bgs_result_t sv_res[MAXRES];
static int sv_nres;
static struct { char name[96]; uint32_t x, y; } favs[MAXFAV], recent[10];
static int nfav, nrecent;

static int list_load(const char *file, void *dst, int max);

static void fav_load(void) { nfav = list_load("biggps-favoritos.txt", favs, MAXFAV); }

/* lista "lat lon nome" de um arquivo da pasta de dados */
static int list_load(const char *file, void *dst, int max)
{
    struct { char name[96]; uint32_t x, y; } *out = dst;
    char path[512], line[200];
    data_path(path, sizeof path, file);
    int n = 0;
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    while(n < max && fgets(line, sizeof line, f)) {
        double lat, lon;
        int off = 0;
        if(sscanf(line, "%lf %lf %n", &lat, &lon, &off) < 2 || !off || fabs(lat) > 85 || fabs(lon) > 180) continue;
        char *nm = line + off;
        nm[strcspn(nm, "\r\n")] = 0;
        if(!*nm) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", nm);
        bgr_lonlat_to_world(lon, lat, &out[n].x, &out[n].y);
        n++;
    }
    fclose(f);
    return n;
}

/* destinos recentes: o mais novo primeiro, sem repetir, no máximo 10 */
static void recent_add(const char *name, uint32_t x, uint32_t y)
{
    nrecent = list_load("biggps-recentes.txt", recent, 10);
    char path[512];
    data_path(path, sizeof path, "biggps-recentes.txt");
    FILE *f = fopen(path, "w");
    if(!f) return;
    double lon, lat;
    bgr_world_to_lonlat(x, y, &lon, &lat);
    char clean[96];
    snprintf(clean, sizeof clean, "%s", name);
    clean[strcspn(clean, "\r\n")] = 0;
    fprintf(f, "%.7f %.7f %s\n", lat, lon, clean);
    int written = 1;
    for(int i = 0; i < nrecent && written < 10; i++) {
        if(!strcmp(recent[i].name, clean)) continue;
        bgr_world_to_lonlat(recent[i].x, recent[i].y, &lon, &lat);
        fprintf(f, "%.7f %.7f %s\n", lat, lon, recent[i].name);
        written++;
    }
    fclose(f);
}

static void fav_add(const char *name, uint32_t x, uint32_t y)
{
    fav_load();
    if(nfav >= MAXFAV) return;
    char path[512];
    data_path(path, sizeof path, "biggps-favoritos.txt");
    FILE *f = fopen(path, "a");
    if(!f) return;
    double lon, lat;
    bgr_world_to_lonlat(x, y, &lon, &lat);
    char clean[96];
    snprintf(clean, sizeof clean, "%s", name);
    clean[strcspn(clean, "\r\n")] = 0;                          /* uma linha por favorito */
    fprintf(f, "%.7f %.7f %s\n", lat, lon, clean);
    fclose(f);
}

static void search_close(void)
{
    if(sv_timer) { lv_timer_delete(sv_timer); sv_timer = NULL; }
    if(sv_kb) { lv_obj_delete(sv_kb); sv_kb = NULL; }
    if(sv) { lv_obj_delete(sv); sv = NULL; }
    sv_ta = sv_list = NULL;
}

static void go_to(uint32_t x, uint32_t y, const char *name)
{
    search_close();
    set_follow(false);
    cur.cx = x;
    cur.cy = y;
    cur.rot = 0;
    if(cur.zoom256 < 15 * 256) cur.zoom256 = 16 * 256;
    update_scale();
    request();
    open_dest_sheet_named(x, y, name);
}

static void res_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i >= 2000) {                                             /* recente */
        i -= 2000;
        if(i < nrecent) go_to(recent[i].x, recent[i].y, recent[i].name);
    } else if(i >= 1000) {                                      /* favorito */
        i -= 1000;
        if(i < nfav) go_to(favs[i].x, favs[i].y, favs[i].name);
    } else if(i < sv_nres) {
        go_to(sv_res[i].x, sv_res[i].y, sv_res[i].name);
    }
}

static void list_row(const char *sym, const char *name, const char *sub, uint32_t dist_m, intptr_t ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *r = lv_button_create(sv_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(r, p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_add_event_cb(r, res_cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *ic = bf_label(r, sym, BF_FONT_M, p->primary);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *nl = bf_label(r, name, BF_FONT_M, p->text);
    lv_obj_set_pos(nl, 44, 4);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, 600);
    if(sub && *sub) {
        lv_obj_t *sl = bf_label(r, sub, BF_FONT_S, p->text_dim);
        lv_obj_set_pos(sl, 44, 30);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sl, 600);
    }
    if(dist_m != UINT32_MAX) {
        char d[24];
        bgn_dist_text(dist_m * 10, d, sizeof d);
        lv_obj_t *dl = bf_label(r, d, BF_FONT_S, p->text_dim);
        lv_obj_align(dl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

static void ref_point(uint32_t *x, uint32_t *y)
{
    if(have_pos) bgr_lonlat_to_world(pos_lon, pos_lat, x, y);
    else { *x = cur.cx; *y = cur.cy; }
}

static void show_results(void)
{
    lv_obj_clean(sv_list);
    static const char *sym[] = { LV_SYMBOL_HOME, LV_SYMBOL_RIGHT, LV_SYMBOL_GPS, LV_SYMBOL_HOME };
    for(int i = 0; i < sv_nres; i++)
        list_row(sym[sv_res[i].type & 3], sv_res[i].name, sv_res[i].context, sv_res[i].dist_m, i);
    if(!sv_nres) bf_label(sv_list, tr(S_MAP_NORESULT), BF_FONT_M, pal()->text_dim);
}

static void cat_cb(lv_event_t *e)
{
    int cat = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t x, y;
    ref_point(&x, &y);
    sv_nres = six ? bgs_nearby(six, cat, x, y, 30000, sv_res, MAXRES) : 0;
    show_results();
}

static void show_home(void)
{
    lv_obj_clean(sv_list);
    if(!six) {
        lv_obj_t *l = bf_label(sv_list, tr(S_MAP_NOSEARCH), BF_FONT_M, pal()->text_dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    } else {
        lv_obj_t *row = bf_plain(sv_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(row, BF_SP_S, 0);
        lv_obj_set_style_pad_row(row, BF_SP_S, 0);
        static const struct { bf_str_t s; int cat; } cats[] = {
            { S_MAP_C_FUEL, BGS_C_FUEL }, { S_MAP_C_HOSPITAL, BGS_C_HOSPITAL }, { S_MAP_C_PHARMACY, BGS_C_PHARMACY },
            { S_MAP_C_MARKET, BGS_C_MARKET }, { S_MAP_C_FOOD, BGS_C_FOOD }, { S_MAP_C_HOTEL, BGS_C_HOTEL },
        };
        for(unsigned i = 0; i < sizeof cats / sizeof cats[0]; i++) {
            lv_obj_t *b = bf_button(row, NULL, tr(cats[i].s), 0);
            lv_obj_set_height(b, 44);
            lv_obj_add_event_cb(b, cat_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cats[i].cat);
        }
    }
    fav_load();
    if(nfav) bf_label(sv_list, tr(S_MAP_FAVORITES), BF_FONT_S, pal()->text_dim);
    for(int i = 0; i < nfav; i++) list_row(LV_SYMBOL_SAVE, favs[i].name, NULL, UINT32_MAX, 1000 + i);
    nrecent = list_load("biggps-recentes.txt", recent, 10);
    if(nrecent) bf_label(sv_list, tr(S_MAP_RECENT), BF_FONT_S, pal()->text_dim);
    for(int i = 0; i < nrecent; i++) list_row(LV_SYMBOL_LOOP, recent[i].name, NULL, UINT32_MAX, 2000 + i);
}

static void sv_query(lv_timer_t *t)
{
    (void)t;
    sv_timer = NULL;
    if(!sv_ta) return;
    const char *q = lv_textarea_get_text(sv_ta);
    char norm[128];
    if(!bgs_norm(q, norm, sizeof norm) || !six) { show_home(); return; }
    uint32_t x, y;
    ref_point(&x, &y);
    sv_nres = bgs_query(six, q, x, y, sv_res, MAXRES);
    show_results();
}

static void sv_changed(lv_event_t *e)
{
    (void)e;
    if(sv_timer) lv_timer_reset(sv_timer);
    else {
        sv_timer = lv_timer_create(sv_query, 350, NULL);
        lv_timer_set_repeat_count(sv_timer, 1);
    }
}

static void sv_close_cb(lv_event_t *e) { (void)e; search_close(); }

static void search_open(lv_event_t *e)
{
    (void)e;
    if(sv || N.navigating) return;
    const bf_palette_t *p = pal();
    close_sheet();
    sv = lv_obj_create(ov);
    lv_obj_remove_style_all(sv);
    lv_obj_set_size(sv, MAP_W, MAP_H);
    lv_obj_set_style_bg_color(sv, p->bg, 0);
    lv_obj_set_style_bg_opa(sv, LV_OPA_COVER, 0);
    lv_obj_add_flag(sv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sv, LV_OBJ_FLAG_SCROLLABLE);
    sv_ta = lv_textarea_create(sv);
    lv_textarea_set_one_line(sv_ta, true);
    lv_textarea_set_max_length(sv_ta, 80);
    lv_textarea_set_placeholder_text(sv_ta, tr(S_MAP_SEARCH_HINT));
    lv_obj_set_style_text_font(sv_ta, BF_FONT_M, 0);
    lv_obj_set_size(sv_ta, MAP_W - 96, 52);
    lv_obj_set_pos(sv_ta, 12, 8);
    lv_obj_add_event_cb(sv_ta, sv_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *x = bf_button(sv, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 60, 52);
    lv_obj_set_pos(x, MAP_W - 72, 8);
    lv_obj_add_event_cb(x, sv_close_cb, LV_EVENT_CLICKED, NULL);
    sv_list = bf_plain(sv);
    lv_obj_set_size(sv_list, MAP_W - 24, MAP_H - 70 - 210);
    lv_obj_set_pos(sv_list, 12, 66);
    lv_obj_set_flex_flow(sv_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sv_list, 2, 0);
    bf_scrollable(sv_list);
    sv_kb = lv_keyboard_create(sv);
    lv_obj_set_size(sv_kb, MAP_W, 210);
    lv_obj_align(sv_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(sv_kb, sv_ta);
    lv_obj_add_event_cb(sv_kb, sv_close_cb, LV_EVENT_CANCEL, NULL);
    show_home();
}

/* -------------------------------------------------------------- eventos */
static void gps_update(void)
{
    gps_info_t g;
    gps_read(&g);
    update_info(&g);
    if(N.navigating && N.simulate) { sim_step(); return; }   /* a simulação substitui o GPS */
    bool live = g.running && g.state >= GPS_FIX2D;
    if(!live) {
        if(pos_live) { pos_live = false; update_marker(); }
        return;
    }
    uint32_t x, y;
    bgr_lonlat_to_world(g.lon, g.lat, &x, &y);
    on_fix(x, y, g.speed_kmh, g.speed_kmh >= 3.0 ? g.course : -1, true);
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
    bgrt_res_t rr;
    if(bgrt_poll(&rr)) route_result(&rr);
    if(N.arrived_ms) {                                /* chegou: encerra depois de 5 s */
        if((N.arrived_ms += 100) > 5000) { N.arrived_ms = 0; end_navigation(); }
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
    } else if(c == LV_EVENT_LONG_PRESSED) {
        if(dragging || N.navigating || !R.started) return;
        rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
        uint32_t x, y;
        bgr_screen_to_world(&shown, &s, p.x - off_x, p.y - off_y, &x, &y);
        open_dest_sheet(x, y);
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

static lv_obj_t *voice_lbl;

static void voice_cb(lv_event_t *e)
{
    (void)e;
    settings()->nav_voice = !settings()->nav_voice;
    settings_save();
    lv_label_set_text(voice_lbl, settings()->nav_voice ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    if(!settings()->nav_voice) voice_stop();
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

void scr_map_demo(int step);
static bool demo_alive;
static void demo_timer(lv_timer_t *t)
{
    if(demo_alive) scr_map_demo((int)(intptr_t)lv_timer_get_user_data(t));
}

/* todos os .bgm de <dir>/mapas (ou de <dir>), em ordem alfabética: um mapa pode vir em
 * várias partes (limite de 4 GB por arquivo no FAT32) */
#define MAXMAPS 8
static int find_maps(char paths[][1024], int max)
{
    const char *base = getenv("BIGFOSTON_BIGGPS");
    if(!base || !*base) base = BIGGPS_DIR_DEFAULT;
    char dirs[2][512];
    snprintf(dirs[0], sizeof dirs[0], "%s/mapas", base);
    snprintf(dirs[1], sizeof dirs[1], "%s", base);
    for(int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        if(!dp) continue;
        int n = 0;
        struct dirent *de;
        while((de = readdir(dp)) && n < max) {
            size_t l = strlen(de->d_name);
            if(l > 4 && !strcasecmp(de->d_name + l - 4, ".bgm") && de->d_name[0] != '.'
               && snprintf(paths[n], 1024, "%s/%s", dirs[d], de->d_name) < 1024) n++;
        }
        closedir(dp);
        for(int i = 1; i < n; i++)                  /* ordem alfabética (poucos arquivos) */
            for(int j = i; j > 0 && strcmp(paths[j - 1], paths[j]) > 0; j--) {
                char t[1024];
                memcpy(t, paths[j], sizeof t);
                memcpy(paths[j], paths[j - 1], sizeof t);
                memcpy(paths[j - 1], t, sizeof t);
            }
        if(n) return n;
    }
    return 0;
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

    if(!mk_dsc[0].data) mk_build();
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
    lv_obj_add_event_cb(ov, press_cb, LV_EVENT_LONG_PRESSED, NULL);

    img = lv_image_create(ov);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    layer = lv_obj_create(ov);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, MAP_W, MAP_H);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(layer, layer_draw, LV_EVENT_DRAW_MAIN, NULL);
    dmark = lv_image_create(ov);
    lv_image_set_src(dmark, &mk_dsc[3]);
    lv_obj_remove_flag(dmark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN);
    mark = lv_image_create(ov);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);

    /* controles */
    lv_obj_t *b = round_btn(ov, LV_SYMBOL_LEFT, close_cb, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 12, 12);
    search_btn = round_btn(ov, "", search_open, 0);
    lv_obj_align(search_btn, LV_ALIGN_TOP_LEFT, 12 + BTN + 10, 12);
    lv_obj_t *mg = lv_image_create(search_btn);
    lv_image_set_src(mg, &mg_dsc);
    lv_obj_set_style_image_recolor(mg, p->text, 0);
    lv_obj_set_style_image_recolor_opa(mg, LV_OPA_COVER, 0);
    lv_obj_center(mg);
    lv_obj_remove_flag(mg, LV_OBJ_FLAG_CLICKABLE);
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
    b = round_btn(ov, settings()->nav_voice ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE, voice_cb, 0);
    voice_lbl = lv_obj_get_child(b, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12 + 5 * (BTN + 10));
    if(!voice_available()) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);   /* sem motor ou dados de voz */

    info_chip = chip(ov);
    lv_obj_align(info_chip, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    info_lbl = bf_label(info_chip, "", BF_FONT_S, p->text);

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

    /* navegação: faixa da manobra, barra do que falta, aviso de simulação */
    banner = lv_obj_create(ov);
    lv_obj_remove_style_all(banner);
    lv_obj_set_size(banner, 632, 104);
    lv_obj_set_pos(banner, 80, 12);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x1e6e34), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, 14, 0);
    lv_obj_set_style_shadow_width(banner, 14, 0);
    lv_obj_set_style_shadow_opa(banner, LV_OPA_40, 0);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    memset(&icon_dsc, 0, sizeof icon_dsc);
    icon_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    icon_dsc.header.w = 80;
    icon_dsc.header.h = 80;
    icon_dsc.header.stride = 160;
    icon_dsc.data = (const uint8_t *)icon_px;
    icon_dsc.data_size = sizeof icon_px;
    icon_type = icon_exit = -1;
    ban_icon = lv_image_create(banner);
    lv_obj_set_pos(ban_icon, 12, 12);
    ban_dist = bf_label(banner, "", BF_FONT_L, lv_color_white());
    lv_obj_set_pos(ban_dist, 104, 8);
    ban_text = bf_label(banner, "", BF_FONT_M, lv_color_white());
    lv_obj_set_pos(ban_text, 104, 44);
    lv_label_set_long_mode(ban_text, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ban_text, 516);
    ban_then = bf_label(banner, "", BF_FONT_S, lv_color_hex(0xcfe8d4));
    lv_obj_set_pos(ban_then, 104, 74);
    lv_label_set_long_mode(ban_then, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ban_then, 516);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    botbar = chip(ov);
    lv_obj_add_flag(botbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(botbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(botbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(botbar, BF_SP_M, 0);
    lv_obj_align(botbar, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    bot_lbl = bf_label(botbar, "", BF_FONT_M, p->text);
    lv_obj_t *eb = bf_button(botbar, LV_SYMBOL_CLOSE, tr(S_MAP_END), 2);
    lv_obj_set_height(eb, 44);
    lv_obj_add_event_cb(eb, end_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(botbar, LV_OBJ_FLAG_HIDDEN);

    sim_chip = chip(ov);
    lv_obj_set_style_bg_color(sim_chip, lv_color_hex(0xd93025), 0);
    lv_obj_set_style_bg_opa(sim_chip, LV_OPA_COVER, 0);
    bf_label(sim_chip, tr(S_MAP_SIMULATING), BF_FONT_S, lv_color_white());
    lv_obj_align(sim_chip, LV_ALIGN_TOP_MID, 6, 124);
    lv_obj_add_flag(sim_chip, LV_OBJ_FLAG_HIDDEN);

    set_follow(follow);
    lv_label_set_text(rot_lbl, heading_up ? LV_SYMBOL_UP : "N");

    static char paths[MAXMAPS][1024];
    char err[256];
    int nmaps = find_maps(paths, MAXMAPS);
    if(!nmaps) {
        show_message(tr(S_MAP_NOMAP), tr(S_MAP_NOMAP_SUB));
        gps_update();
        tick = lv_timer_create(tick_cb, 100, NULL);
        return;
    }
    R.pack = bgm_open(paths[0], err, sizeof err);
    for(int i = 1; R.pack && i < nmaps; i++)
        if(bgm_add(R.pack, paths[i], err, sizeof err)) fprintf(stderr, "biggps: %s\n", err);
    if(!R.pack) {
        show_message(tr(S_MAP_NOMAP), err);
        gps_update();
        tick = lv_timer_create(tick_cb, 100, NULL);
        return;
    }
    bgm_set_cache_limit(R.pack, 8u << 20);
    fprintf(stderr, "biggps: mapa %s (%d arquivo%s)\n", paths[0], nmaps, nmaps > 1 ? "s" : "");
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
    {
        const char *base = getenv("BIGFOSTON_BIGGPS");
        bgrt_start(base && *base ? base : BIGGPS_DIR_DEFAULT);
        char sd[600], sp[1100], serr[256];
        snprintf(sd, sizeof sd, "%s/busca", base && *base ? base : BIGGPS_DIR_DEFAULT);
        DIR *dp = opendir(sd);
        if(dp) {
            struct dirent *de;
            char best[256] = "";
            while((de = readdir(dp))) {
                size_t l = strlen(de->d_name);
                if(l > 4 && de->d_name[0] != '.' && !strcasecmp(de->d_name + l - 4, ".bgs") && (!best[0] || strcmp(de->d_name, best) < 0)
                   && snprintf(best, sizeof best, "%s", de->d_name) >= (int)sizeof best) best[0] = 0;
            }
            closedir(dp);
            if(best[0] && snprintf(sp, sizeof sp, "%s/%s", sd, best) < (int)sizeof sp && !(six = bgs_open(sp, serr, sizeof serr)))
                fprintf(stderr, "biggps: %s\n", serr);
        }
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
    /* teste no aparelho sem toque (docs/big-gps): BIGGPS_DEMO=1 marca o destino da
     * demonstração e, 6 s depois, simula o percurso */
    demo_alive = true;
    if(getenv("BIGGPS_DEMO")) {
        lv_timer_t *t1 = lv_timer_create(demo_timer, 3000, (void *)0);
        lv_timer_set_repeat_count(t1, 1);
        lv_timer_t *t2 = lv_timer_create(demo_timer, 9000, (void *)1);
        lv_timer_set_repeat_count(t2, 1);
    }
}

static void destroy(void)
{
    demo_alive = false;
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    if(R.started) {
        pthread_mutex_lock(&R.mu);
        R.stop = 1;
        pthread_cond_signal(&R.cv);
        pthread_mutex_unlock(&R.mu);
        pthread_join(R.th, NULL);
        R.started = 0;
    }
    search_close();
    bgs_close(six);
    six = NULL;
    bgrt_stop();
    voice_stop();
    if(N.navigating) heading_up = N.prev_heading_up;
    bgn_free(&N.route);
    memset(&N, 0, sizeof N);
    free(R.route);
    R.route = NULL;
    R.route_n = 0;
    last_save();
    rot_clear();
    if(glyph_buf) { lv_draw_buf_destroy(glyph_buf); glyph_buf = NULL; }
    if(ov) { lv_obj_delete(ov); ov = NULL; }
    if(R.pack) { bgm_close(R.pack); R.pack = NULL; }
    for(int i = 0; i < 2; i++) { free(R.buf[i]); R.buf[i] = NULL; }
    img = layer = mark = dmark = info_chip = info_lbl = scale_lbl = scale_bar = follow_btn = rot_lbl = msg = NULL;
    voice_lbl = search_btn = NULL;
    sheet = banner = ban_icon = ban_dist = ban_text = ban_then = botbar = bot_lbl = sim_chip = NULL;
    have_shown = 0;
    nplaced = 0;
}

/* --screenshot: 0 destino em Taguatinga (carro), 1 começa a simulação, 2 avança 60 s,
 * 3 abre a busca, 4 busca "hospital", 5 fecha a busca */
void scr_map_demo(int step)
{
    if(step == 0) {
        uint32_t x, y;
        bgr_lonlat_to_world(-48.0560, -15.8335, &x, &y);
        open_dest_sheet(x, y);
        if(bgrt_have_profile(BGR_CAR) && have_pos) route_request(BGR_CAR, false);
    } else if(step == 1) {
        if(N.active) start_navigation(true);
    } else if(step == 2) {
        for(int i = 0; i < 60 && N.navigating; i++) sim_step();
    } else if(step == 3) {
        search_open(NULL);
    } else if(step == 4) {
        if(sv_ta) { lv_textarea_set_text(sv_ta, "hospital"); sv_query(NULL); }
    } else if(step == 5) {
        search_close();
    }
}

const bf_screen_def_t scr_map_def = { create, destroy, S_APP_GPS, NULL };
