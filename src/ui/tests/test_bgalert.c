/* test_bgalert.c - alert points along a route: only those on the road, cameras facing the way
 * the route goes, duplicates merged, in route order (biggps/bgalert.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../biggps/bgalert.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double U;                       /* world units per metre near the equator */
#define X0 1000000000u
#define Y0 2147483648u

static bga_entry_t E[32];
static int ne;
static void add(double xm, double ym, int kind, int speed, int dir)
{
    E[ne++] = (bga_entry_t){ (uint32_t)lround(X0 + xm * U), (uint32_t)lround(Y0 - ym * U), (uint8_t)kind, (uint8_t)speed,
                             (uint16_t)(dir < 0 ? BGA_DIR_ANY : dir) };
}

static int cmp(const void *a, const void *b)
{
    const bga_entry_t *x = a, *y = b;
    return x->x < y->x ? -1 : x->x > y->x;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/teste.bga", dir);
    U = 4294967296.0 / 40075016.686;
    /* rota: 1 km para leste e depois 500 m para o norte */
    add(100, 0, BGA_SIGNAL, 0, -1);        /* na via */
    add(130, 0, BGA_SIGNAL, 0, -1);        /* 30 m depois: o mesmo cruzamento, conta uma vez */
    add(300, 15, BGA_BUMP, 0, -1);         /* 15 m ao lado: rua paralela, fora */
    add(400, 2, BGA_BUMP, 0, -1);          /* 2 m: na via */
    add(600, 12, BGA_CAMERA, 60, 90);      /* ao lado, virado para leste: vale, com 60 km/h */
    add(700, -12, BGA_CAMERA, 60, 270);    /* virado para oeste (outra pista): fora */
    add(800, 10, BGA_CAMERA, 0, -1);       /* sem sentido: vale */
    add(1000, 250, BGA_RAIL, 0, -1);       /* no trecho para o norte */
    add(5000, 0, BGA_SIGNAL, 0, -1);       /* longe */
    qsort(E, (size_t)ne, sizeof E[0], cmp);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(BGA_MAGIC, 1, 4, f);
    put32(f, BGA_VERSION);
    put32(f, (uint32_t)ne);
    put32(f, 1);
    fwrite(E, sizeof E[0], (size_t)ne, f);
    fclose(f);

    char err[256];
    bga_pack_t *p = bga_open(path, err, sizeof err);
    CHECK(p, "não abriu: %s", err);
    if (!p) return 1;
    CHECK(bga_count(p) == (uint32_t)ne, "contagem %u", bga_count(p));
    const bga_entry_t *q[16];
    int k = bga_query(p, X0, Y0 - (uint32_t)(20 * U), X0 + (uint32_t)(450 * U), Y0 + (uint32_t)(20 * U), q, 16);
    CHECK(k == 4, "consulta na caixa: %d pontos (esperado 4)", k);

    uint32_t xy[] = { X0, Y0, X0 + (uint32_t)lround(1000 * U), Y0, X0 + (uint32_t)lround(1000 * U), Y0 - (uint32_t)lround(500 * U) };
    uint32_t cd[] = { 0, 10000, 15000 };
    int n;
    bga_hit_t *h = bga_route(p, xy, cd, 3, &n);
    static const struct { int kind; uint32_t at_m; int speed; } want[] = {
        { BGA_SIGNAL, 100, 0 }, { BGA_BUMP, 400, 0 }, { BGA_CAMERA, 600, 60 }, { BGA_CAMERA, 800, 0 }, { BGA_RAIL, 1250, 0 },
    };
    int nw = (int)(sizeof want / sizeof want[0]);
    CHECK(n == nw, "na rota: %d alertas (esperado %d)", n, nw);
    for (int i = 0; i < n && i < nw; i++) {
        CHECK(h[i].kind == want[i].kind, "alerta %d: tipo %d (esperado %d)", i, h[i].kind, want[i].kind);
        CHECK(abs((int)h[i].at_dm / 10 - (int)want[i].at_m) <= 1, "alerta %d em %u m (esperado %u)", i, h[i].at_dm / 10, want[i].at_m);
        CHECK(h[i].maxspeed == want[i].speed, "alerta %d: limite %d", i, h[i].maxspeed);
    }
    for (int i = 0; i < n; i++) printf("  %u m: tipo %d limite %d\n", h[i].at_dm / 10, h[i].kind, h[i].maxspeed);
    free(h);
    bga_close(p);
    /* arquivo cortado: recusa */
    f = fopen(path, "r+b");
    fseek(f, 8, SEEK_SET);
    put32(f, (uint32_t)ne + 1);
    fclose(f);
    CHECK(!bga_open(path, err, sizeof err), "aceitou arquivo com contagem errada");
    remove(path);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
