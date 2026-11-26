/* bgvehicle.c - see bgvehicle.h.  Each model is a few layers (rounded rectangles, ellipses,
 * polygons), painted bottom to top; a dark outline layer under the body keeps it readable on
 * any map colour.  Runs once per change of the marker, never per frame.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgvehicle.h"
#include "bgroute_format.h"
#include <math.h>
#include <string.h>

const uint32_t bgveh_color[BGVEH_COLORS] = {
    0x1a73e8, 0xd93025, 0x202124, 0xf1f3f4, 0x9aa0a6, 0x188038, 0xf9ab00, 0xe8710a,
};

int bgveh_models(int profile, const int **models)
{
    static const int car[] = { BGVEH_ARROW, BGVEH_SEDAN, BGVEH_COMPACT, BGVEH_SUV, BGVEH_SPORT };
    static const int moto[] = { BGVEH_ARROW, BGVEH_MOTO, BGVEH_DOT };
    static const int bike[] = { BGVEH_ARROW, BGVEH_BIKE, BGVEH_DOT };
    static const int foot[] = { BGVEH_DOT, BGVEH_WALKER, BGVEH_ARROW };
    switch (profile) {
    case BGR_MOTO: *models = moto; return 3;
    case BGR_BIKE: *models = bike; return 3;
    case BGR_FOOT: *models = foot; return 3;
    default: *models = car; return 5;
    }
}

enum { L_RRECT, L_ELLIPSE, L_POLY };
typedef struct {
    int kind;
    float a, b, c, d, r;       /* rrect: x0 y0 x1 y1 radius; ellipse: cx cy rx ry */
    const float *pts;          /* poly: n points */
    int n;
    int role;                  /* colour role, see paint() */
} layer_t;

enum { R_OUTLINE, R_BODY, R_GLASS, R_LIGHT, R_WHITE, R_DARK, R_ROOF, R_TIRE, R_RED, R_PURE, R_CLEAR, R_ROLES };

#define RR(x0, y0, x1, y1, r, role) { L_RRECT, x0, y0, x1, y1, r, 0, 0, role }
#define EL(cx, cy, rx, ry, role) { L_ELLIPSE, cx, cy, rx, ry, 0, 0, 0, role }
#define PO(p, role) { L_POLY, 0, 0, 0, 0, 0, p, (int)(sizeof p / sizeof p[0] / 2), role }

/* contornos de 1,6 px: a mesma forma, um pouco maior, por baixo */
static const float arrow_o[] = { 24, 1, 42, 45, 24, 35, 6, 45 }, arrow_i[] = { 24, 6.5f, 37.5f, 40.5f, 24, 32.5f, 10.5f, 40.5f };
static const float sport_o[] = { 24, 1.5f, 34, 10, 35, 44, 13, 44, 14, 10 }, sport_b[] = { 24, 3, 32.5f, 10.5f, 33.5f, 42.5f, 14.5f, 42.5f, 15.5f, 10.5f };
static const float sport_g[] = { 17.5f, 15, 30.5f, 15, 29, 22, 19, 22 };
static const float walk_nose[] = { 24, 8, 28, 15, 20, 15 };
static const float pin_tail[] = { 11, 18, 37, 18, 24, 46 };
static const float mag_handle[] = { 20.7f, 17.3f, 30.2f, 26.8f, 26.8f, 30.2f, 17.3f, 20.7f };

static const layer_t L_ARROW[] = { PO(arrow_o, R_WHITE), PO(arrow_i, R_BODY) };
static const layer_t L_DOT[] = { EL(24, 24, 12, 12, R_WHITE), EL(24, 24, 8.5f, 8.5f, R_BODY) };
static const layer_t L_SEDAN[] = {
    RR(12.4f, 2.4f, 35.6f, 45.6f, 8.6f, R_OUTLINE), RR(10, 14, 38, 17, 1.5f, R_OUTLINE),     /* espelhos */
    RR(14, 4, 34, 44, 7, R_BODY), RR(11, 14.6f, 37, 16.4f, 1, R_BODY),
    RR(16.5f, 14, 31.5f, 21, 2.5f, R_GLASS), RR(17, 21, 31, 31, 2, R_ROOF), RR(16.5f, 31, 31.5f, 37, 2.5f, R_GLASS),
    EL(17.5f, 5.8f, 2.2f, 1.2f, R_LIGHT), EL(30.5f, 5.8f, 2.2f, 1.2f, R_LIGHT),
};
static const layer_t L_COMPACT[] = {
    RR(12.4f, 6.4f, 35.6f, 41.6f, 9.6f, R_OUTLINE), RR(10.5f, 15, 37.5f, 18, 1.5f, R_OUTLINE),
    RR(14, 8, 34, 40, 8, R_BODY), RR(11.5f, 15.6f, 36.5f, 17.4f, 1, R_BODY),
    RR(16.5f, 14.5f, 31.5f, 21, 2.5f, R_GLASS), RR(17, 21, 31, 30, 2, R_ROOF), RR(16.5f, 30, 31.5f, 35, 2.5f, R_GLASS),
    EL(17.5f, 9.8f, 2, 1.1f, R_LIGHT), EL(30.5f, 9.8f, 2, 1.1f, R_LIGHT),
};
static const layer_t L_SUV[] = {
    RR(11.4f, 2.4f, 36.6f, 45.6f, 5.6f, R_OUTLINE), RR(9.5f, 13, 38.5f, 16, 1.5f, R_OUTLINE),
    RR(13, 4, 35, 44, 4, R_BODY), RR(10.5f, 13.6f, 37.5f, 15.4f, 1, R_BODY),
    RR(15.5f, 12, 32.5f, 18, 1.5f, R_GLASS), RR(16, 18, 32, 38, 1.5f, R_ROOF),
    RR(16.5f, 19.5f, 17.8f, 36.5f, 0.6f, R_DARK), RR(30.2f, 19.5f, 31.5f, 36.5f, 0.6f, R_DARK),  /* bagageiro */
    RR(15.5f, 38, 32.5f, 42, 1.5f, R_GLASS),
    RR(15.5f, 5, 19.5f, 6.8f, 0.8f, R_LIGHT), RR(28.5f, 5, 32.5f, 6.8f, 0.8f, R_LIGHT),
};
static const layer_t L_SPORT[] = {
    PO(sport_o, R_OUTLINE), PO(sport_b, R_BODY), PO(sport_g, R_GLASS),
    RR(19, 22, 29, 33, 2, R_ROOF), RR(19, 33, 29, 36, 1.2f, R_GLASS),
    RR(17, 39.5f, 31, 41, 0.6f, R_DARK),                                                         /* aerofólio */
};
static const layer_t L_MOTO[] = {
    EL(24, 24, 6.2f, 20, R_OUTLINE), RR(12.6f, 11.6f, 35.4f, 15.4f, 1.9f, R_OUTLINE),
    EL(24, 24, 4.6f, 18.4f, R_BODY), RR(14, 12.5f, 34, 14.5f, 1, R_DARK),                          /* guidão */
    EL(24, 7.5f, 1.8f, 1.2f, R_LIGHT), EL(24, 25, 5.6f, 6, R_DARK), EL(24, 24, 4, 4.2f, R_BODY),   /* capacete */
};
static const layer_t L_BIKE[] = {
    RR(21.6f, 3.6f, 26.4f, 17.4f, 2.4f, R_OUTLINE), RR(21.6f, 30.6f, 26.4f, 44.4f, 2.4f, R_OUTLINE),
    RR(22.6f, 14, 25.4f, 34, 1.2f, R_OUTLINE), RR(14.6f, 10.6f, 33.4f, 13.4f, 1.4f, R_OUTLINE),
    RR(22.8f, 4.8f, 25.2f, 16.2f, 1.2f, R_TIRE), RR(22.8f, 31.8f, 25.2f, 43.2f, 1.2f, R_TIRE),     /* rodas */
    RR(23.2f, 14, 24.8f, 34, 0.8f, R_BODY), RR(15.8f, 11.4f, 32.2f, 12.6f, 0.6f, R_BODY),         /* quadro, guidão */
    EL(24, 24, 6.6f, 4.4f, R_OUTLINE), EL(24, 24, 5.2f, 3.2f, R_BODY), EL(24, 22.5f, 3.4f, 3.4f, R_DARK),
};
static const layer_t L_WALKER[] = {
    PO(walk_nose, R_BODY),
    EL(24, 26, 12.4f, 6.4f, R_OUTLINE), EL(24, 26, 10.8f, 4.8f, R_BODY),                          /* ombros */
    EL(24, 24.5f, 6.6f, 6.6f, R_OUTLINE), EL(24, 24.5f, 5, 5, R_DARK),                            /* cabeça */
};

static const layer_t L_PIN[] = { EL(24, 18, 13, 13, R_RED), PO(pin_tail, R_RED), EL(24, 18, 5, 5, R_PURE) };
static const layer_t L_MAG[] = { EL(13, 13, 9.5f, 9.5f, R_PURE), EL(13, 13, 6.5f, 6.5f, R_CLEAR), PO(mag_handle, R_PURE) };

static uint32_t mix(uint32_t a, uint32_t b, float t)   /* a -> b */
{
    int ra = a >> 16 & 255, ga = a >> 8 & 255, ba = a & 255, rb = b >> 16 & 255, gb = b >> 8 & 255, bb = b & 255;
    return (uint32_t)lroundf(ra + (rb - ra) * t) << 16 | (uint32_t)lroundf(ga + (gb - ga) * t) << 8 | (uint32_t)lroundf(ba + (bb - ba) * t);
}

static uint32_t role_color(int role, uint32_t body)
{
    int lum = (int)((body >> 16 & 255) * 3 + (body >> 8 & 255) * 6 + (body & 255)) / 10;
    switch (role) {
    case R_OUTLINE: return lum < 70 ? 0xbdc1c6 : 0x1b1d21;    /* contorno claro em carro escuro */
    case R_GLASS: return 0x24303c;
    case R_LIGHT: return 0xfff4c2;
    case R_WHITE: return lum > 200 ? 0x3c4043 : 0xffffff;     /* seta branca: borda escura */
    case R_TIRE: return 0x80868b;                             /* rodas visíveis no mapa claro e no escuro */
    case R_RED: return 0xd93025;
    case R_PURE: return 0xffffff;
    case R_DARK: return lum < 70 ? 0x5f6368 : 0x202124;
    case R_ROOF: return mix(body, lum > 200 ? 0x000000 : 0xffffff, lum > 200 ? 0.08f : 0.18f);
    default: return body;
    }
}

/* the layers in integers (1/16 px) with their bounding boxes: the device has no FPU, so the
 * per-sample tests must not touch floating point (8 draws of 48x48x16 samples ~ 100 ms) */
#define FX 16
typedef struct {
    int kind, role, x0, y0, x1, y1;          /* box (also the rrect / ellipse extent) */
    int r, cx, cy, rx, ry;                   /* rrect radius; ellipse centre and radii */
    int n, p[16];                            /* polygon */
} ilayer_t;

static int fx(float v) { return (int)lroundf(v * FX); }

static void to_int(const layer_t *l, ilayer_t *o)
{
    memset(o, 0, sizeof *o);
    o->kind = l->kind;
    o->role = l->role;
    if (l->kind == L_RRECT) {
        o->x0 = fx(l->a); o->y0 = fx(l->b); o->x1 = fx(l->c); o->y1 = fx(l->d); o->r = fx(l->r);
    } else if (l->kind == L_ELLIPSE) {
        o->cx = fx(l->a); o->cy = fx(l->b); o->rx = fx(l->c); o->ry = fx(l->d);
        o->x0 = o->cx - o->rx; o->x1 = o->cx + o->rx; o->y0 = o->cy - o->ry; o->y1 = o->cy + o->ry;
    } else {
        o->n = l->n > 8 ? 8 : l->n;
        o->x0 = o->y0 = 1 << 30; o->x1 = o->y1 = -(1 << 30);
        for (int i = 0; i < o->n; i++) {
            int x = fx(l->pts[2 * i]), y = fx(l->pts[2 * i + 1]);
            o->p[2 * i] = x; o->p[2 * i + 1] = y;
            if (x < o->x0) o->x0 = x;
            if (x > o->x1) o->x1 = x;
            if (y < o->y0) o->y0 = y;
            if (y > o->y1) o->y1 = y;
        }
    }
}

static int inside(const ilayer_t *l, int x, int y)
{
    if (x < l->x0 || x > l->x1 || y < l->y0 || y > l->y1) return 0;
    if (l->kind == L_RRECT) {
        int r = l->r, cx = x < l->x0 + r ? l->x0 + r : x > l->x1 - r ? l->x1 - r : x;
        int cy = y < l->y0 + r ? l->y0 + r : y > l->y1 - r ? l->y1 - r : y;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }
    if (l->kind == L_ELLIPSE) {
        int64_t dx = (int64_t)(x - l->cx) * l->ry, dy = (int64_t)(y - l->cy) * l->rx, rr = (int64_t)l->rx * l->ry;
        return dx * dx + dy * dy <= rr * rr;
    }
    int in = 0;
    for (int i = 0, j = l->n - 1; i < l->n; j = i++) {
        int xi = l->p[2 * i], yi = l->p[2 * i + 1], xj = l->p[2 * j], yj = l->p[2 * j + 1];
        if ((yi > y) == (yj > y)) continue;
        /* x < (xj - xi) * (y - yi) / (yj - yi) + xi, without the division */
        int64_t lhs = (int64_t)(x - xi) * (yj - yi), rhs = (int64_t)(xj - xi) * (y - yi);
        if (yj > yi ? lhs < rhs : lhs > rhs) in = !in;
    }
    return in;
}

void bgveh_draw(uint32_t *px, int model, uint32_t color, int grey) { bgveh_draw_n(px, BGVEH_SIZE, model, color, grey); }

void bgveh_draw_n(uint32_t *px, int side, int model, uint32_t color, int grey)
{
    const layer_t *ls;
    int n;
    switch (model) {
    case BGVEH_DOT: ls = L_DOT; n = sizeof L_DOT / sizeof L_DOT[0]; break;
    case BGVEH_SEDAN: ls = L_SEDAN; n = sizeof L_SEDAN / sizeof L_SEDAN[0]; break;
    case BGVEH_COMPACT: ls = L_COMPACT; n = sizeof L_COMPACT / sizeof L_COMPACT[0]; break;
    case BGVEH_SUV: ls = L_SUV; n = sizeof L_SUV / sizeof L_SUV[0]; break;
    case BGVEH_SPORT: ls = L_SPORT; n = sizeof L_SPORT / sizeof L_SPORT[0]; break;
    case BGVEH_MOTO: ls = L_MOTO; n = sizeof L_MOTO / sizeof L_MOTO[0]; break;
    case BGVEH_BIKE: ls = L_BIKE; n = sizeof L_BIKE / sizeof L_BIKE[0]; break;
    case BGVEH_WALKER: ls = L_WALKER; n = sizeof L_WALKER / sizeof L_WALKER[0]; break;
    case BGVEH_PIN: ls = L_PIN; n = sizeof L_PIN / sizeof L_PIN[0]; break;
    case BGVEH_MAGNIFIER: ls = L_MAG; n = sizeof L_MAG / sizeof L_MAG[0]; break;
    default: ls = L_ARROW; n = sizeof L_ARROW / sizeof L_ARROW[0]; break;
    }
    if (grey) color = 0x8a8f98;
    uint32_t cols[R_ROLES];
    for (int r = 0; r < R_ROLES; r++) cols[r] = role_color(r, color);
    ilayer_t il[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) to_int(&ls[i], &il[i]);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            int cnt = 0, sr = 0, sg = 0, sb = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int qx = x * FX + sx * 4 + 2, qy = y * FX + sy * 4 + 2;   /* 4x4 amostras por pixel */
                    int top = -1;
                    for (int i = 0; i < n; i++)
                        if (inside(&il[i], qx, qy)) top = i;
                    if (top < 0 || il[top].role == R_CLEAR) continue;
                    uint32_t c = cols[il[top].role];
                    cnt++;
                    sr += c >> 16 & 255; sg += c >> 8 & 255; sb += c & 255;
                }
            if (!cnt) { px[y * side + x] = 0; continue; }
            uint32_t a = (uint32_t)(cnt * 255 / 16);
            if (grey) a = a * 3 / 4;
            px[y * side + x] = a << 24 | (uint32_t)(sr / cnt) << 16 | (uint32_t)(sg / cnt) << 8 | (uint32_t)(sb / cnt);
        }
}
