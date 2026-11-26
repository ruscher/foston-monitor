/* yuvz.c — ver yuvz.h. Com YUVZ_ENCODER (PC, liblz4) também o codificador.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "yuvz.h"
#include <stdlib.h>
#include <string.h>
#include "../ui/core/lz4dec.h"

static int grow(uint8_t **p, size_t *cap, size_t n)
{
    if(n <= *cap) return 0;
    uint8_t *q = realloc(*p, n);
    if(!q) return -1;
    *p = q;
    *cap = n;
    return 0;
}

/* soma de conferência do quadro reconstruído (a mesma do proto.h do Big Remote Play) */
static uint32_t sum32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for(size_t i = 0; i + 4 <= n; i += 4) {
        a += (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        b += a;
    }
    return a ^ (b << 7 | b >> 25);
}

int yuvz_decode(yuvz_dec_t *d, const uint8_t *p, size_t l)
{
    if(l < 5) return -1;
    int w = p[0] | p[1] << 8, h = p[2] | p[3] << 8, delta = p[4] & 1, with_sum = p[4] & 2;
    if(w <= 0 || (w & 7) || h <= 0 || w > 1024 || h > 1024) return -1;
    size_t need = yuvz_size(w, h), hl = 5 + (with_sum ? 4u : 0u);
    int cw = w / 2, chh = (h + 1) / 2;
    size_t ml = delta ? (size_t)(chh + 7) / 8 : 0;
    if(l < hl + ml) return -1;
    if(delta && (w != d->w || h != d->h)) { d->w = 0; return -1; }   /* sem o quadro base */
    uint32_t want = with_sum ? (uint32_t)p[5] | (uint32_t)p[6] << 8 | (uint32_t)p[7] << 16 | (uint32_t)p[8] << 24 : 0;
    const uint8_t *mask = p + hl, *z = p + hl + ml;
    size_t zn = l - hl - ml;
    int changed = 1;
    if(delta) {
        size_t xn = 0;
        int pairs = 0;
        for(int r = 0; r < chh; r++)
            if(mask[r / 8] >> (r % 8) & 1) { pairs++; xn += (size_t)(2 * r + 1 < h ? 2 : 1) * w + 2 * (size_t)cw; }
        if(grow(&d->x, &d->x_cap, xn + 1)) return -1;
        if(xn ? lz4_block_decode(z, zn, d->x, xn) != (long)xn : zn != 0) { d->w = 0; return -1; }
        const uint8_t *x = d->x;
        uint8_t *yu = d->yuv + (size_t)w * h, *yv = yu + (size_t)cw * chh;
        for(int r = 0; r < chh; r++) {
            if(!(mask[r / 8] >> (r % 8) & 1)) continue;
            size_t ly = (size_t)(2 * r + 1 < h ? 2 : 1) * w;
            uint8_t *dy = d->yuv + (size_t)2 * r * w;
            for(size_t i = 0; i < ly; i++) dy[i] ^= *x++;
            for(int i = 0; i < cw; i++) yu[(size_t)r * cw + i] ^= *x++;
            for(int i = 0; i < cw; i++) yv[(size_t)r * cw + i] ^= *x++;
        }
        changed = pairs > 0;
    } else {
        if(grow(&d->yuv, &d->cap, need)) return -1;
        if(lz4_block_decode(z, zn, d->yuv, need) != (long)need) { d->w = 0; return -1; }
    }
    d->w = w;
    d->h = h;
    if(with_sum && sum32(d->yuv, need) != want) { d->w = 0; d->bad++; return -1; }
    return changed;
}

void yuvz_dec_free(yuvz_dec_t *d)
{
    free(d->yuv);
    free(d->x);
    memset(d, 0, sizeof *d);
}

#ifdef YUVZ_ENCODER
#include <lz4hc.h>

static int compress_into(const uint8_t *src, size_t n, uint8_t **out, size_t *cap)
{
    size_t zmax = (size_t)LZ4_compressBound((int)n);
    if(grow(out, cap, zmax)) return -1;
    return LZ4_compress_HC((const char *)src, (char *)*out, (int)n, (int)zmax, 9);
}

long yuvz_encode(yuvz_enc_t *e, const uint8_t *yuv, int w, int h, int force_key, const uint8_t **out)
{
    if(w <= 0 || (w & 7) || h <= 0 || w > 1024 || h > 1024) return -1;
    size_t n = yuvz_size(w, h), ny = (size_t)w * h;
    if(w != e->w || h != e->h) e->have_prev = 0;
    if(grow(&e->cur, &e->cap, n) || !(e->prev = e->prev && e->w == w && e->h == h ? e->prev : realloc(e->prev, n)) ||
       !(e->x = e->x && e->w == w && e->h == h ? e->x : realloc(e->x, n)))
        return -1;
    e->w = w;
    e->h = h;
    int delta = e->have_prev && !force_key && e->since_key < 60;
    for(size_t i = 0; i < n; i++) {                       /* 6 bits de brilho, 5 de cor (arredondado) */
        int luma = i < ny, raw = yuv[i];
        if(delta) {                                       /* histerese: menos de um degrau, fica */
            int dd = raw - e->prev[i];
            if(dd < 0) dd = -dd;
            if(dd <= (luma ? 3 : 6)) { e->cur[i] = e->prev[i]; continue; }
        }
        int v = raw + (luma ? 2 : 4);
        if(v > 255) v = 255;
        e->cur[i] = (uint8_t)(v & (luma ? 0xFC : 0xF8));
    }
    int cw = w / 2, chh = (h + 1) / 2;
    uint8_t mask[64];                                     /* até 512 pares (h <= 1024) */
    size_t xn = 0;
    int dirty = 0;
    if(delta) {
        memset(mask, 0, (size_t)(chh + 7) / 8);
        const uint8_t *lu = e->cur + ny, *lv = lu + (size_t)cw * chh, *pu = e->prev + ny, *pv = pu + (size_t)cw * chh;
        for(int r = 0; r < chh; r++) {
            int ly = 2 * r + 1 < h ? 2 : 1;
            size_t ro = (size_t)2 * r * w, co = (size_t)r * cw;
            if(!memcmp(e->cur + ro, e->prev + ro, (size_t)ly * w) && !memcmp(lu + co, pu + co, (size_t)cw) &&
               !memcmp(lv + co, pv + co, (size_t)cw))
                continue;
            mask[r / 8] |= (uint8_t)(1 << (r % 8));
            dirty++;
            for(size_t i = 0; i < (size_t)ly * w; i++) e->x[xn++] = e->cur[ro + i] ^ e->prev[ro + i];
            for(int i = 0; i < cw; i++) e->x[xn++] = lu[co + i] ^ pu[co + i];
            for(int i = 0; i < cw; i++) e->x[xn++] = lv[co + i] ^ pv[co + i];
        }
    }
    memcpy(e->prev, e->cur, n);
    e->have_prev = 1;
    int zn = -1, sent_delta = 0, have_key = 0;
    const uint8_t *z = NULL;
    if(delta && !dirty) zn = 0, sent_delta = 1;           /* nada mudou: só o cabeçalho */
    else if(delta) {
        zn = compress_into(e->x, xn, &e->zd, &e->zd_cap);
        z = e->zd;
        sent_delta = zn > 0;
        if(dirty * 10 > chh * 8) {                        /* quase tudo mudou: o inteiro sai menor? */
            int kn = compress_into(e->cur, n, &e->zk, &e->zk_cap);
            if(kn > 0 && (zn <= 0 || kn < zn + zn / 4)) { zn = kn; z = e->zk; sent_delta = 0; have_key = 1; }
        }
    }
    if(!sent_delta && !have_key) { zn = compress_into(e->cur, n, &e->zk, &e->zk_cap); z = e->zk; }
    if(zn < 0) { e->have_prev = 0; return -1; }
    e->since_key = sent_delta ? e->since_key + 1 : 1;
    int with_sum = !sent_delta || e->n++ % 30 == 0;       /* conferência: inteiros e 1 a cada 30 */
    size_t ml = sent_delta ? (size_t)(chh + 7) / 8 : 0, total = 5 + (with_sum ? 4u : 0u) + ml + (size_t)zn;
    if(grow(&e->out, &e->out_cap, total)) return -1;
    uint8_t *o = e->out;
    o[0] = (uint8_t)w; o[1] = (uint8_t)(w >> 8); o[2] = (uint8_t)h; o[3] = (uint8_t)(h >> 8);
    o[4] = (uint8_t)((sent_delta ? 1 : 0) | (with_sum ? 2 : 0));
    o += 5;
    if(with_sum) { uint32_t s = sum32(e->cur, n); for(int i = 0; i < 4; i++) *o++ = (uint8_t)(s >> 8 * i); }
    if(ml) { memcpy(o, mask, ml); o += ml; }
    if(zn) memcpy(o, z, (size_t)zn);
    *out = e->out;
    return (long)total;
}

void yuvz_enc_free(yuvz_enc_t *e)
{
    free(e->cur); free(e->prev); free(e->x); free(e->zk); free(e->zd); free(e->out);
    memset(e, 0, sizeof *e);
}
#endif
