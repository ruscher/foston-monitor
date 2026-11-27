/* bgspeed.c - see bgspeed.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgspeed.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct bgk_pack {
    void *map;
    size_t len;
    const bgk_entry_t *e;
    uint32_t n, built;
};

static uint32_t rd32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

bgk_pack_t *bgk_open(const char *path, char *err, int errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if(fd < 0 || fstat(fd, &st) < 0) {
        snprintf(err, (size_t)errlen, "%s: não abre", path);
        if(fd >= 0) close(fd);
        return NULL;
    }
    void *m = st.st_size >= 16 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if(m == MAP_FAILED) { snprintf(err, (size_t)errlen, "%s: não mapeia", path); return NULL; }
    const unsigned char *h = m;
    uint32_t n = rd32(h + 8);
    if(memcmp(h, BGK_MAGIC, 4) || rd32(h + 4) != BGK_VERSION || (size_t)st.st_size != 16 + (size_t)n * sizeof(bgk_entry_t)) {
        snprintf(err, (size_t)errlen, "%s: formato desconhecido", path);
        munmap(m, (size_t)st.st_size);
        return NULL;
    }
    bgk_pack_t *p = calloc(1, sizeof *p);
    if(!p) { munmap(m, (size_t)st.st_size); snprintf(err, (size_t)errlen, "sem memória"); return NULL; }
    p->map = m;
    p->len = (size_t)st.st_size;
    p->e = (const bgk_entry_t *)(h + 16);
    p->n = n;
    p->built = rd32(h + 12);
    return p;
}

void bgk_close(bgk_pack_t *p)
{
    if(!p) return;
    munmap(p->map, p->len);
    free(p);
}

uint32_t bgk_count(const bgk_pack_t *p) { return p ? p->n : 0; }
uint32_t bgk_built(const bgk_pack_t *p) { return p ? p->built : 0; }

static uint32_t minx(const bgk_entry_t *e) { return e->xa < e->xb ? e->xa : e->xb; }

int bgk_at(const bgk_pack_t *p, uint32_t x, uint32_t y, int bearing)
{
    if(!p || !p->n) return 0;
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat);
    uint32_t tol = (uint32_t)(8.0 / mpu), span = (uint32_t)((BGK_MAX_PIECE_M + 10) / mpu);
    uint32_t from = x > span ? x - span : 0;                /* a peça pode começar até 200 m antes */
    uint32_t lo = 0, hi = p->n;
    while(lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if(minx(&p->e[mid]) < from) lo = mid + 1; else hi = mid; }
    double best = (double)tol * tol;
    int lim = 0;
    for(uint32_t i = lo; i < p->n && minx(&p->e[i]) <= x + tol; i++) {
        const bgk_entry_t *e = &p->e[i];
        uint32_t ex1 = e->xa > e->xb ? e->xa : e->xb, ey0 = e->ya < e->yb ? e->ya : e->yb, ey1 = e->ya > e->yb ? e->ya : e->yb;
        if(ex1 + tol < x || ey0 > y + tol || ey1 + tol < y) continue;
        double dx = (double)(int32_t)(e->xb - e->xa), dy = (double)(int32_t)(e->yb - e->ya), l2 = dx * dx + dy * dy;
        double px = (double)(int32_t)(x - e->xa), py = (double)(int32_t)(y - e->ya);
        double t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
        if(t < 0) t = 0;
        if(t > 1) t = 1;
        double qx = px - t * dx, qy = py - t * dy, d = qx * qx + qy * qy;
        if(d > best) continue;
        int v;
        if(bearing < 0) v = e->fwd ? e->fwd : e->bwd;
        else {
            double b = fmod(atan2(dx, -dy) * 180 / M_PI + 360, 360);
            double diff = fabs(fmod(bearing - b + 540, 360) - 180);
            if(diff <= 35) v = e->fwd;
            else if(diff >= 145) v = e->bwd;
            else continue;                                  /* a via que cruza */
        }
        if(!v) continue;
        best = d;
        lim = v;
    }
    return lim;
}

int bgk_route(const bgk_pack_t *p, const uint32_t *xy, int npts, uint8_t *out)
{
    int got = 0;
    for(int s = 0; s + 1 < npts; s++) {
        uint32_t ax = xy[2 * s], ay = xy[2 * s + 1], bx = xy[2 * s + 2], by = xy[2 * s + 3];
        double dx = (double)(int32_t)(bx - ax), dy = (double)(int32_t)(by - ay);
        int bearing = dx == 0 && dy == 0 ? -1 : (int)lround(fmod(atan2(dx, -dy) * 180 / M_PI + 360, 360));
        uint32_t mx = ax + (uint32_t)(int32_t)lround(dx / 2), my = ay + (uint32_t)(int32_t)lround(dy / 2);
        int v = bgk_at(p, mx, my, bearing);
        out[s] = (uint8_t)v;
        got += v > 0;
    }
    return got;
}
