/* test_bglanes.c - lane guidance: the right junction and direction are found, lanes fit the
 * maneuver, nothing is advised when the data does not say (biggps/bglanes.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../biggps/bglanes.h"
#include "../biggps/bgnav.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bgl_entry_t mk(uint32_t x, uint32_t y, int bearing, int n, const uint16_t *lanes)
{
    bgl_entry_t e;
    memset(&e, 0, sizeof e);
    e.x = x; e.y = y; e.bearing = (uint16_t)bearing; e.n = (uint8_t)n;
    memcpy(e.lane, lanes, (size_t)n * 2);
    return e;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    char path[512];
    snprintf(path, sizeof path, "%s/teste.bgl", argc > 1 ? argv[1] : "/tmp");
    const uint32_t X = 1000000000u, Y = 2147483648u;   /* ~107 unidades por metro */
    /* chegando para o norte: esquerda | frente | frente;direita */
    uint16_t a[] = { BGL_LEFT, BGL_THROUGH, BGL_THROUGH | BGL_RIGHT };
    /* chegando para o sul, no mesmo cruzamento: outra pista */
    uint16_t b[] = { BGL_LEFT, BGL_RIGHT };
    /* bifurcação: frente | frente | levemente à direita */
    uint16_t c[] = { BGL_THROUGH, BGL_THROUGH, BGL_SLIGHT_RIGHT };
    bgl_entry_t E[3] = { mk(X, Y, 0, 3, a), mk(X + 5, Y, 180, 2, b), mk(X + 1000000, Y, 90, 3, c) };
    FILE *f = fopen(path, "wb");
    fwrite(BGL_MAGIC, 1, 4, f);
    put32(f, BGL_VERSION);
    put32(f, 3);
    put32(f, 1);
    fwrite(E, sizeof E[0], 3, f);
    fclose(f);
    char err[256];
    bgl_pack_t *p = bgl_open(path, err, sizeof err);
    CHECK(p, "%s", err);
    if (!p) return 1;
    const bgl_entry_t *e = bgl_find(p, X + 1070, Y + 500, 10);   /* 10 m ao lado, rumo 10° */
    CHECK(e && e->n == 3, "acha o cruzamento chegando para o norte");
    CHECK(bgl_find(p, X, Y, 175) && bgl_find(p, X, Y, 175)->n == 2, "o mesmo cruzamento chegando para o sul é outra entrada");
    CHECK(!bgl_find(p, X + 10000, Y, 0), "a 93 m: nada");
    CHECK(!bgl_find(p, X, Y, 90), "chegando de lado: nada");
    CHECK(bgl_fit(e, BGN_LEFT) == 0x1, "virar à esquerda: faixa 1 (%x)", bgl_fit(e, BGN_LEFT));
    CHECK(bgl_fit(e, BGN_RIGHT) == 0x4, "virar à direita: faixa 3 (%x)", bgl_fit(e, BGN_RIGHT));
    CHECK(bgl_fit(e, BGN_STRAIGHT) == 0x6, "seguir: faixas 2 e 3 (%x)", bgl_fit(e, BGN_STRAIGHT));
    CHECK(bgl_fit(e, BGN_ROUNDABOUT) == 0, "rotatória: sem conselho");
    CHECK(bgl_fit(e, BGN_UTURN) == 0, "retorno sem faixa de retorno: nada");
    CHECK(bgl_arrow(e, 2, BGN_RIGHT, 0x4) == BGN_RIGHT && bgl_arrow(e, 0, BGN_RIGHT, 0x4) == BGN_LEFT, "setas");
    const bgl_entry_t *fk = bgl_find(p, X + 1000000, Y, 92);
    CHECK(fk && bgl_fit(fk, BGN_KEEP_RIGHT) == 0x4, "mantenha-se à direita na bifurcação: faixa 3");
    CHECK(fk && bgl_fit(fk, BGN_KEEP_LEFT) == 0x3, "mantenha-se à esquerda: as faixas de frente à esquerda da saída");
    uint16_t d[] = { BGL_THROUGH, BGL_THROUGH };
    bgl_entry_t only = mk(0, 0, 0, 2, d);
    CHECK(bgl_fit(&only, BGN_KEEP_LEFT) == 0, "nada marca a bifurcação: sem conselho");
    bgl_close(p);
    remove(path);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
