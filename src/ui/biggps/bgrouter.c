/* bgrouter.c - see bgrouter.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgrouter.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *SUFFIX[BGR_PROFILES][2] = {
    { "-carro.bgr", "-carro-sem-pedagio.bgr" }, { "-moto.bgr", "-moto-sem-pedagio.bgr" },
    { "-bicicleta.bgr", NULL }, { "-ape.bgr", NULL } };

static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, stop;
    int has_req, has_res;
    bgrt_req_t req;
    bgrt_res_t res;
    int has_alts;
    bgrt_alts_t alts;
    char path[BGR_PROFILES][2][1024];   /* [profile][0 normal, 1 sem pedágio] */
    bgr_pack_t *pack;          /* only the pack in use stays open (8 MB cache each) */
    int pack_profile;          /* profile * 2 + variant */
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pack_profile = -1 };

static unsigned now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000u + t.tv_nsec / 1000000);
}

/* called with T.mu held or from the worker only.  Moto without a file of its own follows
 * the car rules (docs/big-gps/07-roteamento.md); without a toll-free pack, the normal one. */
static int file_profile(int profile, int variant, int *v_out)
{
    for (int v = variant; v >= 0; v--) {
        if (T.path[profile][v][0]) { *v_out = v; return profile; }
        if (profile == BGR_MOTO && !T.path[BGR_MOTO][0][0] && T.path[BGR_CAR][v][0]) { *v_out = v; return BGR_CAR; }
    }
    *v_out = 0;
    return -1;
}

static bgr_pack_t *pack_for(int profile, int variant, int *used_variant)
{
    *used_variant = 0;
    if (profile < 0 || profile >= BGR_PROFILES) return NULL;
    int v;
    profile = file_profile(profile, variant ? 1 : 0, &v);
    if (profile < 0) return NULL;
    *used_variant = v;
    int key = profile * 2 + v;
    if (T.pack && T.pack_profile == key) return T.pack;
    bgr_close(T.pack);
    T.pack = NULL;
    T.pack_profile = -1;
    char err[256];
    T.pack = bgr_open(T.path[profile][v], err, sizeof err);
    if (!T.pack) { fprintf(stderr, "biggps: %s\n", err); return NULL; }
    T.pack_profile = key;
    return T.pack;
}

/* a route through the points (start, stops..., destination), one ordinary query per leg,
 * joined with bgn_append.  0 = ok, else BGRT_*; dest_name and toll_arcs may be NULL. */
static int route_via(bgr_pack_t *p, const uint32_t *px, const uint32_t *py, int np, int heading,
                     bgn_route_t *out, char *dest_name, size_t dn, int *toll_arcs)
{
    memset(out, 0, sizeof *out);
    for (int leg = 0; leg + 1 < np; leg++) {
        bgr_snap_t s[2], d[2];
        int hd = leg == 0 ? heading : -1;
        int ns = bgr_snap(p, px[leg], py[leg], hd, leg == 0 ? 200 : 500, s, 2);
        int nd = bgr_snap(p, px[leg + 1], py[leg + 1], -1, 500, d, 2);
        if (!ns) { bgn_free(out); return leg == 0 ? BGRT_NOSTART : BGRT_NODEST; }
        if (!nd) { bgn_free(out); return BGRT_NODEST; }
        /* moving: only the road in the direction of travel (no U-turn on the spot) */
        if (hd >= 0 && ns > 1) {
            int diff = abs(((s[1].bearing - hd) % 360 + 540) % 360 - 180);
            if (diff > 90) ns = 1;
        }
        bgr_route_t rt;
        int rc = bgr_route(p, s, ns, d, nd, &rt, NULL);
        if (rc) { bgn_free(out); return rc == -1 ? BGRT_NOROUTE : BGRT_ERROR; }
        bgn_route_t part;
        if (bgn_build(p, &rt, &part)) { bgr_route_free(&rt); bgn_free(out); return BGRT_ERROR; }
        if (dest_name && leg + 2 == np) bgr_way_name(p, bgr_arc_way(p, rt.arcs[rt.narcs - 1]), dest_name, dn);
        if (toll_arcs) *toll_arcs += rt.toll_arcs;
        bgr_route_free(&rt);
        if (bgn_append(out, &part)) { bgn_free(out); return BGRT_ERROR; }
    }
    return 0;
}

static void compute(const bgrt_req_t *q, bgrt_res_t *r)
{
    memset(r, 0, sizeof *r);
    r->id = q->id;
    unsigned t0 = now_ms();
    int variant;
    bgr_pack_t *p = pack_for(q->profile, q->avoid_toll, &variant);
    if (!p) { r->rc = BGRT_NOFILE; return; }
    r->toll_avoided = variant == 1 && bgr_avoids_toll(p);
    /* start -> stops -> destination: one ordinary query per leg, joined (bgn_append) */
    uint32_t px[BGRT_MAXSTOPS + 2], py[BGRT_MAXSTOPS + 2];
    int np = 0, nst = q->nstops < 0 ? 0 : q->nstops > BGRT_MAXSTOPS ? BGRT_MAXSTOPS : q->nstops;
    px[np] = q->sx; py[np++] = q->sy;
    for (int i = 0; i < nst; i++) { px[np] = q->stx[i]; py[np++] = q->sty[i]; }
    px[np] = q->dx; py[np++] = q->dy;
    int rc = route_via(p, px, py, np, q->heading, &r->route, r->dest_name, sizeof r->dest_name, &r->toll_arcs);
    if (rc) { r->rc = rc; return; }
    r->ms = now_ms() - t0;
    fprintf(stderr, "biggps: rota %.1f km em %u ms\n", r->route.total_dm / 1e4, r->ms);
}

static int cancelled(void)
{
    pthread_mutex_lock(&T.mu);
    int c = T.has_req || T.stop;
    pthread_mutex_unlock(&T.mu);
    return c;
}

/* see bgrouter.h.  main: a private copy of the main route (the caller owns the original). */
static void compute_alts(const bgrt_req_t *q, const bgn_route_t *main, bgrt_alts_t *out)
{
    memset(out, 0, sizeof *out);
    out->id = q->id;
    unsigned t0 = now_ms();
    int variant;
    bgr_pack_t *p = pack_for(q->profile, q->avoid_toll, &variant);
    if (!p || main->npts < 2 || main->total_dm < 30000) return;          /* under 3 km: no */
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * main->xy[1] / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat);
    double d_m = main->total_dm / 10.0 * 0.18;
    if (d_m < 700) d_m = 700;
    if (d_m > 12000) d_m = 12000;
    static const int frac[3] = { 500, 330, 670 };
    int tried = 0;
    for (int f = 0; f < 3 && out->n < BGRT_MAXALT; f++)
        for (int side = -1; side <= 1 && out->n < BGRT_MAXALT; side += 2) {
            if (cancelled()) return;
            uint32_t vx, vy;
            double br;
            bgn_point_at(main, (uint32_t)((uint64_t)main->total_dm * frac[f] / 1000), &vx, &vy, &br);
            double r = br * M_PI / 180, k = side * d_m / mpu;              /* perpendicular, side*d */
            uint32_t px[3] = { q->sx, vx + (uint32_t)(int32_t)lround(cos(r) * k), q->dx };
            uint32_t py[3] = { q->sy, vy + (uint32_t)(int32_t)lround(sin(r) * k), q->dy };
            bgn_route_t alt;
            int tolls = 0;
            tried++;
            if (route_via(p, px, py, 3, q->heading, &alt, NULL, 0, &tolls)) continue;
            /* the via point is not a stop: drop its maneuver; reject "there and back" spurs */
            int spur = 0;
            for (int i = 0; i < alt.nman; i++) {
                if (alt.man[i].type != BGN_WAYPOINT) continue;
                uint32_t at = alt.man[i].at_dm, ax, ay, bx, by;
                double b1;
                bgn_point_at(&alt, at > 1500 ? at - 1500 : 0, &ax, &ay, &b1);
                bgn_point_at(&alt, at + 1500, &bx, &by, &b1);
                double dx = (double)(int32_t)(bx - ax), dy = (double)(int32_t)(by - ay);
                spur = sqrt(dx * dx + dy * dy) * mpu < 60;
                memmove(&alt.man[i], &alt.man[i + 1], (size_t)(alt.nman - i - 1) * sizeof alt.man[0]);
                alt.nman--;
                break;
            }
            int ok = !spur && alt.total_ds <= (uint64_t)main->total_ds * 135 / 100
                  && bgn_shared_permille(main, &alt, 15) <= 700;
            for (int a = 0; ok && a < out->n; a++) ok = bgn_shared_permille(&out->r[a], &alt, 15) <= 800;
            if (!ok) { bgn_free(&alt); continue; }
            out->r[out->n] = alt;
            out->toll_arcs[out->n] = tolls;
            out->n++;
        }
    out->ms = now_ms() - t0;
    fprintf(stderr, "biggps: %d rota(s) alternativa(s) de %d tentativas em %u ms\n", out->n, tried, out->ms);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&T.mu);
    for (;;) {
        while (!T.stop && !T.has_req) pthread_cond_wait(&T.cv, &T.mu);
        if (T.stop) break;
        bgrt_req_t q = T.req;
        T.has_req = 0;
        pthread_mutex_unlock(&T.mu);
        bgrt_res_t r;
        compute(&q, &r);
        /* the alternatives need the main route; the caller will own (and free) it */
        bgn_route_t mcopy = { 0 };
        int want_alts = q.alternatives && q.nstops == 0 && r.rc == BGRT_OK && r.route.npts >= 2;
        if (want_alts) {
            mcopy.npts = r.route.npts;
            mcopy.total_dm = r.route.total_dm;
            mcopy.total_ds = r.route.total_ds;
            mcopy.xy = malloc((size_t)r.route.npts * 2 * sizeof *mcopy.xy);
            mcopy.cum_dm = malloc((size_t)r.route.npts * sizeof *mcopy.cum_dm);
            if (mcopy.xy && mcopy.cum_dm) {
                memcpy(mcopy.xy, r.route.xy, (size_t)r.route.npts * 2 * sizeof *mcopy.xy);
                memcpy(mcopy.cum_dm, r.route.cum_dm, (size_t)r.route.npts * sizeof *mcopy.cum_dm);
            } else want_alts = 0;
        }
        pthread_mutex_lock(&T.mu);
        if (T.has_res) bgn_free(&T.res.route);   /* nobody took the previous one */
        T.res = r;
        T.has_res = 1;
        pthread_mutex_unlock(&T.mu);
        if (want_alts) {
            bgrt_alts_t a;
            compute_alts(&q, &mcopy, &a);
            pthread_mutex_lock(&T.mu);
            if (T.has_alts) for (int i = 0; i < T.alts.n; i++) bgn_free(&T.alts.r[i]);
            T.alts = a;
            T.has_alts = 1;
            pthread_mutex_unlock(&T.mu);
        }
        free(mcopy.xy);
        free(mcopy.cum_dm);
        pthread_mutex_lock(&T.mu);
    }
    pthread_mutex_unlock(&T.mu);
    return NULL;
}

int bgrt_start(const char *dir)
{
    if (T.started) return 0;
    memset(T.path, 0, sizeof T.path);
    char rd[1024];
    snprintf(rd, sizeof rd, "%s/rotas", dir);
    DIR *dp = opendir(rd);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            size_t l = strlen(de->d_name);
            if (de->d_name[0] == '.') continue;
            for (int i = 0; i < BGR_PROFILES; i++)
                for (int v = 0; v < 2; v++) {
                    if (!SUFFIX[i][v]) continue;
                    size_t sl = strlen(SUFFIX[i][v]);
                    char *dst = T.path[i][v];
                    if (l > sl && !strcasecmp(de->d_name + l - sl, SUFFIX[i][v])
                        && (!dst[0] || strcmp(de->d_name, strrchr(dst, '/') + 1) < 0)
                        && snprintf(dst, sizeof T.path[i][v], "%s/%s", rd, de->d_name) >= (int)sizeof T.path[i][v])
                        dst[0] = 0;
                }
        }
        closedir(dp);
    }
    T.stop = T.has_req = T.has_res = T.has_alts = 0;
    if (pthread_create(&T.th, NULL, worker, NULL)) return -1;
    T.started = 1;
    return 0;
}

void bgrt_stop(void)
{
    if (!T.started) return;
    pthread_mutex_lock(&T.mu);
    T.stop = 1;
    pthread_cond_signal(&T.cv);
    pthread_mutex_unlock(&T.mu);
    pthread_join(T.th, NULL);
    T.started = 0;
    if (T.has_res) bgn_free(&T.res.route);
    T.has_res = 0;
    if (T.has_alts) for (int i = 0; i < T.alts.n; i++) bgn_free(&T.alts.r[i]);
    T.has_alts = 0;
    bgr_close(T.pack);
    T.pack = NULL;
    T.pack_profile = -1;
}

int bgrt_have_profile(int profile)
{
    int v;
    return profile >= 0 && profile < BGR_PROFILES && file_profile(profile, 0, &v) >= 0;
}

int bgrt_have_toll_free(int profile)
{
    int v;
    return profile >= 0 && profile < BGR_PROFILES && file_profile(profile, 1, &v) >= 0 && v == 1;
}

void bgrt_request(const bgrt_req_t *r)
{
    pthread_mutex_lock(&T.mu);
    T.req = *r;
    T.has_req = 1;
    pthread_cond_signal(&T.cv);
    pthread_mutex_unlock(&T.mu);
}

int bgrt_poll_alts(bgrt_alts_t *out)
{
    int got = 0;
    pthread_mutex_lock(&T.mu);
    if (T.has_alts) { *out = T.alts; T.has_alts = 0; got = 1; }
    pthread_mutex_unlock(&T.mu);
    return got;
}

int bgrt_poll(bgrt_res_t *out)
{
    int got = 0;
    pthread_mutex_lock(&T.mu);
    if (T.has_res) {
        *out = T.res;
        T.has_res = 0;
        got = 1;
    }
    pthread_mutex_unlock(&T.mu);
    return got;
}
