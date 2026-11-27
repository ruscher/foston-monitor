/* test_bgspeed.c - road speed limits (biggps/bgspeed.c): the limit of the road a route runs on,
 * in its direction, never the crossing road's nor a parallel road's 15 m away.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../biggps/bgspeed.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double U;
#define X0 1000000000u
#define Y0 2147483648u
static uint32_t wx(double m) { return (uint32_t)lround(X0 + m * U); }
static uint32_t wy(double m) { return (uint32_t)lround(Y0 - m * U); }   /* metros para o norte */

static bgk_entry_t E[64];
static int ne;
/* uma via de (x0,y0) a (x1,y1) em metros, em pedaços de até 200 m */
static void road(double x0, double y0, double x1, double y1, int fwd, int bwd)
{
    double len = hypot(x1 - x0, y1 - y0);
    int k = (int)ceil(len / 200);
    for (int i = 0; i < k; i++) {
        double a = (double)i / k, b = (double)(i + 1) / k;
        E[ne++] = (bgk_entry_t){ wx(x0 + (x1 - x0) * a), wy(y0 + (y1 - y0) * a), wx(x0 + (x1 - x0) * b), wy(y0 + (y1 - y0) * b),
                                 (uint8_t)fwd, (uint8_t)bwd, 0 };
    }
}
static int cmp(const void *a, const void *b)
{
    const bgk_entry_t *x = a, *y = b;
    uint32_t mx = x->xa < x->xb ? x->xa : x->xb, my = y->xa < y->xb ? y->xa : y->xb;
    return mx < my ? -1 : mx > my;
}
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/teste.bgk", dir);
    U = 4294967296.0 / 40075016.686;
    road(0, 0, 1000, 0, 60, 40);           /* avenida para leste: 60 indo, 40 voltando */
    road(500, -300, 500, 300, 80, 80);     /* via que cruza no meio */
    road(0, 15, 1000, 15, 30, 30);         /* rua paralela 15 m ao norte */
    road(1000, 0, 1000, 600, 0, 0);        /* sem limite no mapa: não entra (o construtor nem escreve) */
    road(1000, 600, 1000, 900, 50, 50);    /* depois sobe com 50 */
    road(0, -500, 400, -500, 0, 40);       /* só o sentido de volta tem limite no mapa */
    road(0, -506, 400, -506, 60, 60);      /* outra via, paralela, 6 m ao sul, com 60 */
    qsort(E, (size_t)ne, sizeof E[0], cmp);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(BGK_MAGIC, 1, 4, f);
    put32(f, BGK_VERSION);
    put32(f, (uint32_t)ne);
    put32(f, 1);
    fwrite(E, sizeof E[0], (size_t)ne, f);
    fclose(f);

    char err[256];
    bgk_pack_t *p = bgk_open(path, err, sizeof err);
    CHECK(p, "não abriu: %s", err);
    if (!p) return 1;
    CHECK(bgk_at(p, wx(250), wy(0), 90) == 60, "indo para leste: %d", bgk_at(p, wx(250), wy(0), 90));
    CHECK(bgk_at(p, wx(250), wy(0), 270) == 40, "voltando: %d", bgk_at(p, wx(250), wy(0), 270));
    CHECK(bgk_at(p, wx(250), wy(3), 90) == 60, "3 m ao lado (GPS): %d", bgk_at(p, wx(250), wy(3), 90));
    CHECK(bgk_at(p, wx(500), wy(0), 90) == 60, "no cruzamento, indo para leste: %d", bgk_at(p, wx(500), wy(0), 90));
    CHECK(bgk_at(p, wx(500), wy(0), 0) == 80, "no cruzamento, indo para o norte: %d", bgk_at(p, wx(500), wy(0), 0));
    CHECK(bgk_at(p, wx(1000), wy(300), 0) == 0, "trecho sem limite no mapa: %d", bgk_at(p, wx(1000), wy(300), 0));
    CHECK(bgk_at(p, wx(250), wy(40), 90) == 0, "40 m de tudo: %d", bgk_at(p, wx(250), wy(40), 90));
    CHECK(bgk_at(p, wx(200), wy(-500), 90) == 0, "sem limite para leste na via, não o 60 da paralela: %d", bgk_at(p, wx(200), wy(-500), 90));
    CHECK(bgk_at(p, wx(200), wy(-500), 270) == 40, "a mesma via para oeste: %d", bgk_at(p, wx(200), wy(-500), 270));
    CHECK(bgk_at(p, wx(200), wy(-506), 90) == 60, "na paralela: %d", bgk_at(p, wx(200), wy(-506), 90));

    /* rota: leste até 1000, depois norte até 900; um ponto a cada 100 m */
    uint32_t xy[64];
    int n = 0;
    for (int m = 0; m <= 1000; m += 100) { xy[2 * n] = wx(m); xy[2 * n + 1] = wy(0); n++; }
    for (int m = 100; m <= 900; m += 100) { xy[2 * n] = wx(1000); xy[2 * n + 1] = wy(m); n++; }
    uint8_t lim[64];
    int got = bgk_route(p, xy, n, lim);
    for (int s = 0; s < 10; s++) CHECK(lim[s] == 60, "rota, trecho %d para leste: %d (não a paralela nem a que cruza)", s, lim[s]);
    for (int s = 10; s < 16; s++) CHECK(lim[s] == 0, "rota, trecho %d sem limite: %d", s, lim[s]);
    for (int s = 16; s < n - 1; s++) CHECK(lim[s] == 50, "rota, trecho %d para o norte: %d", s, lim[s]);
    printf("  %d de %d trechos da rota com limite\n", got, n - 1);
    bgk_close(p);
    /* arquivo cortado: recusa */
    f = fopen(path, "r+b");
    CHECK(f, "não reabriu %s", path);
    if (!f) return 1;
    fseek(f, 8, SEEK_SET);
    put32(f, (uint32_t)ne + 1);
    fclose(f);
    CHECK(!bgk_open(path, err, sizeof err), "aceitou arquivo com contagem errada");
    remove(path);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
