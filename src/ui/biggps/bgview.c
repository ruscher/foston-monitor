/* bgview.c - render a Big GPS map view to a PPM file and measure frame times.
 *
 *   bgview MAP.bgm[,PARTE2.bgm...] LAT LON ZOOM [options]
 *     -o FILE.ppm      write the image (with labels marked as small boxes)
 *     -r DEG           map rotation (clockwise)
 *     -n               night palette
 *     -s WxH           surface size (default 800x480)
 *     -b N             benchmark: N frames panning east at ~60 km/h (1 frame per second
 *                      of driving) after the first frame, report times
 *     -z               benchmark: zoom sweep from ZOOM-3 to ZOOM+2 instead
 *
 * Runs on the PC and on the device (same code as the shell).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgmap.h"
#include "bgrender.h"
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -p: sample the program counter every 1 ms of CPU time (profile on the device) */
static unsigned long prof_pc[200000];
static volatile int prof_n;
static void prof_sig(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
#if defined(__arm__)
    unsigned long pc = ((ucontext_t *)uc)->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
    unsigned long pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    unsigned long pc = 0;
#endif
    if (prof_n < 200000) prof_pc[prof_n++] = pc;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void write_ppm(const char *path, const rast_surface_t *s, const bgr_label_t *lb, int nl)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    unsigned char *row = malloc((size_t)s->w * 3);
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            uint16_t c = s->px[(size_t)y * s->stride + x];
            int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            int mark = 0;
            for (int i = 0; i < nl && !mark; i++)
                mark = abs(x - lb[i].x) <= 2 && abs(y - lb[i].y) <= 2;
            row[3 * x] = mark ? 200 : (unsigned char)(r << 3 | r >> 2);
            row[3 * x + 1] = mark ? 0 : (unsigned char)(g << 2 | g >> 4);
            row[3 * x + 2] = mark ? 0 : (unsigned char)(b << 3 | b >> 2);
        }
        fwrite(row, 1, (size_t)s->w * 3, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "uso: %s MAPA.bgm LAT LON ZOOM [-o arq.ppm] [-r graus] [-n] [-s LxA] [-b N] [-z]\n", argv[0]);
        return 2;
    }
    const char *out = NULL;
    int rot = 0, night = 0, w = 800, h = 480, bench = 0, zsweep = 0;
    const char *prof = NULL;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) night = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) return 2; }
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z")) zsweep = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prof = argv[++i];
        else { fprintf(stderr, "opção desconhecida: %s\n", argv[i]); return 2; }
    }
    char err[256];
    double t = now();
    /* MAPA may list several files of one map: a.bgm,b.bgm */
    char list[2048];
    snprintf(list, sizeof list, "%s", argv[1]);
    char *save = NULL, *first = strtok_r(list, ",", &save);
    bgm_pack_t *p = first ? bgm_open(first, err, sizeof err) : NULL;
    if (!p) { fprintf(stderr, "%s\n", err); return 1; }
    for (char *more = strtok_r(NULL, ",", &save); more; more = strtok_r(NULL, ",", &save))
        if (bgm_add(p, more, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    printf("abrir: %.1f ms (%s)\n", (now() - t) * 1e3, bgm_attribution(p));
    rast_surface_t s = {malloc((size_t)w * h * 2), w, h, w};
    bgr_view_t v = {0};
    bgr_lonlat_to_world(atof(argv[3]), atof(argv[2]), &v.cx, &v.cy);
    v.zoom256 = (int)(atof(argv[4]) * 256);
    v.rot = rot;
    v.night = night;
    bgr_label_t lb[128];
    bgr_stats_t st;
    bgm_stats_t ms;
    t = now();
    int nl = bgr_render(p, &v, &s, lb, 128, &st);
    double ms1 = (now() - t) * 1e3;
    bgm_get_stats(p, &ms, 1);
    printf("quadro 1 (cache frio): %.1f ms; busca %.1f ms, desenho %.1f ms; %u tiles, %u feições, %u pontos, %d rótulos; "
           "lidos %u bytes, decodificação %.1f ms, cache %zu kB; %.2f m/px\n",
           ms1, st.us_fetch / 1e3, st.us_draw / 1e3, st.tiles, st.feats, st.pts, nl, ms.bytes_read,
           ms.decode_us / 1e3, ms.cache_bytes / 1024, bgr_metres_per_pixel(&v));
    t = now();
    nl = bgr_render(p, &v, &s, lb, 128, &st);
    printf("quadro 2 (cache quente): %.1f ms; desenho %.1f ms; limpar %.1f ms\n", (now() - t) * 1e3, st.us_draw / 1e3, st.us_clear / 1e3);
    printf("  por tipo (ms):");
    for (int k = 0; k < BGK_COUNT; k++)
        if (st.us_kind[k] >= 500) printf(" %d:%.1f", k, st.us_kind[k] / 1e3);
    printf("\n");
    if (out) write_ppm(out, &s, lb, nl);
    if (prof) {
        struct sigaction sa = {0};
        sa.sa_sigaction = prof_sig;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval it = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_PROF, &it, NULL);
    }
    if (bench > 0) {
        double sum = 0, mx = 0;
        int z0 = v.zoom256;
        double world_per_m = 4294967296.0 / (40075016.686 * cos(atof(argv[2]) * M_PI / 180.0));
        for (int i = 0; i < bench; i++) {
            if (zsweep) v.zoom256 = z0 - 3 * 256 + (5 * 256 * i) / (bench > 1 ? bench - 1 : 1);
            else v.cx += (uint32_t)(16.7 * world_per_m);   /* 16.7 m per frame: 60 km/h at 1 frame/s */
            t = now();
            bgr_render(p, &v, &s, lb, 128, &st);
            double d = (now() - t) * 1e3;
            sum += d;
            if (d > mx) mx = d;
        }
        bgm_get_stats(p, &ms, 1);
        printf("%s: %d quadros, média %.1f ms, pior %.1f ms; lidos %u bytes, %u tiles novos, cache %zu kB\n",
               zsweep ? "varredura de zoom" : "deslocamento 60 km/h", bench, sum / bench, mx, ms.bytes_read, ms.misses, ms.cache_bytes / 1024);
    }
    if (prof) {
        struct itimerval it = {{0, 0}, {0, 0}};
        setitimer(ITIMER_PROF, &it, NULL);
        FILE *f = fopen(prof, "w");
        if (f) {
            for (int i = 0; i < prof_n; i++) fprintf(f, "%lx\n", prof_pc[i]);
            fclose(f);
        }
        printf("perfil: %d amostras em %s\n", prof_n, prof);
    }
    bgm_close(p);
    free(s.px);
    return 0;
}
