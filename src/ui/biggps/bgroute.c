/* bgroute.c - see bgroute.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#include "bgroute.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BGR_PAGE_SHIFT
#define BGR_PAGE_SHIFT 13      /* 8 KB: measured on the device, docs/big-gps/07-roteamento.md */
#endif
#define PAGE_SHIFT BGR_PAGE_SHIFT
#define PAGE_SIZE  (1u << PAGE_SHIFT)
#define NPAGES     ((8u << 20) >> PAGE_SHIFT)   /* 8 MB of cached file pages */
#define PHASH      4096
#define INF        0xFFFFFFFFu

typedef struct {
    uint32_t pageno;
    int next;                  /* hash chain */
    uint8_t ref, valid;
} slot_t;

struct bgr_pack {
    int fd;
    off_t size;
    bgr_header_t h;
    slot_t slot[NPAGES];
    uint8_t *mem;              /* NPAGES * PAGE_SIZE */
    int bucket[PHASH];
    int hand;
    unsigned misses;
};

/* ------------------------------------------------------------- page cache */

static const uint8_t *page(bgr_pack_t *p, uint32_t pageno)
{
    int *bp = &p->bucket[pageno % PHASH];
    for (int i = *bp; i >= 0; i = p->slot[i].next)
        if (p->slot[i].pageno == pageno) { p->slot[i].ref = 1; return p->mem + (size_t)i * PAGE_SIZE; }
    /* clock eviction */
    int v;
    for (;;) {
        v = p->hand;
        p->hand = (p->hand + 1) % NPAGES;
        if (!p->slot[v].valid || !p->slot[v].ref) break;
        p->slot[v].ref = 0;
    }
    if (p->slot[v].valid) {
        int *pp = &p->bucket[p->slot[v].pageno % PHASH];
        while (*pp != v) pp = &p->slot[*pp].next;
        *pp = p->slot[v].next;
    }
    uint8_t *dst = p->mem + (size_t)v * PAGE_SIZE;
    off_t off = (off_t)pageno << PAGE_SHIFT;
    size_t want = PAGE_SIZE, got = 0;
    if (off + (off_t)want > p->size) want = off < p->size ? (size_t)(p->size - off) : 0;
    while (got < want) {
        ssize_t r = pread(p->fd, dst + got, want - got, off + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < PAGE_SIZE) memset(dst + got, 0, PAGE_SIZE - got);
    p->misses++;
    p->slot[v].pageno = pageno;
    p->slot[v].valid = 1;
    p->slot[v].ref = 1;
    p->slot[v].next = *bp;
    *bp = v;
    return dst;
}

static uint32_t rd(bgr_pack_t *p, int sec, uint32_t i)
{
    uint32_t off = p->h.sec[sec].off + i * 4u;
    const uint8_t *b = page(p, off >> PAGE_SHIFT) + (off & (PAGE_SIZE - 1));
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint32_t count(const bgr_pack_t *p, int sec) { return p->h.sec[sec].len / 4; }

void bgr_cache_stats(bgr_pack_t *p, unsigned *pages, unsigned *misses)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NPAGES; i++) n += p->slot[i].valid;
    *pages = n;
    *misses = p->misses;
}

/* ------------------------------------------------------------- open/close */

bgr_pack_t *bgr_open(const char *path, char *err, size_t errlen)
{
    bgr_pack_t *p = calloc(1, sizeof *p);
    struct stat st;
    if (!p) { snprintf(err, errlen, "sem memória"); return NULL; }
    p->mem = malloc((size_t)NPAGES * PAGE_SIZE);
    p->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (!p->mem || p->fd < 0 || fstat(p->fd, &st)) { snprintf(err, errlen, "%s: %s", path, strerror(errno ? errno : ENOMEM)); goto fail; }
    p->size = st.st_size;
    for (int i = 0; i < PHASH; i++) p->bucket[i] = -1;
    if (pread(p->fd, &p->h, sizeof p->h, 0) != (ssize_t)sizeof p->h || p->h.magic != BGR_MAGIC || p->h.version != BGR_VERSION) {
        snprintf(err, errlen, "%s: não é um arquivo de rotas do Big GPS (versão %u)", path, BGR_VERSION);
        goto fail;
    }
    const bgr_header_t *h = &p->h;
    for (int s = 0; s < BGR_NSEC; s++)
        if ((off_t)h->sec[s].off + h->sec[s].len > p->size || (h->sec[s].off & 3)) { snprintf(err, errlen, "%s: seção %d inválida", path, s); goto fail; }
    /* sizes must agree with the header, so every index used later stays inside its section */
    if (count(p, BGR_S_NODE_X) != h->nodes || count(p, BGR_S_NODE_Y) != h->nodes || count(p, BGR_S_FIRST_OUT) != h->nodes + 1
        || count(p, BGR_S_ARC_TAIL) != h->arcs || count(p, BGR_S_ARC_HEAD) != h->arcs || count(p, BGR_S_ARC_WAY) != h->arcs || count(p, BGR_S_ARC_LEN) != h->arcs
        || count(p, BGR_S_ARC_TIME) != h->arcs || count(p, BGR_S_GEOM_FIRST) != h->arcs + 1 || count(p, BGR_S_GEOM_X) != h->geom
        || count(p, BGR_S_GEOM_Y) != h->geom || count(p, BGR_S_WAY_NAME) != h->ways || count(p, BGR_S_WAY_REF) != h->ways
        || count(p, BGR_S_WAY_INFO) != h->ways || count(p, BGR_S_EX_HEAD) != h->ex_edges || count(p, BGR_S_CH_ID) != h->arcs
        || count(p, BGR_S_CHF_FIRST) != h->arcs + 1 || count(p, BGR_S_CHF_HEAD) != h->ch_fwd || count(p, BGR_S_CHF_WEIGHT) != h->ch_fwd
        || count(p, BGR_S_CHF_A) != h->ch_fwd || count(p, BGR_S_CHF_B) != h->ch_fwd || count(p, BGR_S_CHF_ORIG) != (h->ch_fwd + 31) / 32
        || count(p, BGR_S_CHB_FIRST) != h->arcs + 1 || count(p, BGR_S_CHB_HEAD) != h->ch_bwd || count(p, BGR_S_CHB_WEIGHT) != h->ch_bwd
        || count(p, BGR_S_CHB_A) != h->ch_bwd || count(p, BGR_S_CHB_B) != h->ch_bwd || count(p, BGR_S_CHB_ORIG) != (h->ch_bwd + 31) / 32
        || count(p, BGR_S_GRID_FIRST) != h->grid_w * h->grid_h + 1 || h->grid_shift < 8 || h->grid_shift > 24) {
        snprintf(err, errlen, "%s: tamanhos inconsistentes", path);
        goto fail;
    }
    return p;
fail:
    bgr_close(p);
    return NULL;
}

void bgr_close(bgr_pack_t *p)
{
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    free(p->mem);
    free(p);
}

const bgr_header_t *bgr_info(const bgr_pack_t *p) { return &p->h; }

/* ---------------------------------------------------------- graph access */

static uint32_t clampi(bgr_pack_t *p, uint32_t v, uint32_t n) { (void)p; return v < n ? v : (n ? n - 1 : 0); }

uint32_t bgr_arc_head(bgr_pack_t *p, uint32_t a) { return clampi(p, rd(p, BGR_S_ARC_HEAD, a), p->h.nodes); }
uint32_t bgr_arc_way(bgr_pack_t *p, uint32_t a) { return clampi(p, rd(p, BGR_S_ARC_WAY, a), p->h.ways); }
uint32_t bgr_arc_len_dm(bgr_pack_t *p, uint32_t a) { return rd(p, BGR_S_ARC_LEN, a); }
uint32_t bgr_arc_time_ds(bgr_pack_t *p, uint32_t a) { return rd(p, BGR_S_ARC_TIME, a); }
uint32_t bgr_way_info(bgr_pack_t *p, uint32_t w) { return rd(p, BGR_S_WAY_INFO, w); }

uint32_t bgr_arc_tail(bgr_pack_t *p, uint32_t a) { return clampi(p, rd(p, BGR_S_ARC_TAIL, a), p->h.nodes); }

uint32_t bgr_node_first_out(bgr_pack_t *p, uint32_t u)
{
    if (u > p->h.nodes) u = p->h.nodes;
    uint32_t f = rd(p, BGR_S_FIRST_OUT, u);
    return f <= p->h.arcs ? f : p->h.arcs;
}

void bgr_node_xy(bgr_pack_t *p, uint32_t u, uint32_t *x, uint32_t *y)
{
    *x = rd(p, BGR_S_NODE_X, u);
    *y = rd(p, BGR_S_NODE_Y, u);
}

int bgr_arc_points(bgr_pack_t *p, uint32_t a, uint32_t *xy, int max)
{
    if (max < 2 || a >= p->h.arcs) return 0;
    uint32_t g0 = rd(p, BGR_S_GEOM_FIRST, a), g1 = rd(p, BGR_S_GEOM_FIRST, a + 1);
    if (g1 < g0 || g1 > p->h.geom) g1 = g0;
    int n = 0;
    bgr_node_xy(p, bgr_arc_tail(p, a), &xy[0], &xy[1]);
    n = 1;
    for (uint32_t k = g0; k < g1 && n < max - 1; k++, n++) {
        xy[2 * n] = rd(p, BGR_S_GEOM_X, k);
        xy[2 * n + 1] = rd(p, BGR_S_GEOM_Y, k);
    }
    bgr_node_xy(p, bgr_arc_head(p, a), &xy[2 * n], &xy[2 * n + 1]);
    return n + 1;
}

static void read_string(bgr_pack_t *p, uint32_t off, char *buf, size_t n)
{
    size_t i = 0;
    uint32_t base = p->h.sec[BGR_S_STRINGS].off, len = p->h.sec[BGR_S_STRINGS].len;
    while (i + 1 < n && off < len) {
        uint32_t o = base + off;
        char c = (char)page(p, o >> PAGE_SHIFT)[o & (PAGE_SIZE - 1)];
        if (!c) break;
        buf[i++] = c;
        off++;
    }
    buf[i] = 0;
}

const char *bgr_way_name(bgr_pack_t *p, uint32_t w, char *buf, size_t n)
{
    buf[0] = 0;
    if (w >= p->h.ways || n < 2) return buf;
    uint32_t o = rd(p, BGR_S_WAY_NAME, w);
    if (o == INF) o = rd(p, BGR_S_WAY_REF, w);
    if (o != INF) read_string(p, o, buf, n);
    return buf;
}

/* -------------------------------------------------------------- snapping */

int bgr_snap(bgr_pack_t *p, uint32_t x, uint32_t y, int heading, int max_m, bgr_snap_t *out, int max)
{
    const bgr_header_t *h = &p->h;
    if (max < 1) return 0;
    if (max > 64) max = 64;
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y / 4294967296.0)));
    double m_per_unit = 40075016.686 / 4294967296.0 * cos(lat);
    double max_units = max_m / m_per_unit;
    int64_t cx = ((int64_t)x - h->grid_x0) >> h->grid_shift, cy = ((int64_t)y - h->grid_y0) >> h->grid_shift;
    int reach = (int)(max_units / (double)(1u << h->grid_shift)) + 1;
    if (reach > 3) reach = 3;
    int n = 0;
    double score[64];
    uint32_t pts[2 * 512];
    for (int64_t gy = cy - reach; gy <= cy + reach; gy++)
        for (int64_t gx = cx - reach; gx <= cx + reach; gx++) {
            if (gx < 0 || gy < 0 || gx >= h->grid_w || gy >= h->grid_h) continue;
            uint32_t c = (uint32_t)(gy * h->grid_w + gx);
            uint32_t a0 = rd(p, BGR_S_GRID_FIRST, c), a1 = rd(p, BGR_S_GRID_FIRST, c + 1);
            if (a1 > count(p, BGR_S_GRID_ARCS)) a1 = count(p, BGR_S_GRID_ARCS);
            for (uint32_t i = a0; i < a1; i++) {
                uint32_t a = rd(p, BGR_S_GRID_ARCS, i);
                if (a >= h->arcs) continue;
                int dup = 0;
                for (int k = 0; k < n && !dup; k++) dup = out[k].arc == a;
                if (dup) continue;
                int np = bgr_arc_points(p, a, pts, 512);
                double best = 1e30, bt = 0, total = 0, bx = 0, by = 0, bdir = 0;
                for (int k = 0; k + 1 < np; k++) {
                    double ax = (double)(int32_t)(pts[2 * k] - x), ay = (double)(int32_t)(pts[2 * k + 1] - y);
                    double ex = (double)(int32_t)(pts[2 * k + 2] - x), ey = (double)(int32_t)(pts[2 * k + 3] - y);
                    double dx = ex - ax, dy = ey - ay, l2 = dx * dx + dy * dy, l = sqrt(l2);
                    double t = l2 > 0 ? -(ax * dx + ay * dy) / l2 : 0;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    double px = ax + t * dx, py = ay + t * dy, d = px * px + py * py;
                    if (d < best) { best = d; bt = total + t * l; bx = px; by = py; bdir = atan2(dx, -dy) * 180 / M_PI; }
                    total += l;
                }
                double dist = sqrt(best);
                if (dist > max_units) continue;
                double s = dist * m_per_unit;
                if (heading >= 0) {
                    double dd = fabs(fmod(bdir - heading + 540.0, 360.0) - 180.0);
                    if (dd > 60) s += 40 + dd / 4;   /* wrong way: worse than a farther road */
                }
                bgr_snap_t cand = { a, total > 0 ? (uint32_t)(bt / total * 65536.0) : 0,
                                    x + (uint32_t)(int32_t)lround(bx), y + (uint32_t)(int32_t)lround(by),
                                    (uint32_t)lround(dist * m_per_unit * 10), (int16_t)lround(bdir < 0 ? bdir + 360 : bdir) };
                if (n == max && s >= score[max - 1]) continue;   /* list full and this one is worse */
                int k = n < max ? n++ : max - 1;
                while (k > 0 && score[k - 1] > s) { out[k] = out[k - 1]; score[k] = score[k - 1]; k--; }
                out[k] = cand;
                score[k] = s;
            }
        }
    return n;
}

/* ---------------------------------------------------------------- search */

typedef struct {
    uint32_t key;              /* CH node + 1 (0 = empty) */
    uint32_t d[2];             /* distance per side */
    uint32_t pnode[2], parc[2];/* predecessor per side (INF = start) */
} vis_t;

typedef struct {
    vis_t *t;
    uint32_t cap, used;
} vtab_t;

static vis_t *vget(vtab_t *v, uint32_t rank, int create)
{
    if (create && (v->used + 1) * 2 > v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 1u << 14;
        vis_t *nt = calloc(nc, sizeof *nt);
        if (!nt) return NULL;
        for (uint32_t i = 0; i < v->cap; i++) {
            if (!v->t[i].key) continue;
            uint32_t j = (v->t[i].key * 2654435761u) & (nc - 1);
            while (nt[j].key) j = (j + 1) & (nc - 1);
            nt[j] = v->t[i];
        }
        free(v->t);
        v->t = nt;
        v->cap = nc;
    }
    if (!v->cap) return NULL;
    uint32_t key = rank + 1, j = (key * 2654435761u) & (v->cap - 1);
    while (v->t[j].key) {
        if (v->t[j].key == key) return &v->t[j];
        j = (j + 1) & (v->cap - 1);
    }
    if (!create) return NULL;
    vis_t *e = &v->t[j];
    e->key = key;
    e->d[0] = e->d[1] = INF;
    e->pnode[0] = e->pnode[1] = e->parc[0] = e->parc[1] = INF;
    v->used++;
    return e;
}

typedef struct { uint32_t k, node; } hitem_t;
typedef struct { hitem_t *a; int n, cap; } heap_t;

static int hpush(heap_t *h, uint32_t k, uint32_t node)
{
    if (h->n == h->cap) {
        int nc = h->cap ? h->cap * 2 : 1024;
        hitem_t *na = realloc(h->a, (size_t)nc * sizeof *na);
        if (!na) return -1;
        h->a = na;
        h->cap = nc;
    }
    int i = h->n++;
    while (i > 0 && h->a[(i - 1) / 2].k > k) { h->a[i] = h->a[(i - 1) / 2]; i = (i - 1) / 2; }
    h->a[i].k = k;
    h->a[i].node = node;
    return 0;
}

static hitem_t hpop(heap_t *h)
{
    hitem_t top = h->a[0], last = h->a[--h->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n) break;
        if (c + 1 < h->n && h->a[c + 1].k < h->a[c].k) c++;
        if (h->a[c].k >= last.k) break;
        h->a[i] = h->a[c];
        i = c;
    }
    if (h->n) h->a[i] = last;
    return top;
}

static uint32_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}

/* Bidirectional upward search.  Sources/targets as (CH node, initial distance).
 * Returns the meeting node or INF; *best = distance. */
static uint32_t search(bgr_pack_t *p, vtab_t *vt, heap_t hp[2], const uint32_t *srank, const uint32_t *sinit, int ns,
                       const uint32_t *trank, const uint32_t *tinit, int nt, uint32_t *best, bgr_qstats_t *st)
{
    static const int S_FIRST[2] = { BGR_S_CHF_FIRST, BGR_S_CHB_FIRST }, S_HEAD[2] = { BGR_S_CHF_HEAD, BGR_S_CHB_HEAD },
                     S_W[2] = { BGR_S_CHF_WEIGHT, BGR_S_CHB_WEIGHT };
    uint32_t nm[2] = { p->h.ch_fwd, p->h.ch_bwd };
    *best = INF;
    uint32_t meet = INF;
    for (int side = 0; side < 2; side++) {
        const uint32_t *r = side ? trank : srank, *in = side ? tinit : sinit;
        int cnt = side ? nt : ns;
        for (int i = 0; i < cnt; i++) {
            vis_t *e = vget(vt, r[i], 1);
            if (!e) return INF;
            if (in[i] < e->d[side]) { e->d[side] = in[i]; if (hpush(&hp[side], in[i], r[i])) return INF; }
        }
    }
    for (;;) {
        uint32_t k0 = hp[0].n ? hp[0].a[0].k : INF, k1 = hp[1].n ? hp[1].a[0].k : INF;
        int a0 = k0 < *best, a1 = k1 < *best;
        if (!a0 && !a1) break;
        int side = a0 && (!a1 || k0 <= k1) ? 0 : 1;
        hitem_t it = hpop(&hp[side]);
        vis_t *e = vget(vt, it.node, 0);
        if (!e || it.k > e->d[side]) continue;          /* stale entry */
        st->settled++;
        if (e->d[!side] != INF && (uint64_t)e->d[0] + e->d[1] < *best) { *best = e->d[0] + e->d[1]; meet = it.node; }
        uint32_t f0 = rd(p, S_FIRST[side], it.node), f1 = rd(p, S_FIRST[side], it.node + 1);
        if (f1 > nm[side]) f1 = nm[side];
        for (uint32_t arc = f0; arc < f1; arc++) {
            uint32_t y = rd(p, S_HEAD[side], arc);
            if (y >= p->h.arcs) continue;
            uint64_t nd = (uint64_t)it.k + rd(p, S_W[side], arc);
            if (nd >= INF) continue;
            st->relaxed++;
            vis_t *ey = vget(vt, y, 1);
            if (!ey) return INF;
            e = vget(vt, it.node, 0);                   /* the table may have grown */
            if (nd < ey->d[side]) {
                ey->d[side] = (uint32_t)nd;
                ey->pnode[side] = it.node;
                ey->parc[side] = arc;
                if (hpush(&hp[side], (uint32_t)nd, y)) return INF;
                if (ey->d[!side] != INF && (uint64_t)ey->d[0] + ey->d[1] < *best) { *best = ey->d[0] + ey->d[1]; meet = y; }
            }
        }
    }
    return meet;
}

/* unpack CH arcs into expanded edge ids (explicit stack) */
typedef struct { uint32_t *v; int n, cap; } ubuf_t;

static int upush(ubuf_t *b, uint32_t x)
{
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        uint32_t *nv = realloc(b->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        b->v = nv;
        b->cap = nc;
    }
    b->v[b->n++] = x;
    return 0;
}

static int unpack(bgr_pack_t *p, int side, uint32_t arc, ubuf_t *outb, ubuf_t *stack)
{
    static const int S_A[2] = { BGR_S_CHF_A, BGR_S_CHB_A }, S_B[2] = { BGR_S_CHF_B, BGR_S_CHB_B },
                     S_O[2] = { BGR_S_CHF_ORIG, BGR_S_CHB_ORIG };
    uint32_t nm[2] = { p->h.ch_fwd, p->h.ch_bwd };
    stack->n = 0;
    if (upush(stack, arc << 1 | (uint32_t)side)) return -1;
    while (stack->n) {
        uint32_t it = stack->v[--stack->n];
        int s = (int)(it & 1);
        uint32_t a = it >> 1;
        if (a >= nm[s]) return -1;
        if (rd(p, S_O[s], a / 32) >> (a % 32) & 1) {
            if (upush(outb, rd(p, S_A[s], a))) return -1;
        } else {
            /* shortcut = backward arc A, then forward arc B: push B first */
            if (upush(stack, rd(p, S_B[s], a) << 1 | 0) || upush(stack, rd(p, S_A[s], a) << 1 | 1)) return -1;
            if (stack->n > 100000) return -1;            /* corrupt data: no cycles */
        }
    }
    return 0;
}

static uint32_t part(uint32_t v, uint32_t frac) { return (uint32_t)((uint64_t)v * frac >> 16); }

int bgr_route(bgr_pack_t *p, const bgr_snap_t *src, int nsrc, const bgr_snap_t *dst, int ndst, bgr_route_t *out, bgr_qstats_t *st)
{
    bgr_qstats_t lst = {0};
    if (!st) st = &lst;
    memset(st, 0, sizeof *st);
    memset(out, 0, sizeof *out);
    unsigned m0 = p->misses;
    uint32_t t0 = now_us();
    if (nsrc < 1 || ndst < 1 || nsrc > 8 || ndst > 8) return -1;
    uint32_t sr[8], si[8], tr[8], ti[8], C = 0;
    for (int i = 0; i < nsrc; i++) {
        sr[i] = rd(p, BGR_S_CH_ID, src[i].arc);
        si[i] = part(bgr_arc_time_ds(p, src[i].arc), 65536 - src[i].frac);
    }
    for (int i = 0; i < ndst; i++) {
        uint32_t rest = part(bgr_arc_time_ds(p, dst[i].arc), 65536 - dst[i].frac);
        if (rest > C) C = rest;
        tr[i] = rd(p, BGR_S_CH_ID, dst[i].arc);
        ti[i] = rest;                                   /* fixed below: C - rest */
    }
    for (int i = 0; i < ndst; i++) ti[i] = C - ti[i];

    /* same arc, target ahead of the source: no search needed */
    uint32_t direct = INF;
    int dsi = -1, ddi = -1;
    for (int i = 0; i < nsrc; i++)
        for (int j = 0; j < ndst; j++)
            if (src[i].arc == dst[j].arc && dst[j].frac >= src[i].frac) {
                uint32_t d = part(bgr_arc_time_ds(p, src[i].arc), dst[j].frac - src[i].frac);
                if (d < direct) { direct = d; dsi = i; ddi = j; }
            }

    vtab_t vt = {0};
    heap_t hp[2] = {{0}, {0}};
    ubuf_t path = {0}, stack = {0}, ups = {0};
    uint32_t best;
    uint32_t meet = search(p, &vt, hp, sr, si, nsrc, tr, ti, ndst, &best, st);
    int rc = -1;
    uint32_t chtime = meet != INF && best >= C ? best - C : INF;
    /* a meeting on the source/target arc itself with the target behind is not a route */
    if (meet != INF) {
        vis_t *e = vget(&vt, meet, 0);
        if (e && e->pnode[0] == INF && e->pnode[1] == INF) {
            int ok = 0;
            for (int i = 0; i < nsrc; i++)
                for (int j = 0; j < ndst; j++)
                    if (sr[i] == meet && tr[j] == meet && dst[j].frac >= src[i].frac) ok = 1;
            if (!ok) chtime = INF;
        }
    }
    if (direct != INF && direct <= chtime) {
        if (upush(&path, src[dsi].arc)) { rc = -2; goto done; }
        out->time_ds = direct;
        out->src_frac = src[dsi].frac;
        out->dst_frac = dst[ddi].frac;
        rc = 0;
    } else if (chtime != INF) {
        /* forward chain: meeting node back to its source */
        uint32_t x = meet;
        for (vis_t *e = vget(&vt, x, 0); e && e->pnode[0] != INF; e = vget(&vt, x, 0)) {
            if (upush(&ups, e->parc[0])) { rc = -2; goto done; }
            x = e->pnode[0];
            if (ups.n > 1000000) { rc = -2; goto done; }
        }
        int si_ = -1;
        for (int i = 0; i < nsrc; i++) if (sr[i] == x) si_ = i;
        if (si_ < 0) { rc = -2; goto done; }
        ubuf_t edges = {0};
        int bad = 0;
        for (int i = ups.n - 1; i >= 0 && !bad; i--) bad = unpack(p, 0, ups.v[i], &edges, &stack);
        x = meet;
        for (vis_t *e = vget(&vt, x, 0); e && e->pnode[1] != INF && !bad; e = vget(&vt, x, 0)) {
            bad = unpack(p, 1, e->parc[1], &edges, &stack);
            x = e->pnode[1];
        }
        int di_ = -1;
        for (int j = 0; j < ndst; j++) if (tr[j] == x) di_ = j;
        if (bad || di_ < 0 || upush(&path, src[si_].arc)) { free(edges.v); rc = -2; goto done; }
        for (int i = 0; i < edges.n; i++) {
            uint32_t e = edges.v[i];
            if (e >= p->h.ex_edges || upush(&path, rd(p, BGR_S_EX_HEAD, e))) { bad = 1; break; }
        }
        free(edges.v);
        if (bad) { rc = -2; goto done; }
        out->time_ds = chtime;
        out->src_frac = src[si_].frac;
        out->dst_frac = dst[di_].frac;
        rc = 0;
    }
    if (rc == 0) {
        out->arcs = path.v;
        out->narcs = path.n;
        path.v = NULL;
        uint64_t len = 0;
        for (int i = 0; i < out->narcs; i++) len += bgr_arc_len_dm(p, out->arcs[i]);
        uint32_t lf = bgr_arc_len_dm(p, out->arcs[0]), ll = bgr_arc_len_dm(p, out->arcs[out->narcs - 1]);
        if (out->narcs == 1) len = part(lf, out->dst_frac - out->src_frac);
        else len -= part(lf, out->src_frac) + part(ll, 65536 - out->dst_frac);
        out->len_dm = (uint32_t)len;
    }
done:
    free(vt.t);
    free(hp[0].a);
    free(hp[1].a);
    free(path.v);
    free(stack.v);
    free(ups.v);
    st->pages_read = p->misses - m0;
    st->us = now_us() - t0;
    return rc;
}

void bgr_route_free(bgr_route_t *r)
{
    free(r->arcs);
    r->arcs = NULL;
    r->narcs = 0;
}

uint32_t bgr_arc_distance(bgr_pack_t *p, uint32_t s, uint32_t t, bgr_qstats_t *st)
{
    bgr_qstats_t lst = {0};
    if (!st) st = &lst;
    memset(st, 0, sizeof *st);
    if (s >= p->h.arcs || t >= p->h.arcs) return INF;
    uint32_t sr = rd(p, BGR_S_CH_ID, s), tr = rd(p, BGR_S_CH_ID, t), z = 0, best;
    vtab_t vt = {0};
    heap_t hp[2] = {{0}, {0}};
    uint32_t t0 = now_us();
    unsigned m0 = p->misses;
    search(p, &vt, hp, &sr, &z, 1, &tr, &z, 1, &best, st);
    free(vt.t);
    free(hp[0].a);
    free(hp[1].a);
    st->pages_read = p->misses - m0;
    st->us = now_us() - t0;
    return best;
}
