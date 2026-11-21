/* bgroute-cli.c - routes on a Big GPS route pack from the command line (PC and device).
 *
 *   bgroute ROTAS.bgr LAT1 LON1 LAT2 LON2 [-h RUMO]   rota entre dois pontos
 *   bgroute ROTAS.bgr -c TESTES                       confere com as consultas de referência
 *                                                     (bgroute-build --tests)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgroute.h"
#include "bgrender.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int check(bgr_pack_t *p, const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) { perror(file); return 1; }
    unsigned s, t, d, n = 0, bad = 0, unreach = 0;
    double sum = 0, mx = 0;
    unsigned settled = 0, pages = 0;
    while (fscanf(f, "%u %u %u", &s, &t, &d) == 3) {
        bgr_qstats_t st;
        double t0 = now();
        uint32_t r = bgr_arc_distance(p, s, t, &st);
        double ms = (now() - t0) * 1e3;
        sum += ms;
        if (ms > mx) mx = ms;
        settled += st.settled;
        pages += st.pages_read;
        if (d == 0xFFFFFFFFu) unreach++;
        if (r != d) {
            if (bad < 10) printf("DIFERENTE: %u -> %u: CH %u, Dijkstra %u\n", s, t, r, d);
            bad++;
        }
        n++;
    }
    fclose(f);
    printf("%u consultas, %u diferentes, %u sem caminho; média %.1f ms, pior %.1f ms, %.0f nós/consulta, %u páginas lidas\n",
           n, bad, unreach, sum / (n ? n : 1), mx, (double)settled / (n ? n : 1), pages);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: %s ROTAS.bgr LAT1 LON1 LAT2 LON2 [-h RUMO] | ROTAS.bgr -c TESTES\n", argv[0]);
        return 2;
    }
    char err[256];
    double t0 = now();
    bgr_pack_t *p = bgr_open(argv[1], err, sizeof err);
    if (!p) { fprintf(stderr, "%s\n", err); return 1; }
    const bgr_header_t *h = bgr_info(p);
    printf("abrir: %.1f ms; perfil %u, %u nós, %u arcos, CH %u+%u\n", (now() - t0) * 1e3, h->profile, h->nodes, h->arcs, h->ch_fwd, h->ch_bwd);
    if (!strcmp(argv[2], "-c")) return check(p, argv[3]);
    if (argc < 6) return 2;
    int heading = -1;
    if (argc >= 8 && !strcmp(argv[6], "-h")) heading = atoi(argv[7]);
    uint32_t x1, y1, x2, y2;
    bgr_lonlat_to_world(atof(argv[3]), atof(argv[2]), &x1, &y1);
    bgr_lonlat_to_world(atof(argv[5]), atof(argv[4]), &x2, &y2);
    bgr_snap_t s[4], d[4];
    t0 = now();
    int ns = bgr_snap(p, x1, y1, heading, 300, s, 2), nd = bgr_snap(p, x2, y2, -1, 300, d, 2);
    printf("encaixe: %.1f ms; origem %d candidatos (%.1f m), destino %d (%.1f m)\n", (now() - t0) * 1e3, ns,
           ns ? s[0].dist_dm / 10.0 : 0, nd, nd ? d[0].dist_dm / 10.0 : 0);
    if (!ns || !nd) { printf("sem via perto da origem ou do destino\n"); return 1; }
    if (getenv("BGROUTE_DEBUG"))
        for (int i = 0; i < ns + nd; i++) {
            const bgr_snap_t *c = i < ns ? &s[i] : &d[i - ns];
            printf("  %s arco %u frac %u dist %u rumo %d\n", i < ns ? "origem" : "destino", c->arc, c->frac, c->dist_dm, c->bearing);
        }
    bgr_route_t r;
    bgr_qstats_t st;
    int rc = bgr_route(p, s, ns, d, nd, &r, &st);
    if (rc) { printf("sem rota (%d); %u nós, %.1f ms\n", rc, st.settled, st.us / 1e3); return 1; }
    printf("rota: %.2f km, %d min %02d s, %d arcos; busca %.1f ms, %u nós, %u relaxados, %u páginas lidas\n",
           r.len_dm / 1e4, (int)(r.time_ds / 600), (int)(r.time_ds / 10 % 60), r.narcs, st.us / 1e3, st.settled, st.relaxed, st.pages_read);
    char name[128], last[128] = "";
    for (int i = 0; i < r.narcs; i++) {
        bgr_way_name(p, bgr_arc_way(p, r.arcs[i]), name, sizeof name);
        if (strcmp(name, last)) { printf("  %s\n", name[0] ? name : "(sem nome)"); snprintf(last, sizeof last, "%s", name); }
    }
    bgr_route_free(&r);
    bgr_close(p);
    return 0;
}
