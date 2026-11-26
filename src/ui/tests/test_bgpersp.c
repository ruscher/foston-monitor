/* test_bgpersp.c - the tilted view: scale 1 at the vehicle row, farther rows compressed, project
 * and unproject agree, remap reads the right source pixels and never outside the source.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "../biggps/bgpersp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    static bgp_t p;
    enum { W = 800, H = 480 };
    CHECK(bgp_init(&p, W, H, 360, -800) == 0, "init");
    printf("fonte %dx%d (%.2fx a tela), carro na linha %d da fonte\n", p.sw, p.sh, (double)p.sw * p.sh / (W * H), p.sy0);
    /* na linha do carro: escala 1 e a mesma linha da fonte */
    CHECK(p.row[360] == 0, "linha do carro mapeia para sy0 (%d)", p.row[360]);
    CHECK(abs(p.du[360] - 65536) < 200, "escala 1 na linha do carro (%d)", p.du[360]);
    /* mais acima: mais longe e comprimido; mais abaixo: mais perto */
    /* para baixo, cada vez mais perto (abaixo do carro a imagem é ampliada: linhas podem repetir) */
    for (int y = 1; y < H; y++) CHECK(p.row[y] >= p.row[y - 1], "linhas em ordem (%d)", y);
    for (int y = 1; y <= 360; y++) CHECK(p.row[y] > p.row[y - 1], "acima do carro, linhas distintas (%d)", y);
    CHECK(p.du[0] > 65536 * 13 / 10, "no alto mostra mais largura (%d)", p.du[0]);
    CHECK(p.du[H - 1] < 65536, "embaixo mostra menos largura");
    /* projetar e desprojetar */
    int bad = 0;
    for (int y = 0; y < H; y += 7)
        for (int x = 0; x < W; x += 13) {
            int sx, sy, x2, y2;
            if (!bgp_unproject(&p, x, y, &sx, &sy) || !bgp_project(&p, sx, sy, &x2, &y2)) { bad++; continue; }
            if (abs(x2 - x) > 2 || abs(y2 - y) > 2) bad++;
        }
    CHECK(!bad, "%d pontos não voltam ao lugar", bad);
    /* o carro fica no mesmo lugar */
    int cx, cy;
    CHECK(bgp_project(&p, p.scx, p.sy0, &cx, &cy) && cx == W / 2 && cy == 360, "carro em (%d, %d)", cx, cy);
    /* remap: fonte com o valor = linha*… lê o pixel certo e preenche o resto com o fundo */
    uint16_t *src = malloc((size_t)p.sw * p.sh * 2), *dst = malloc((size_t)W * H * 2);
    for (int y = 0; y < p.sh; y++)
        for (int x = 0; x < p.sw; x++) src[y * p.sw + x] = (uint16_t)((y * 7 + x) & 0x7FFF);
    bgp_remap(&p, src, dst, W, 0, 0, 0xFFFF);
    int wrong = 0, bgs = 0;
    for (int y = 0; y < H; y += 3)
        for (int x = 0; x < W; x += 5) {
            uint16_t v = dst[y * W + x];
            if (v == 0xFFFF) { bgs++; continue; }
            int sx, sy;
            bgp_unproject(&p, x, y, &sx, &sy);
            int ex = (sy * 7 + sx) & 0x7FFF;
            if (abs((int)v - ex) > 7 * 2 + 2) wrong++;
        }
    CHECK(!wrong, "%d pixels lidos do lugar errado", wrong);
    CHECK(bgs == 0, "%d pixels de fundo sem deslocamento (a fonte deveria cobrir a tela)", bgs);
    /* com deslocamento grande: fundo nas bordas, nunca fora da fonte (ASan acusa) */
    bgp_remap(&p, src, dst, W, 300, -300, 0xFFFF);
    bgp_remap(&p, src, dst, W, -300, 400, 0xFFFF);
    free(src);
    free(dst);
    CHECK(bgp_init(&p, W, H, 360, -50) == -1, "inclinação exagerada deveria ser recusada");
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
