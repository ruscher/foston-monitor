/* bgvoice-pack.c - Big GPS voice pack builder (runs on the PC).
 *
 *   bgvoice-pack LIST.tsv OUT.bgv
 *
 * LIST: one "phrase<TAB>file.wav" per line (16-bit mono WAV, all the same rate, as made by
 * piper-tts).  Each clip: silence trimmed (30 ms kept), peak normalized to -1 dBFS (gain at
 * most 4x), 10 ms fades, IMA ADPCM.  Writes src/ui/biggps/bgvoice.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../src/ui/biggps/bgvoice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    uint8_t *adpcm;
    size_t alen;
    uint32_t n;
} clip_t;

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static int16_t *read_wav(const char *path, uint32_t *n, uint32_t *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len || len < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fclose(f); free(b); return NULL;
    }
    fclose(f);
    long o = 12;
    int16_t *pcm = NULL;
    int ok_fmt = 0;
    while (o + 8 <= len) {
        uint32_t cl = rd32(b + o + 4);
        if (!memcmp(b + o, "fmt ", 4) && cl >= 16)
            ok_fmt = (b[o + 8] | b[o + 9] << 8) == 1 && (b[o + 10] | b[o + 11] << 8) == 1 && (b[o + 22] | b[o + 23] << 8) == 16,
            *rate = rd32(b + o + 12);
        if (!memcmp(b + o, "data", 4) && ok_fmt) {
            if (o + 8 + (long)cl > len) cl = (uint32_t)(len - o - 8);
            *n = cl / 2;
            pcm = malloc((size_t)*n * 2 + 2);
            if (pcm) for (uint32_t i = 0; i < *n; i++) pcm[i] = (int16_t)(b[o + 8 + 2 * i] | b[o + 9 + 2 * i] << 8);
            break;
        }
        o += 8 + cl + (cl & 1);
    }
    free(b);
    return pcm;
}

static int cmp(const void *a, const void *b)
{
    const clip_t *x = a, *y = b;
    size_t lx = strlen(x->key), ly = strlen(y->key);
    int c = memcmp(x->key, y->key, lx < ly ? lx : ly);
    return c ? c : (lx > ly) - (lx < ly);
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "uso: %s LISTA.tsv SAIDA.bgv\n", argv[0]); return 2; }
    FILE *l = fopen(argv[1], "r");
    if (!l) { perror(argv[1]); return 1; }
    size_t cap = 1024, n = 0;
    clip_t *c = malloc(cap * sizeof *c);
    uint32_t rate = 0;
    double secs = 0;
    char line[4096];
    while (fgets(line, sizeof line, l)) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!tab) continue;
        *tab = 0;
        uint32_t ns = 0, r = 0;
        int16_t *pcm = read_wav(tab + 1, &ns, &r);
        if (!pcm || !ns) { fprintf(stderr, "%s: WAV inválido\n", tab + 1); return 1; }
        if (rate && r != rate) { fprintf(stderr, "%s: taxa %u diferente de %u\n", tab + 1, r, rate); return 1; }
        rate = r;
        uint32_t a = 0, b = ns, keep = rate * 30 / 1000;
        while (a < ns && abs(pcm[a]) < 400) a++;
        while (b > a && abs(pcm[b - 1]) < 400) b--;
        a = a > keep ? a - keep : 0;
        b = b + keep < ns ? b + keep : ns;
        int16_t *s = pcm + a;
        uint32_t m = b > a ? b - a : 1;
        int peak = 1;
        for (uint32_t i = 0; i < m; i++) if (abs(s[i]) > peak) peak = abs(s[i]);
        double g = 29205.0 / peak;                      /* -1 dBFS */
        if (g > 4) g = 4;
        uint32_t fade = rate / 100;
        for (uint32_t i = 0; i < m; i++) {
            double f = 1;
            if (i < fade) f = (double)i / fade;
            if (m - 1 - i < fade) f = (double)(m - 1 - i) / fade;
            double v = s[i] * g * f;
            s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (n == cap) c = realloc(c, (cap *= 2) * sizeof *c);
        char key[512];
        bgv_normalize(line, key, sizeof key);
        c[n].key = strdup(key);
        c[n].adpcm = malloc(4 + (m + 1) / 2);
        c[n].alen = bgv_encode(s, m, c[n].adpcm);
        c[n].n = m;
        secs += (double)m / rate;
        n++;
        free(pcm);
    }
    fclose(l);
    qsort(c, n, sizeof *c, cmp);
    size_t w = 0;                                       /* the same phrase twice: keep one */
    for (size_t i = 0; i < n; i++)
        if (!w || cmp(&c[w - 1], &c[i])) c[w++] = c[i];
    n = w;
    uint32_t klen = 0, dlen = 0;
    for (size_t i = 0; i < n; i++) { klen += (uint32_t)strlen(c[i].key); dlen += (uint32_t)c[i].alen; }
    uint32_t ko = 24 + (uint32_t)n * 16, dof = ko + klen;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[2]);
    FILE *f = fopen(tmp, "wb");
    if (!f) { perror(tmp); return 1; }
    fwrite(BGV_MAGIC, 1, 4, f);
    put32(f, BGV_VERSION);
    put32(f, (uint32_t)n);
    put32(f, rate);
    put32(f, ko);
    put32(f, dof);
    uint32_t kp = 0, dp = 0;
    for (size_t i = 0; i < n; i++) {
        size_t kl = strlen(c[i].key);
        put32(f, kp);
        uint8_t b[4] = { (uint8_t)kl, (uint8_t)(kl >> 8), 0, 0 };
        fwrite(b, 1, 4, f);
        put32(f, dp);
        put32(f, c[i].n);
        kp += (uint32_t)kl;
        dp += (uint32_t)c[i].alen;
    }
    for (size_t i = 0; i < n; i++) fwrite(c[i].key, 1, strlen(c[i].key), f);
    for (size_t i = 0; i < n; i++) fwrite(c[i].adpcm, 1, c[i].alen, f);
    if (fclose(f) || rename(tmp, argv[2])) { perror(argv[2]); return 1; }
    printf("%s: %zu frases, %.0f s de fala, %u Hz, %.1f MB\n", argv[2], n, secs, rate, (dof + dlen) / 1e6);
    return 0;
}
