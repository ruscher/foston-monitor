/* bgmap.c - see bgmap.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#include "bgmap.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HASH 256
#define EMPTY_TILE ((bgm_tile_t *)1)

typedef struct {
    int level, tx, ty;
    uint32_t used;
} empty_t;

struct bgm_pack {
    int fd;
    bgm_header_t hdr;
    bgm_level_t lv[BGM_MAX_LEVELS];
    bgm_index_t *idx[BGM_MAX_LEVELS];
    char *attr;
    uint32_t frame;
    size_t limit, mem;
    bgm_tile_t *hash[HASH];
    empty_t empties[64];       /* recent "no data here" answers */
    int nempty;
    uint8_t *rbuf;
    size_t rcap;
    bgm_stats_t st;
};

static unsigned hkey(int level, int tx, int ty) { return ((unsigned)tx * 73856093u ^ (unsigned)ty * 19349663u ^ (unsigned)level) % HASH; }

static uint32_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}

static int read_at(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *b = buf;
    while (n) {
        ssize_t r = pread(fd, b, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r; n -= (size_t)r; off += r;
    }
    return 0;
}

bgm_pack_t *bgm_open(const char *path, char *err, size_t errlen)
{
    bgm_pack_t *p = calloc(1, sizeof *p);
    struct stat st;
    if (!p) { snprintf(err, errlen, "sem memória"); return NULL; }
    p->fd = open(path, O_RDONLY | O_CLOEXEC);
    p->limit = 12u << 20;
    if (p->fd < 0 || fstat(p->fd, &st)) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); goto fail; }
    if (read_at(p->fd, &p->hdr, sizeof p->hdr, 0) || p->hdr.magic != BGM_MAGIC || p->hdr.version != BGM_VERSION
        || p->hdr.nlevels < 1 || p->hdr.nlevels > BGM_MAX_LEVELS) {
        snprintf(err, errlen, "%s: não é um mapa do Big GPS (versão %u)", path, BGM_VERSION);
        goto fail;
    }
    if (read_at(p->fd, p->lv, p->hdr.nlevels * sizeof(bgm_level_t), sizeof p->hdr)) { snprintf(err, errlen, "%s: cabeçalho curto", path); goto fail; }
    for (unsigned l = 0; l < p->hdr.nlevels; l++) {
        size_t n = p->lv[l].ntiles * sizeof(bgm_index_t);
        if ((off_t)p->lv[l].index_off + (off_t)n > st.st_size || (l && p->lv[l].zoom <= p->lv[l - 1].zoom)) { snprintf(err, errlen, "%s: índice inválido", path); goto fail; }
        p->idx[l] = malloc(n ? n : 1);
        if (!p->idx[l] || read_at(p->fd, p->idx[l], n, p->lv[l].index_off)) { snprintf(err, errlen, "%s: erro ao ler o índice", path); goto fail; }
        for (uint32_t i = 0; i < p->lv[l].ntiles; i++)
            if ((off_t)p->idx[l][i].off + p->idx[l][i].len > st.st_size || (i && p->idx[l][i].key <= p->idx[l][i - 1].key)) {
                snprintf(err, errlen, "%s: índice inválido", path);
                goto fail;
            }
    }
    if (p->hdr.attr_off < st.st_size) {
        size_t n = (size_t)(st.st_size - p->hdr.attr_off);
        if (n > 1024) n = 1024;
        p->attr = calloc(1, n + 1);
        if (p->attr && read_at(p->fd, p->attr, n, p->hdr.attr_off)) p->attr[0] = 0;
    }
    return p;
fail:
    bgm_close(p);
    return NULL;
}

static void tile_free(bgm_tile_t *t) { free(t); }   /* one allocation per tile */

void bgm_close(bgm_pack_t *p)
{
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    for (int i = 0; i < BGM_MAX_LEVELS; i++) free(p->idx[i]);
    for (int i = 0; i < HASH; i++)
        for (bgm_tile_t *t = p->hash[i], *n; t; t = n) { n = t->next; tile_free(t); }
    free(p->attr);
    free(p->rbuf);
    free(p);
}

const bgm_header_t *bgm_header(const bgm_pack_t *p) { return &p->hdr; }
const char *bgm_attribution(const bgm_pack_t *p) { return p->attr ? p->attr : ""; }
int bgm_nlevels(const bgm_pack_t *p) { return (int)p->hdr.nlevels; }
int bgm_level_zoom(const bgm_pack_t *p, int level) { return p->lv[level].zoom; }

int bgm_level_for_zoom(const bgm_pack_t *p, int zoom)
{
    int best = 0;
    for (unsigned l = 0; l < p->hdr.nlevels; l++)
        if (p->lv[l].zoom <= zoom) best = (int)l;
    return best;
}

void bgm_frame(bgm_pack_t *p) { p->frame++; }
void bgm_set_cache_limit(bgm_pack_t *p, size_t bytes) { p->limit = bytes; }

void bgm_get_stats(bgm_pack_t *p, bgm_stats_t *st, int reset)
{
    p->st.cache_bytes = p->mem;
    *st = p->st;
    if (reset) memset(&p->st, 0, sizeof p->st);
}

static const bgm_index_t *find(const bgm_pack_t *p, int level, uint32_t key)
{
    const bgm_index_t *a = p->idx[level];
    uint32_t lo = 0, hi = p->lv[level].ntiles;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (a[m].key < key) lo = m + 1;
        else hi = m;
    }
    return lo < p->lv[level].ntiles && a[lo].key == key ? &a[lo] : NULL;
}



/* inline varint for the hot loop; on a truncated blob it stops at the end (checked after) */
#define VARINT(dst) do { \
        uint32_t r_ = 0; int sh_ = 0; uint8_t b_; \
        do { if (s >= e) { bad = 1; b_ = 0; break; } b_ = *s++; r_ |= (uint32_t)(b_ & 0x7F) << sh_; sh_ += 7; } while ((b_ & 0x80) && sh_ < 35); \
        (dst) = r_; } while (0)
#define ZZ(dst) do { uint32_t u_; VARINT(u_); (dst) = (int32_t)(u_ >> 1) ^ -(int32_t)(u_ & 1); } while (0)

/* One pass: the blob header gives the totals, so one allocation holds everything. */
static bgm_tile_t *decode(const uint8_t *b, size_t n, int level, int tx, int ty)
{
    const uint8_t *s = b, *e = b + n;
    int bad = 0;
    uint32_t nstr, nfeat, nring, npts, v;
    VARINT(nstr);
    if (bad || nstr > 65535) return NULL;
    const uint8_t *strs = s;
    size_t namebytes = 0;
    for (uint32_t i = 0; i < nstr; i++) {
        VARINT(v);
        if (bad || v > (uint32_t)(e - s)) return NULL;
        s += v;
        namebytes += v + 1;
    }
    VARINT(nfeat); VARINT(nring); VARINT(npts);
    if (bad || nfeat > 1000000 || nring > 4000000 || npts > 8000000 || nring < nfeat || npts > (uint32_t)(e - s)) return NULL;
    size_t mem = sizeof(bgm_tile_t) + nfeat * sizeof(bgm_feat_t) + (nring + 1) * sizeof(uint32_t)
                 + nstr * sizeof(char *) + npts * 2 * sizeof(int16_t) + namebytes + 16;
    bgm_tile_t *t = calloc(1, mem);
    if (!t) return NULL;
    uint8_t *m = (uint8_t *)(t + 1);
    t->feat = (bgm_feat_t *)m; m += nfeat * sizeof(bgm_feat_t);
    t->ring_start = (uint32_t *)m; m += (nring + 1) * sizeof(uint32_t);
    t->names = (const char **)m; m += nstr * sizeof(char *);
    t->pts = (int16_t *)m; m += npts * 2 * sizeof(int16_t);
    char *names = (char *)m;
    t->level = level; t->tx = tx; t->ty = ty;
    t->mem = mem; t->nfeat = (int)nfeat; t->nring = (int)nring; t->nname = (int)nstr;
    const uint8_t *feats = s;
    s = strs;
    for (uint32_t i = 0; i < nstr; i++) {
        VARINT(v);
        memcpy(names, s, v);
        names[v] = 0;
        t->names[i] = names;
        names += v + 1;
        s += v;
    }
    s = feats;
    uint32_t ri = 0, pi = 0;
    int last_kind = -1;
    int16_t *o = t->pts;
    for (uint32_t i = 0; i < nfeat && !bad; i++) {
        bgm_feat_t *f = &t->feat[i];
        if (e - s < 2) { bad = 1; break; }
        f->kind = s[0] < BGK_COUNT ? s[0] : BGK_NONE;
        f->flags = s[1];
        s += 2;
        f->name = 0xFFFF;
        if (f->flags & BGM_F_NAME) { VARINT(v); if (v < nstr) f->name = (uint16_t)v; }
        if (f->kind < last_kind) f->kind = (uint8_t)last_kind;   /* keep the draw order monotonic */
        while (last_kind < f->kind) t->kind_first[++last_kind] = (int)i;
        f->ring = ri;
        int g = f->flags & BGM_F_GEOM;
        int32_t x, y, px = 0, py = 0, x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
        uint32_t nr = 1, k;
        if (g == BGM_F_AREA) VARINT(nr);
        if (g == BGM_F_POINT) nr = 1;
        if (nr > 65535 || ri + nr > nring) { bad = 1; break; }
        f->nrings = (uint16_t)nr;
        for (uint32_t r = 0; r < nr && !bad; r++) {
            if (g == BGM_F_POINT) k = 1;
            else VARINT(k);
            if (k > npts - pi) { bad = 1; break; }
            t->ring_start[ri++] = pi;
            for (uint32_t j = 0; j < k; j++) {
                ZZ(x); ZZ(y);
                if (g == BGM_F_POINT) { px = x; py = y; }
                else { px += x; py += y; }
                if (px < x0) x0 = px;
                if (px > x1) x1 = px;
                if (py < y0) y0 = py;
                if (py > y1) y1 = py;
                o[0] = (int16_t)px; o[1] = (int16_t)py; o += 2;
            }
            pi += k;
        }
        f->bx0 = (int16_t)x0; f->by0 = (int16_t)y0; f->bx1 = (int16_t)x1; f->by1 = (int16_t)y1;
    }
    if (bad || ri != nring || pi != npts) { free(t); return NULL; }
    t->ring_start[ri] = pi;
    while (last_kind < BGK_COUNT) t->kind_first[++last_kind] = (int)nfeat;
    return t;
}

static void evict(bgm_pack_t *p)
{
    while (p->mem > p->limit) {
        bgm_tile_t **victim = NULL;
        for (int i = 0; i < HASH; i++)
            for (bgm_tile_t **pp = &p->hash[i]; *pp; pp = &(*pp)->next)
                if ((*pp)->used != p->frame && (!victim || (*pp)->used < (*victim)->used)) victim = pp;
        if (!victim) return;                     /* everything is in use by this frame */
        bgm_tile_t *t = *victim;
        *victim = t->next;
        p->mem -= t->mem;
        tile_free(t);
    }
}

const bgm_tile_t *bgm_tile(bgm_pack_t *p, int level, int tx, int ty)
{
    if (level < 0 || level >= (int)p->hdr.nlevels || tx < 0 || ty < 0 || tx > 0xFFFF || ty > 0xFFFF) return NULL;
    unsigned h = hkey(level, tx, ty);
    for (bgm_tile_t *t = p->hash[h]; t; t = t->next)
        if (t->level == level && t->tx == tx && t->ty == ty) { t->used = p->frame; p->st.hits++; return t; }
    for (int i = 0; i < p->nempty; i++)
        if (p->empties[i].level == level && p->empties[i].tx == tx && p->empties[i].ty == ty) { p->st.empty++; return NULL; }
    const bgm_index_t *ix = find(p, level, (uint32_t)ty << 16 | (uint32_t)tx);
    if (!ix) {
        empty_t *e = &p->empties[p->nempty < 64 ? p->nempty++ : (int)(p->frame % 64)];
        e->level = level; e->tx = tx; e->ty = ty; e->used = p->frame;
        p->st.empty++;
        return NULL;
    }
    uint32_t t0 = now_us();
    if (ix->len > p->rcap) {
        uint8_t *nb = realloc(p->rbuf, ix->len);
        if (!nb) return NULL;
        p->rbuf = nb;
        p->rcap = ix->len;
    }
    if (read_at(p->fd, p->rbuf, ix->len, ix->off)) return NULL;
    bgm_tile_t *t = decode(p->rbuf, ix->len, level, tx, ty);
    p->st.misses++;
    p->st.bytes_read += ix->len;
    p->st.decode_us += now_us() - t0;
    if (!t) return NULL;
    t->used = p->frame;
    t->next = p->hash[h];
    p->hash[h] = t;
    p->mem += t->mem;
    evict(p);
    return t;
}
