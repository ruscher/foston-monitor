/* bgrouter.c - see bgrouter.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgrouter.h"
#include <dirent.h>
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
    for (int leg = 0; leg + 1 < np; leg++) {
        bgr_snap_t s[2], d[2];
        int hd = leg == 0 ? q->heading : -1;
        int ns = bgr_snap(p, px[leg], py[leg], hd, leg == 0 ? 200 : 500, s, 2);
        int nd = bgr_snap(p, px[leg + 1], py[leg + 1], -1, 500, d, 2);
        if (!ns) { bgn_free(&r->route); r->rc = leg == 0 ? BGRT_NOSTART : BGRT_NODEST; return; }
        if (!nd) { bgn_free(&r->route); r->rc = BGRT_NODEST; return; }
        /* moving: only the road in the direction of travel (no U-turn on the spot) */
        if (hd >= 0 && ns > 1) {
            int diff = abs(((s[1].bearing - hd) % 360 + 540) % 360 - 180);
            if (diff > 90) ns = 1;
        }
        bgr_route_t rt;
        int rc = bgr_route(p, s, ns, d, nd, &rt, NULL);
        if (rc) { bgn_free(&r->route); r->rc = rc == -1 ? BGRT_NOROUTE : BGRT_ERROR; return; }
        bgn_route_t part;
        if (bgn_build(p, &rt, &part)) { bgr_route_free(&rt); bgn_free(&r->route); r->rc = BGRT_ERROR; return; }
        if (leg + 2 == np) bgr_way_name(p, bgr_arc_way(p, rt.arcs[rt.narcs - 1]), r->dest_name, sizeof r->dest_name);
        r->toll_arcs += rt.toll_arcs;
        bgr_route_free(&rt);
        if (bgn_append(&r->route, &part)) { bgn_free(&r->route); r->rc = BGRT_ERROR; return; }
    }
    r->ms = now_ms() - t0;
    fprintf(stderr, "biggps: rota %.1f km em %u ms\n", r->route.total_dm / 1e4, r->ms);
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
        pthread_mutex_lock(&T.mu);
        if (T.has_res) bgn_free(&T.res.route);   /* nobody took the previous one */
        T.res = r;
        T.has_res = 1;
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
    T.stop = T.has_req = T.has_res = 0;
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
