/* bgroute-cli.c - routes on a Big GPS route pack from the command line (PC and device).
 *
 *   bgroute ROTAS.bgr LAT1 LON1 LAT2 LON2 [-h RUMO] [-s]
 *                                                     rota entre dois pontos e as instruções;
 *                                                     -s simula o percurso (GPS com ruído e um
 *                                                     desvio no meio) e mostra o acompanhamento
 *   bgroute ROTAS.bgr -c TESTES                       confere com as consultas de referência
 *                                                     (bgroute-build --tests)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgroute.h"
#include "bgnav.h"
#include <math.h>
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
    int heading = -1, simulate = 0;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "-h") && i + 1 < argc) heading = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) simulate = 1;
    }
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
    bgn_route_t nr;
    t0 = now();
    if (bgn_build(p, &r, &nr)) { printf("erro ao montar as instruções\n"); return 1; }
    printf("instruções: %d manobras, %d pontos, %.1f ms\n", nr.nman, nr.npts, (now() - t0) * 1e3);
    char txt[160], dtx[32];
    for (int i = 0; i < nr.nman; i++)
        printf("  %8s  %s\n", bgn_dist_text(nr.man[i].at_dm, dtx, sizeof dtx), bgn_text(&nr.man[i], txt, sizeof txt));
    if (simulate) {
        /* drive along the polyline at the arcs' speed, 1 fix per second, ±5 m noise,
         * and an 80 m sideways detour for 8 s at 40 % of the route */
        unsigned seed = 1;
        int last_next = -1, offs = 0, arrived = 0, fixes = 0;
        double tsim = 0;
        bgn_state_t stt;
        uint32_t total_t = nr.cum_ds[nr.npts - 1];
        for (int sec = 0; sec < 20000 && !arrived; sec++) {
            tsim += 10;                              /* deciseconds */
            if (tsim > total_t) tsim = total_t;
            int k = 0;
            while (k + 1 < nr.npts - 1 && nr.cum_ds[k + 1] < tsim) k++;
            double span = (double)nr.cum_ds[k + 1] - nr.cum_ds[k], f = span > 0 ? (tsim - nr.cum_ds[k]) / span : 1;
            const uint32_t *a = &nr.xy[2 * k], *b = &nr.xy[2 * k + 2];
            double x = a[0] + f * (double)(int32_t)(b[0] - a[0]), y = a[1] + f * (double)(int32_t)(b[1] - a[1]);
            double hd = atan2((double)(int32_t)(b[0] - a[0]), -(double)(int32_t)(b[1] - a[1])) * 180 / M_PI;
            double mpu = 40075016.686 / 4294967296.0 * cos(atan(sinh(M_PI * (1.0 - 2.0 * y / 4294967296.0))));
            seed = seed * 1103515245u + 12345u;
            x += ((int)(seed >> 16 & 1023) - 512) / 512.0 * 5 / mpu;
            seed = seed * 1103515245u + 12345u;
            y += ((int)(seed >> 16 & 1023) - 512) / 512.0 * 5 / mpu;
            double prog = tsim / total_t;
            if (prog > 0.4 && prog < 0.4 + 80.0 / total_t) {                   /* detour: 80 m to the side */
                double r = (hd + 90) * M_PI / 180;
                x += sin(r) * 80 / mpu;
                y -= cos(r) * 80 / mpu;
            }
            double kmh = span > 0 ? ((double)nr.cum_dm[k + 1] - nr.cum_dm[k]) / 10.0 / (span / 10.0) * 3.6 : 0;
            bgn_update(&nr, (uint32_t)x, (uint32_t)y, kmh, hd < 0 ? hd + 360 : hd, &stt);
            fixes++;
            if (stt.off_route) {
                if (!offs) printf("  [%4d s] FORA DA ROTA (%.0f m): recalcular\n", sec, stt.cross_m);
                offs++;
            } else if (offs && stt.valid) {
                printf("  [%4d s] de volta à rota após %d posições fora\n", sec, offs);
                offs = 0;
            }
            if (stt.next != last_next && stt.valid) {
                printf("  [%4d s] faltam %s, %d min: próxima em %s: %s\n", sec, bgn_dist_text(stt.remain_dm, dtx, sizeof dtx),
                       (int)(stt.remain_ds / 600), bgn_dist_text(stt.next_dm, txt + 100, 32), bgn_text(&nr.man[stt.next], txt, 100));
                last_next = stt.next;
            }
            if (stt.arrived) { printf("  [%4d s] CHEGOU (faltavam %.1f m)\n", sec, stt.remain_dm / 10.0); arrived = 1; }
        }
        if (!arrived) printf("  simulação terminou sem chegada (%d posições)\n", fixes);
    }
    bgn_free(&nr);
    bgr_route_free(&r);
    bgr_close(p);
    return 0;
}
