/* raster.c - see raster.h.
 * Scanline polygon fill with an edge table (one 32-bit division per edge: the ARM1176 has
 * no hardware divide), polygons clipped to the surface first so all per-scanline math is
 * bounded; thick lines as convex quads plus octagon joins; thin lines with Bresenham.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "raster.h"
#include <stdlib.h>
#include <string.h>

/* x in 1/16 px << 15 fits 32 bits because polygons are clipped to the surface first
 * (|x| < 13000 << 15); dx (per scanline) too, since an edge that crosses two scanline
 * centres is at least 16 units tall. */
typedef struct {
    int32_t x, dx;             /* x at the current scanline, step per scanline */
    int y1;                    /* first scanline below the edge */
    int next;                  /* bucket link */
} edge_t;

typedef struct {
    int32_t x, dx;
    int y1;
} active_t;


static edge_t *edges;
static int edge_cap;
static int *bucket;
static int bucket_cap;
static int32_t *clip_a, *clip_b;
static int clip_cap;
static active_t *act;
static int act_cap;

static int grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return 0;
    int n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return -1;
    *p = q;
    *cap = n;
    return 0;
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* (num << 15) / d with a reciprocal table for d < RECIP_N: the ARM1176 has no divide
 * instruction and these slopes are computed for every edge of every polygon */
#define RECIP_N 8192
static int32_t *recip;

static int32_t slope_q15(int32_t num, int32_t d)
{
    if (d < RECIP_N && d > 0) {
        if (!recip) {
            recip = malloc(RECIP_N * sizeof *recip);
            if (recip)
                for (int i = 1; i < RECIP_N; i++) recip[i] = (int32_t)(((1u << 24) + (unsigned)i / 2) / (unsigned)i);
        }
        if (recip) return (int32_t)(((int64_t)num * recip[d]) >> 9);
    }
    return num * 32768 / d;
}

static void span(rast_surface_t *s, int y, int x0, int x1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (x0 >= x1) return;
    uint16_t *p = s->px + (size_t)y * s->stride + x0;
    int n = x1 - x0;
    if (((uintptr_t)p & 2) && n) { *p++ = c; n--; }
    uint32_t c2 = (uint32_t)c | (uint32_t)c << 16;
    uint32_t *q = (uint32_t *)p;
    while (n >= 8) { q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2; q += 4; n -= 8; }
    while (n >= 2) { *q++ = c2; n -= 2; }
    if (n) *(uint16_t *)q = c;
}

void rast_clear(rast_surface_t *s, uint16_t c)
{
    for (int y = 0; y < s->h; y++) span(s, y, 0, s->w, c);
}

/* Sutherland-Hodgman on one edge; out must hold 2*n points */
static int clip_edge(const int32_t *in, int n, int32_t *out, int axis, int32_t v, int keep_ge)
{
    int m = 0;
    if (n == 0) return 0;
    const int32_t *pv = in + 2 * (n - 1);
    int pin = keep_ge ? pv[axis] >= v : pv[axis] <= v;
    for (int i = 0; i < n; i++) {
        const int32_t *cv = in + 2 * i;
        int cin = keep_ge ? cv[axis] >= v : cv[axis] <= v;
        if (cin != pin) {
            int o = axis ^ 1;
            int64_t num = (int64_t)(v - pv[axis]) * (cv[o] - pv[o]);
            int32_t d = cv[axis] - pv[axis];
            out[2 * m + axis] = v;
            out[2 * m + o] = pv[o] + (int32_t)(num / d);
            m++;
        }
        if (cin) { out[2 * m] = cv[0]; out[2 * m + 1] = cv[1]; m++; }
        pv = cv;
        pin = cin;
    }
    return m;
}

void rast_fill(rast_surface_t *s, const int32_t *xy, const int *ring_len, int nrings, uint16_t color)
{
    const int32_t L = -RAST_SUB, T = -RAST_SUB, R = (s->w + 1) * RAST_SUB, B = (s->h + 1) * RAST_SUB;
    int ne = 0, total = 0;
    for (int r = 0; r < nrings; r++) total += ring_len[r];
    if (grow((void **)&edges, &edge_cap, total * 2 + 8, sizeof(edge_t))) return;
    if (s->h + 1 > bucket_cap) {                 /* buckets stay -1 between calls */
        int old = bucket_cap;
        if (grow((void **)&bucket, &bucket_cap, s->h + 1, sizeof(int))) return;
        for (int y = old; y < bucket_cap; y++) bucket[y] = -1;
    }
    int ymin = s->h, ymax = 0;                   /* scanlines touched by edges: [ymin, ymax) */

    const int32_t *ring = xy;
    for (int r = 0; r < nrings; ring += 2 * ring_len[r], r++) {
        int n = ring_len[r];
        if (n < 3) continue;
        int32_t x0 = ring[0], x1 = ring[0], y0 = ring[1], y1 = ring[1];
        for (int i = 1; i < n; i++) {
            int32_t x = ring[2 * i], y = ring[2 * i + 1];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        if (x1 < L || x0 > R || y1 < T || y0 > B) continue;   /* even-odd: an outside ring changes nothing */
        const int32_t *p = ring;
        if (x0 < L || x1 > R || y0 < T || y1 > B) {
            int cap = n * 4 + 16;
            if (cap * 2 > clip_cap) {
                int nc = cap * 2;
                int32_t *a = realloc(clip_a, (size_t)nc * sizeof(int32_t)), *b;
                if (!a) goto fail;
                clip_a = a;
                b = realloc(clip_b, (size_t)nc * sizeof(int32_t));
                if (!b) goto fail;
                clip_b = b;
                clip_cap = nc;
            }
            int m = clip_edge(ring, n, clip_a, 0, L, 1);
            m = clip_edge(clip_a, m, clip_b, 0, R, 0);
            m = clip_edge(clip_b, m, clip_a, 1, T, 1);
            m = clip_edge(clip_a, m, clip_b, 1, B, 0);
            if (m < 3) continue;
            p = clip_b;
            n = m;
            if (grow((void **)&edges, &edge_cap, ne + n + 8, sizeof(edge_t))) goto fail;
        }
        for (int i = 0; i < n; i++) {
            int32_t ax = p[2 * i], ay = p[2 * i + 1];
            int32_t bx = p[2 * ((i + 1) % n)], by = p[2 * ((i + 1) % n) + 1];
            if (ay == by) continue;
            if (ay > by) { int32_t t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
            int yi0 = (ay - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
            int yi1 = (by - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
            if (yi0 < 0) yi0 = 0;
            if (yi1 > s->h) yi1 = s->h;
            if (yi0 >= yi1) continue;
            int32_t slope = (int32_t)((bx - ax) * 32768 / (by - ay));   /* 1/16 px per 1/16 px, Q15 */
            edge_t *e = &edges[ne];
            int32_t yc = yi0 * RAST_SUB + RAST_SUB / 2;
            e->x = (int32_t)((int64_t)ax * 32768 + (int64_t)(yc - ay) * slope);
            e->dx = yi1 - yi0 > 1 ? slope * RAST_SUB : 0;
            e->y1 = yi1;
            e->next = bucket[yi0];
            bucket[yi0] = ne++;
            if (yi0 < ymin) ymin = yi0;
            if (yi1 > ymax) ymax = yi1;
        }
    }
    if (!ne) return;
    /* active edges: a compact array kept sorted by x; between scanlines it stays nearly
     * sorted, so the insertion sort is close to linear even for long polylines */
    if (grow((void **)&act, &act_cap, ne, sizeof(active_t))) goto fail;
    int na = 0;
    for (int y = ymin; y < ymax; y++) {
        for (int ei = bucket[y]; ei >= 0; ei = edges[ei].next) {
            const edge_t *e = &edges[ei];
            active_t a = {e->x, e->dx, e->y1};
            int j = na++;
            while (j > 0 && act[j - 1].x > a.x) { act[j] = act[j - 1]; j--; }
            act[j] = a;
        }
        bucket[y] = -1;
        for (int i = 0; i + 1 < na; i += 2) {
            int a = ((act[i].x >> 15) - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
            int b = ((act[i + 1].x >> 15) - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
            span(s, y, a, b, color);
        }
        /* step to the next scanline, drop finished edges, keep the order */
        int k = 0;
        for (int i = 0; i < na; i++) {
            if (act[i].y1 <= y + 1) continue;
            active_t a = act[i];
            a.x += a.dx;
            int j = k++;
            while (j > 0 && act[j - 1].x > a.x) { act[j] = act[j - 1]; j--; }
            act[j] = a;
        }
        na = k;
    }
    return;
fail:
    for (int y = ymin; y < ymax; y++) bucket[y] = -1;
}

void rast_disc(rast_surface_t *s, int32_t cx, int32_t cy, int r16, uint16_t color)
{
    if (r16 <= 0 || cx + r16 < 0 || cy + r16 < 0 || cx - r16 > s->w * RAST_SUB || cy - r16 > s->h * RAST_SUB) return;
    int y0 = (cy - r16 - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
    int y1 = (cy + r16 - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;
    uint32_t rr = (uint32_t)r16 * (uint32_t)r16;
    for (int y = y0; y < y1; y++) {
        int32_t d = y * RAST_SUB + RAST_SUB / 2 - cy;
        uint32_t dd = (uint32_t)(d * d);
        if (dd >= rr) continue;
        int32_t hw = (int32_t)isqrt32(rr - dd);
        int a = (cx - hw - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
        int b = (cx + hw - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
        span(s, y, a, b, color);
    }
}

/* Liang-Barsky in 1/16 px; returns 0 when the segment is outside */
static int clip_seg(int32_t *a, int32_t *b, int32_t L, int32_t T, int32_t R, int32_t B)
{
    int64_t x0 = a[0], y0 = a[1], dx = (int64_t)b[0] - a[0], dy = (int64_t)b[1] - a[1];
    /* t as a fraction num/den kept in Q16 */
    int32_t t0 = 0, t1 = 65536;
    int64_t p[4] = {-dx, dx, -dy, dy}, q[4] = {x0 - L, R - x0, y0 - T, B - y0};
    for (int k = 0; k < 4; k++) {
        if (p[k] == 0) { if (q[k] < 0) return 0; continue; }
        int64_t t = q[k] * 65536 / p[k];
        if (t > 65536) t = 65537;
        if (t < -1) t = -1;
        if (p[k] < 0) { if (t > t1) return 0; if (t > t0) t0 = (int32_t)t; }
        else { if (t < t0) return 0; if (t < t1) t1 = (int32_t)t; }
    }
    if (t1 < 65536) { b[0] = (int32_t)(x0 + ((dx * t1) >> 16)); b[1] = (int32_t)(y0 + ((dy * t1) >> 16)); }
    if (t0 > 0) { a[0] = (int32_t)(x0 + ((dx * t0) >> 16)); a[1] = (int32_t)(y0 + ((dy * t0) >> 16)); }
    return 1;
}

static void thin_seg(rast_surface_t *s, int32_t ax, int32_t ay, int32_t bx, int32_t by, uint16_t c)
{
    int x0 = ax >> RAST_SHIFT, y0 = ay >> RAST_SHIFT, x1 = bx >> RAST_SHIFT, y1 = by >> RAST_SHIFT;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if ((unsigned)x0 < (unsigned)s->w && (unsigned)y0 < (unsigned)s->h) s->px[(size_t)y0 * s->stride + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Convex polygon (thick line quads and joins): walk the two chains from the top vertex;
 * no edge table, no sorting.  Opaque single colour, so overlapping pieces just overdraw. */
typedef struct {
    const int32_t *p;
    int n, i, step;
    int32_t x, dx, yj;
} chain_t;

static int chain_edge(chain_t *c, int32_t yc, int bot)
{
    for (;;) {
        if (c->i == bot) return 0;
        int j = c->i + c->step;
        if (j < 0) j += c->n;
        else if (j >= c->n) j -= c->n;
        int32_t xi = c->p[2 * c->i], yi = c->p[2 * c->i + 1], xj = c->p[2 * j], yj = c->p[2 * j + 1];
        if (yj > yc) {
            int32_t dy = yj - yi, slope = slope_q15(xj - xi, dy);
            c->x = (int32_t)((int64_t)xi * 32768 + (int64_t)(yc - yi) * slope);
            c->dx = dy >= RAST_SUB ? slope * RAST_SUB : 0;
            c->yj = yj;
            return 1;
        }
        c->i = j;
    }
}

static void fill_convex(rast_surface_t *s, const int32_t *p, int n, uint16_t color)
{
    int top = 0, bot = 0;
    for (int i = 1; i < n; i++) {
        if (p[2 * i + 1] < p[2 * top + 1]) top = i;
        if (p[2 * i + 1] > p[2 * bot + 1]) bot = i;
    }
    int y0 = (p[2 * top + 1] - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
    int y1 = (p[2 * bot + 1] - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT;
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;
    if (y0 >= y1) return;
    int32_t yc = y0 * RAST_SUB + RAST_SUB / 2;
    chain_t a = {p, n, top, 1, 0, 0, 0}, b = {p, n, top, -1, 0, 0, 0};
    if (!chain_edge(&a, yc, bot) || !chain_edge(&b, yc, bot)) return;
    for (int y = y0; y < y1; y++, yc += RAST_SUB) {
        if (yc >= a.yj && !chain_edge(&a, yc, bot)) return;
        if (yc >= b.yj && !chain_edge(&b, yc, bot)) return;
        int32_t xa = a.x >> 15, xb = b.x >> 15;
        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }
        span(s, y, (xa - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT, (xb - RAST_SUB / 2 + RAST_SUB - 1) >> RAST_SHIFT, color);
        a.x += a.dx;
        b.x += b.dx;
    }
}

static void add_quad(rast_surface_t *s, int32_t ax, int32_t ay, int32_t bx, int32_t by, int hw, uint16_t color)
{
    int32_t dx = bx - ax, dy = by - ay;
    uint32_t len = isqrt32((uint32_t)(dx * dx + dy * dy));
    if (!len) return;
    int32_t inv = slope_q15(hw, (int32_t)len);               /* hw / len in Q15 */
    int32_t nx = (int32_t)(((int64_t)-dy * inv) >> 15), ny = (int32_t)(((int64_t)dx * inv) >> 15);
    int32_t q[8] = {ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny};
    fill_convex(s, q, 4, color);
}

static void add_join(rast_surface_t *s, int32_t cx, int32_t cy, int hw, uint16_t color)
{
    int32_t d = hw * 181 / 256;                              /* hw * cos 45 */
    int32_t q[16] = {cx + hw, cy, cx + d, cy - d, cx, cy - hw, cx - d, cy - d,
                     cx - hw, cy, cx - d, cy + d, cx, cy + hw, cx + d, cy + d};
    fill_convex(s, q, 8, color);
}

void rast_line(rast_surface_t *s, const int32_t *xy, int n, int w16, uint16_t color, int dash)
{
    int hw = w16 / 2;
    int32_t m = hw + RAST_SUB * 2;
    const int32_t L = -m, T = -m, R = s->w * RAST_SUB + m, B = s->h * RAST_SUB + m;
    int thick = w16 > RAST_SUB * 3 / 2;
    int joins = w16 >= RAST_SUB * 4;
    int32_t dash16 = dash * RAST_SUB, dpos = 0;      /* position inside the on/off period */
    for (int i = 0; i + 1 < n; i++) {
        int32_t a[2] = {xy[2 * i], xy[2 * i + 1]}, b[2] = {xy[2 * i + 2], xy[2 * i + 3]};
        int32_t ox = a[0], oy = a[1];
        if ((a[0] < L && b[0] < L) || (a[0] > R && b[0] > R) || (a[1] < T && b[1] < T) || (a[1] > B && b[1] > B)) {
            dpos = 0;
            continue;
        }
        if ((a[0] < L || a[0] > R || a[1] < T || a[1] > B || b[0] < L || b[0] > R || b[1] < T || b[1] > B)
            && !clip_seg(a, b, L, T, R, B)) continue;
        if (dash) {
            /* on pieces only; the phase restarts where the segment was clipped */
            if (a[0] != ox || a[1] != oy) dpos = 0;
            int32_t dx = b[0] - a[0], dy = b[1] - a[1];
            int32_t len = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
            if (!len) continue;
            int32_t ux = slope_q15(dx, len) * 2, uy = slope_q15(dy, len) * 2;   /* unit vector, Q16 */
            int32_t period = 2 * dash16, t = 0;
            while (t < len) {
                int32_t ph = dpos % period;
                int32_t step = ph < dash16 ? dash16 - ph : period - ph;
                if (step > len - t) step = len - t;
                if (ph < dash16) {
                    int32_t x0 = a[0] + (int32_t)(((int64_t)ux * t) >> 16), y0 = a[1] + (int32_t)(((int64_t)uy * t) >> 16);
                    int32_t x1 = a[0] + (int32_t)(((int64_t)ux * (t + step)) >> 16), y1 = a[1] + (int32_t)(((int64_t)uy * (t + step)) >> 16);
                    if (thick) add_quad(s, x0, y0, x1, y1, hw, color);
                    else thin_seg(s, x0, y0, x1, y1, color);
                }
                t += step;
                dpos += step;
            }
            continue;
        }
        if (thick) {
            add_quad(s, a[0], a[1], b[0], b[1], hw, color);
            if (joins && i + 2 < n) add_join(s, b[0], b[1], hw, color);   /* interior vertex */
        } else {
            thin_seg(s, a[0], a[1], b[0], b[1], color);
        }
    }
}
