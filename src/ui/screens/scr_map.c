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
#include "../core/perf.h"
#include "../third_party/lvgl/src/core/lv_obj_event_private.h"   /* lv_cover_check_info_t */
#include "../biggps/bgmap.h"
#include "../biggps/bgrender.h"
#include "../biggps/bgrouter.h"
#include "../biggps/bgicons.h"
#include "../biggps/bgsearch.h"
#include "../biggps/bgmotion.h"
#include "../biggps/bgalert.h"
#include "../biggps/bgvehicle.h"
#include "../biggps/bgpersp.h"
#include "../biggps/bglanes.h"
#include "../biggps/bgicons.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define MAP_W   800
#define MAP_H   480
#define ROUTE_PANEL_W 300      /* painel das rotas à esquerda; o mapa enquadra à direita dele */
#define NAV_COL_W 150          /* coluna da navegação à esquerda: tempo, distância, chegada, velocidade */
/* superfície onde os rótulos são postos: a tela (2D) ou a fonte da vista inclinada */
static int lab_w = MAP_W, lab_h = MAP_H;
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
    int angle;                 /* != 0: nome de via girado */
    int cx, cy;                /* centro do texto no quadro */
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
    /* contorno: máximo numa janela 5x5, separável (linha, depois coluna) */
    uint8_t *hm = malloc((size_t)W * H), *hv = malloc((size_t)W * H);
    if(!hm || !hv) { free(hm); free(hv); free(cov); free(px); return NULL; }
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++) {
            int m = 0;
            for(int dx = -2; dx <= 2; dx++) {
                int X = x + dx;
                if(X >= 0 && X < W && cov[y * W + X] > m) m = cov[y * W + X];
            }
            hm[y * W + x] = (uint8_t)m;
        }
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++) {
            int m = 0;
            for(int dy = -2; dy <= 2; dy++) {
                int Y = y + dy;
                if(Y >= 0 && Y < H && hm[Y * W + x] > m) m = hm[Y * W + x];
            }
            hv[y * W + x] = (uint8_t)m;
        }
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++) {
            int a = cov[y * W + x], hl = hv[y * W + x];
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
    free(hm);
    free(hv);
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

/* Rótulos compostos no próprio quadro do mapa (docs/performance/08-lvgl-optimization.md):
 * antes cada nome era desenhado 9 vezes pelo LVGL (contorno + texto) em todo quadro da
 * tela e os nomes de vias girados eram imagens transformadas a cada quadro (~120 ms
 * por quadro no arrasto). Agora cada rótulo vira um bitmap com contorno (guardado num
 * cache entre quadros do mapa) e é misturado uma vez no buffer RGB565 quando o quadro
 * chega; arrastar só move uma imagem opaca. */
#define LCACHE 64
static struct { char text[56]; const lv_font_t *f; int night; uint32_t *px; int w, h; unsigned used; } lcache[LCACHE];
static unsigned lcache_clock;

static const uint32_t *label_bitmap(const char *text, const lv_font_t *f, int night, int *w, int *h)
{
    int victim = 0;
    for(int i = 0; i < LCACHE; i++) {
        if(lcache[i].px && lcache[i].f == f && lcache[i].night == night && !strcmp(lcache[i].text, text)) {
            lcache[i].used = ++lcache_clock;
            *w = lcache[i].w;
            *h = lcache[i].h;
            return lcache[i].px;
        }
        if(lcache[victim].px && (!lcache[i].px || lcache[i].used < lcache[victim].used)) victim = i;
    }
    int W, H;
    uint32_t *px = text_bitmap(text, f, night ? 0xE8E8E8 : 0x202020, night ? 0x101318 : 0xFFFFFF, &W, &H);
    if(!px) return NULL;
    free(lcache[victim].px);
    snprintf(lcache[victim].text, sizeof lcache[victim].text, "%s", text);
    lcache[victim].f = f;
    lcache[victim].night = night;
    lcache[victim].px = px;
    lcache[victim].w = W;
    lcache[victim].h = H;
    lcache[victim].used = ++lcache_clock;
    *w = W;
    *h = H;
    return px;
}

static void lcache_clear(void)
{
    for(int i = 0; i < LCACHE; i++) { free(lcache[i].px); lcache[i].px = NULL; }
}

static inline void blend565(uint16_t *d, uint32_t argb, unsigned a)
{
    if(!a) return;
    uint32_t sr = argb >> 19 & 31, sg = argb >> 10 & 63, sb = argb >> 3 & 31;
    if(a >= 255) { *d = (uint16_t)(sr << 11 | sg << 5 | sb); return; }
    uint32_t v = *d, dr = v >> 11, dg = v >> 5 & 63, db = v & 31;
    dr += ((sr - dr) * a + 128) >> 8;
    dg += ((sg - dg) * a + 128) >> 8;
    db += ((sb - db) * a + 128) >> 8;
    *d = (uint16_t)(dr << 11 | dg << 5 | db);
}

/* bitmap ARGB8888 (w x h) com o centro em (cx, cy), girado 'ang' graus (horário) */
static void compose_bitmap(uint16_t *buf, const uint32_t *px, int w, int h, int cx, int cy, int ang)
{
    if(!ang) {
        int x0 = cx - w / 2, y0 = cy - h / 2;
        for(int y = 0; y < h; y++) {
            int Y = y0 + y;
            if(Y < 0 || Y >= lab_h) continue;
            for(int x = 0; x < w; x++) {
                int X = x0 + x;
                if(X < 0 || X >= lab_w) continue;
                uint32_t c = px[y * w + x];
                blend565(&buf[Y * lab_w + X], c, c >> 24);
            }
        }
        return;
    }
    /* mapeamento inverso com interpolação bilinear, ponto fixo Q16 */
    double r = ang * M_PI / 180;
    int32_t cs = (int32_t)lround(cos(r) * 65536), sn = (int32_t)lround(sin(r) * 65536);
    int hw = (int)(fabs(w * cos(r)) + fabs(h * sin(r))) / 2 + 2, hh = (int)(fabs(w * sin(r)) + fabs(h * cos(r))) / 2 + 2;
    for(int Y = cy - hh; Y <= cy + hh; Y++) {
        if(Y < 0 || Y >= lab_h) continue;
        for(int X = cx - hw; X <= cx + hw; X++) {
            if(X < 0 || X >= lab_w) continue;
            int dx = X - cx, dy = Y - cy;
            /* rotação inversa: fonte = R(-ang) * destino */
            int32_t u = (int32_t)(((int64_t)dx * cs + (int64_t)dy * sn) >> 8) + (w << 7) - 128;
            int32_t v = (int32_t)(((int64_t)-dx * sn + (int64_t)dy * cs) >> 8) + (h << 7) - 128;
            int ui = u >> 8, vi = v >> 8;
            if(ui < -1 || vi < -1 || ui >= w || vi >= h) continue;
            unsigned fu = (unsigned)u & 255, fv = (unsigned)v & 255;
            uint32_t p[4] = { 0, 0, 0, 0 };
            if(ui >= 0 && vi >= 0) p[0] = px[vi * w + ui];
            if(ui + 1 < w && vi >= 0) p[1] = px[vi * w + ui + 1];
            if(ui >= 0 && vi + 1 < h) p[2] = px[(vi + 1) * w + ui];
            if(ui + 1 < w && vi + 1 < h) p[3] = px[(vi + 1) * w + ui + 1];
            unsigned wt[4] = { (256 - fu) * (256 - fv), fu * (256 - fv), (256 - fu) * fv, fu * fv };
            uint32_t A = 0, Rr = 0, G = 0, B = 0;
            for(int k = 0; k < 4; k++) {
                uint32_t a = p[k] >> 24, aw = a * wt[k];
                A += aw;
                Rr += (p[k] >> 16 & 255) * aw;
                G += (p[k] >> 8 & 255) * aw;
                B += (p[k] & 255) * aw;
            }
            if(!A) continue;
            uint32_t c = (Rr / A) << 16 | (G / A) << 8 | (B / A);
            blend565(&buf[Y * lab_w + X], c, A >> 16);
        }
    }
}

/* ------------------------------------------------ vista inclinada (biggps/bgpersp.h)
 * Carro na linha 350, horizonte 800 px acima da tela: a fonte tem ~1,9x a área da tela e o
 * remapeamento custa ~27 ms no FOSTON. Liga com o acompanhamento no rumo e o botão em "3D";
 * volta sozinha ao 2D se os quadros do motor passarem de 500 ms em média. */
#define PERSP_Y0 350
#define PERSP_YH (-800)
static bgp_t PJ;
static bool pj_ok, shown_persp, persp_auto_off;

/* avisos marcados pelo usuário (lidos e gravados pela interface; o motor copia sob a trava) */
#define MAXUPT 200
static bga_entry_t upts[MAXUPT];
static int nupt;

/* ------------------------------------------------ pontos de alerta no quadro
 * Pequenos e desenhados com o rasterizador (sem imagem nem SVG por quadro). Cada tipo só
 * aparece a partir de um zoom, e no máximo MAXAL por quadro: nada de centenas de ícones. */
#define MAXAL 160
static void quad(rast_surface_t *s, int x0, int y0, int x1, int y1, uint16_t c)
{
    const int S = RAST_SUB;
    int32_t q[] = { x0 * S, y0 * S, x1 * S, y0 * S, x1 * S, y1 * S, x0 * S, y1 * S };
    int n4 = 4;
    rast_fill(s, q, &n4, 1, c);
}

static void icon_signal(rast_surface_t *s, int x, int y)
{
    const int S = RAST_SUB;
    quad(s, x - 6, y - 12, x + 6, y + 12, rgb565(0xffffff));
    quad(s, x - 5, y - 11, x + 5, y + 11, rgb565(0x2b2f36));
    rast_disc(s, x * S, (y - 6) * S, 3 * S, rgb565(0xe5484d));     /* sem estado real: as três luzes */
    rast_disc(s, x * S, y * S, 3 * S, rgb565(0xf5b83d));
    rast_disc(s, x * S, (y + 6) * S, 3 * S, rgb565(0x30a46c));
}

static void icon_camera(rast_surface_t *s, int x, int y)
{
    const int S = RAST_SUB;
    rast_disc(s, x * S, y * S, 12 * S, rgb565(0xffffff));
    rast_disc(s, x * S, y * S, 11 * S, rgb565(0xd93025));
    rast_disc(s, x * S, y * S, 8 * S, rgb565(0xffffff));
    quad(s, x - 5, y - 3, x + 3, y + 4, rgb565(0x202124));
    int32_t lens[] = { (x + 3) * S, (y - 1) * S, (x + 6) * S, (y - 3) * S, (x + 6) * S, (y + 4) * S, (x + 3) * S, (y + 2) * S };
    int n4 = 4;
    rast_fill(s, lens, &n4, 1, rgb565(0x202124));
}

static void icon_bump(rast_surface_t *s, int x, int y)
{
    const int S = RAST_SUB;
    int32_t out[] = { x * S, (y - 12) * S, (x + 12) * S, (y + 9) * S, (x - 12) * S, (y + 9) * S };
    int32_t in[] = { x * S, (y - 8) * S, (x + 8) * S, (y + 6) * S, (x - 8) * S, (y + 6) * S };
    int n3 = 3;
    rast_fill(s, out, &n3, 1, rgb565(0x202124));
    rast_fill(s, in, &n3, 1, rgb565(0xf5b83d));
    rast_disc(s, x * S, (y + 5) * S, 4 * S, rgb565(0x202124));     /* a lombada */
    quad(s, x - 7, y + 5, x + 7, y + 7, rgb565(0x202124));
}

static void icon_rail(rast_surface_t *s, int x, int y)
{
    const int S = RAST_SUB;
    int32_t a[] = { (x - 9) * S, (y - 9) * S, (x + 9) * S, (y + 9) * S }, b[] = { (x + 9) * S, (y - 9) * S, (x - 9) * S, (y + 9) * S };
    rast_line(s, a, 2, 7 * S, rgb565(0x202124), 0);
    rast_line(s, b, 2, 7 * S, rgb565(0x202124), 0);
    rast_line(s, a, 2, 4 * S, rgb565(0xffffff), 0);
    rast_line(s, b, 2, 4 * S, rgb565(0xffffff), 0);
}

/* os avisos do usuário: o mesmo triângulo, em azul (não são dados do mapa) */
static void draw_user_alerts(const bgr_view_t *v, rast_surface_t *s, const bga_entry_t *u, int n)
{
    if((v->zoom256 >> 8) < 14) return;
    const int S = RAST_SUB;
    for(int i = 0; i < n; i++) {
        int x, y;
        bgr_world_to_screen(v, s, u[i].x, u[i].y, &x, &y);
        if(x < -14 || y < -14 || x > s->w + 14 || y > s->h + 14) continue;
        int32_t out[] = { x * S, (y - 12) * S, (x + 12) * S, (y + 9) * S, (x - 12) * S, (y + 9) * S };
        int32_t in[] = { x * S, (y - 8) * S, (x + 8) * S, (y + 6) * S, (x - 8) * S, (y + 6) * S };
        int n3 = 3;
        rast_fill(s, out, &n3, 1, rgb565(0xffffff));
        rast_fill(s, in, &n3, 1, rgb565(0x1a73e8));
        rast_disc(s, x * S, (y + 5) * S, 4 * S, rgb565(0xffffff));
    }
}

static void draw_alerts(const bgr_view_t *v, rast_surface_t *s, const bga_pack_t *p)
{
    int z = v->zoom256 >> 8;
    if(!p || z < 13) return;
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    const int cx[4] = { 0, s->w, 0, s->w }, cy[4] = { 0, 0, s->h, s->h };
    for(int i = 0; i < 4; i++) {                       /* caixa do quadro (girado) no mundo */
        uint32_t wx, wy;
        bgr_screen_to_world(v, s, cx[i], cy[i], &wx, &wy);
        if(wx < x0) x0 = wx;
        if(wx > x1) x1 = wx;
        if(wy < y0) y0 = wy;
        if(wy > y1) y1 = wy;
    }
    static const bga_entry_t *found[2048];
    int n = bga_query(p, x0, y0, x1, y1, found, 2048), drawn = 0;
    /* radares primeiro (os mais importantes), depois passagens de nível, lombadas, semáforos */
    static const int order[] = { BGA_CAMERA, BGA_RAIL, BGA_BUMP, BGA_SIGNAL };
    static const int minz[BGA_KINDS] = { 99, 15, 13, 15, 14 };
    for(int o = 0; o < 4 && drawn < MAXAL; o++) {
        int k = order[o];
        if(z < minz[k]) continue;
        for(int i = 0; i < n && drawn < MAXAL; i++) {
            if(found[i]->kind != k) continue;
            int sx, sy;
            bgr_world_to_screen(v, s, found[i]->x, found[i]->y, &sx, &sy);
            if(sx < -14 || sy < -14 || sx > s->w + 14 || sy > s->h + 14) continue;
            if(k == BGA_SIGNAL) icon_signal(s, sx, sy);
            else if(k == BGA_CAMERA) icon_camera(s, sx, sy);
            else if(k == BGA_BUMP) icon_bump(s, sx, sy);
            else icon_rail(s, sx, sy);
            drawn++;
        }
    }
}

/* ------------------------------------------------ thread de renderização */
static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, stop, has_req, done, busy;
    bgr_view_t req, done_view;
    uint16_t *buf[2];
    int front;                 /* a interface mostra buf[front] */
    lbl_t lbl[2][MAXLBL];
    int nlbl[2];
    unsigned ms, us_fetch, us_draw;
    bgm_pack_t *pack;
    uint32_t *route;           /* rota a desenhar (pontos x,y no mundo), dono: interface */
    int route_n;
    int route_k;               /* já percorrido até route[route_k] + (route_px, route_py); -1: nada */
    uint32_t route_px, route_py;
    bga_pack_t *alerts;        /* semáforos, radares, lombadas (só leitura, mapeado) */
    uint16_t *src[2];          /* vista inclinada: quadro de cima da área maior (fonte), dono: motor */
    uint32_t *alt[BGRT_MAXALT];   /* rotas alternativas a desenhar (azul-claro, por baixo), dono: interface */
    int alt_n[BGRT_MAXALT];
    int req_persp, done_persp;
} R = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void *worker(void *arg)
{
    (void)arg;
    /* um núcleo só: o quadro do mapa (50-350 ms) roda com prioridade menor que a
     * interface, que assim continua desenhando o arrasto e atendendo o toque
     * (docs/performance/12-scheduler-tasks.md); BIGFOSTON_MAP_NICE muda (0 = igual) */
    {
        const char *e = getenv("BIGFOSTON_MAP_NICE");
        int nv = e ? atoi(e) : 10;
        if(nv) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nv);
    }
    static bgr_label_t tmp[MAXLBL];
    uint32_t *rcopy = NULL;
    int rcap = 0;
    pthread_mutex_lock(&R.mu);
    for(;;) {
        while(!R.stop && (!R.has_req || R.done)) pthread_cond_wait(&R.cv, &R.mu);
        if(R.stop) break;
        bgr_view_t v = R.req;
        int persp = R.req_persp;
        R.has_req = 0;
        R.busy = 1;
        int back = !R.front;
        int rn = R.route_n;                         /* cópia da rota: a interface pode trocá-la */
        if(rn > rcap) {
            uint32_t *nb = realloc(rcopy, (size_t)rn * 2 * sizeof *nb);
            if(nb) { rcopy = nb; rcap = rn; }
            else rn = 0;
        }
        if(rn) memcpy(rcopy, R.route, (size_t)rn * 2 * sizeof *rcopy);
        int rk = R.route_k < rn - 1 ? R.route_k : -1;
        static bga_entry_t ucopy[MAXUPT];
        int nu = nupt;                                /* os avisos do usuário (mudam na interface) */
        memcpy(ucopy, upts, (size_t)nu * sizeof ucopy[0]);
        static uint32_t *acopy[BGRT_MAXALT];
        static int acap[BGRT_MAXALT];
        int an[BGRT_MAXALT];
        for(int a = 0; a < BGRT_MAXALT; a++) {
            an[a] = R.alt_n[a];
            if(an[a] > acap[a]) {
                uint32_t *nb = realloc(acopy[a], (size_t)an[a] * 2 * sizeof *nb);
                if(nb) { acopy[a] = nb; acap[a] = an[a]; } else an[a] = 0;
            }
            if(an[a]) memcpy(acopy[a], R.alt[a], (size_t)an[a] * 2 * sizeof *acopy[a]);
        }
        uint32_t rpx = R.route_px, rpy = R.route_py;
        pthread_mutex_unlock(&R.mu);

        /* vista inclinada: o motor desenha a fonte (maior); a interface projeta na tela */
        rast_surface_t s = persp ? (rast_surface_t){ R.src[back], PJ.sw, PJ.sh, PJ.sw }
                                 : (rast_surface_t){ R.buf[back], MAP_W, MAP_H, MAP_W };
        bgr_stats_t st;
        int n = bgr_render(R.pack, &v, &s, tmp, MAXLBL, &st);
        for(int a = 0; a < BGRT_MAXALT; a++)        /* alternativas: azul-claro, mais finas, por baixo */
            if(an[a] >= 2) {
                int z = v.zoom256 >> 8, w = z >= 16 ? 6 : z >= 14 ? 5 : 3;
                bgr_draw_polyline(&v, &s, acopy[a], an[a], w * RAST_SUB, rgb565(v.night ? 0x6f8fc4 : 0x8ab4f8),
                                  (w + 2) * RAST_SUB, rgb565(v.night ? 0x2a3a57 : 0x5b85c9));
            }
        if(rn >= 2) {
            int z = v.zoom256 >> 8, w = z >= 16 ? 8 : z >= 14 ? 6 : 4;
            uint16_t blue = rgb565(v.night ? 0x4c8bf5 : 0x1a73e8), edge = rgb565(v.night ? 0x0d2b5e : 0x0b3d91);
            if(rk >= 0) {
                /* trecho já percorrido mais claro; o que falta em azul forte */
                uint32_t kx = rcopy[2 * rk + 2], ky = rcopy[2 * rk + 3];
                rcopy[2 * rk + 2] = rpx; rcopy[2 * rk + 3] = rpy;
                bgr_draw_polyline(&v, &s, rcopy, rk + 2, w * RAST_SUB, rgb565(v.night ? 0x5b6f8f : 0x9fbbe6),
                                  (w + 3) * RAST_SUB, rgb565(v.night ? 0x2a3447 : 0x7a93bd));
                rcopy[2 * rk + 2] = kx; rcopy[2 * rk + 3] = ky;
                kx = rcopy[2 * rk]; ky = rcopy[2 * rk + 1];
                rcopy[2 * rk] = rpx; rcopy[2 * rk + 1] = rpy;
                bgr_draw_polyline(&v, &s, rcopy + 2 * rk, rn - rk, w * RAST_SUB, blue, (w + 3) * RAST_SUB, edge);
                rcopy[2 * rk] = kx; rcopy[2 * rk + 1] = ky;
            } else {
                bgr_draw_polyline(&v, &s, rcopy, rn, w * RAST_SUB, blue, (w + 3) * RAST_SUB, edge);
            }
        }
        draw_alerts(&v, &s, R.alerts);              /* por cima da rota: o radar na via aparece */
        draw_user_alerts(&v, &s, ucopy, nu);
        for(int i = 0; i < n; i++) {
            lbl_t *l = &R.lbl[back][i];
            l->x = tmp[i].x; l->y = tmp[i].y; l->angle = tmp[i].angle;
            l->kind = tmp[i].kind; l->prio = tmp[i].prio;
            snprintf(l->text, sizeof l->text, "%s", tmp[i].text ? tmp[i].text : "");
        }

        pthread_mutex_lock(&R.mu);
        R.nlbl[back] = n;
        R.done_view = v;
        R.done_persp = persp;
        R.ms = (st.us_fetch + st.us_draw) / 1000;
        R.us_fetch = st.us_fetch;
        R.us_draw = st.us_draw;
        R.done = 1;
        R.busy = 0;
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
static int off_x, off_y;
static int swap_serial;        /* muda a cada quadro novo do motor na tela */       /* deslocamento da imagem enquanto o quadro novo não chega */
static lv_point_t drag_start;
static bgr_view_t drag_view;
static bool dragging;
static unsigned gps_ms;
/* movimento entre as posições do GPS (biggps/bgmotion.h): a câmera e o marcador andam a
 * 20 Hz deslizando o quadro pronto; o motor só desenha um quadro novo quando a imagem
 * deslocada já não basta (docs/big-gps-2/04-arquitetura-renderizacao.md) */
static bgmo_t MO;
static lv_timer_t *anim;
static uint64_t anim_req_us;
static int zoom_goal = -1;     /* zoom pela velocidade na navegação: muda aos poucos; -1 = nenhum */
static int nav_zoom = -1;      /* o último zoom de navegação calculado (volta da visão geral) */
#define ZOOM_STEP 26           /* ~0,1 nível por quadro do motor */
#define ANIM_MS 50

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
    bga_hit_t *al;             /* alertas na rota (radar, lombada, passagem de nível) */
    int nal, al_next, al_said;
    int nstops, pick_stop;     /* paradas no caminho (em ordem); esperando o toque longo da próxima */
    int toll_arcs, toll_avoided;            /* da rota mostrada */
    int nalt;                               /* rotas alternativas de verdade (bgrt_poll_alts) */
    bgn_route_t alt[BGRT_MAXALT];
    int alt_toll[BGRT_MAXALT];
    uint32_t stx[BGRT_MAXSTOPS], sty[BGRT_MAXSTOPS];
} N;
static lv_obj_t *stop_mk[BGRT_MAXSTOPS];   /* círculos numerados das paradas */
static lv_obj_t *pick_chip;                /* "toque e segure onde fica a parada" */
static int pin_drag_done;      /* o painel reabre depois de mover o pino: mantém as paradas */

/* ------------------------------------------------------------ guia de faixas
 * Do arquivo .bgl em rotas (turn:lanes do OSM): as faixas do cruzamento da próxima manobra, as
 * que servem em azul. Só aparece quando os dados dizem quais servem (bgl_fit != 0). */
#define LANE_W 44
#define LANE_H 48
static bgl_pack_t *lanes_pack;
static lv_obj_t *lane_img;
static lv_image_dsc_t lane_dsc;
static uint16_t lane_px[BGL_MAXLANES * LANE_W * LANE_H];
static int lane_for = -1;      /* manobra cujo desenho está pronto */
static const bgl_entry_t *lane_e;
static unsigned lane_fit;

static void lanes_draw(int type)
{
    int n = lane_e->n, w = n * LANE_W;
    for(int i = 0; i < n; i++) {
        bool ok = lane_fit & (1u << i);
        rast_surface_t s = { lane_px + i * LANE_W, LANE_W, LANE_H, w };
        uint16_t bg = rgb565(ok ? 0x1a73e8 : 0x2b2f36);
        bgn_icon(&s, bgl_arrow(lane_e, i, type, lane_fit), 0, rgb565(ok ? 0xffffff : 0x9aa0a6), bg, bg);
        for(int y = 0; y < LANE_H; y++) lane_px[y * w + i * LANE_W] = rgb565(0x15181d);   /* divisória */
    }
    memset(&lane_dsc, 0, sizeof lane_dsc);
    lane_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lane_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    lane_dsc.header.w = (uint32_t)w;
    lane_dsc.header.h = LANE_H;
    lane_dsc.header.stride = (uint32_t)w * 2;
    lane_dsc.data = (const uint8_t *)lane_px;
    lane_dsc.data_size = (uint32_t)(w * LANE_H * 2);
    lv_image_cache_drop(&lane_dsc);
    lv_image_set_src(lane_img, &lane_dsc);
}

static void lanes_update(void)
{
    if(!lane_img) return;
    const bgn_state_t *st = &N.st;
    if(!lanes_pack || !N.navigating || !st->valid || st->next < 0 || st->next >= N.route.nman) {
        lv_obj_add_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const bgn_man_t *m = &N.route.man[st->next];
    if(lane_for != st->next) {                       /* manobra nova: procura as faixas uma vez */
        lane_for = st->next;
        lane_e = NULL;
        lane_fit = 0;
        uint32_t ax, ay;
        double br;
        bgn_point_at(&N.route, m->at_dm > 300 ? m->at_dm - 300 : 0, &ax, &ay, &br);   /* rumo de chegada */
        lane_e = bgl_find(lanes_pack, m->x, m->y, (int)lround(br));
        lane_fit = bgl_fit(lane_e, m->type);
        if(lane_e && lane_fit) {
            lanes_draw(m->type);
            fprintf(stderr, "biggps: faixas na manobra %d: %d faixas, servem %#x\n", st->next, lane_e->n, lane_fit);
        }
    }
    double v = pos_speed / 3.6;
    uint32_t show = (uint32_t)fmax(400, v * 15);
    if(lane_e && lane_fit && st->next_dm / 10 <= show) lv_obj_remove_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *al_chip, *al_lbl;     /* aviso de radar/lombada sob a faixa da manobra */
static uint32_t last_say_ms;
static lv_obj_t *col_time, *col_dist, *col_eta, *col_speed;   /* coluna da navegação */
static lv_obj_t *sheet, *banner, *ban_icon, *ban_dist, *ban_text, *ban_then, *botbar, *bot_lbl, *sim_chip, *dmark, *search_btn;
static void set_follow(bool on);
static lv_image_dsc_t icon_dsc;
static uint16_t icon_px[80 * 80];
static int icon_type = -1, icon_exit = -1;
static void on_fix(uint32_t x, uint32_t y, double speed, double course, bool live);
static void anim_step(bool fix);
static bool persp_wanted(void)
{
    static int env = -1;                               /* BIGGPS_PERSP=0 desliga (comparação A/B) */
    if(env < 0) { const char *e = getenv("BIGGPS_PERSP"); env = !(e && *e == '0'); }
    /* "Econômico" (Ajustes > Desempenho) também fica no 2D: a inclinada custa ~2x por quadro */
    return env && pj_ok && !persp_auto_off && settings()->map_persp && follow && heading_up
        && settings()->map_detail != BF_DETAIL_ECONOMIC;
}

static void anchor(int *ax, int *ay)
{
    if(persp_wanted()) { *ax = PJ.scx; *ay = PJ.sy0; return; }   /* o carro na fonte */
    *ax = MAP_W / 2;
    *ay = follow && heading_up ? MAP_H * 2 / 3 : MAP_H / 2;   /* à frente aparece mais */
}

/* nível de detalhe do motor (Ajustes > Desempenho). Automático: econômico ao navegar
 * ou quando os quadros passam de 300 ms em média (volta ao equilibrado abaixo de
 * 150 ms), equilibrado no resto (docs/performance/09-gps-map-engine.md) */
static unsigned frame_ema_ms;      /* média móvel do tempo dos quadros do motor */
static bool auto_slow;
static int detail_now(void)
{
    static int forced = -2;                              /* teste A/B: BIGFOSTON_MAP_DETAIL=0..3 */
    if(forced == -2) { const char *e = getenv("BIGFOSTON_MAP_DETAIL"); forced = e ? atoi(e) : -1; }
    switch(forced >= 0 ? forced : settings()->map_detail) {
        case BF_DETAIL_ECONOMIC: return BGR_DETAIL_ECONOMIC;
        case BF_DETAIL_BALANCED: return BGR_DETAIL_BALANCED;
        case BF_DETAIL_DETAILED: return BGR_DETAIL_DETAILED;
    }
    if(frame_ema_ms > 300) auto_slow = true;
    else if(frame_ema_ms < 150) auto_slow = false;
    return N.navigating || auto_slow ? BGR_DETAIL_ECONOMIC : BGR_DETAIL_BALANCED;
}

static void request(void)
{
    if(!R.started) return;
    cur.detail = detail_now();
    anchor(&cur.ox, &cur.oy);
    cur.night = theme_dark_now();
    pthread_mutex_lock(&R.mu);
    R.req = cur;
    R.req_persp = persp_wanted();
    R.has_req = 1;
    pthread_cond_signal(&R.cv);
    pthread_mutex_unlock(&R.mu);
}

static void update_marker(void);
static void update_offset(void);

static void rot_label(void)
{
    bool d3 = settings()->map_persp && pj_ok && !persp_auto_off && settings()->map_detail != BF_DETAIL_ECONOMIC;
    if(rot_lbl) lv_label_set_text(rot_lbl, !heading_up ? "N" : d3 ? "3D" : LV_SYMBOL_UP);
}

static int render_busy(void)
{
    pthread_mutex_lock(&R.mu);
    int b = R.has_req || R.busy || R.done;
    pthread_mutex_unlock(&R.mu);
    return b;
}

/* até onde a rota já foi percorrida (o quadro seguinte a desenha mais clara) */
static void route_progress(void)
{
    int k = -1;
    uint32_t px = 0, py = 0;
    if(N.navigating && MO.fix_on_route && MO.xy && MO.npts >= 2) {
        int lo = 0, hi = MO.npts - 1;
        while(hi - lo > 1) { int mid = (lo + hi) / 2; if(MO.cum_dm[mid] <= MO.s) lo = mid; else hi = mid; }
        k = lo;
        px = (uint32_t)llround(MO.x);
        py = (uint32_t)llround(MO.y);
    }
    pthread_mutex_lock(&R.mu);
    R.route_k = k;
    R.route_px = px;
    R.route_py = py;
    pthread_mutex_unlock(&R.mu);
}

static int cam_rot(void)
{
    return heading_up ? -(int)lround(MO.has_disp ? MO.cam : pos_course) : 0;
}

/* um passo da animação: posição e câmera do instante atual; pede quadro novo ao motor só
 * quando a imagem deslocada já não basta (16 px de deslocamento, 2° de giro, outro zoom, ou
 * 1 s com o quadro fora do centro) e o motor está livre */
static void update_scale(void);

static void anim_step(bool fix)
{
    if(!have_pos || !MO.has_disp) return;
    uint64_t now = perf_now_us();
    int moved = bgmo_step(&MO, now);
    if(!moved && !fix && (zoom_goal < 0 || zoom_goal == cur.zoom256 || !follow)) return;
    if(!follow) { route_progress(); update_marker(); return; }
    route_progress();
    cur.cx = (uint32_t)llround(MO.x);
    cur.cy = (uint32_t)llround(MO.y);
    cur.rot = cam_rot();
    if(zoom_goal > 0 && zoom_goal != cur.zoom256 && !render_busy()) {
        /* ~0,1 nível por quadro; diferenças grandes (volta da visão geral) em ~6 quadros */
        int d = zoom_goal - cur.zoom256, st = abs(d) / 6 > ZOOM_STEP ? abs(d) / 6 : ZOOM_STEP;
        cur.zoom256 += d > st ? st : d < -st ? -st : d;
        update_scale();
    }
    if(!have_shown) { if(!render_busy()) { request(); anim_req_us = now; } return; }
    update_offset();
    int dr = abs(shown.rot - cur.rot) % 360;
    if(dr > 180) dr = 360 - dr;
    int far = abs(off_x) + abs(off_y);
    bool need = shown.zoom256 != cur.zoom256 || dr >= 2 || far >= 16 || (far && now - anim_req_us > 1000000);
    if(need && !render_busy()) { request(); anim_req_us = now; }
}

static void anim_cb(lv_timer_t *t)
{
    (void)t;
    if(!dragging) anim_step(false);
}

/* a superfície do quadro na tela (2D) ou da fonte (inclinada) */
static rast_surface_t shown_surf(void)
{
    return shown_persp ? (rast_surface_t){ NULL, PJ.sw, PJ.sh, PJ.sw } : (rast_surface_t){ NULL, MAP_W, MAP_H, MAP_W };
}

/* um ponto do mundo na tela, com o deslocamento atual (e a projeção, na vista inclinada);
 * 0 quando cai além do fundo da vista inclinada */
static int disp_point(uint32_t wx, uint32_t wy, int *x, int *y)
{
    rast_surface_t s = shown_surf();
    bgr_world_to_screen(&shown, &s, wx, wy, x, y);
    *x += off_x;
    *y += off_y;
    if(!shown_persp) return 1;
    int px, py;
    if(!bgp_project(&PJ, *x, *y, &px, &py)) return 0;
    *x = px;
    *y = py;
    return 1;
}

/* vista inclinada: projeta a fonte (com o deslocamento atual) no quadro da tela */
static void persp_draw(void)
{
    uint16_t *src = R.src[R.front];
    bgp_remap(&PJ, src, R.buf[R.front], MAP_W, off_x, off_y, src[0]);
}

/* desloca a imagem atual para mostrar 'cur' enquanto o quadro novo é desenhado */
static void update_offset(void)
{
    if(!have_shown) return;
    off_x = off_y = 0;
    int drot = abs(shown.rot - cur.rot) % 360;
    if(drot > 180) drot = 360 - drot;
    /* giro pequeno (a câmera virando aos poucos): desloca pelo centro, o quadro novo acerta o resto */
    if(shown.zoom256 == cur.zoom256 && drot <= 12 && shown.ox == cur.ox && shown.oy == cur.oy) {
        rast_surface_t s = shown_surf();
        int sx, sy;
        bgr_world_to_screen(&shown, &s, cur.cx, cur.cy, &sx, &sy);
        off_x = shown.ox - sx;
        off_y = shown.oy - sy;
    }
    /* nada mudou em pixels (o carro anda menos de 1 px por passo da animação): não
     * redesenha a tela inteira à toa — só o marcador, se ele girou */
    static int last_x = INT32_MIN, last_y, last_img;
    if(off_x == last_x && off_y == last_y && last_img == swap_serial) { update_marker(); return; }
    last_x = off_x;
    last_y = off_y;
    last_img = swap_serial;
    if(shown_persp) {                                /* a imagem fica parada; a fonte é que anda */
        persp_draw();
        lv_obj_set_pos(img, 0, 0);
        lv_image_cache_drop(&dsc[R.front]);
    } else lv_obj_set_pos(img, off_x, off_y);
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
    for(unsigned i = 0; !shown_persp && i < sizeof reserved / sizeof reserved[0]; i++) {
        placed[nplaced].a = reserved[i];
        placed[nplaced].font = NULL;
        placed[nplaced].text[0] = 0;
        placed[nplaced].angle = 0;
        nplaced++;
    }
    if(have_pos) {                                   /* nem sobre o marcador da posição */
        uint32_t wx, wy;
        int sx, sy;
        rast_surface_t s = shown_surf();
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
        if(a.x1 < 2 || a.y1 < 2 || a.x2 > lab_w - 2 || a.y2 > lab_h - 2 || overlaps(&a)) continue;
        placed[nplaced].a = a;
        placed[nplaced].font = f;
        placed[nplaced].angle = ang;
        placed[nplaced].cx = l[i].x;
        placed[nplaced].cy = l[i].y;
        snprintf(placed[nplaced].text, sizeof placed[nplaced].text, "%s", l[i].text);
        nplaced++;
        if(ang) nrot++;                                      /* só conta: limite de nomes girados */
    }
}

static void compose_labels(uint16_t *buf)
{
    for(int i = 0; i < nplaced; i++) {
        const placed_t *p = &placed[i];
        if(!p->font) continue;
        int w, h;
        const uint32_t *px = label_bitmap(p->text, p->font, shown.night, &w, &h);
        if(px) compose_bitmap(buf, px, w, h, p->cx, p->cy, p->angle);
    }
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

static void mk_build(void)
{
    /* pino e lupa em inteiros (bgvehicle): o aparelho não tem FPU — a versão em float custava
     * 433 ms a cada abertura do mapa. Os marcadores da posição saem de veh_sync. */
    bgveh_draw(mk_pin, BGVEH_PIN, 0, 0);
    bgveh_draw_n(mg_px, MG, BGVEH_MAGNIFIER, 0, 0);
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

/* ------------------------------------------------- marcador da posição (perfil)
 * mk_dsc[0] = o marcador ativo (seta ou veículo), [1] = parado (ponto, só no modelo seta),
 * [2] = posição incerta (cinza). Refeitos só quando o perfil ou a escolha mudam. */
static int veh_built_prof = -1, veh_gen, veh_built_gen = -1, veh_model = BGVEH_ARROW, veh_size = 1;

static int veh_model_of(int prof)
{
    const int *m;
    int n = bgveh_models(prof, &m), sel = settings()->map_veh_model[prof];
    for(int i = 0; i < n; i++) if(m[i] == sel) return sel;
    return m[0];
}

static void veh_sync(void)
{
    int prof = N.profile >= 0 && N.profile < BGR_PROFILES ? N.profile : BGR_CAR;
    if(prof == veh_built_prof && veh_gen == veh_built_gen) return;
    veh_built_prof = prof;
    veh_built_gen = veh_gen;
    veh_model = veh_model_of(prof);
    veh_size = settings()->map_veh_size[prof];
    uint32_t c = bgveh_color[settings()->map_veh_color[prof] & 7];
    int still = veh_model == BGVEH_ARROW ? BGVEH_DOT : veh_model;
    bgveh_draw(mk_arrow, veh_model, c, 0);
    bgveh_draw(mk_dot, still, c, 0);
    bgveh_draw(mk_stale, still, 0, 1);
    for(int i = 0; i < 3; i++) lv_image_cache_drop(&mk_dsc[i]);
    if(mark) lv_obj_invalidate(mark);
}

static void update_dest_marker(void)
{
    for(int i = 0; i < BGRT_MAXSTOPS; i++) {
        if(!stop_mk[i]) continue;
        int sx, sy;
        if(i >= N.nstops || !N.has_dest || !have_shown || !disp_point(N.stx[i], N.sty[i], &sx, &sy)
           || sx < -20 || sy < -20 || sx > MAP_W + 20 || sy > MAP_H + 20) { lv_obj_add_flag(stop_mk[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_pos(stop_mk[i], sx - 15, sy - 15);
        lv_obj_remove_flag(stop_mk[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(!dmark) return;
    if(!N.has_dest || !have_shown) { lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN); return; }
    int sx, sy;
    if(!disp_point(N.dx, N.dy, &sx, &sy)) { lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN); return; }
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
    if(MO.has_disp) { wx = (uint32_t)llround(MO.x); wy = (uint32_t)llround(MO.y); }   /* posição animada */
    else bgr_lonlat_to_world(pos_lon, pos_lat, &wx, &wy);
    if(!disp_point(wx, wy, &sx, &sy)) { lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN); return; }
    if(sx < -MK || sy < -MK || sx > MAP_W + MK || sy > MAP_H + MK) { lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN); return; }
    veh_sync();
    bool moving = pos_live && !MO.stale && pos_speed >= 3.0, ok = pos_live && !MO.stale;
    double head = MO.has_disp && MO.head >= 0 ? MO.head : pos_course;
    /* seta: anda = seta no rumo, parado = ponto; veículo: sempre ele, no último rumo; incerto = cinza */
    bool turn = veh_model == BGVEH_ARROW ? moving : veh_model != BGVEH_DOT && head >= 0;
    const lv_image_dsc_t *d = !ok ? &mk_dsc[2] : veh_model == BGVEH_ARROW && !moving ? &mk_dsc[1] : &mk_dsc[0];
    if(lv_image_get_src(mark) != d) lv_image_set_src(mark, d);
    int py = turn && veh_model == BGVEH_ARROW ? 30 : 24;
    lv_image_set_pivot(mark, 24, py);
    lv_image_set_scale(mark, veh_size == 0 ? 205 : veh_size == 2 ? 320 : LV_SCALE_NONE);
    lv_image_set_rotation(mark, turn ? (int32_t)lround(fmod(head + shown.rot + 720.0, 360.0) * 10) : 0);
    lv_obj_set_pos(mark, sx - 24, sy - py);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
}

static void layer_draw(lv_event_t *e)
{
    lv_layer_t *ly = lv_event_get_layer(e);
    if(!have_shown) return;
    bool night = shown.night;
    lv_color_t fg = night ? lv_color_hex(0xE8E8E8) : lv_color_hex(0x202020);
    lv_color_t halo = night ? lv_color_hex(0x101318) : lv_color_white();
    (void)ly; (void)fg; (void)halo;                 /* rótulos: compostos no quadro (compose_labels) */
}

static lv_color_t ov_bg;

static void ov_cover(lv_event_t *e)
{
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if(info->res != LV_COVER_RES_MASKED) info->res = LV_COVER_RES_COVER;
}

static void ov_draw(lv_event_t *e)
{
    lv_layer_t *ly = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = ov_bg;
    d.bg_opa = LV_OPA_COVER;
    bool full = !have_shown || lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN);
    int ox = full || shown_persp ? 0 : off_x, oy = full || shown_persp ? 0 : off_y;
    lv_area_t a;
    if(full || ox >= MAP_W || ox <= -MAP_W || oy >= MAP_H || oy <= -MAP_H) {
        a = (lv_area_t){ 0, 0, MAP_W - 1, MAP_H - 1 };
        lv_draw_rect(ly, &d, &a);
        return;
    }
    if(oy > 0) { a = (lv_area_t){ 0, 0, MAP_W - 1, oy - 1 }; lv_draw_rect(ly, &d, &a); }
    if(oy < 0) { a = (lv_area_t){ 0, MAP_H + oy, MAP_W - 1, MAP_H - 1 }; lv_draw_rect(ly, &d, &a); }
    int y1 = oy > 0 ? oy : 0, y2 = oy < 0 ? MAP_H + oy - 1 : MAP_H - 1;
    if(ox > 0) { a = (lv_area_t){ 0, y1, ox - 1, y2 }; lv_draw_rect(ly, &d, &a); }
    if(ox < 0) { a = (lv_area_t){ MAP_W + ox, y1, MAP_W - 1, y2 }; lv_draw_rect(ly, &d, &a); }
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
    /* o marcador anda pela geometria da rota (só N.route tem as distâncias) */
    if(n >= 2 && xy == N.route.xy) bgmo_set_route(&MO, N.route.xy, N.route.cum_dm, N.route.npts);
    else bgmo_set_route(&MO, NULL, NULL, 0);
    pthread_mutex_lock(&R.mu);
    free(R.route);
    R.route = copy;
    R.route_n = n;
    R.route_k = -1;
    pthread_mutex_unlock(&R.mu);
}

/* as alternativas que o motor desenha (cópias, como a rota) */
static void set_alt_overlays(void)
{
    pthread_mutex_lock(&R.mu);
    for(int a = 0; a < BGRT_MAXALT; a++) {
        free(R.alt[a]);
        R.alt[a] = NULL;
        R.alt_n[a] = 0;
        if(a < N.nalt && N.alt[a].npts >= 2) {
            R.alt[a] = malloc((size_t)N.alt[a].npts * 2 * sizeof *R.alt[a]);
            if(R.alt[a]) {
                memcpy(R.alt[a], N.alt[a].xy, (size_t)N.alt[a].npts * 2 * sizeof *R.alt[a]);
                R.alt_n[a] = N.alt[a].npts;
            }
        }
    }
    pthread_mutex_unlock(&R.mu);
}

static void clear_alts(void)
{
    for(int a = 0; a < N.nalt; a++) bgn_free(&N.alt[a]);
    N.nalt = 0;
    set_alt_overlays();
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
    /* px = world * 2^(z - 24): a rota cabe na área livre — à direita do painel das rotas
     * (planejando) ou da coluna da navegação — e entre o topo e a base, com margem */
    double left = N.navigating ? 180 : 12 + ROUTE_PANEL_W + 12, right = MAP_W - 96;
    double aw = right - left - 24, ah = MAP_H - 48;
    double z = fmin(log2(aw / w), log2(ah / h)) + 24;
    if(z > 17) z = 17;
    if(z < 5) z = 5;
    cur.zoom256 = (int)(z * 256);
    double k = pow(2.0, z - 24), shift = ((left + right) / 2 - MAP_W / 2) / k;   /* mundo por px */
    cur.cx = x0 + (x1 - x0) / 2 - (uint32_t)(int32_t)lround(shift);
    cur.cy = y0 + (y1 - y0) / 2;
    cur.rot = 0;
    follow = false;
    update_scale();
    request();
}

/* voz: eSpeak NG (core/voice), se ligada nos ajustes e instalada no cartão */
/* text: o que se lê no registro; parts: os trechos da voz Faber separados por '|' */
static void say(const char *text, const char *parts)
{
    fprintf(stderr, "biggps: voz: %s\n", text);
    last_say_ms = (uint32_t)(perf_now_us() / 1000);
    if(settings()->nav_voice) voice_say(parts);
}

/* "Vire à direita|na Rua X" da manobra */
static void man_parts(const bgn_man_t *m, char *out, int n)
{
    char v[96], r[96];
    bgn_voice_parts(m, v, sizeof v, r, sizeof r);
    snprintf(out, (size_t)n, r[0] ? "%s|%s" : "%s", v, r);
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
        char p1[200], p2[200], parts[420];
        man_parts(m, p1, sizeof p1);
        if(st->next + 1 < N.route.nman && N.route.man[st->next + 1].at_dm - m->at_dm < 1500 && m->type != BGN_ARRIVE
           && m->type != BGN_WAYPOINT) {
            char t2[160];
            bgn_text(&N.route.man[st->next + 1], t2, sizeof t2);
            lower_first(t2);
            snprintf(full, sizeof full, "%s, e depois %s", t, t2);
            man_parts(&N.route.man[st->next + 1], p2, sizeof p2);
            snprintf(parts, sizeof parts, "%s|e depois|%s", p1, p2);
            say(full, parts);
        } else say(t, p1);
    } else if(!N.said_far && dist <= far && dist > near + 50 && m->type != BGN_ARRIVE && m->type != BGN_WAYPOINT) {
        N.said_far = 1;
        lower_first(t);
        snprintf(full, sizeof full, "Em %s, %s", spoken_dist(st->next_dm, d, sizeof d), t);
        char p1[200], parts[260];
        man_parts(m, p1, sizeof p1);
        snprintf(parts, sizeof parts, "Em %s|%s", d, p1);
        say(full, parts);
    }
}

/* ------------------------------------------------------- alertas na rota
 * Radar, lombada e passagem de nível que estão na rota à frente: voz uma vez, com
 * antecedência pela velocidade, e um chip enquanto se aproxima. Semáforos só no mapa (seriam
 * avisos demais). O limite só é dito quando o próprio ponto do OSM o informa. */

/* ---------------------------------------------- avisos marcados pelo usuário
 * Lombadas que o mapa não tem, marcadas no próprio aparelho (biggps-avisos.txt: "lat lon tipo"
 * por linha). Aparecem em azul (os do OSM são amarelos) e o aviso diz "sua". */

static void upts_load(void)
{
    char path[512], line[128];
    data_path(path, sizeof path, "biggps-avisos.txt");
    FILE *f = fopen(path, "r");
    nupt = 0;
    while(f && nupt < MAXUPT && fgets(line, sizeof line, f)) {
        double lat, lon;
        int kind;
        if(sscanf(line, "%lf %lf %d", &lat, &lon, &kind) != 3 || kind <= 0 || kind >= BGA_KINDS) continue;
        uint32_t x, y;
        bgr_lonlat_to_world(lon, lat, &x, &y);
        upts[nupt++] = (bga_entry_t){ x, y, (uint8_t)kind, 0, BGA_DIR_ANY };
    }
    if(f) fclose(f);
}

static void upts_save(void)
{
    char path[512], tmp[520];
    data_path(path, sizeof path, "biggps-avisos.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    for(int i = 0; i < nupt; i++) {
        double lon, lat;
        bgr_world_to_lonlat(upts[i].x, upts[i].y, &lon, &lat);
        fprintf(f, "%.6f %.6f %d\n", lat, lon, upts[i].kind);
    }
    if(fclose(f) == 0) rename(tmp, path);
}

/* o aviso do usuário a até 30 m de (x, y), ou -1 */
static int upt_near(uint32_t x, uint32_t y)
{
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat), best = 30;
    int k = -1;
    for(int i = 0; i < nupt; i++) {
        double dx = (double)(int32_t)(upts[i].x - x), dy = (double)(int32_t)(upts[i].y - y), d = sqrt(dx * dx + dy * dy) * mpu;
        if(d < best) { best = d; k = i; }
    }
    return k;
}

static void route_alerts(void)
{
    free(N.al);
    N.al = NULL;
    N.nal = N.al_next = 0;
    N.al_said = -1;
    if(N.route.npts < 2) return;
    uint64_t t0 = perf_now_us();
    int n = 0;
    bga_hit_t *h = R.alerts ? bga_route(R.alerts, N.route.xy, N.route.cum_dm, N.route.npts, &n) : NULL;
    int w = 0;
    for(int i = 0; h && i < n; i++) if(h[i].kind != BGA_SIGNAL) h[w++] = h[i];
    w = bga_route_points(upts, nupt, N.route.xy, N.route.cum_dm, N.route.npts, &h, w);   /* os seus */
    N.al = h;
    N.nal = w;
    fprintf(stderr, "biggps: %d alertas na rota (de %d pontos com semáforos) em %u ms\n", w, n,
            (unsigned)((perf_now_us() - t0) / 1000));
}

static void alert_check(void)
{
    if(!N.nal || !N.st.valid) { if(al_chip) lv_obj_add_flag(al_chip, LV_OBJ_FLAG_HIDDEN); return; }
    uint32_t at = N.st.progress_dm;
    while(N.al_next < N.nal && N.al[N.al_next].at_dm + 50 < at) N.al_next++;   /* passou (5 m de folga) */
    if(N.al_next >= N.nal) { lv_obj_add_flag(al_chip, LV_OBJ_FLAG_HIDDEN); return; }
    const bga_hit_t *a = &N.al[N.al_next];
    double v = pos_speed / 3.6;
    if(v < 8) v = 8;
    uint32_t dist = a->at_dm > at ? (a->at_dm - at) / 10 : 0;
    uint32_t warn = a->kind == BGA_CAMERA ? (uint32_t)fmax(300, v * 15) : (uint32_t)fmax(120, v * 8);
    if(!settings()->map_alerts || dist > warn) { lv_obj_add_flag(al_chip, LV_OBJ_FLAG_HIDDEN); return; }
    char t[96];
    const char *name = tr(a->kind == BGA_CAMERA ? S_MAP_AL_CAMERA : a->kind == BGA_BUMP ? S_MAP_AL_BUMP : S_MAP_AL_RAIL);
    if(a->kind == BGA_CAMERA && a->maxspeed) snprintf(t, sizeof t, "%s · %u m · %u km/h", name, dist, a->maxspeed);
    else if(a->user) snprintf(t, sizeof t, "%s (%s) · %u m", name, tr(S_MAP_AL_YOURS), dist);   /* não é dado do mapa */
    else snprintf(t, sizeof t, "%s · %u m", name, dist);
    lv_label_set_text(al_lbl, t);
    lv_obj_remove_flag(al_chip, LV_OBJ_FLAG_HIDDEN);
    if(N.al_said == N.al_next) return;
    /* não fala por cima de uma manobra que acabou de ser dita */
    uint32_t now = (uint32_t)(perf_now_us() / 1000);
    if(now - last_say_ms < 2500) return;
    N.al_said = N.al_next;
    char d[48], full[160], parts[160];
    if(a->kind == BGA_CAMERA) {
        if(dist >= 150) snprintf(full, sizeof full, "Radar a %s", spoken_dist(dist * 10, d, sizeof d));
        else snprintf(full, sizeof full, "Radar à frente");
        snprintf(parts, sizeof parts, "%s", full);
        if(a->maxspeed) {
            size_t o = strlen(full), q = strlen(parts);
            snprintf(full + o, sizeof full - o, ", limite de %u quilômetros por hora", a->maxspeed);
            snprintf(parts + q, sizeof parts - q, "|limite de %u quilômetros por hora", a->maxspeed);
        }
    } else snprintf(full, sizeof full, "%s", a->kind == BGA_BUMP ? "Lombada à frente" : "Passagem de nível à frente");
    if(a->kind != BGA_CAMERA) snprintf(parts, sizeof parts, "%s", full);
    say(full, parts);
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
    if(!col_time) return;
    char d[32], tm[24];
    uint32_t rem_dm = N.navigating ? N.st.remain_dm : N.route.total_dm;
    uint32_t rem_ds = N.navigating ? N.st.remain_ds : N.route.total_ds;
    int min = (int)((rem_ds + 300) / 600);
    if(min >= 60) snprintf(tm, sizeof tm, "%d h %02d", min / 60, min % 60);
    else snprintf(tm, sizeof tm, "%d min", min);
    lv_label_set_text(col_time, tm);
    lv_label_set_text(col_dist, bgn_dist_text(rem_dm, d, sizeof d));
    time_t now = time(NULL);
    struct tm lt;
    char a[32] = "";
    if(now > 1700000000) {                                  /* só com relógio acertado */
        time_t eta = now + rem_ds / 10;
        if(localtime_r(&eta, &lt)) snprintf(a, sizeof a, tr(S_MAP_ARRIVAL), lt.tm_hour, lt.tm_min);
    }
    lv_label_set_text(col_eta, a);
    char v[24] = "";                                        /* a velocidade só com GPS de verdade */
    if(pos_live && !N.simulate ? pos_speed >= 1 : N.simulate) snprintf(v, sizeof v, "%d km/h", (int)lround(pos_speed));
    lv_label_set_text(col_speed, v);
}

static void close_sheet(void)
{
    if(sheet) { lv_obj_delete(sheet); sheet = NULL; }
}

static void end_navigation(void)
{
    N.navigating = N.simulate = N.rerouting = N.active = N.has_dest = N.waiting = 0;
    free(N.al);
    N.al = NULL;
    N.nal = 0;
    N.nstops = N.pick_stop = 0;
    clear_alts();
    lane_for = -1;
    if(lane_img) lv_obj_add_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
    if(al_chip) lv_obj_add_flag(al_chip, LV_OBJ_FLAG_HIDDEN);
    if(pick_chip) lv_obj_add_flag(pick_chip, LV_OBJ_FLAG_HIDDEN);
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
    rot_label();
    cur.rot = 0;
    zoom_goal = nav_zoom = -1;
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
    clear_alts();                                    /* navega pela rota escolhida */
    recent_add(dest_title[0] ? dest_title : N.dest_name[0] ? N.dest_name : tr(S_MAP_POINT), N.dx, N.dy);
    N.navigating = 1;
    N.simulate = simulate;
    N.sim_t = 0;
    N.said_next = -1;
    N.route.seg = 0;
    N.route.off_count = 0;
    N.prev_heading_up = heading_up;
    heading_up = true;
    rot_label();
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
    if(N.route.nman) {
        char p1[200];
        bgn_text(&N.route.man[0], t, sizeof t);
        man_parts(&N.route.man[0], p1, sizeof p1);
        say(t, p1);
    }
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
    /* recalculando no caminho: as paradas já alcançadas saem da lista */
    if(reroute && N.navigating) {
        int passed = 0;
        for(int i = 0; i < N.route.nman; i++)
            if(N.route.man[i].type == BGN_WAYPOINT && N.route.man[i].at_dm <= N.st.progress_dm) passed++;
        if(passed > N.nstops) passed = N.nstops;
        for(int i = passed; i < N.nstops; i++) { N.stx[i - passed] = N.stx[i]; N.sty[i - passed] = N.sty[i]; }
        N.nstops -= passed;
    }
    bgrt_req_t q = { .id = ++N.req_id, .profile = profile, .sx = sx, .sy = sy, .heading = heading,
                     .dx = N.dx, .dy = N.dy, .nstops = N.nstops, .alternatives = !reroute && !N.navigating,
                     .avoid_toll = (profile == BGR_CAR || profile == BGR_MOTO) && settings()->map_avoid_toll };
    for(int i = 0; i < N.nstops; i++) { q.stx[i] = N.stx[i]; q.sty[i] = N.sty[i]; }
    N.profile = profile;
    N.waiting = 1;
    if(!reroute) clear_alts();
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

/* "Lombada aqui" no ponto do destino marcado, ou tirar o aviso seu que está ali */
static void upt_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if(k >= 0 && k < nupt) upts[k] = upts[--nupt];
    else if(k < 0 && nupt < MAXUPT) upts[nupt++] = (bga_entry_t){ N.dx, N.dy, BGA_BUMP, 0, BGA_DIR_ANY };
    upts_save();
    N.has_dest = 0;                                   /* era só para marcar o aviso */
    close_sheet();
    update_marker();
    request();
}

static void alerts_sw_cb(lv_event_t *e)
{
    settings()->map_alerts = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

static void avoid_toll_cb(lv_event_t *e)
{
    settings()->map_avoid_toll = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

/* o texto ao lado também alterna (alvo maior para o dedo) */
static void avoid_toll_label_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_user_data(e);
    if(lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static void open_dest_sheet_named(uint32_t x, uint32_t y, const char *name)
{
    if(N.navigating) return;
    snprintf(dest_title, sizeof dest_title, "%s", name ? name : "");
    bgn_free(&N.route);
    set_route_overlay(NULL, 0);
    N.active = 0;
    if(!N.has_dest || pin_drag_done == 0) N.nstops = 0;   /* destino novo: sem paradas (mover o pino as mantém) */
    pin_drag_done = 0;
    N.pick_stop = 0;
    clear_alts();
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
    {
        double lon, lat;
        char line[160];
        bgr_world_to_lonlat(x, y, &lon, &lat);
        snprintf(line, sizeof line, "%.5f, %.5f · %s", lat, lon, tr(S_MAP_PIN_HINT));
        bf_label(sh, line, BF_FONT_S, pal()->text_dim);
    }
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
    /* evitar pedágios (carro e moto): só aparece com os dados de rotas sem pedágio */
    if(bgrt_have_toll_free(BGR_CAR) || bgrt_have_toll_free(BGR_MOTO)) {
        lv_obj_t *tr_ = sheet_row(sh);
        lv_obj_set_flex_align(tr_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *sw = lv_switch_create(tr_);
        lv_obj_set_size(sw, 64, 34);
        if(settings()->map_avoid_toll) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, avoid_toll_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_t *l = bf_label(tr_, tr(S_MAP_AVOID_TOLL), BF_FONT_M, pal()->text);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, avoid_toll_label_cb, LV_EVENT_CLICKED, sw);
    }
    {                                               /* aviso seu: marcar ou tirar aqui */
        lv_obj_t *ur = sheet_row(sh);
        int k = upt_near(x, y);
        lv_obj_t *ub = bf_button(ur, k >= 0 ? LV_SYMBOL_TRASH : LV_SYMBOL_WARNING, tr(k >= 0 ? S_MAP_UPT_DEL : S_MAP_UPT_ADD), 0);
        lv_obj_set_height(ub, 44);
        lv_obj_add_event_cb(ub, upt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    if(R.alerts) {                                 /* avisos de radar e lombada na rota */
        lv_obj_t *ar = sheet_row(sh);
        lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *sw = lv_switch_create(ar);
        lv_obj_set_size(sw, 64, 34);
        if(settings()->map_alerts) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, alerts_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_t *l = bf_label(ar, tr(S_MAP_AL_SWITCH), BF_FONT_M, pal()->text);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, avoid_toll_label_cb, LV_EVENT_CLICKED, sw);
    }
}

/* "+ Parada": o próximo toque longo no mapa marca a parada e recalcula */
static void add_stop_cb(lv_event_t *e)
{
    (void)e;
    if(N.nstops >= BGRT_MAXSTOPS) return;
    N.pick_stop = 1;
    close_sheet();
    if(pick_chip) lv_obj_remove_flag(pick_chip, LV_OBJ_FLAG_HIDDEN);
}

static void clear_stops_cb(lv_event_t *e)
{
    (void)e;
    N.nstops = 0;
    N.pick_stop = 0;
    if(pick_chip) lv_obj_add_flag(pick_chip, LV_OBJ_FLAG_HIDDEN);
    update_dest_marker();
    route_request(N.profile, false);
}

static void route_sheet(void);
static void route_sheet_async(void *ud) { (void)ud; if(ov && N.active && !N.navigating) route_sheet(); }

/* escolher uma alternativa: ela vira a rota, a de antes vira alternativa */
static void select_alt(int a)
{
    if(a < 0 || a >= N.nalt || N.navigating) return;
    bgn_route_t t = N.route;
    N.route = N.alt[a];
    N.alt[a] = t;
    int tt = N.toll_arcs;
    N.toll_arcs = N.alt_toll[a];
    N.alt_toll[a] = tt;
    set_route_overlay(N.route.xy, N.route.npts);
    set_alt_overlays();
    route_alerts();
    lv_async_call(route_sheet_async, NULL);          /* fora do evento do botão */
    request();
}

static void alt_cb(lv_event_t *e) { select_alt((int)(intptr_t)lv_event_get_user_data(e)); }

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
    route_alerts();
    lane_for = -1;                                   /* índices de manobra novos */
    if(N.rerouting) {
        N.rerouting = 0;
        N.said_next = -1;
        say(tr(S_MAP_REROUTE), "Recalculando a rota");
        return;
    }
    N.toll_arcs = r->toll_arcs;
    N.toll_avoided = r->toll_avoided;
    /* visão geral com resumo */
    fit_route();
    route_sheet();
}

/* resumo da rota: tempo, distância, pedágio, iniciar/simular, paradas e alternativas */
/* um cartão de rota no painel lateral: tempo grande, distância e chegada; o da rota escolhida
 * com borda azul; tocar num outro o escolhe */
static void route_card(lv_obj_t *parent, const bgn_route_t *r, int toll_arcs, int alt, bool chosen)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = lv_button_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, p->surface2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_border_width(c, chosen ? 3 : 0, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1a73e8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 2, 0);
    char t[48], d[24], line[96];
    int min = (int)((r->total_ds + 300) / 600);
    if(min >= 60) snprintf(t, sizeof t, "%d h %02d min", min / 60, min % 60);
    else snprintf(t, sizeof t, "%d min", min);
    bf_label(c, t, BF_FONT_L, chosen ? p->text : lv_color_hex(0x8ab4f8));
    int n = snprintf(line, sizeof line, "%s", bgn_dist_text(r->total_dm, d, sizeof d));
    time_t now = time(NULL);
    struct tm lt;
    if(now > 1700000000) {                             /* só com relógio acertado */
        time_t eta = now + r->total_ds / 10;
        if(localtime_r(&eta, &lt) && n > 0 && n < (int)sizeof line) {
            char a[32];
            snprintf(a, sizeof a, tr(S_MAP_ARRIVAL), lt.tm_hour, lt.tm_min);
            snprintf(line + n, sizeof line - (size_t)n, " · %s", a);
        }
    }
    bf_label(c, line, BF_FONT_S, p->text_dim);
    if(toll_arcs > 0) bf_label(c, tr(S_MAP_TOLL), BF_FONT_S, p->warning);
    if(alt >= 0) lv_obj_add_event_cb(c, alt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)alt);
    else lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
}

/* planejamento da rota (referência: o painel de rotas na lateral do modo automotivo): a lista
 * das rotas à esquerda, o mapa enquadrado à direita, Iniciar em verde */
static void route_sheet(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *sh = sheet_new();
    lv_obj_set_size(sh, ROUTE_PANEL_W, MAP_H - 88);
    lv_obj_align(sh, LV_ALIGN_TOP_LEFT, 12, 76);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sh, LV_DIR_VER);
    lv_obj_t *head = sheet_row(sh);                  /* destino e fechar */
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = bf_label(head, dest_title[0] ? dest_title : N.dest_name[0] ? N.dest_name : tr(S_MAP_DEST), BF_FONT_M, p->text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *x = bf_button(head, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_size(x, 44, 40);
    lv_obj_add_event_cb(x, cancel_cb, LV_EVENT_CLICKED, NULL);
    route_card(sh, &N.route, N.toll_arcs, -1, true);
    for(int a = 0; a < N.nalt; a++) route_card(sh, &N.alt[a], N.alt_toll[a], a, false);
    /* pedágio: o que se sabe pelos dados (toll=yes e cabines do OSM) */
    bool want_free = (N.profile == BGR_CAR || N.profile == BGR_MOTO) && settings()->map_avoid_toll;
    const char *tollmsg = NULL;
    if(want_free && !N.toll_avoided) tollmsg = tr(S_MAP_TOLL_NOFILE);
    else if(N.toll_arcs > 0 && N.toll_avoided) tollmsg = tr(S_MAP_TOLL_NOALT);
    if(tollmsg) {
        lv_obj_t *tl = bf_label(sh, tollmsg, BF_FONT_S, N.toll_avoided ? p->warning : p->text_dim);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tl, LV_PCT(100));
    }
    lv_obj_t *b = bf_button(sh, LV_SYMBOL_PLAY, tr(S_MAP_START), 1);
    lv_obj_set_size(b, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1e8e3e), 0);            /* iniciar: verde */
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_add_event_cb(b, start_cb, LV_EVENT_CLICKED, NULL);
    if(!pos_live) lv_obj_add_state(b, LV_STATE_DISABLED);       /* sem GPS: só simulação */
    lv_obj_t *row = sheet_row(sh);                   /* simular e paradas numa linha */
    b = bf_button(row, LV_SYMBOL_SHUFFLE, tr(S_MAP_SIM), 0);
    lv_obj_set_height(b, 44);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_event_cb(b, sim_cb, LV_EVENT_CLICKED, NULL);
    if(N.nstops < BGRT_MAXSTOPS) {
        b = bf_button(row, LV_SYMBOL_PLUS, tr(S_MAP_ADD_STOP), 0);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, add_stop_cb, LV_EVENT_CLICKED, NULL);
    }
    if(N.nstops) {
        b = bf_button(row, LV_SYMBOL_TRASH, NULL, 0);
        lv_obj_set_size(b, 52, 44);
        lv_obj_add_event_cb(b, clear_stops_cb, LV_EVENT_CLICKED, NULL);
    }
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
        alert_check();
        update_banner();
        lanes_update();
        update_botbar();
        if(N.st.arrived && !N.arrived_ms) N.arrived_ms = 1;
        /* zoom pela velocidade (mais longe na estrada) e mais perto ao chegar numa manobra,
         * mais ainda nas complexas (biggps/bgnav.c: bgn_zoom256) */
        int z = bgn_zoom256(&N.route, &N.st, speed, N.profile);
        nav_zoom = z;
        /* anim_step chega lá aos poucos; um zoom feito à mão vale até a situação mudar */
        static int last_z;
        if(follow && z != last_z) zoom_goal = z;
        last_z = z;
    }
    (void)cx; (void)cy;
    /* a câmera e o marcador seguem a posição animada (anim_cb); parado, o ruído do GPS
     * não mexe no mapa (bgmotion ignora menos de 6 m) */
    bgmo_fix(&MO, perf_now_us(), x, y, speed, speed >= 3 ? pos_course : -1,
             N.navigating && N.st.valid, N.st.progress_dm);
    anim_step(true);
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
static lv_timer_t *sv_timer, *sv_poll;
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
    if(sv_poll) { lv_timer_delete(sv_poll); sv_poll = NULL; }
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

static char sv_fixed[128];      /* a busca corrigida que achou os resultados ("" = como digitada) */

static void show_results(void)
{
    lv_obj_clean(sv_list);
    if(sv_fixed[0] && sv_nres) {
        char t[200];
        snprintf(t, sizeof t, tr(S_MAP_RESULTS_FOR), sv_fixed);
        bf_label(sv_list, t, BF_FONT_S, pal()->text_dim);
    }
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
    if(sv_poll) { lv_timer_delete(sv_poll); sv_poll = NULL; }   /* a categoria vence a busca em curso */
    sv_nres = six ? bgs_nearby(six, cat, x, y, 30000, sv_res, MAXRES) : 0;
    sv_fixed[0] = 0;
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

/* a busca roda numa thread própria: no índice nacional, com o cartão frio, uma consulta
 * pode levar mais de um segundo e o teclado não pode parar. A interface pede (seq) e um
 * timer curto recolhe o resultado do último pedido; pedidos antigos são descartados. */
static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, stop;
    unsigned seq, done;          /* último pedido / último respondido */
    char text[128];
    uint32_t x, y;
    bgs_result_t res[MAXRES];
    int nres;
    char fixed[128];             /* bgs_corrected do último resultado */
} SQ = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void *search_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&SQ.mu);
    for(;;) {
        while(!SQ.stop && SQ.done == SQ.seq) pthread_cond_wait(&SQ.cv, &SQ.mu);
        if(SQ.stop) break;
        unsigned seq = SQ.seq;
        char text[128];
        uint32_t x = SQ.x, y = SQ.y;
        memcpy(text, SQ.text, sizeof text);
        pthread_mutex_unlock(&SQ.mu);
        bgs_result_t res[MAXRES];
        int n = bgs_query(six, text, x, y, res, MAXRES);
        char fixed[128];
        snprintf(fixed, sizeof fixed, "%s", bgs_corrected(six));   /* erro de digitação corrigido */
        pthread_mutex_lock(&SQ.mu);
        if(seq == SQ.seq) { memcpy(SQ.res, res, sizeof res); SQ.nres = n; memcpy(SQ.fixed, fixed, sizeof fixed); }
        SQ.done = seq;
    }
    pthread_mutex_unlock(&SQ.mu);
    return NULL;
}

static void search_stop(void)
{
    if(sv_poll) { lv_timer_delete(sv_poll); sv_poll = NULL; }
    if(!SQ.started) return;
    pthread_mutex_lock(&SQ.mu);
    SQ.stop = 1;
    pthread_cond_signal(&SQ.cv);
    pthread_mutex_unlock(&SQ.mu);
    pthread_join(SQ.th, NULL);
    SQ.started = SQ.stop = 0;
    SQ.seq = SQ.done = 0;
}

static void sv_poll_cb(lv_timer_t *t)
{
    (void)t;
    pthread_mutex_lock(&SQ.mu);
    int ready = SQ.done == SQ.seq;
    if(ready) { memcpy(sv_res, SQ.res, sizeof sv_res); sv_nres = SQ.nres; memcpy(sv_fixed, SQ.fixed, sizeof sv_fixed); }
    pthread_mutex_unlock(&SQ.mu);
    if(!ready) return;
    lv_timer_delete(sv_poll);
    sv_poll = NULL;
    if(sv_list) show_results();
}

static void sv_query(lv_timer_t *t)
{
    (void)t;
    sv_timer = NULL;
    if(!sv_ta) return;
    const char *q = lv_textarea_get_text(sv_ta);
    char norm[128];
    if(!bgs_norm(q, norm, sizeof norm) || !six) {
        pthread_mutex_lock(&SQ.mu);
        SQ.done = SQ.seq;                           /* um pedido em curso fica sem efeito */
        pthread_mutex_unlock(&SQ.mu);
        if(sv_poll) { lv_timer_delete(sv_poll); sv_poll = NULL; }
        show_home();
        return;
    }
    uint32_t x, y;
    ref_point(&x, &y);
    if(!SQ.started) {
        if(pthread_create(&SQ.th, NULL, search_worker, NULL) == 0) SQ.started = 1;
        else {                                      /* sem thread: na interface, como antes */
            sv_nres = bgs_query(six, q, x, y, sv_res, MAXRES);
            show_results();
            return;
        }
    }
    pthread_mutex_lock(&SQ.mu);
    snprintf(SQ.text, sizeof SQ.text, "%s", q);
    SQ.x = x;
    SQ.y = y;
    SQ.seq++;
    pthread_cond_signal(&SQ.cv);
    pthread_mutex_unlock(&SQ.mu);
    if(!sv_poll) {
        lv_obj_clean(sv_list);
        bf_label(sv_list, tr(S_MAP_SEARCHING), BF_FONT_M, pal()->text_dim);
        sv_poll = lv_timer_create(sv_poll_cb, 50, NULL);
    }
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
        shown_persp = R.done_persp;
        have_shown = 1;
        uint64_t t0 = perf_now_us();
        /* inclinada: os rótulos vão na fonte (a projeção os leva junto); 2D: direto no quadro */
        lab_w = shown_persp ? PJ.sw : MAP_W;
        lab_h = shown_persp ? PJ.sh : MAP_H;
        layout_labels(R.lbl[R.front], R.nlbl[R.front]);
        compose_labels(shown_persp ? R.src[R.front] : R.buf[R.front]);
        perf_add(PERF_MAP_LABELS, (uint32_t)(perf_now_us() - t0));
        perf_add(PERF_MAP, R.us_fetch + R.us_draw);
        {
            unsigned ms = (R.us_fetch + R.us_draw) / 1000;
            frame_ema_ms = frame_ema_ms ? (frame_ema_ms * 3 + ms) / 4 : ms;
            /* inclinada pesada de forma sustentada (6 quadros seguidos acima de 700 ms; um pico
             * de leitura do cartão não conta): volta ao 2D nesta sessão; o botão 3D religa */
            static int slow_run;
            slow_run = shown_persp && ms > 700 ? slow_run + 1 : 0;
            if(slow_run >= 6 && !persp_auto_off) {
                persp_auto_off = true;
                slow_run = 0;
                fprintf(stderr, "biggps: vista inclinada desligada (6 quadros seguidos acima de 700 ms)\n");
                rot_label();
            }
        }
        perf_add(PERF_MAP_FETCH, R.us_fetch);
        perf_add(PERF_MAP_DRAW, R.us_draw);
        pthread_cond_signal(&R.cv);
        swapped = 1;
    }
    pthread_mutex_unlock(&R.mu);
    if(swapped) {
        swap_serial++;
        lv_image_set_src(img, &dsc[R.front]);
        update_offset();
        lv_obj_invalidate(img);
    }
    bgrt_res_t rr;
    if(bgrt_poll(&rr)) route_result(&rr);
    bgrt_alts_t ra;
    if(bgrt_poll_alts(&ra)) {
        if(ra.id == N.req_id && N.active && !N.navigating && !N.nstops && ra.n) {
            clear_alts();
            N.nalt = ra.n;
            for(int a = 0; a < ra.n; a++) { N.alt[a] = ra.r[a]; N.alt_toll[a] = ra.toll_arcs[a]; }
            set_alt_overlays();
            if(sheet) route_sheet();                 /* o painel ganha os botões */
            request();
        } else for(int a = 0; a < ra.n; a++) bgn_free(&ra.r[a]);
    }
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
    if(!on) zoom_goal = -1;                         /* o usuário assumiu o mapa */
    if(follow_btn) {
        lv_obj_set_style_bg_color(follow_btn, on ? lv_color_hex(C_POS) : pal()->surface, 0);
        lv_obj_set_style_text_color(follow_btn, on ? lv_color_white() : pal()->text, 0);
    }
}

/* ------------------------------------------------------ pino arrastável
 * Toque simples, toque longo e arrasto (tela resistiva de um ponto): segurar o pino do
 * destino o solta do mapa; ele segue o dedo com a ponta um pouco acima (o dedo não a cobre);
 * perto da borda o mapa rola; ao soltar, o painel do destino volta com o ponto novo. */
#define PIN_LIFT 36                    /* a ponta fica 36 px acima do dedo */
#define PIN_TOP 50                     /* a ponta não sobe além disso: o pino inteiro aparece */
static bool pin_hit, pin_drag, pin_moved;
static lv_point_t pin_grab;

static bool pin_touched(lv_point_t p)
{
    if(!N.has_dest || N.navigating || !dmark || lv_obj_has_flag(dmark, LV_OBJ_FLAG_HIDDEN)) return false;
    int x = lv_obj_get_x(dmark) + 24, y = lv_obj_get_y(dmark) + 46;    /* ponta */
    return p.x >= x - 30 && p.x <= x + 30 && p.y >= y - 56 && p.y <= y + 16;
}

static void pin_move(lv_point_t p)
{
    rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
    int px = p.x, py = p.y - PIN_LIFT;
    /* na borda o mapa anda junto — só depois que o pino de fato saiu do lugar (pegar um pino
     * que já está perto da borda não faz o mapa correr) */
    if(!pin_moved) pin_moved = abs(p.x - pin_grab.x) + abs(p.y - pin_grab.y) > 16;
    int ex = px < 40 ? -8 : px > MAP_W - 40 ? 8 : 0, ey = py < PIN_TOP + 10 ? -8 : py > MAP_H - 40 ? 8 : 0;
    if(pin_moved && (ex || ey)) {
        bgr_view_t v = cur;
        anchor(&v.ox, &v.oy);
        bgr_screen_to_world(&v, &s, v.ox + ex, v.oy + ey, &cur.cx, &cur.cy);
        set_follow(false);
        update_offset();
        if(!render_busy()) request();
    }
    if(py < PIN_TOP) py = PIN_TOP;
    uint32_t x, y;
    bgr_screen_to_world(&shown, &s, px - off_x, py - off_y, &x, &y);
    N.dx = x;
    N.dy = y;
    update_dest_marker();
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
        pin_drag = false;
        pin_hit = pin_touched(p);
    } else if(c == LV_EVENT_PRESSING && pin_drag) {
        pin_move(p);
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
        if(N.pick_stop && N.has_dest) {               /* marcando uma parada */
            rast_surface_t s = shown_surf();
            uint32_t x, y;
            bgr_screen_to_world(&shown, &s, p.x - off_x, p.y - off_y, &x, &y);
            N.pick_stop = 0;
            if(pick_chip) lv_obj_add_flag(pick_chip, LV_OBJ_FLAG_HIDDEN);
            if(N.nstops < BGRT_MAXSTOPS) { N.stx[N.nstops] = x; N.sty[N.nstops] = y; N.nstops++; }
            update_dest_marker();
            route_request(N.profile, false);
            return;
        }
        if(pin_hit) {                                 /* segurou o pino: passa a arrastá-lo */
            pin_drag = true;
            pin_moved = false;
            pin_grab = p;
            close_sheet();
            lv_image_set_scale(dmark, 320);
            lv_image_set_pivot(dmark, 24, 46);
            pin_move(p);
            return;
        }
        rast_surface_t s = { NULL, MAP_W, MAP_H, MAP_W };
        uint32_t x, y;
        bgr_screen_to_world(&shown, &s, p.x - off_x, p.y - off_y, &x, &y);
        open_dest_sheet(x, y);
    } else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        dragging = false;
        if(pin_drag) {                                /* soltou: novo destino, oferece a rota de novo */
            if(c == LV_EVENT_RELEASED) pin_move(p);   /* o último ponto do dedo */
            pin_drag = false;
            lv_image_set_scale(dmark, LV_SCALE_NONE);
            pin_drag_done = 1;
            open_dest_sheet(N.dx, N.dy);
        }
    }
}

static void zoom_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int z = (cur.zoom256 + 128) / 256 * 256 + d * 256;
    if(z < BGR_ZOOM_MIN) z = BGR_ZOOM_MIN;
    if(z > BGR_ZOOM_MAX) z = BGR_ZOOM_MAX;
    if(z == cur.zoom256) return;
    zoom_goal = -1;                                  /* zoom escolhido à mão vale até a próxima mudança de velocidade */
    cur.zoom256 = z;
    update_scale();
    request();
}

static void follow_cb(lv_event_t *e)
{
    (void)e;
    set_follow(true);
    if(N.navigating && nav_zoom > 0) zoom_goal = nav_zoom;   /* da visão geral volta aos poucos */
    if(have_pos) {
        if(MO.has_disp) { cur.cx = (uint32_t)llround(MO.x); cur.cy = (uint32_t)llround(MO.y); }
        else bgr_lonlat_to_world(pos_lon, pos_lat, &cur.cx, &cur.cy);
        cur.rot = cam_rot();
        request();
    }
}

/* visão geral da rota durante a navegação: enquadra tudo; o botão de posição volta */
static void overview_cb(lv_event_t *e)
{
    (void)e;
    if(!N.navigating) return;
    set_follow(false);
    fit_route();
}

/* norte para cima -> no rumo -> inclinada (3D) -> norte ... */
static void rot_cb(lv_event_t *e)
{
    (void)e;
    if(!heading_up) { heading_up = true; settings()->map_persp = false; }
    else if(!settings()->map_persp && pj_ok) { settings()->map_persp = true; persp_auto_off = false; }
    else { heading_up = false; settings()->map_persp = false; }
    settings_save();
    rot_label();
    cur.rot = follow ? cam_rot() : 0;
    anchor(&cur.ox, &cur.oy);
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

/* ------------------------------------------------------ painel "Seu marcador"
 * Tocar no marcador da posição: perfil, modelo (com prévia na cor escolhida), cor, tamanho,
 * restaurar o padrão. Salvo por perfil (settings: map_veh_*). */
static int vs_prof;
static uint32_t vs_px[5][BGVEH_SIZE * BGVEH_SIZE];
static lv_image_dsc_t vs_dsc[5];
static void veh_sheet(int prof);

/* refaz o painel depois do toque: o botão tocado é filho do painel que será apagado */
static void vs_rebuild(void *ud) { (void)ud; if(ov) veh_sheet(vs_prof); }

static void vs_changed(void)
{
    settings_save();
    veh_gen++;
    update_marker();
    lv_async_call(vs_rebuild, NULL);                  /* refaz as prévias e as seleções */
}

static void vs_prof_cb(lv_event_t *e) { vs_prof = (int)(intptr_t)lv_event_get_user_data(e); lv_async_call(vs_rebuild, NULL); }
static void vs_model_cb(lv_event_t *e) { settings()->map_veh_model[vs_prof] = (signed char)(intptr_t)lv_event_get_user_data(e); vs_changed(); }
static void vs_color_cb(lv_event_t *e) { settings()->map_veh_color[vs_prof] = (signed char)(intptr_t)lv_event_get_user_data(e); vs_changed(); }
static void vs_size_cb(lv_event_t *e) { settings()->map_veh_size[vs_prof] = (signed char)(intptr_t)lv_event_get_user_data(e); vs_changed(); }
static void vs_reset_cb(lv_event_t *e)
{
    (void)e;
    settings()->map_veh_model[vs_prof] = -1;
    settings()->map_veh_color[vs_prof] = 0;
    settings()->map_veh_size[vs_prof] = 1;
    vs_changed();
}
static void vs_close_cb(lv_event_t *e) { (void)e; close_sheet(); }

static void veh_sheet(int prof)
{
    if(N.navigating) return;
    vs_prof = prof;
    const bf_palette_t *p = pal();
    lv_obj_t *sh = sheet_new();
    lv_obj_t *head = sheet_row(sh);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = bf_label(head, tr(S_MAP_VEH_TITLE), BF_FONT_M, p->text);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *x = bf_button(head, LV_SYMBOL_CLOSE, NULL, 0);
    lv_obj_set_height(x, 44);
    lv_obj_add_event_cb(x, vs_close_cb, LV_EVENT_CLICKED, NULL);
    /* perfil */
    static const bf_str_t names[BGR_PROFILES] = { S_MAP_CAR, S_MAP_MOTO, S_MAP_BIKE, S_MAP_FOOT };
    lv_obj_t *row = sheet_row(sh);
    for(int i = 0; i < BGR_PROFILES; i++) {
        lv_obj_t *b = bf_button(row, NULL, tr(names[i]), i == prof ? 1 : 0);
        lv_obj_set_height(b, 44);
        lv_obj_add_event_cb(b, vs_prof_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* modelos, na cor escolhida */
    const int *models;
    int nm = bgveh_models(prof, &models), cur_m = veh_model_of(prof);
    uint32_t col = bgveh_color[settings()->map_veh_color[prof] & 7];
    row = sheet_row(sh);
    uint64_t t0 = perf_now_us();
    for(int i = 0; i < nm && i < 5; i++) {
        bgveh_draw(vs_px[i], models[i], col, 0);
        memset(&vs_dsc[i], 0, sizeof vs_dsc[i]);
        vs_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        vs_dsc[i].header.cf = LV_COLOR_FORMAT_ARGB8888;
        vs_dsc[i].header.w = vs_dsc[i].header.h = BGVEH_SIZE;
        vs_dsc[i].header.stride = BGVEH_SIZE * 4;
        vs_dsc[i].data = (const uint8_t *)vs_px[i];
        vs_dsc[i].data_size = sizeof vs_px[i];
        lv_image_cache_drop(&vs_dsc[i]);
        lv_obj_t *b = lv_button_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 64, 64);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, p->surface2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, models[i] == cur_m ? 3 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_POS), 0);
        lv_obj_t *im = lv_image_create(b);
        lv_image_set_src(im, &vs_dsc[i]);
        lv_obj_center(im);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, vs_model_cb, LV_EVENT_CLICKED, (void *)(intptr_t)models[i]);
    }
    fprintf(stderr, "biggps: prévias do marcador em %u ms\n", (unsigned)((perf_now_us() - t0) / 1000));
    /* cores */
    row = sheet_row(sh);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for(int i = 0; i < BGVEH_COLORS; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 44, 44);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(bgveh_color[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        bool sel = (settings()->map_veh_color[prof] & 7) == i;
        lv_obj_set_style_border_width(b, sel ? 4 : 2, 0);
        lv_obj_set_style_border_color(b, sel ? lv_color_hex(C_POS) : p->surface2, 0);
        lv_obj_add_event_cb(b, vs_color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* tamanho e padrão */
    row = sheet_row(sh);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *sz[3] = { "P", "M", "G" };
    for(int i = 0; i < 3; i++) {
        lv_obj_t *b = bf_button(row, NULL, sz[i], settings()->map_veh_size[prof] == i ? 1 : 0);
        lv_obj_set_size(b, 64, 44);
        lv_obj_add_event_cb(b, vs_size_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *rb = bf_button(row, LV_SYMBOL_REFRESH, tr(S_MAP_VEH_RESET), 0);
    lv_obj_set_height(rb, 44);
    lv_obj_add_event_cb(rb, vs_reset_cb, LV_EVENT_CLICKED, NULL);
}

static void mark_cb(lv_event_t *e) { (void)e; veh_sheet(N.profile >= 0 && N.profile < BGR_PROFILES ? N.profile : BGR_CAR); }

/* --------------------------------------------------------------- criação */
static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN, BTN);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, pal()->surface, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);     /* opaco: mistura de cor custa por pixel */
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
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

void scr_map_demo(int step);
static bool demo_alive;

/* ---------------------------------------------------------------- teste de desempenho
 * BIGFOSTON_BENCH=<arquivo>: roteiro fixo sem toque (docs/performance/10-rendering-fps.md),
 * num ponto público de Brasília (BIGFOSTON_BENCH_AT=lat,lon,zoom muda), e grava uma
 * linha de medidas por fase no arquivo. As posições simuladas aparecem como simulação. */
static const char *bench_file;
static lv_timer_t *bench_tm;
static int bench_phase, bench_tick;
static double bench_lat = -15.7975, bench_lon = -47.8919;
static int bench_zoom = 15;
static uint64_t bench_cpu0, bench_t0;
static const struct { const char *name; int ticks; } bench_ph[] = {
    { "parado", 100 }, { "arrasto", 80 }, { "assentar", 60 }, { "arrasto-so-lvgl", 60 }, { "assentar2", 40 }, { "zoom", 160 },
    { "rotacao", 80 }, { "seguir", 200 }, { "navegacao", 400 },
};
#define BENCH_TICK_MS 50

static uint64_t self_cpu_ticks(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    char b[512];
    uint64_t ut = 0, st = 0;
    if(f) {
        if(fgets(b, sizeof b, f)) {
            char *r = strrchr(b, ')');
            if(r) sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                         (unsigned long long *)&ut, (unsigned long long *)&st);
        }
        fclose(f);
    }
    return ut + st;
}

static long self_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char b[128];
    long v = -1;
    while(f && fgets(b, sizeof b, f)) if(!strncmp(b, "VmRSS:", 6)) v = atol(b + 6);
    if(f) fclose(f);
    return v;
}

static void bench_end_phase(void)
{
    char line[1024];
    int o = perf_format(line, sizeof line, bench_ph[bench_phase].name);
    double sec = (perf_now_us() - bench_t0) / 1e6;
    long hz = sysconf(_SC_CLK_TCK);
    snprintf(line + o, sizeof line - (size_t)o, " | cpu %.0f%% rss %ld KB",
             100.0 * (double)(self_cpu_ticks() - bench_cpu0) / ((double)hz * sec), self_rss_kb());
    fprintf(stderr, "bench: %s\n", line);
    FILE *f = fopen(bench_file, "a");
    if(f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void bench_fix(double lat, double lon, double course, double speed)
{
    uint32_t x, y;
    bgr_lonlat_to_world(lon, lat, &x, &y);
    on_fix(x, y, speed, course, false);
}

static void bench_cb(lv_timer_t *t)
{
    (void)t;
    if(!R.started) return;
    int k = bench_tick++;
    const char *ph = bench_ph[bench_phase].name;
    if(!strcmp(ph, "arrasto-so-lvgl")) {            /* só a imagem anda: custo do LVGL */
        off_x = (k % 40 < 20 ? k % 20 : 20 - k % 20) * 6;
        off_y = 0;
        lv_obj_set_pos(img, off_x, off_y);
        lv_obj_invalidate(layer);
        for(int i = 0; i < nrot; i++)
            if(rot[i].img) lv_obj_set_pos(rot[i].img, rot[i].cx - (int)rot[i].dsc.header.w / 2 + off_x, rot[i].cy - (int)rot[i].dsc.header.h / 2 + off_y);
        if(k + 1 == bench_ph[bench_phase].ticks) update_offset();
    } else if(!strcmp(ph, "arrasto")) {             /* como um dedo: 12 px a cada 50 ms */
        rast_surface_t sf = { NULL, MAP_W, MAP_H, MAP_W };
        bgr_view_t v = cur;
        anchor(&v.ox, &v.oy);
        bgr_screen_to_world(&v, &sf, v.ox + 12, v.oy + (k % 40 < 20 ? 4 : -4), &cur.cx, &cur.cy);
        update_offset();
        request();
    } else if(!strcmp(ph, "zoom")) {
        if(k % 40 == 0) {
            static const int dz[4] = { 1, 1, -1, -1 };
            cur.zoom256 += dz[(k / 40) % 4] * 256;
            update_scale();
            request();
        }
    } else if(!strcmp(ph, "rotacao")) {
        if(k % 2 == 0) { cur.rot = (cur.rot + 6) % 360; update_offset(); request(); }
    } else if(!strcmp(ph, "seguir")) {              /* GPS simulado a 50 km/h para leste, 1 Hz */
        if(k == 0) { heading_up = true; set_follow(true); }
        if(k % 20 == 0) bench_fix(bench_lat, bench_lon + (k / 20) * 0.00013, 90, 50);
    } else if(!strcmp(ph, "navegacao")) {           /* rota de demonstração e simulação */
        if(k == 0) scr_map_demo(0);
        if(k == 100) scr_map_demo(1);
    }
    if(bench_tick >= bench_ph[bench_phase].ticks) {
        bench_end_phase();
        bench_tick = 0;
        if(++bench_phase >= (int)(sizeof bench_ph / sizeof bench_ph[0])) {
            lv_timer_delete(bench_tm);
            bench_tm = NULL;
            fprintf(stderr, "bench: fim\n");
            FILE *f = fopen(bench_file, "a");
            if(f) { fprintf(f, "fim\n"); fclose(f); }
            return;
        }
        if(!strcmp(bench_ph[bench_phase].name, "rotacao")) { set_follow(false); heading_up = false; }
        perf_reset();
        bench_cpu0 = self_cpu_ticks();
        bench_t0 = perf_now_us();
        if(!strcmp(bench_ph[bench_phase].name, "arrasto-so-lvgl")) perf_prof_start();
        if(!strcmp(bench_ph[bench_phase].name, "assentar2")) perf_prof_stop();
    }
}

static void bench_start(void)
{
    bench_file = getenv("BIGFOSTON_BENCH");
    if(!bench_file || !*bench_file) return;
    const char *at = getenv("BIGFOSTON_BENCH_AT");
    if(at) sscanf(at, "%lf,%lf,%d", &bench_lat, &bench_lon, &bench_zoom);
    set_follow(false);
    heading_up = false;
    bgr_lonlat_to_world(bench_lon, bench_lat, &cur.cx, &cur.cy);
    cur.zoom256 = bench_zoom * 256;
    cur.rot = 0;
    update_scale();
    request();
    bench_fix(bench_lat, bench_lon, 90, 0);         /* posição para a rota da navegação */
    set_follow(false);
    bench_phase = bench_tick = 0;
    perf_reset();
    bench_cpu0 = self_cpu_ticks();
    bench_t0 = perf_now_us();
    bench_tm = lv_timer_create(bench_cb, BENCH_TICK_MS, NULL);
    FILE *f = fopen(bench_file, "a");
    if(f) { fprintf(f, "inicio %.5f,%.5f z%d\n", bench_lat, bench_lon, bench_zoom); fclose(f); }
}
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

/* guia de faixas: o primeiro .bgl em big-gps/rotas (opcional) */
static bgl_pack_t *open_lanes(void)
{
    const char *base = getenv("BIGFOSTON_BIGGPS");
    if(!base || !*base) base = BIGGPS_DIR_DEFAULT;
    char dir[512], path[1024], err[256];
    snprintf(dir, sizeof dir, "%s/rotas", base);
    DIR *dp = opendir(dir);
    if(!dp) return NULL;
    bgl_pack_t *p = NULL;
    struct dirent *de;
    while(!p && (de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if(l < 5 || strcasecmp(de->d_name + l - 4, ".bgl") || de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        p = bgl_open(path, err, sizeof err);
        if(!p) fprintf(stderr, "biggps: %s\n", err);
    }
    closedir(dp);
    if(p) fprintf(stderr, "biggps: faixas de %u cruzamentos\n", bgl_count(p));
    return p;
}

/* pontos de alerta: o .bga mais novo em big-gps/alertas (opcional; sem ele o mapa funciona igual) */
static bga_pack_t *open_alerts(void)
{
    const char *base = getenv("BIGFOSTON_BIGGPS");
    if(!base || !*base) base = BIGGPS_DIR_DEFAULT;
    char dir[512], path[1024], err[256];
    snprintf(dir, sizeof dir, "%s/alertas", base);
    DIR *dp = opendir(dir);
    if(!dp) return NULL;
    bga_pack_t *best = NULL;
    struct dirent *de;
    while((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if(l < 5 || strcasecmp(de->d_name + l - 4, ".bga") || de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        bga_pack_t *p = bga_open(path, err, sizeof err);
        if(!p) { fprintf(stderr, "biggps: %s\n", err); continue; }
        if(!best || bga_built(p) > bga_built(best)) { bga_close(best); best = p; }
        else bga_close(p);
    }
    closedir(dp);
    if(best) fprintf(stderr, "biggps: %u pontos de alerta\n", bga_count(best));
    return best;
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

    if(!mk_dsc[0].data) {
        uint64_t t0 = perf_now_us();
        mk_build();
        fprintf(stderr, "biggps: marcadores em %u ms\n", (unsigned)((perf_now_us() - t0) / 1000));
    }
    ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, MAP_W, MAP_H);
    /* sem fundo: ov_draw pinta só as faixas que o arrasto descobre (o fundo da tela
     * inteira sob a imagem custava ~20 % de cada quadro) e ov_cover diz ao LVGL que
     * a tela fica toda coberta (o LVGL não desenha as telas por baixo) */
    ov_bg = lv_color_hex(night ? 0x1A1D23 : 0xF2EFE9);
    lv_obj_add_event_cb(ov, ov_cover, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(ov, ov_draw, LV_EVENT_DRAW_MAIN, NULL);
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
    for(int i = 0; i < BGRT_MAXSTOPS; i++) {        /* paradas: círculo laranja numerado */
        stop_mk[i] = lv_obj_create(ov);
        lv_obj_remove_style_all(stop_mk[i]);
        lv_obj_set_size(stop_mk[i], 30, 30);
        lv_obj_set_style_radius(stop_mk[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(stop_mk[i], lv_color_hex(0xe8710a), 0);
        lv_obj_set_style_bg_opa(stop_mk[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(stop_mk[i], lv_color_white(), 0);
        lv_obj_set_style_border_width(stop_mk[i], 3, 0);
        lv_obj_remove_flag(stop_mk[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        char n[4];
        snprintf(n, sizeof n, "%d", i + 1);
        lv_obj_t *l = bf_label(stop_mk[i], n, BF_FONT_S, lv_color_white());
        lv_obj_center(l);
        lv_obj_add_flag(stop_mk[i], LV_OBJ_FLAG_HIDDEN);
    }
    dmark = lv_image_create(ov);
    lv_image_set_src(dmark, &mk_dsc[3]);
    lv_obj_remove_flag(dmark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dmark, LV_OBJ_FLAG_HIDDEN);
    mark = lv_image_create(ov);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_CLICKABLE);    /* tocar abre "Seu marcador" */
    lv_obj_set_ext_click_area(mark, 10);
    lv_obj_add_event_cb(mark, mark_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    veh_built_prof = -1;                             /* refaz o marcador com a escolha salva */

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

    /* coluna da navegação (à esquerda, abaixo da faixa da manobra): o que falta e a velocidade;
     * visão geral e encerrar embaixo */
    botbar = chip(ov);
    lv_obj_add_flag(botbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(botbar, NAV_COL_W, MAP_H - 124 - 12);
    lv_obj_set_style_pad_all(botbar, 10, 0);
    lv_obj_set_style_radius(botbar, 14, 0);
    lv_obj_set_flex_flow(botbar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(botbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(botbar, 4, 0);
    lv_obj_align(botbar, LV_ALIGN_TOP_LEFT, 12, 124);
    col_time = bf_label(botbar, "", BF_FONT_L, p->text);
    col_dist = bf_label(botbar, "", BF_FONT_M, p->text);
    col_eta = bf_label(botbar, "", BF_FONT_S, p->text_dim);
    col_speed = bf_label(botbar, "", BF_FONT_M, p->text);
    lv_obj_set_style_pad_top(col_speed, 8, 0);
    bot_lbl = col_time;
    lv_obj_t *grow = bf_plain(botbar);                               /* empurra os botões para baixo */
    lv_obj_set_width(grow, 1);
    lv_obj_set_flex_grow(grow, 1);
    lv_obj_t *vb = bf_button(botbar, LV_SYMBOL_EYE_OPEN, NULL, 0);     /* visão geral da rota */
    lv_obj_set_size(vb, LV_PCT(100), 44);
    lv_obj_add_event_cb(vb, overview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eb = bf_button(botbar, LV_SYMBOL_CLOSE, tr(S_MAP_END), 2);
    lv_obj_set_size(eb, LV_PCT(100), 48);
    lv_obj_add_event_cb(eb, end_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(botbar, LV_OBJ_FLAG_HIDDEN);

    sim_chip = chip(ov);
    lv_obj_set_style_bg_color(sim_chip, lv_color_hex(0xd93025), 0);
    lv_obj_set_style_bg_opa(sim_chip, LV_OPA_COVER, 0);
    bf_label(sim_chip, tr(S_MAP_SIMULATING), BF_FONT_S, lv_color_white());
    lv_obj_align(sim_chip, LV_ALIGN_TOP_RIGHT, -96, 124);
    lv_obj_add_flag(sim_chip, LV_OBJ_FLAG_HIDDEN);

    pick_chip = chip(ov);                         /* marcando uma parada */
    lv_obj_set_style_bg_color(pick_chip, lv_color_hex(0xe8710a), 0);
    bf_label(pick_chip, tr(S_MAP_PICK_STOP), BF_FONT_M, lv_color_white());
    lv_obj_align(pick_chip, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_flag(pick_chip, LV_OBJ_FLAG_HIDDEN);

    al_chip = chip(ov);                           /* radar / lombada à frente */
    lv_obj_set_style_bg_color(al_chip, lv_color_hex(0xf5b83d), 0);
    al_lbl = bf_label(al_chip, "", BF_FONT_M, lv_color_hex(0x202124));
    lv_obj_align(al_chip, LV_ALIGN_TOP_LEFT, 12 + NAV_COL_W + 12, 180);   /* abaixo das faixas, ao lado da coluna */

    lane_img = lv_image_create(ov);                  /* faixas da próxima manobra */
    lv_obj_align(lane_img, LV_ALIGN_TOP_MID, -20, 124);
    lv_obj_set_style_radius(lane_img, 8, 0);
    lv_obj_set_style_clip_corner(lane_img, true, 0);
    lv_obj_remove_flag(lane_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
    lane_for = -1;
    lv_obj_add_flag(al_chip, LV_OBJ_FLAG_HIDDEN);

    set_follow(follow);
    rot_label();

    static char paths[MAXMAPS][1024];
    char err[256];
    int nmaps = find_maps(paths, MAXMAPS);
    if(!nmaps) {
        show_message(tr(S_MAP_NOMAP), tr(S_MAP_NOMAP_SUB));
        gps_update();
        tick = lv_timer_create(tick_cb, 100, NULL);
        return;
    }
    R.alerts = open_alerts();
    upts_load();
    lanes_pack = open_lanes();
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
    /* vista inclinada: fontes de ~1,5 MB cada; sem memória, fica só o 2D */
    pj_ok = false;
    if(bgp_init(&PJ, MAP_W, MAP_H, PERSP_Y0, PERSP_YH) == 0) {
        for(int i = 0; i < 2; i++) R.src[i] = malloc((size_t)PJ.sw * PJ.sh * 2);
        pj_ok = R.src[0] && R.src[1];
        if(!pj_ok) { free(R.src[0]); free(R.src[1]); R.src[0] = R.src[1] = NULL; }
    }
    rot_label();
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
        /* o índice de versão mais alta (o formato 3 busca por região); empate: ordem
         * alfabética. Um shell antigo abre o primeiro .bgs em ordem alfabética, então a
         * cópia v2 se chama brasil-v2.bgs e a v3 brasil.bgs: cada um acha o seu */
        DIR *dp = opendir(sd);
        if(dp) {
            struct dirent *de;
            char bestname[256] = "";
            uint32_t bestv = 0;
            while((de = readdir(dp))) {
                size_t l = strlen(de->d_name);
                if(l <= 4 || de->d_name[0] == '.' || strcasecmp(de->d_name + l - 4, ".bgs")) continue;
                if(snprintf(sp, sizeof sp, "%s/%s", sd, de->d_name) >= (int)sizeof sp) continue;
                bgs_index_t *ix = bgs_open(sp, serr, sizeof serr);
                if(!ix) { fprintf(stderr, "biggps: %s\n", serr); continue; }
                uint32_t v = bgs_version(ix);
                if(!six || v > bestv || (v == bestv && strcmp(de->d_name, bestname) < 0)) {
                    bgs_close(six);
                    six = ix;
                    bestv = v;
                    snprintf(bestname, sizeof bestname, "%s", de->d_name);
                } else bgs_close(ix);
            }
            closedir(dp);
        }
    }
    R.stop = R.has_req = R.done = R.busy = 0;
    R.front = 0;
    bgmo_reset(&MO);
    int rc = pthread_create(&R.th, NULL, worker, NULL);
    if(rc == 0) R.started = 1;
    else fprintf(stderr, "biggps: pthread_create: %s\n", strerror(rc));
    update_scale();
    gps_update();
    request();
    tick = lv_timer_create(tick_cb, 100, NULL);
    anim = lv_timer_create(anim_cb, ANIM_MS, NULL);
    bench_start();
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
    if(bench_tm) { lv_timer_delete(bench_tm); bench_tm = NULL; }
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    if(anim) { lv_timer_delete(anim); anim = NULL; }
    if(R.started) {
        pthread_mutex_lock(&R.mu);
        R.stop = 1;
        pthread_cond_signal(&R.cv);
        pthread_mutex_unlock(&R.mu);
        pthread_join(R.th, NULL);
        R.started = 0;
    }
    search_close();
    search_stop();                               /* antes de fechar o índice que ela usa */
    bgs_close(six);
    six = NULL;
    bgrt_stop();
    voice_stop();
    if(N.navigating) heading_up = N.prev_heading_up;
    bgn_free(&N.route);
    free(N.al);
    for(int a = 0; a < N.nalt; a++) bgn_free(&N.alt[a]);
    for(int a = 0; a < BGRT_MAXALT; a++) { free(R.alt[a]); R.alt[a] = NULL; R.alt_n[a] = 0; }
    memset(&N, 0, sizeof N);
    bgmo_reset(&MO);
    free(R.route);
    R.route = NULL;
    R.route_n = 0;
    last_save();
    rot_clear();
    lcache_clear();
    if(glyph_buf) { lv_draw_buf_destroy(glyph_buf); glyph_buf = NULL; }
    if(ov) { lv_obj_delete(ov); ov = NULL; }
    if(R.pack) { bgm_close(R.pack); R.pack = NULL; }
    bga_close(R.alerts);
    R.alerts = NULL;
    bgl_close(lanes_pack);
    lanes_pack = NULL;
    lane_img = NULL;
    lane_e = NULL;
    al_chip = al_lbl = pick_chip = NULL;
    for(int i = 0; i < 2; i++) { free(R.buf[i]); R.buf[i] = NULL; free(R.src[i]); R.src[i] = NULL; }
    pj_ok = shown_persp = false;
    lab_w = MAP_W;
    lab_h = MAP_H;
    for(int i = 0; i < BGRT_MAXSTOPS; i++) stop_mk[i] = NULL;
    img = layer = mark = dmark = info_chip = info_lbl = scale_lbl = scale_bar = follow_btn = rot_lbl = msg = NULL;
    voice_lbl = search_btn = NULL;
    sheet = banner = ban_icon = ban_dist = ban_text = ban_then = botbar = bot_lbl = sim_chip = NULL;
    col_time = col_dist = col_eta = col_speed = NULL;
    have_shown = 0;
    nplaced = 0;
}

/* --screenshot: 0 destino em Taguatinga (carro), 1 começa a simulação, 2 avança 60 s,
 * 3 abre a busca, 4 busca "hospital", 5 fecha a busca, 6 abre "Seu marcador",
 * 7 escolhe o sedã vermelho grande, 8 fecha o painel */
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
    } else if(step == 6) {
        veh_sheet(BGR_CAR);                          /* "Seu marcador" */
    } else if(step == 7) {
        settings()->map_veh_model[BGR_CAR] = BGVEH_SEDAN;   /* escolhe o sedã vermelho, G */
        settings()->map_veh_color[BGR_CAR] = 1;
        settings()->map_veh_size[BGR_CAR] = 2;
        vs_changed();
    } else if(step == 8) {
        close_sheet();
    } else if(step == 9) {                           /* escolhe a 1ª alternativa, se houver */
        bgrt_alts_t ra;
        for(int i = 0; i < 100 && !N.nalt; i++) {     /* espera as alternativas (segundo plano) */
            if(bgrt_poll_alts(&ra)) {
                if(ra.id == N.req_id && ra.n) {
                    N.nalt = ra.n;
                    for(int k = 0; k < ra.n; k++) { N.alt[k] = ra.r[k]; N.alt_toll[k] = ra.toll_arcs[k]; }
                    set_alt_overlays();
                } else for(int k = 0; k < ra.n; k++) bgn_free(&ra.r[k]);
            }
            if(!N.nalt) usleep(20000);
        }
        if(N.nalt) select_alt(0);
    } else if(step == 11) {                          /* painel do destino no centro do mapa */
        open_dest_sheet(cur.cx + 6400, cur.cy);       /* ~60 m a leste da posição */
    } else if(step == 12) {                          /* marca "Lombada aqui" nesse ponto */
        if(nupt < MAXUPT && upt_near(N.dx, N.dy) < 0) { upts[nupt++] = (bga_entry_t){ N.dx, N.dy, BGA_BUMP, 0, BGA_DIR_ANY }; upts_save(); }
        N.has_dest = 0;
        close_sheet();
        cur.zoom256 = 17 * 256;
        update_scale();
        request();
    } else if(step == 10) {                          /* guia de faixas de exemplo (captura) */
        static bgl_entry_t ex = { .n = 4, .lane = { BGL_LEFT, BGL_THROUGH, BGL_THROUGH, BGL_THROUGH | BGL_RIGHT } };
        lane_e = &ex;
        lane_fit = bgl_fit(&ex, BGN_RIGHT);
        lane_for = -2;                                /* não é de nenhuma manobra: a próxima refaz */
        lanes_draw(BGN_RIGHT);
        lv_obj_remove_flag(lane_img, LV_OBJ_FLAG_HIDDEN);
    }
}

const bf_screen_def_t scr_map_def = { create, destroy, S_APP_GPS, NULL };
