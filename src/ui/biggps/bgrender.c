/* bgrender.c - see bgrender.h.
 * Per frame: a few double computations (scale, rotation, tile origins); per vertex only
 * 64-bit integer multiply and shift.  Draw order: areas, water lines, borders, rails, road
 * casings, road fills, ferries; labels are collected for the caller.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgrender.h"
#include "bgkinds.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NZ 15                  /* style tables cover zoom 5..19 */

typedef struct {
    uint32_t day, night;       /* fill colour (0xRRGGBB) */
    uint32_t cday, cnight;     /* casing colour (roads), 0 = no casing */
    uint8_t w[NZ];             /* width in 1/4 px at zoom 5..19 (lines) */
    uint8_t dash;              /* dash length in px, 0 = solid */
} style_t;

#define W(...) {__VA_ARGS__}
static const style_t ST[BGK_COUNT] = {
    [BGK_A_RESIDENTIAL] = {0xE4E1DC, 0x262A31, 0, 0, W(0), 0},
    [BGK_A_COMMERCIAL] = {0xF2DAD9, 0x30282C, 0, 0, W(0), 0},
    [BGK_A_INDUSTRIAL] = {0xEBDBE8, 0x2C2833, 0, 0, W(0), 0},
    [BGK_A_FARM] = {0xEEF0D5, 0x262B22, 0, 0, W(0), 0},
    [BGK_A_FOREST] = {0xADD19E, 0x1E3322, 0, 0, W(0), 0},
    [BGK_A_PARK] = {0xC8E6B0, 0x203526, 0, 0, W(0), 0},
    [BGK_A_SAND] = {0xF5E9C6, 0x3A362A, 0, 0, W(0), 0},
    [BGK_A_WETLAND] = {0xC6DED2, 0x20322E, 0, 0, W(0), 0},
    [BGK_A_WATER] = {0xAAD3DF, 0x173A4D, 0, 0, W(0), 0},
    [BGK_A_AEROWAY] = {0xE5E2EA, 0x2A2B33, 0, 0, W(0), 0},
    [BGK_A_PEDESTRIAN] = {0xDDDDE8, 0x30323A, 0, 0, W(0), 0},
    [BGK_A_PARKING] = {0xEEEEEE, 0x2C2E34, 0, 0, W(0), 0},
    [BGK_A_BUILDING] = {0xD9D0C9, 0x3A3D45, 0, 0, W(0), 0},
    /*                         z: 5  6  7  8  9 10 11 12 13 14 15  16  17  18  19 */
    [BGK_L_STREAM] = {0xAAD3DF, 0x1F4A60, 0, 0, W(0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 6, 8, 10, 14, 16), 0},
    [BGK_L_RIVER] = {0xAAD3DF, 0x1F4A60, 0, 0, W(0, 0, 0, 0, 4, 4, 6, 6, 10, 12, 16, 24, 32, 40, 48), 0},
    [BGK_L_BOUNDARY] = {0x9E8FB0, 0x6E6385, 0, 0, W(4, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6), 6},
    [BGK_L_RAIL] = {0x8A8A8A, 0x6A6E78, 0, 0, W(0, 0, 0, 0, 0, 0, 4, 4, 6, 6, 8, 8, 10, 12, 16), 0},
    [BGK_L_PATH] = {0xE07A6E, 0xA0605A, 0, 0, W(0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 8, 10), 3},
    [BGK_L_TRACK] = {0xA87A3C, 0x8A6A40, 0, 0, W(0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 6, 8, 10, 12, 16), 5},
    [BGK_L_SERVICE] = {0xFFFFFF, 0x464B57, 0xC2C2C2, 0x15181E, W(0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 10, 16, 28, 40, 56), 0},
    [BGK_L_RESIDENTIAL] = {0xFFFFFF, 0x515766, 0xBDBDBD, 0x15181E, W(0, 0, 0, 0, 0, 0, 0, 4, 6, 14, 24, 40, 56, 80, 112), 0},
    [BGK_L_TERTIARY] = {0xFFFFFF, 0x5E6475, 0xA8A8A8, 0x15181E, W(0, 0, 0, 0, 0, 0, 4, 8, 14, 24, 34, 48, 64, 88, 120), 0},
    [BGK_L_SECONDARY] = {0xF7FABF, 0x7A7A48, 0xA6AE4A, 0x15181E, W(0, 0, 0, 0, 4, 6, 10, 14, 20, 28, 38, 52, 72, 96, 128), 0},
    [BGK_L_PRIMARY] = {0xFCD6A4, 0x9A7A40, 0xC08A3A, 0x15181E, W(0, 0, 0, 5, 6, 8, 12, 16, 22, 30, 40, 56, 76, 104, 136), 0},
    [BGK_L_TRUNK] = {0xF9B29C, 0xA8684C, 0xD0603A, 0x15181E, W(4, 4, 6, 8, 10, 12, 14, 18, 24, 32, 44, 60, 80, 112, 144), 0},
    [BGK_L_MOTORWAY] = {0xE892A2, 0xB05468, 0xDC2A67, 0x15181E, W(4, 4, 6, 8, 10, 12, 14, 18, 24, 32, 44, 60, 80, 112, 144), 0},
    [BGK_L_FERRY] = {0x6666FF, 0x5A5AC0, 0, 0, W(0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 6, 6, 6, 6, 6), 6},
};

static const uint8_t LABEL_PRIO[BGK_COUNT] = {
    [BGK_P_CITY] = 0, [BGK_P_TOWN] = 1, [BGK_L_MOTORWAY] = 2, [BGK_L_TRUNK] = 2, [BGK_P_SUBURB] = 3,
    [BGK_L_PRIMARY] = 4, [BGK_P_VILLAGE] = 5, [BGK_L_SECONDARY] = 6, [BGK_L_TERTIARY] = 7,
    [BGK_P_NEIGHBOURHOOD] = 8, [BGK_L_RESIDENTIAL] = 9, [BGK_L_SERVICE] = 11, [BGK_L_PATH] = 12,
    [BGK_L_TRACK] = 12, [BGK_L_RIVER] = 10, [BGK_L_STREAM] = 12,
};

int bgr_kind_minzoom(int k, int detail)
{
    int z = bgk_minzoom(k);
    if (detail == BGR_DETAIL_DETAILED) return z;
    int eco = detail == BGR_DETAIL_ECONOMIC;
    int m;
    switch (k) {
    case BGK_A_BUILDING: m = eco ? 17 : 16; break;
    case BGK_A_PARKING: case BGK_A_PEDESTRIAN: m = eco ? 17 : 15; break;
    case BGK_L_PATH: m = eco ? 17 : 16; break;
    case BGK_L_TRACK: m = eco ? 16 : 15; break;
    case BGK_L_SERVICE: m = eco ? 16 : 15; break;
    case BGK_L_STREAM: m = eco ? 15 : 14; break;
    case BGK_L_RAIL: m = eco ? 14 : 12; break;
    case BGK_A_COMMERCIAL: case BGK_A_INDUSTRIAL: case BGK_A_FARM: m = eco ? 13 : 12; break;
    case BGK_P_NEIGHBOURHOOD: m = eco ? 15 : 14; break;
    case BGK_L_RESIDENTIAL: m = eco ? 14 : 13; break;   /* visão geral da rota: 40 mil ruas em z12 */
    case BGK_L_TERTIARY: m = eco ? 13 : 11; break;
    case BGK_A_RESIDENTIAL: m = eco ? 12 : 10; break;
    default: m = z;
    }
    return m > z ? m : z;
}

/* casing (outline) of a road kind at a level of detail: economic outlines only the
 * main roads from zoom 15; the others are drawn as a single line in the casing colour
 * (half the drawing work) */
static int casing_on(int k, int detail, int z)
{
    if (detail != BGR_DETAIL_ECONOMIC) return 1;
    return (k >= BGK_L_SECONDARY && z >= 15) || z >= 17;
}

uint16_t bgr_background(int night) { return rgb565(night ? 0x1A1D23 : 0xF2EFE9); }

static uint32_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}

/* width in 1/16 px at zoom256 (linear between integer zooms) */
static int width16(int kind, int zoom256)
{
    int zi = (zoom256 >> 8) - 5, f = zoom256 & 255;
    if (zi < 0) return 0;
    if (zi >= NZ - 1) return ST[kind].w[NZ - 1] * 4;
    int a = ST[kind].w[zi], b = ST[kind].w[zi + 1];
    if (!a) return 0;
    return (a * 4 * (256 - f) + b * 4 * f) >> 8;
}

void bgr_lonlat_to_world(double lon, double lat, uint32_t *x, uint32_t *y)
{
    const double W = 4294967296.0;
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double r = lat * M_PI / 180.0;
    double fx = (lon + 180.0) / 360.0 * W;
    double fy = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * W;
    if (fx < 0) fx = 0;
    if (fx > W - 1) fx = W - 1;
    if (fy < 0) fy = 0;
    if (fy > W - 1) fy = W - 1;
    *x = (uint32_t)fx;
    *y = (uint32_t)fy;
}

void bgr_world_to_lonlat(uint32_t x, uint32_t y, double *lon, double *lat)
{
    const double W = 4294967296.0;
    *lon = x / W * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * y / W))) * 180.0 / M_PI;
}

typedef struct {
    double k, c, s;            /* px per world unit, cos, sin of rotation */
    double sx0, sy0;           /* screen point of the centre */
} xf_t;

static void xf_init(const bgr_view_t *v, const rast_surface_t *s, xf_t *x)
{
    x->k = ldexp(pow(2.0, (v->zoom256 & 255) / 256.0), (v->zoom256 >> 8) + 8 - 32);
    x->c = cos(v->rot * M_PI / 180.0);
    x->s = sin(v->rot * M_PI / 180.0);
    x->sx0 = v->ox || v->oy ? v->ox : s->w / 2.0;
    x->sy0 = v->ox || v->oy ? v->oy : s->h / 2.0;
}

void bgr_world_to_screen(const bgr_view_t *v, const rast_surface_t *s, uint32_t x, uint32_t y, int *sx, int *sy)
{
    xf_t t;
    xf_init(v, s, &t);
    double dx = (int32_t)(x - v->cx), dy = (int32_t)(y - v->cy);
    *sx = (int)lround(t.sx0 + (dx * t.c - dy * t.s) * t.k);
    *sy = (int)lround(t.sy0 + (dx * t.s + dy * t.c) * t.k);
}

void bgr_screen_to_world(const bgr_view_t *v, const rast_surface_t *s, int sx, int sy, uint32_t *x, uint32_t *y)
{
    xf_t t;
    xf_init(v, s, &t);
    double px = (sx - t.sx0) / t.k, py = (sy - t.sy0) / t.k;
    double dx = px * t.c + py * t.s, dy = -px * t.s + py * t.c;
    *x = v->cx + (uint32_t)(int32_t)lround(dx);
    *y = v->cy + (uint32_t)(int32_t)lround(dy);
}

double bgr_metres_per_pixel(const bgr_view_t *v)
{
    double lon, lat;
    bgr_world_to_lonlat(v->cx, v->cy, &lon, &lat);
    xf_t t;
    rast_surface_t s = {0};
    xf_init(v, &s, &t);
    return 40075016.686 / 4294967296.0 * cos(lat * M_PI / 180.0) / t.k;
}

/* integer atan2 in degrees (-180..180), error < 1 degree */
static int iatan2(int32_t y, int32_t x)
{
    if (!x && !y) return 0;
    int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int a;
    if (ax >= ay) {
        int32_t r = (int32_t)(((int64_t)ay << 10) / ax);                  /* tan in Q10, 0..1024 */
        a = (int)((r * (45 * 1024 + (1024 - r) * 15)) >> 20);              /* ~45*r - 15.6*r*(r-1) */
    } else {
        int32_t r = (int32_t)(((int64_t)ax << 10) / ay);
        a = 90 - (int)((r * (45 * 1024 + (1024 - r) * 15)) >> 20);
    }
    if (x < 0) a = 180 - a;
    return y < 0 ? -a : a;
}

typedef struct {
    const bgm_tile_t *t;
    int32_t ox16, oy16;        /* screen position of the tile origin, 1/16 px */
    int32_t cu0, cv0, cu1, cv1;    /* visible part of the screen in tile units (culling) */
} vis_t;

#define CULL_MARGIN 48         /* px around the screen: widest casing, label anchors */

static int32_t *pts;
static int pts_cap;
static int *rlen;
static int rlen_cap;

static int ensure(int npts, int nrings)
{
    if (npts * 2 > pts_cap) {
        int n = pts_cap ? pts_cap : 4096;
        while (n < npts * 2) n *= 2;
        int32_t *q = realloc(pts, (size_t)n * sizeof *q);
        if (!q) return -1;
        pts = q;
        pts_cap = n;
    }
    if (nrings > rlen_cap) {
        int n = rlen_cap ? rlen_cap : 64;
        while (n < nrings) n *= 2;
        int *q = realloc(rlen, (size_t)n * sizeof *q);
        if (!q) return -1;
        rlen = q;
        rlen_cap = n;
    }
    return 0;
}

/* transform one feature's rings into pts/rlen; returns the point count */
static int transform(const vis_t *vt, const bgm_feat_t *f, int64_t A, int64_t B)
{
    const bgm_tile_t *t = vt->t;
    uint32_t r0 = f->ring, r1 = f->ring + f->nrings;
    int n = (int)(t->ring_start[r1] - t->ring_start[r0]);
    if (ensure(n, f->nrings)) return 0;
    const int16_t *p = t->pts + 2 * t->ring_start[r0];
    int32_t *o = pts;
    for (uint32_t r = r0; r < r1; r++) rlen[r - r0] = (int)(t->ring_start[r + 1] - t->ring_start[r]);
    if (!B) {
        for (int i = 0; i < n; i++, p += 2, o += 2) {
            o[0] = vt->ox16 + (int32_t)((p[0] * A) >> 16);
            o[1] = vt->oy16 + (int32_t)((p[1] * A) >> 16);
        }
    } else {
        for (int i = 0; i < n; i++, p += 2, o += 2) {
            o[0] = vt->ox16 + (int32_t)((p[0] * A - p[1] * B) >> 16);
            o[1] = vt->oy16 + (int32_t)((p[0] * B + p[1] * A) >> 16);
        }
    }
    return n;
}

/* clip p..q (64-bit screen units) to |v| <= L; 0 if outside */
static int clip64(int64_t *px, int64_t *py, int64_t *qx, int64_t *qy, int64_t L)
{
    double x0 = (double)*px, y0 = (double)*py, dx = (double)(*qx - *px), dy = (double)(*qy - *py), t0 = 0, t1 = 1;
    double p_[4] = {-dx, dx, -dy, dy}, q_[4] = {x0 + L, L - x0, y0 + L, L - y0};
    for (int k = 0; k < 4; k++) {
        if (p_[k] == 0) { if (q_[k] < 0) return 0; continue; }
        double t = q_[k] / p_[k];
        if (p_[k] < 0) { if (t > t1) return 0; if (t > t0) t0 = t; }
        else { if (t < t0) return 0; if (t < t1) t1 = t; }
    }
    int64_t ax = (int64_t)(x0 + t0 * dx), ay = (int64_t)(y0 + t0 * dy), bx = (int64_t)(x0 + t1 * dx), by = (int64_t)(y0 + t1 * dy);
    *px = ax; *py = ay; *qx = bx; *qy = by;
    return 1;
}

void bgr_draw_polyline(const bgr_view_t *v, rast_surface_t *s, const uint32_t *xy, int n,
                       int w16, uint16_t color, int cw16, uint16_t casing)
{
    if (n < 2) return;
    xf_t x;
    xf_init(v, s, &x);
    /* per point: 64-bit multiply and shift; screen = centre + R * (p - c) * k, Q32 */
    double k16 = x.k * RAST_SUB * 4294967296.0;
    int64_t A = llround(k16 * x.c), B = llround(k16 * x.s);
    int64_t sx0 = llround(x.sx0 * RAST_SUB), sy0 = llround(x.sy0 * RAST_SUB);
    const int64_t LIM = (int64_t)1 << 26;            /* fits int32 for rast_line, far beyond the screen */
    int32_t buf[2 * 512];
    for (int pass = cw16 ? 0 : 1; pass < 2; pass++) {
        int m = 0;
        int64_t px = 0, py = 0;
        for (int i = 0; i < n; i++) {
            int64_t dx = (int32_t)(xy[2 * i] - v->cx), dy = (int32_t)(xy[2 * i + 1] - v->cy);
            int64_t qx = sx0 + ((dx * A - dy * B) >> 32), qy = sy0 + ((dx * B + dy * A) >> 32);
            if (i > 0) {
                int64_t ax = px, ay = py, bx = qx, by = qy;
                int inside = ax >= -LIM && ax <= LIM && ay >= -LIM && ay <= LIM && bx >= -LIM && bx <= LIM && by >= -LIM && by <= LIM;
                if (inside || clip64(&ax, &ay, &bx, &by, LIM)) {
                    int joined = m > 0 && buf[2 * m - 2] == (int32_t)ax && buf[2 * m - 1] == (int32_t)ay;
                    if (!joined) {                    /* new piece */
                        if (m > 1) rast_line(s, buf, m, pass ? w16 : cw16, pass ? color : casing, 0);
                        m = 0;
                        buf[0] = (int32_t)ax; buf[1] = (int32_t)ay;
                        m = 1;
                    }
                    buf[2 * m] = (int32_t)bx; buf[2 * m + 1] = (int32_t)by;
                    m++;
                    if (m == 512) {                   /* full: draw, keep the last point */
                        rast_line(s, buf, m, pass ? w16 : cw16, pass ? color : casing, 0);
                        buf[0] = buf[2 * m - 2]; buf[1] = buf[2 * m - 1];
                        m = 1;
                    }
                }
            }
            px = qx;
            py = qy;
        }
        if (m > 1) rast_line(s, buf, m, pass ? w16 : cw16, pass ? color : casing, 0);
    }
}

static int label_cmp(const void *a, const void *b)
{
    return (int)((const bgr_label_t *)a)->prio - (int)((const bgr_label_t *)b)->prio;
}

int bgr_render(bgm_pack_t *p, const bgr_view_t *v, rast_surface_t *s, bgr_label_t *labels, int max_labels, bgr_stats_t *st)
{
    uint32_t t0 = now_us();
    bgr_stats_t stats = {0};
    int zoom256 = v->zoom256 < BGR_ZOOM_MIN ? BGR_ZOOM_MIN : v->zoom256 > BGR_ZOOM_MAX ? BGR_ZOOM_MAX : v->zoom256;
    bgr_view_t vv = *v;
    vv.zoom256 = zoom256;
    int z = zoom256 >> 8;
    int night = v->night;
    rast_clear(s, bgr_background(night));
    stats.us_clear = now_us() - t0;

    int level = bgm_level_for_zoom(p, z);
    int lz = bgm_level_zoom(p, level);
    xf_t x;
    xf_init(&vv, s, &x);
    double unit = ldexp(1.0, 32 - lz - BGM_EXTENT_BITS);
    double a16 = unit * x.k * RAST_SUB;
    int32_t tiny = (int32_t)(2.0 * RAST_SUB / a16);   /* 2 px in tile units */
    int64_t A = llround(a16 * x.c * 65536.0), B = llround(a16 * x.s * 65536.0);
    if (!v->rot) B = 0;

    /* visible tiles: everything within the distance from the centre point to the farthest corner */
    double fx = fmax(x.sx0, s->w - x.sx0), fy = fmax(x.sy0, s->h - x.sy0);
    double rw = sqrt(fx * fx + fy * fy) / x.k;           /* world units */
    int sh = 32 - lz;
    int64_t maxt = ((int64_t)1 << lz) - 1;
    int64_t tx0 = ((int64_t)v->cx - (int64_t)rw) >> sh, tx1 = ((int64_t)v->cx + (int64_t)rw) >> sh;
    int64_t ty0 = ((int64_t)v->cy - (int64_t)rw) >> sh, ty1 = ((int64_t)v->cy + (int64_t)rw) >> sh;
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 > maxt) tx1 = maxt;
    if (ty1 > maxt) ty1 = maxt;

    enum { MAXVIS = 96 };
    vis_t vis[MAXVIS];
    int nvis = 0;
    bgm_frame(p);
    for (int64_t ty = ty0; ty <= ty1; ty++)
        for (int64_t tx = tx0; tx <= tx1 && nvis < MAXVIS; tx++) {
            const bgm_tile_t *t = bgm_tile(p, level, (int)tx, (int)ty);
            if (!t) continue;
            double dx = (double)(int32_t)((uint32_t)(tx << sh) - v->cx), dy = (double)(int32_t)((uint32_t)(ty << sh) - v->cy);
            vis[nvis].t = t;
            vis[nvis].ox16 = (int32_t)lround((x.sx0 + (dx * x.c - dy * x.s) * x.k) * RAST_SUB);
            vis[nvis].oy16 = (int32_t)lround((x.sy0 + (dx * x.s + dy * x.c) * x.k) * RAST_SUB);
            /* screen corners (with margin) back to tile units: inverse rotation and scale */
            double u0 = 1e9, v0 = 1e9, u1 = -1e9, v1 = -1e9;
            for (int c = 0; c < 4; c++) {
                double px = ((c & 1) ? s->w + CULL_MARGIN : -CULL_MARGIN) * RAST_SUB - vis[nvis].ox16;
                double py = ((c & 2) ? s->h + CULL_MARGIN : -CULL_MARGIN) * RAST_SUB - vis[nvis].oy16;
                double u = (px * x.c + py * x.s) / a16, w_ = (-px * x.s + py * x.c) / a16;
                u0 = fmin(u0, u); u1 = fmax(u1, u); v0 = fmin(v0, w_); v1 = fmax(v1, w_);
            }
            vis[nvis].cu0 = (int32_t)fmax(u0 - 1, -32768); vis[nvis].cu1 = (int32_t)fmin(u1 + 1, 32767);
            vis[nvis].cv0 = (int32_t)fmax(v0 - 1, -32768); vis[nvis].cv1 = (int32_t)fmin(v1 + 1, 32767);
            if (vis[nvis].cu0 > vis[nvis].cu1 || vis[nvis].cv0 > vis[nvis].cv1) continue;
            nvis++;
        }
    uint32_t t1 = now_us();
    stats.us_fetch = t1 - t0;
    stats.tiles = (unsigned)nvis;

    /* passes: 0 areas, 1 lines without casing, 2 road casings, 3 road fills */
    static const struct { int k0, k1, casing; } passes[] = {
        {BGK_A_RESIDENTIAL, BGK_A_BUILDING, 0},
        {BGK_L_STREAM, BGK_L_PATH - 1, 0},
        {BGK_L_PATH, BGK_L_MOTORWAY, 1},
        {BGK_L_PATH, BGK_L_FERRY, 0},
    };
    for (unsigned pi = 0; pi < sizeof passes / sizeof passes[0]; pi++) {
        for (int k = passes[pi].k0; k <= passes[pi].k1; k++) {
            if (bgr_kind_minzoom(k, v->detail) > z) continue;
            if (passes[pi].casing && !casing_on(k, v->detail, z)) continue;
            uint32_t tk = now_us();
            const style_t *sty = &ST[k];
            int area = k < BGK_L_STREAM;
            int w16 = area ? 0 : width16(k, zoom256);
            if (!area && !w16) continue;
            uint16_t col;
            int cw16 = 0;
            if (passes[pi].casing) {
                uint32_t cc = night ? sty->cnight : sty->cday;
                if (!cc || w16 < RAST_SUB * 4) continue;
                col = rgb565(cc);
                cw16 = w16 + (z >= 16 ? RAST_SUB * 3 : RAST_SUB * 2);
            } else {
                col = rgb565(night ? sty->night : sty->day);
                /* without its casing a white minor road disappears on the light
                 * background: draw it closer to the casing colour */
                if (pi == 3 && !night && sty->cday && !casing_on(k, v->detail, z)) {
                    uint32_t a = sty->day, b = sty->cday;          /* meio-termo entre as duas cores */
                    col = rgb565((((a >> 16 & 255) + (b >> 16 & 255) * 2) / 3) << 16 |
                                 (((a >> 8 & 255) + (b >> 8 & 255) * 2) / 3) << 8 | ((a & 255) + (b & 255) * 2) / 3);
                }
            }
            for (int vi = 0; vi < nvis; vi++) {
                const vis_t *vt = &vis[vi];
                const bgm_tile_t *t = vt->t;
                for (int fi = t->kind_first[k]; fi < t->kind_first[k + 1]; fi++) {
                    const bgm_feat_t *f = &t->feat[fi];
                    if (f->bx1 < vt->cu0 || f->bx0 > vt->cu1 || f->by1 < vt->cv0 || f->by0 > vt->cv1) continue;
                    /* areas smaller than 2 px on screen (dense blocks of buildings at zoom 15):
                     * invisible detail, most of the drawing time in big cities */
                    if (area && f->bx1 - f->bx0 < tiny && f->by1 - f->by0 < tiny) continue;
                    int n = transform(vt, f, A, B);
                    if (!n) continue;
                    stats.feats++;
                    stats.pts += (unsigned)n;
                    if (area) {
                        rast_fill(s, pts, rlen, f->nrings, col);
                    } else {
                        int w = passes[pi].casing ? cw16 : w16;
                        if (f->flags & BGM_F_LINK) w = w * 3 / 4;
                        rast_line(s, pts, n, w, col, sty->dash);
                    }
                }
            }
            stats.us_kind[k] += now_us() - tk;
        }
    }
    uint32_t t2 = now_us();
    stats.us_draw = t2 - t1;

    /* labels */
    int nl = 0;
    for (int vi = 0; vi < nvis && max_labels > 0; vi++) {
        const vis_t *vt = &vis[vi];
        const bgm_tile_t *t = vt->t;
        for (int fi = 0; fi < t->nfeat; fi++) {
            const bgm_feat_t *f = &t->feat[fi];
            if (f->name == 0xFFFF || bgr_kind_minzoom(f->kind, v->detail) > z) continue;
            if (f->bx1 < vt->cu0 || f->bx0 > vt->cu1 || f->by1 < vt->cv0 || f->by0 > vt->cv1) continue;
            int g = f->flags & BGM_F_GEOM;
            if (g == BGM_F_AREA) continue;
            bgr_label_t L = {0};
            L.kind = f->kind;
            L.prio = LABEL_PRIO[f->kind] ? LABEL_PRIO[f->kind] : 13;
            L.text = t->names[f->name];
            int n = transform(vt, f, A, B);
            if (g == BGM_F_POINT) {
                L.x = (int16_t)(pts[0] >> RAST_SHIFT);
                L.y = (int16_t)(pts[1] >> RAST_SHIFT);
                if (L.x < -100 || L.y < -20 || L.x > s->w + 100 || L.y > s->h + 20) continue;
            } else {
                if (z < 13 && f->kind < BGK_L_SECONDARY) continue;
                if (z < 15 && f->kind < BGK_L_TERTIARY) continue;
                /* longest visible segment */
                int64_t best = 0;
                int bi = -1;
                for (int i = 0; i + 1 < n; i++) {
                    int32_t ax = pts[2 * i], ay = pts[2 * i + 1], bx = pts[2 * i + 2], by = pts[2 * i + 3];
                    int32_t mx = (ax + bx) >> (RAST_SHIFT + 1), my = (ay + by) >> (RAST_SHIFT + 1);
                    if (mx < 20 || my < 20 || mx > s->w - 20 || my > s->h - 20) continue;
                    int64_t dx = bx - ax, dy = by - ay, d = dx * dx + dy * dy;
                    if (d > best) { best = d; bi = i; }
                }
                if (bi < 0 || best < (int64_t)(70 * RAST_SUB) * (70 * RAST_SUB)) continue;
                int32_t ax = pts[2 * bi], ay = pts[2 * bi + 1], bx = pts[2 * bi + 2], by = pts[2 * bi + 3];
                L.x = (int16_t)((ax + bx) >> (RAST_SHIFT + 1));
                L.y = (int16_t)((ay + by) >> (RAST_SHIFT + 1));
                int ang = iatan2(by - ay, bx - ax);
                if (ang > 90) ang -= 180;
                if (ang < -90) ang += 180;
                L.angle = (int16_t)ang;
            }
            if (nl < max_labels) labels[nl++] = L;
            else {
                /* replace the worst candidate if this one is better */
                int wi = 0;
                for (int i = 1; i < nl; i++) if (labels[i].prio > labels[wi].prio) wi = i;
                if (labels[wi].prio > L.prio) labels[wi] = L;
            }
        }
    }
    qsort(labels, (size_t)nl, sizeof *labels, label_cmp);
    stats.labels = (unsigned)nl;
    if (st) *st = stats;
    return nl;
}
