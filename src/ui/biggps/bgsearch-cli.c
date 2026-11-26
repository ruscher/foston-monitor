/* bgsearch-cli.c - offline search from the command line (PC and device).
 *   bgsearch INDICE.bgs LAT LON "texto"        busca perto de LAT LON
 *   bgsearch INDICE.bgs LAT LON -c CATEGORIA   pontos de interesse mais próximos (número BGS_C_*)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgsearch.h"
#include "bgrender.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "uso: %s INDICE.bgs LAT LON \"texto\" | INDICE.bgs LAT LON -c CATEGORIA\n", argv[0]);
        return 2;
    }
    char err[256];
    bgs_index_t *ix = bgs_open(argv[1], err, sizeof err);
    if (!ix) { fprintf(stderr, "%s\n", err); return 1; }
    uint32_t x, y;
    bgr_lonlat_to_world(atof(argv[3]), atof(argv[2]), &x, &y);
    bgs_result_t r[20];
    static const char *types[] = { "lugar", "rua", "local", "endereço" };
    double t = now();
    int n = argc >= 6 && !strcmp(argv[4], "-c") ? bgs_nearby(ix, atoi(argv[5]), x, y, 20000, r, 10) : bgs_query(ix, argv[4], x, y, r, 10);
    printf("%d resultados em %.1f ms (%u entradas no índice)%s%s\n", n, (now() - t) * 1e3, bgs_count(ix), bgs_corrected(ix)[0] ? " — corrigido para: " : "", bgs_corrected(ix));
    for (int i = 0; i < n; i++)
        printf("  %-9s %-44s %-24s %6.1f km\n", types[r[i].type & 3], r[i].name, r[i].context, r[i].dist_m / 1000.0);
    bgs_close(ix);
    return 0;
}
