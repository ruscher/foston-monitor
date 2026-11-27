/* test_yuvz.c - TV frames over USB (src/tv/yuvz.c): what the FOSTON rebuilds is exactly what the
 * PC quantized, through full frames, deltas, unchanged frames, a size change (odd height) and a
 * full frame with a damaged checksum (refused, then a new full frame).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../tv/yuvz.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rnd = 12345;
static int noise(void) { rnd = rnd * 1103515245u + 12345u; return (int)(rnd >> 16 & 7) - 3; }

/* gradiente com um quadrado que anda e um pouco de ruído (como vídeo de TV) */
static void frame(uint8_t *f, int w, int h, int t, int still)
{
    int cw = w / 2, chh = (h + 1) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = (x * 255 / w + y) / 2 + (still ? 0 : noise());
            if (x >= t * 3 % w && x < t * 3 % w + 40 && y >= 50 && y < 90) v = 230;
            f[y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    for (int i = 0; i < 2 * cw * chh; i++) f[(size_t)w * h + i] = (uint8_t)(128 + (i % cw) / 8);
}

int main(void)
{
    yuvz_enc_t e = { 0 };
    yuvz_dec_t d = { 0 };
    static uint8_t f[1024 * 1024];
    long total = 0;
    int n = 0, deltas = 0;
    for (int t = 0; t < 130; t++) {
        int w = t < 90 ? 320 : 400, h = t < 90 ? 180 : 225;   /* troca para a tela cheia (altura ímpar) */
        int still = t >= 40 && t < 50;                      /* parado: nada muda depois do 1º */
        frame(f, w, h, still ? 40 : t, still);
        const uint8_t *m;
        long l = yuvz_encode(&e, f, w, h, t == 60 || t == 70, &m);
        CHECK(l > 0, "quadro %d: codificou %ld", t, l);
        if (l <= 0) break;
        if (m[4] & 1) deltas++;
        if (t == 60) {                                      /* inteiro com a soma estragada: recusa */
            CHECK(!(m[4] & 1) && (m[4] & 2), "quadro %d: inteiro pedido sem soma (flags %d)", t, m[4]);
            static uint8_t bad[200000];
            memcpy(bad, m, (size_t)l);
            bad[5] ^= 0x55;                                 /* o primeiro byte da soma */
            unsigned long bad0 = d.bad;
            int r = yuvz_decode(&d, bad, (size_t)l);
            CHECK(r == -1 && d.bad == bad0 + 1 && d.w == 0, "quadro %d: soma errada aceita (%d)", t, r);
            l = yuvz_encode(&e, f, w, h, 1, &m);           /* o FOSTON pede um inteiro (R) */
            CHECK(l > 0 && !(m[4] & 1), "pedido de inteiro não atendido (%ld)", l);
        }
        int r = yuvz_decode(&d, m, (size_t)l);
        CHECK(r >= 0, "quadro %d: decodificar deu %d", t, r);
        if (still && t > 40) CHECK(r == 0, "quadro %d parado: deveria dizer que nada mudou (%d)", t, r);
        CHECK(d.w == w && d.h == h && !memcmp(d.yuv, e.cur, yuvz_size(w, h)), "quadro %d: imagem diferente da do PC", t);
        total += l;
        n++;
    }
    /* delta sem a base (o FOSTON acabou de conectar): -1 */
    yuvz_dec_t fresh = { 0 };
    frame(f, 320, 180, 3, 1);                               /* só o quadrado anda: delta */
    yuvz_enc_t e2 = { 0 };
    const uint8_t *m;
    yuvz_encode(&e2, f, 320, 180, 0, &m);
    frame(f, 320, 180, 4, 1);
    long l = yuvz_encode(&e2, f, 320, 180, 0, &m);
    CHECK((m[4] & 1) && yuvz_decode(&fresh, m, (size_t)l) == -1, "delta sem base aceito");
    CHECK(yuvz_encode(&e2, f, 322, 180, 0, &m) == -1, "largura que não é múltipla de 8 aceita");
    printf("  %d quadros, %d deltas, média %.1f KB\n", n, deltas, total / 1024.0 / n);
    yuvz_enc_free(&e); yuvz_enc_free(&e2); yuvz_dec_free(&d); yuvz_dec_free(&fresh);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
