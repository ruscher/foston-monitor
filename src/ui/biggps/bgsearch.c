/* bgsearch.c - see bgsearch.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#include "bgsearch.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXW 6                 /* words used from the query */
#define MAXCAND 40000

struct bgs_index {
    int fd;
    off_t size;
    bgs_header_t h;
};

static int rd(const bgs_index_t *ix, off_t off, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(ix->fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

bgs_index_t *bgs_open(const char *path, char *err, size_t errlen)
{
    bgs_index_t *ix = calloc(1, sizeof *ix);
    struct stat st;
    if (!ix) { snprintf(err, errlen, "sem memória"); return NULL; }
    ix->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ix->fd < 0 || fstat(ix->fd, &st) || rd(ix, 0, &ix->h, sizeof ix->h)) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        goto fail;
    }
    ix->size = st.st_size;
    const bgs_header_t *h = &ix->h;
    if (h->magic != BGS_MAGIC || h->version != BGS_VERSION
        || (off_t)h->off_entries + (off_t)h->nentries * (off_t)sizeof(bgs_entry_t) > ix->size
        || (off_t)h->off_tokens + (off_t)h->ntokens * (off_t)sizeof(bgs_token_t) > ix->size
        || (off_t)h->off_postings + (off_t)h->npostings * 4 > ix->size
        || (off_t)h->off_strings + h->strings_len > ix->size) {
        snprintf(err, errlen, "%s: não é um índice de busca do Big GPS (versão %u)", path, BGS_VERSION);
        goto fail;
    }
    return ix;
fail:
    bgs_close(ix);
    return NULL;
}

void bgs_close(bgs_index_t *ix)
{
    if (!ix) return;
    if (ix->fd >= 0) close(ix->fd);
    free(ix);
}

uint32_t bgs_count(const bgs_index_t *ix) { return ix->h.nentries; }

static void str_at(const bgs_index_t *ix, uint32_t off, char *buf, size_t n)
{
    buf[0] = 0;
    if (off == 0xFFFFFFFFu || off >= ix->h.strings_len || n < 2) return;
    size_t want = n - 1;
    if (off + want > ix->h.strings_len) want = ix->h.strings_len - off;
    if (rd(ix, (off_t)ix->h.off_strings + off, buf, want)) { buf[0] = 0; return; }
    buf[want] = 0;
    /* cut at the NUL; a cut inside a multibyte character is removed */
    size_t l = strlen(buf);
    if (l == want) while (l && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) buf[--l] = 0;
    if (l && ((unsigned char)buf[l - 1] & 0xC0) == 0xC0 && l == want) buf[--l] = 0;
}

static int token(const bgs_index_t *ix, uint32_t i, bgs_token_t *t, char *word, size_t n)
{
    if (rd(ix, (off_t)ix->h.off_tokens + (off_t)i * sizeof *t, t, sizeof *t)) return -1;
    str_at(ix, t->text, word, n);
    return 0;
}

/* first token >= w (or > every word with prefix w when upper) */
static uint32_t bound(const bgs_index_t *ix, const char *w, int upper)
{
    uint32_t lo = 0, hi = ix->h.ntokens;
    size_t wl = strlen(w);
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        bgs_token_t t;
        char tw[64];
        if (token(ix, m, &t, tw, sizeof tw)) return lo;
        int c = upper ? strncmp(tw, w, wl) : strcmp(tw, w);
        if (upper ? c <= 0 : c < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* every query word is a prefix of some word of the text */
static int all_match(const char *norm, char words[][32], int nw, int *exact)
{
    *exact = 1;
    for (int k = 0; k < nw; k++) {
        size_t wl = strlen(words[k]);
        const char *p = norm;
        int found = 0, ex = 0;
        while (*p) {
            const char *e = strchr(p, ' ');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l >= wl && !strncmp(p, words[k], wl)) { found = 1; if (l == wl) ex = 1; }
            p += l;
            while (*p == ' ') p++;
        }
        if (!found) return 0;
        if (!ex) *exact = 0;
    }
    return 1;
}

static uint32_t dist_m(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y0 / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat);
    double dx = (double)(int32_t)(x1 - x0), dy = (double)(int32_t)(y1 - y0);
    double d = sqrt(dx * dx + dy * dy) * mpu;
    return d > 4e9 ? 4000000000u : (uint32_t)d;
}

static void fill(const bgs_index_t *ix, const bgs_entry_t *e, uint32_t x, uint32_t y, bgs_result_t *r)
{
    str_at(ix, e->name, r->name, sizeof r->name);
    str_at(ix, e->context, r->context, sizeof r->context);
    r->x = e->x;
    r->y = e->y;
    r->type = e->type;
    r->cat = e->cat;
    r->dist_m = dist_m(x, y, e->x, e->y);
}

static void insert(bgs_result_t *out, double *score, int *n, int max, const bgs_result_t *r, double s)
{
    /* the same place mapped twice (a point and a building): keep the better one */
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i].name, r->name) && dist_m(out[i].x, out[i].y, r->x, r->y) < 300) {
            if (s <= score[i]) return;
            for (int k = i; k + 1 < *n; k++) { out[k] = out[k + 1]; score[k] = score[k + 1]; }
            (*n)--;
            break;
        }
    if (*n == max && s <= score[max - 1]) return;
    int k = *n < max ? (*n)++ : max - 1;
    while (k > 0 && score[k - 1] < s) { out[k] = out[k - 1]; score[k] = score[k - 1]; k--; }
    out[k] = *r;
    score[k] = s;
}

int bgs_query(bgs_index_t *ix, const char *text, uint32_t x, uint32_t y, bgs_result_t *out, int max)
{
    char q[256], words[MAXW][32];
    int nw = 0;
    size_t n = bgs_norm(text, q, sizeof q);
    for (size_t a = 0; a < n && nw < MAXW;) {
        size_t b = a;
        while (b < n && q[b] != ' ') b++;
        if (b - a < 32 && !bgs_stopword(q + a, b - a)) {
            memcpy(words[nw], q + a, b - a);
            words[nw][b - a] = 0;
            nw++;
        }
        a = b + 1;
    }
    if (!nw || max < 1) return 0;
    /* the word with the fewest entries drives the search */
    uint32_t best_lo = 0, best_hi = 0;
    uint64_t best_cnt = UINT64_MAX;
    for (int k = 0; k < nw; k++) {
        uint32_t lo = bound(ix, words[k], 0), hi = bound(ix, words[k], 1);
        uint64_t cnt = 0;
        for (uint32_t i = lo; i < hi && cnt < best_cnt; i++) {
            bgs_token_t t;
            char tw[64];
            if (!token(ix, i, &t, tw, sizeof tw)) cnt += t.count;
        }
        if (cnt < best_cnt) { best_cnt = cnt; best_lo = lo; best_hi = hi; }
    }
    if (!best_cnt) return 0;
    uint32_t *cand = malloc(MAXCAND * sizeof *cand);
    if (!cand) return 0;
    int nc = 0;
    for (uint32_t i = best_lo; i < best_hi && nc < MAXCAND; i++) {
        bgs_token_t t;
        char tw[64];
        if (token(ix, i, &t, tw, sizeof tw) || (uint64_t)t.first + t.count > ix->h.npostings) continue;
        uint32_t take = t.count;
        if (take > (uint32_t)(MAXCAND - nc)) take = (uint32_t)(MAXCAND - nc);
        if (rd(ix, (off_t)ix->h.off_postings + (off_t)t.first * 4, cand + nc, take * 4)) continue;
        nc += (int)take;
    }
    qsort(cand, (size_t)nc, sizeof *cand, cmp_u32);
    double *score = malloc((size_t)max * sizeof *score);
    int nr = 0;
    for (int i = 0; i < nc && score; i++) {
        if (i && cand[i] == cand[i - 1]) continue;
        if (cand[i] >= ix->h.nentries) continue;
        bgs_entry_t e;
        if (rd(ix, (off_t)ix->h.off_entries + (off_t)cand[i] * sizeof e, &e, sizeof e)) continue;
        bgs_result_t r;
        fill(ix, &e, x, y, &r);
        char norm[256], full[200];
        snprintf(full, sizeof full, "%s %s", r.name, r.context);
        bgs_norm(full, norm, sizeof norm);
        int exact;
        if (!all_match(norm, words, nw, &exact)) continue;
        double s = e.rank + (exact ? 25 : 0) - 15.0 * log2(1.0 + r.dist_m / 1000.0);
        insert(out, score, &nr, max, &r, s);
    }
    free(score);
    free(cand);
    return nr;
}

int bgs_nearby(bgs_index_t *ix, int cat, uint32_t x, uint32_t y, uint32_t max_m, bgs_result_t *out, int max)
{
    /* linear scan in blocks (fine for a region; a national index needs a spatial order) */
    enum { BLK = 512 };
    bgs_entry_t blk[BLK];
    double *score = malloc((size_t)(max > 0 ? max : 1) * sizeof *score);
    int nr = 0;
    for (uint32_t i = 0; i < ix->h.nentries && score; i += BLK) {
        uint32_t k = ix->h.nentries - i < BLK ? ix->h.nentries - i : BLK;
        if (rd(ix, (off_t)ix->h.off_entries + (off_t)i * sizeof(bgs_entry_t), blk, k * sizeof(bgs_entry_t))) break;
        for (uint32_t j = 0; j < k; j++) {
            if (blk[j].type != BGS_T_POI || blk[j].cat != cat) continue;
            uint32_t d = dist_m(x, y, blk[j].x, blk[j].y);
            if (d > max_m) continue;
            if (nr == max && -(double)d <= score[max - 1]) continue;
            bgs_result_t r;
            fill(ix, &blk[j], x, y, &r);
            insert(out, score, &nr, max, &r, -(double)d);
        }
    }
    free(score);
    return nr;
}
