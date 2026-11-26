/* test_bgnavlegs.c - a route with a stop: two legs joined (biggps/bgnav.c: bgn_append).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../biggps/bgnav.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* leg of n points along x from x0, 100 m apart (1 m ~ 107 units at the equator), with a
 * departure, a right turn in the middle and an arrival */
static void leg(bgn_route_t *r, uint32_t x0, int n)
{
    memset(r, 0, sizeof *r);
    r->npts = n;
    r->xy = malloc((size_t)n * 2 * sizeof *r->xy);
    r->cum_dm = malloc((size_t)n * sizeof *r->cum_dm);
    r->cum_ds = malloc((size_t)n * sizeof *r->cum_ds);
    for (int i = 0; i < n; i++) {
        r->xy[2 * i] = x0 + (uint32_t)i * 10718;
        r->xy[2 * i + 1] = 2147483648u;
        r->cum_dm[i] = (uint32_t)i * 1000;
        r->cum_ds[i] = (uint32_t)i * 100;
    }
    r->total_dm = r->cum_dm[n - 1];
    r->total_ds = r->cum_ds[n - 1];
    r->nman = 3;
    r->man = calloc(3, sizeof *r->man);
    r->man[0].type = BGN_DEPART;
    r->man[1].type = BGN_RIGHT;
    r->man[1].at_dm = r->total_dm / 2;
    snprintf(r->man[1].name, sizeof r->man[1].name, "Rua %u", x0);
    r->man[2].type = BGN_ARRIVE;
    r->man[2].at_dm = r->total_dm;
}

int main(void)
{
    bgn_route_t a, b;
    leg(&a, 1000000, 5);                         /* 400 m */
    leg(&b, 1000000 + 4 * 10718, 7);             /* starts where a ends, 600 m */
    CHECK(bgn_append(&a, &b) == 0, "append");
    CHECK(a.npts == 11, "pontos: %d (5 + 7 - 1 repetido)", a.npts);
    CHECK(a.total_dm == 10000, "distância total %u dm", a.total_dm);
    CHECK(a.total_ds == 1000, "tempo total %u ds", a.total_ds);
    for (int i = 1; i < a.npts; i++) CHECK(a.cum_dm[i] > a.cum_dm[i - 1], "distâncias crescentes (%d)", i);
    CHECK(a.nman == 5, "manobras: %d (saída, curva, parada, curva, chegada)", a.nman);
    if (a.nman == 5) {
        CHECK(a.man[2].type == BGN_WAYPOINT && a.man[2].at_dm == 4000, "parada em %u", a.man[2].at_dm);
        CHECK(a.man[3].type == BGN_RIGHT && a.man[3].at_dm == 4000 + 3000, "curva do 2º trecho em %u", a.man[3].at_dm);
        CHECK(a.man[4].type == BGN_ARRIVE && a.man[4].at_dm == 10000, "chegada em %u", a.man[4].at_dm);
    }
    char t[160];
    bgn_text(&a.man[2], t, sizeof t);
    CHECK(!strcmp(t, "Você chegou à parada"), "texto da parada: %s", t);
    char v[96], r[96];
    bgn_voice_parts(&a.man[2], v, sizeof v, r, sizeof r);
    CHECK(!strcmp(v, "Você chegou à parada") && !r[0], "voz da parada: %s | %s", v, r);
    /* trecho que não começa exatamente no fim: a distância entre os dois entra no total */
    bgn_route_t c;
    leg(&c, 1000000 + 10 * 10718 + 1071, 3);     /* 10 m depois do fim, 200 m */
    CHECK(bgn_append(&a, &c) == 0, "append 2");
    CHECK(a.npts == 14 && a.total_dm >= 12090 && a.total_dm <= 12110, "com folga de 10 m: %d pontos, %u dm", a.npts, a.total_dm);
    int nw = 0;
    for (int i = 0; i < a.nman; i++) nw += a.man[i].type == BGN_WAYPOINT;
    CHECK(nw == 2 && a.man[a.nman - 1].type == BGN_ARRIVE, "duas paradas e a chegada no fim");
    bgn_free(&a);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
