/* bgalert.c - see bgalert.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgalert.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct bga_pack {
    void *map;
    size_t len;
    const bga_entry_t *e;
    uint32_t n, built;
};

static uint32_t rd32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

bga_pack_t *bga_open(const char *path, char *err, int errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        snprintf(err, (size_t)errlen, "%s: não abre", path);
        if (fd >= 0) close(fd);
        return NULL;
    }
    void *m = st.st_size >= 16 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) { snprintf(err, (size_t)errlen, "%s: não mapeia", path); return NULL; }
    const unsigned char *h = m;
    uint32_t n = rd32(h + 8);
    if (memcmp(h, BGA_MAGIC, 4) || rd32(h + 4) != BGA_VERSION || (size_t)st.st_size != 16 + (size_t)n * 12) {
        snprintf(err, (size_t)errlen, "%s: formato desconhecido", path);
        munmap(m, (size_t)st.st_size);
        return NULL;
    }
    bga_pack_t *p = calloc(1, sizeof *p);
    if (!p) { munmap(m, (size_t)st.st_size); snprintf(err, (size_t)errlen, "sem memória"); return NULL; }
    p->map = m;
    p->len = (size_t)st.st_size;
    p->e = (const bga_entry_t *)(h + 16);        /* little endian on disk and on the device */
    p->n = n;
    p->built = rd32(h + 12);
    return p;
}

void bga_close(bga_pack_t *p)
{
    if (!p) return;
    munmap(p->map, p->len);
    free(p);
}

uint32_t bga_count(const bga_pack_t *p) { return p ? p->n : 0; }
uint32_t bga_built(const bga_pack_t *p) { return p ? p->built : 0; }

static uint32_t lower_x(const bga_pack_t *p, uint32_t x)
{
    uint32_t lo = 0, hi = p->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (p->e[mid].x < x) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int bga_query(const bga_pack_t *p, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
              const bga_entry_t **out, int max)
{
    if (!p || max <= 0) return 0;
    int k = 0;
    for (uint32_t i = lower_x(p, x0); i < p->n && p->e[i].x <= x1 && k < max; i++)
        if (p->e[i].y >= y0 && p->e[i].y <= y1) out[k++] = &p->e[i];
    return k;
}

static int cmp_hit(const void *a, const void *b)
{
    const bga_hit_t *x = a, *y = b;
    return x->at_dm < y->at_dm ? -1 : x->at_dm > y->at_dm;
}

int bga_route_points(const bga_entry_t *pts, int npts_in, const uint32_t *xy, const uint32_t *cum_dm, int npts,
                     bga_hit_t **hits, int n)
{
    if (npts < 2 || npts_in <= 0) return n;
    for (int k = 0; k < npts_in; k++) {
        const bga_entry_t *e = &pts[k];
        int best_s = -1;
        double best_d = 1e30, best_t = 0;
        for (int s = 0; s + 1 < npts; s++) {
            uint32_t ax = xy[2 * s], ay = xy[2 * s + 1], bx = xy[2 * s + 2], by = xy[2 * s + 3];
            uint32_t pad = 2000;                    /* ~18 m: box test before any division */
            if (e->x + pad < (ax < bx ? ax : bx) || e->x > (ax > bx ? ax : bx) + pad
                || e->y + pad < (ay < by ? ay : by) || e->y > (ay > by ? ay : by) + pad) continue;
            double dx = (double)(int32_t)(bx - ax), dy = (double)(int32_t)(by - ay), l2 = dx * dx + dy * dy;
            double ex = (double)(int32_t)(e->x - ax), ey = (double)(int32_t)(e->y - ay);
            double t = l2 > 0 ? (ex * dx + ey * dy) / l2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            double px = ex - t * dx, py = ey - t * dy, d = px * px + py * py;
            if (d < best_d) { best_d = d; best_s = s; best_t = t; }
        }
        if (best_s < 0) continue;
        double lat = atan(sinh(M_PI * (1.0 - 2.0 * (double)e->y / 4294967296.0)));
        double mpu = 40075016.686 / 4294967296.0 * cos(lat);
        if (sqrt(best_d) * mpu > 12) continue;      /* on the road the route takes */
        bga_hit_t *nh = realloc(*hits, (size_t)(n + 1) * sizeof *nh);
        if (!nh) break;
        *hits = nh;
        uint32_t at = cum_dm[best_s] + (uint32_t)lround(best_t * (double)(cum_dm[best_s + 1] - cum_dm[best_s]));
        nh[n++] = (bga_hit_t){ at, e->kind, 0, 1, e->x, e->y };
    }
    if (*hits) qsort(*hits, (size_t)n, sizeof **hits, cmp_hit);
    return n;
}

bga_hit_t *bga_route(const bga_pack_t *p, const uint32_t *xy, const uint32_t *cum_dm, int npts, int *n)
{
    *n = 0;
    if (!p || npts < 2) return NULL;
    int cap = 64, cnt = 0;
    bga_hit_t *h = malloc((size_t)cap * sizeof *h);
    if (!h) return NULL;
    const bga_entry_t *found[256];
    for (int s = 0; s + 1 < npts; s++) {
        uint32_t ax = xy[2 * s], ay = xy[2 * s + 1], bx = xy[2 * s + 2], by = xy[2 * s + 3];
        double lat = atan(sinh(M_PI * (1.0 - 2.0 * (double)ay / 4294967296.0)));
        double mpu = 40075016.686 / 4294967296.0 * cos(lat);
        uint32_t pad = (uint32_t)(26.0 / mpu);
        uint32_t x0 = (ax < bx ? ax : bx), x1 = (ax < bx ? bx : ax), y0 = (ay < by ? ay : by), y1 = (ay < by ? by : ay);
        x0 = x0 > pad ? x0 - pad : 0;
        y0 = y0 > pad ? y0 - pad : 0;
        x1 = x1 < UINT32_MAX - pad ? x1 + pad : UINT32_MAX;
        y1 = y1 < UINT32_MAX - pad ? y1 + pad : UINT32_MAX;
        int k = bga_query(p, x0, y0, x1, y1, found, 256);
        double dx = (double)(int32_t)(bx - ax), dy = (double)(int32_t)(by - ay), l2 = dx * dx + dy * dy;
        double bearing = fmod(atan2(dx, -dy) * 180 / M_PI + 360, 360);
        for (int i = 0; i < k; i++) {
            const bga_entry_t *e = found[i];
            double ex = (double)(int32_t)(e->x - ax), ey = (double)(int32_t)(e->y - ay);
            double t = l2 > 0 ? (ex * dx + ey * dy) / l2 : 0;
            if (t < 0 || t > 1) continue;                /* the neighbouring segment owns it */
            double px = ex - t * dx, py = ey - t * dy, d = sqrt(px * px + py * py) * mpu;
            if (d > (e->kind == BGA_CAMERA ? 25 : 6)) continue;
            if (e->kind == BGA_CAMERA && e->dir != BGA_DIR_ANY) {
                double diff = fabs(fmod(e->dir - bearing + 540, 360) - 180);
                if (diff > 60) continue;                 /* faces the other carriageway */
            }
            uint32_t at = cum_dm[s] + (uint32_t)lround(t * (double)(cum_dm[s + 1] - cum_dm[s]));
            if (cnt == cap) {
                bga_hit_t *nh = realloc(h, (size_t)cap * 2 * sizeof *h);
                if (!nh) break;
                h = nh;
                cap *= 2;
            }
            h[cnt++] = (bga_hit_t){ at, e->kind, e->maxspeed, 0, e->x, e->y };
        }
    }
    qsort(h, (size_t)cnt, sizeof *h, cmp_hit);
    int w = 0;                                       /* same kind within 40 m: once */
    for (int i = 0; i < cnt; i++) {
        int dup = 0;
        for (int j = w - 1; j >= 0 && h[i].at_dm - h[j].at_dm <= 400 && !dup; j--)
            dup = h[j].kind == h[i].kind;
        if (!dup) h[w++] = h[i];
    }
    *n = w;
    return h;
}
