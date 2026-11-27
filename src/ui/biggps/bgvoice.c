/* bgvoice.c - see bgvoice.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgvoice.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct bgv_pack {
    void *map;                 /* header, index and keys (the audio stays in the file) */
    size_t maplen;
    int fd;
    uint32_t data_off;
    const uint8_t *idx, *keys;
    uint32_t n, rate;
};

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

bgv_pack_t *bgv_open(const char *path, char *err, int errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        snprintf(err, (size_t)errlen, "%s: não abre", path);
        if (fd >= 0) close(fd);
        return NULL;
    }
    uint8_t hd[24];
    size_t len = (size_t)st.st_size;
    if (len < 24 || pread(fd, hd, 24, 0) != 24) { close(fd); snprintf(err, (size_t)errlen, "%s: formato desconhecido", path); return NULL; }
    uint32_t n = rd32(hd + 8), ko = rd32(hd + 16), dof = rd32(hd + 20);
    if (memcmp(hd, BGV_MAGIC, 4) || rd32(hd + 4) != BGV_VERSION || dof > len || dof < 24) {
        close(fd);
        snprintf(err, (size_t)errlen, "%s: formato desconhecido", path);
        return NULL;
    }
    void *m = mmap(NULL, dof, PROT_READ, MAP_PRIVATE, fd, 0);     /* só o índice e as chaves */
    if (m == MAP_FAILED) { close(fd); snprintf(err, (size_t)errlen, "%s: não mapeia", path); return NULL; }
    const uint8_t *h = m;
    int ok = 24 + (uint64_t)n * 16 <= ko && ko <= dof;
    for (uint32_t i = 0; ok && i < n; i++) {              /* every entry inside the file */
        const uint8_t *e = h + 24 + (size_t)i * 16;
        uint64_t kend = (uint64_t)ko + rd32(e) + rd16(e + 4);
        uint64_t dend = (uint64_t)dof + rd32(e + 8) + 4 + (rd32(e + 12) + 1) / 2;
        ok = kend <= dof && dend <= len;
    }
    if (!ok) {
        snprintf(err, (size_t)errlen, "%s: formato desconhecido ou arquivo cortado", path);
        munmap(m, dof);
        close(fd);
        return NULL;
    }
    bgv_pack_t *p = calloc(1, sizeof *p);
    if (!p) { munmap(m, dof); close(fd); snprintf(err, (size_t)errlen, "sem memória"); return NULL; }
    p->map = m;
    p->maplen = dof;
    p->fd = fd;
    p->data_off = dof;
    p->n = n;
    p->rate = rd32(hd + 12);
    p->idx = h + 24;
    p->keys = h + ko;
    return p;
}

void bgv_close(bgv_pack_t *p)
{
    if (!p) return;
    munmap(p->map, p->maplen);
    close(p->fd);
    free(p);
}

void bgv_clip_release(bgv_clip_t *c)
{
    if (!c) return;
    free(c->buf);
    c->buf = NULL;
    c->data = NULL;
}

uint32_t bgv_count(const bgv_pack_t *p) { return p ? p->n : 0; }
uint32_t bgv_rate(const bgv_pack_t *p) { return p ? p->rate : 0; }

void bgv_normalize(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int space = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s == ' ' || *s == '\t') s++;
    for (; *s && o + 3 < n; s++) {
        unsigned char c = *s;
        if (c == ' ' || c == '\t' || c == '\n') { space = 1; continue; }
        if (space && o) out[o++] = ' ';
        space = 0;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        else if (c == 0xC3 && s[1] >= 0x80 && s[1] <= 0x9E && s[1] != 0x97) {   /* À..Þ (not ×) */
            out[o++] = (char)c;
            out[o++] = (char)(s[1] + 0x20);
            s++;
            continue;
        }
        out[o++] = (char)c;
    }
    /* trailing punctuation and spaces: ", . ; : ! ?" and "…" (E2 80 A6) */
    for (;;) {
        if (o && strchr(",.;:!? ", out[o - 1])) { o--; continue; }
        if (o >= 3 && (unsigned char)out[o - 3] == 0xE2 && (unsigned char)out[o - 2] == 0x80 && (unsigned char)out[o - 1] == 0xA6) { o -= 3; continue; }
        break;
    }
    out[o] = 0;
}

int bgv_find(const bgv_pack_t *p, const char *text, bgv_clip_t *out)
{
    if (!p || !text) return 0;
    char key[512];
    bgv_normalize(text, key, sizeof key);
    size_t kl = strlen(key);
    uint32_t lo = 0, hi = p->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = p->idx + (size_t)mid * 16;
        size_t el = rd16(e + 4);
        int c = memcmp(p->keys + rd32(e), key, el < kl ? el : kl);
        if (!c) c = el < kl ? -1 : el > kl;
        if (!c) {
            uint32_t ns = rd32(e + 12);
            size_t sz = 4 + ((size_t)ns + 1) / 2;
            uint8_t *b = malloc(sz);
            if (!b) return 0;
            if (pread(p->fd, b, sz, (off_t)p->data_off + rd32(e + 8)) != (ssize_t)sz) { free(b); return 0; }   /* cortado */
            out->buf = b;
            out->data = b;
            out->nsamples = ns;
            return 1;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static const int16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static const int8_t idx_adj[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int step_nibble(int *pred, int *index, int nib)
{
    int st = steps[*index], diff = st >> 3;
    if (nib & 4) diff += st;
    if (nib & 2) diff += st >> 1;
    if (nib & 1) diff += st >> 2;
    *pred += nib & 8 ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += idx_adj[nib];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return *pred;
}

void bgv_decode(const bgv_clip_t *c, int16_t *out)
{
    const uint8_t *d = c->data;
    int pred = (int16_t)rd16(d), index = d[2] > 88 ? 88 : d[2];
    d += 4;
    for (uint32_t i = 0; i < c->nsamples; i++) {
        int nib = i & 1 ? d[i >> 1] >> 4 : d[i >> 1] & 15;
        out[i] = (int16_t)step_nibble(&pred, &index, nib);
    }
}

size_t bgv_encode(const int16_t *in, uint32_t n, uint8_t *out)
{
    int pred = n ? in[0] : 0, index = 0;
    /* a start index that fits the first samples avoids a loud first click */
    if (n > 1) {
        int d = abs(in[1] - in[0]);
        while (index < 88 && steps[index] < d) index++;
    }
    out[0] = (uint8_t)pred;
    out[1] = (uint8_t)(pred >> 8);
    out[2] = (uint8_t)index;
    out[3] = 0;
    uint8_t *d = out + 4;
    memset(d, 0, (n + 1) / 2);
    for (uint32_t i = 0; i < n; i++) {
        int diff = in[i] - pred, st = steps[index], nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        if (diff >= st) { nib |= 4; diff -= st; }
        if (diff >= st >> 1) { nib |= 2; diff -= st >> 1; }
        if (diff >= st >> 2) nib |= 1;
        step_nibble(&pred, &index, nib);                  /* the decoder's state, exactly */
        d[i >> 1] |= (uint8_t)(i & 1 ? nib << 4 : nib);
    }
    return 4 + (n + 1) / 2;
}
