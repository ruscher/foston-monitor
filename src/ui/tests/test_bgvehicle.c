/* test_bgvehicle.c - every marker model draws something, centred, front up; grey is translucent;
 * the pin's tip and the magnifier's hole are where the screen code expects (biggps/bgvehicle.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "../biggps/bgvehicle.h"
#include "../biggps/bgroute_format.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t px[BGVEH_SIZE * BGVEH_SIZE];
static int alpha(int x, int y) { return (int)(px[y * BGVEH_SIZE + x] >> 24); }

int main(void)
{
    for (int m = 0; m < BGVEH_MODELS; m++) {
        bgveh_draw(px, m, bgveh_color[1], 0);
        long sx = 0, sy = 0, n = 0;
        for (int y = 0; y < BGVEH_SIZE; y++)
            for (int x = 0; x < BGVEH_SIZE; x++)
                if (alpha(x, y) > 128) { sx += x; sy += y; n++; }
        CHECK(n > 150, "modelo %d: só %ld pixels", m, n);
        if (n) {
            double cx = (double)sx / n, cy = (double)sy / n;
            CHECK(cx > 21 && cx < 27 && cy > 18 && cy < 32, "modelo %d fora do centro (%.1f, %.1f)", m, cx, cy);
        }
        bgveh_draw(px, m, bgveh_color[1], 1);
        int a = 0;
        for (int i = 0; i < BGVEH_SIZE * BGVEH_SIZE; i++) if ((int)(px[i] >> 24) > a) a = (int)(px[i] >> 24);
        CHECK(a > 0 && a < 255, "modelo %d cinza deveria ser translúcido (alfa máx %d)", m, a);
    }
    /* seta: a ponta (frente) em cima */
    bgveh_draw(px, BGVEH_ARROW, bgveh_color[0], 0);
    CHECK(alpha(24, 4) > 128 && alpha(24, 44) < 128, "a seta deveria apontar para cima");
    /* pino: ponta em (24, 46), miolo branco em (24, 18) */
    bgveh_draw(px, BGVEH_PIN, 0, 0);
    CHECK(alpha(24, 44) > 100, "ponta do pino");
    CHECK((px[18 * BGVEH_SIZE + 24] & 0xFFFFFF) == 0xFFFFFF, "miolo branco do pino");
    /* lupa 32x32: furo transparente no centro (13, 13), anel em volta */
    static uint32_t mg[32 * 32];
    bgveh_draw_n(mg, 32, BGVEH_MAGNIFIER, 0, 0);
    CHECK((mg[13 * 32 + 13] >> 24) == 0, "furo da lupa deveria ser transparente");
    CHECK((mg[13 * 32 + 21] >> 24) > 128, "anel da lupa");
    CHECK((mg[26 * 32 + 26] >> 24) > 128, "cabo da lupa");
    /* cada perfil oferece modelos válidos, o padrão primeiro */
    for (int p = 0; p < BGR_PROFILES; p++) {
        const int *m;
        int k = bgveh_models(p, &m);
        CHECK(k >= 3, "perfil %d com %d modelos", p, k);
        for (int i = 0; i < k; i++) CHECK(m[i] >= 0 && m[i] < BGVEH_MODELS, "perfil %d modelo %d", p, m[i]);
    }
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
