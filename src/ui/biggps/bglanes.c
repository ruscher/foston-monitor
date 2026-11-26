/* bglanes.c - see bglanes.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bglanes.h"
#include "bgnav.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct bgl_pack {
    void *map;
    size_t len;
    const bgl_entry_t *e;
    uint32_t n;
};

static uint32_t rd32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

bgl_pack_t *bgl_open(const char *path, char *err, int errlen)
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
    if (memcmp(h, BGL_MAGIC, 4) || rd32(h + 4) != BGL_VERSION || (size_t)st.st_size != 16 + (size_t)n * sizeof(bgl_entry_t)) {
        snprintf(err, (size_t)errlen, "%s: formato desconhecido", path);
        munmap(m, (size_t)st.st_size);
        return NULL;
    }
    bgl_pack_t *p = calloc(1, sizeof *p);
    if (!p) { munmap(m, (size_t)st.st_size); snprintf(err, (size_t)errlen, "sem memória"); return NULL; }
    p->map = m;
    p->len = (size_t)st.st_size;
    p->e = (const bgl_entry_t *)(h + 16);
    p->n = n;
    return p;
}

void bgl_close(bgl_pack_t *p)
{
    if (!p) return;
    munmap(p->map, p->len);
    free(p);
}

uint32_t bgl_count(const bgl_pack_t *p) { return p ? p->n : 0; }

const bgl_entry_t *bgl_find(const bgl_pack_t *p, uint32_t x, uint32_t y, int bearing)
{
    if (!p || !p->n) return NULL;
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / 4294967296.0)));
    uint32_t tol = (uint32_t)(25.0 / (40075016.686 / 4294967296.0 * cos(lat)));
    uint32_t lo = 0, hi = p->n, x0 = x > tol ? x - tol : 0;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (p->e[mid].x < x0) lo = mid + 1; else hi = mid; }
    const bgl_entry_t *best = NULL;
    uint64_t bd = UINT64_MAX;
    for (uint32_t i = lo; i < p->n && p->e[i].x <= x + tol; i++) {
        const bgl_entry_t *e = &p->e[i];
        int64_t dy = (int64_t)e->y - y, dx = (int64_t)e->x - x;
        if (dy > tol || dy < -(int64_t)tol || e->n < 1 || e->n > BGL_MAXLANES) continue;
        int diff = abs(((e->bearing - bearing) % 360 + 540) % 360 - 180);
        if (diff > 35) continue;
        uint64_t d = (uint64_t)(dx * dx + dy * dy);
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

/* the directions a maneuver uses */
static unsigned wanted(int m)
{
    switch (m) {
    case BGN_LEFT: return BGL_LEFT;
    case BGN_SHARP_LEFT: return BGL_SHARP_LEFT | BGL_LEFT;
    case BGN_SLIGHT_LEFT: return BGL_SLIGHT_LEFT | BGL_LEFT;
    case BGN_KEEP_LEFT: case BGN_RAMP_LEFT: return BGL_SLIGHT_LEFT | BGL_LEFT | BGL_MERGE_LEFT;
    case BGN_RIGHT: return BGL_RIGHT;
    case BGN_SHARP_RIGHT: return BGL_SHARP_RIGHT | BGL_RIGHT;
    case BGN_SLIGHT_RIGHT: return BGL_SLIGHT_RIGHT | BGL_RIGHT;
    case BGN_KEEP_RIGHT: case BGN_RAMP_RIGHT: return BGL_SLIGHT_RIGHT | BGL_RIGHT | BGL_MERGE_RIGHT;
    case BGN_STRAIGHT: case BGN_DEPART: return BGL_THROUGH;
    case BGN_UTURN: return BGL_REVERSE;
    default: return 0;                         /* roundabout, arrival: no lane advice */
    }
}

static unsigned mask(const bgl_entry_t *e, int i) { return e->lane[i] ? e->lane[i] : BGL_THROUGH; }

unsigned bgl_fit(const bgl_entry_t *e, int m)
{
    if (!e) return 0;
    unsigned w = wanted(m), fit = 0;
    if (!w) return 0;
    for (int i = 0; i < e->n; i++) if (mask(e, i) & w) fit |= 1u << i;
    if (fit) return fit;
    /* a fork without a marked turn: "through|through|slight_right" and keep left -> the
     * straight lanes left of the marked right turn (and the other way round) */
    if (m == BGN_KEEP_LEFT || m == BGN_RAMP_LEFT || m == BGN_KEEP_RIGHT || m == BGN_RAMP_RIGHT) {
        int left = m == BGN_KEEP_LEFT || m == BGN_RAMP_LEFT;
        unsigned other = left ? (BGL_RIGHT | BGL_SLIGHT_RIGHT | BGL_SHARP_RIGHT | BGL_MERGE_RIGHT)
                              : (BGL_LEFT | BGL_SLIGHT_LEFT | BGL_SHARP_LEFT | BGL_MERGE_LEFT);
        int has_other = 0;
        for (int i = 0; i < e->n; i++) has_other |= (mask(e, i) & other) != 0;
        if (!has_other) return 0;                /* nothing marks the split: no advice */
        for (int i = 0; i < e->n; i++)
            if ((mask(e, i) & BGL_THROUGH) && !(mask(e, i) & other)) fit |= 1u << i;
    }
    return fit;
}

int bgl_arrow(const bgl_entry_t *e, int i, int m, unsigned fit)
{
    if (fit & (1u << i)) {
        if (wanted(m) && (mask(e, i) & wanted(m))) return m == BGN_DEPART ? BGN_STRAIGHT : m;
        return BGN_STRAIGHT;                     /* a straight lane used for a keep maneuver */
    }
    unsigned k = mask(e, i);
    static const struct { unsigned bit; int type; } map[] = {
        { BGL_THROUGH, BGN_STRAIGHT }, { BGL_LEFT, BGN_LEFT }, { BGL_RIGHT, BGN_RIGHT },
        { BGL_SLIGHT_LEFT, BGN_SLIGHT_LEFT }, { BGL_SLIGHT_RIGHT, BGN_SLIGHT_RIGHT },
        { BGL_SHARP_LEFT, BGN_SHARP_LEFT }, { BGL_SHARP_RIGHT, BGN_SHARP_RIGHT },
        { BGL_MERGE_LEFT, BGN_SLIGHT_LEFT }, { BGL_MERGE_RIGHT, BGN_SLIGHT_RIGHT }, { BGL_REVERSE, BGN_UTURN },
    };
    for (unsigned j = 0; j < sizeof map / sizeof map[0]; j++) if (k & map[j].bit) return map[j].type;
    return BGN_STRAIGHT;
}
