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

static const char *SUFFIX[BGR_PROFILES] = { "-carro.bgr", "-moto.bgr", "-bicicleta.bgr", "-ape.bgr" };

static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, stop;
    int has_req, has_res;
    bgrt_req_t req;
    bgrt_res_t res;
    char path[BGR_PROFILES][1024];
    bgr_pack_t *pack;          /* only the pack in use stays open (8 MB cache each) */
    int pack_profile;
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pack_profile = -1 };

static unsigned now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000u + t.tv_nsec / 1000000);
}

/* called with T.mu held or from the worker only */
/* moto has no file of its own: it follows the car rules (docs/big-gps/07-roteamento.md) */
static int file_profile(int profile)
{
    if (profile == BGR_MOTO && !T.path[BGR_MOTO][0] && T.path[BGR_CAR][0]) return BGR_CAR;
    return profile;
}

static bgr_pack_t *pack_for(int profile)
{
    if (profile < 0 || profile >= BGR_PROFILES) return NULL;
    profile = file_profile(profile);
    if (!T.path[profile][0]) return NULL;
    if (T.pack && T.pack_profile == profile) return T.pack;
    bgr_close(T.pack);
    T.pack = NULL;
    T.pack_profile = -1;
    char err[256];
    T.pack = bgr_open(T.path[profile], err, sizeof err);
    if (!T.pack) { fprintf(stderr, "biggps: %s\n", err); return NULL; }
    T.pack_profile = profile;
    return T.pack;
}

static void compute(const bgrt_req_t *q, bgrt_res_t *r)
{
    memset(r, 0, sizeof *r);
    r->id = q->id;
    unsigned t0 = now_ms();
    bgr_pack_t *p = pack_for(q->profile);
    if (!p) { r->rc = BGRT_NOFILE; return; }
    bgr_snap_t s[2], d[2];
    int ns = bgr_snap(p, q->sx, q->sy, q->heading, 200, s, 2);
    int nd = bgr_snap(p, q->dx, q->dy, -1, 500, d, 2);
    if (!ns) { r->rc = BGRT_NOSTART; return; }
    if (!nd) { r->rc = BGRT_NODEST; return; }
    /* moving: only the road in the direction of travel (no U-turn on the spot) */
    if (q->heading >= 0 && ns > 1) {
        int diff = abs(((s[1].bearing - q->heading) % 360 + 540) % 360 - 180);
        if (diff > 90) ns = 1;
    }
    bgr_route_t rt;
    int rc = bgr_route(p, s, ns, d, nd, &rt, NULL);
    if (rc) { r->rc = rc == -1 ? BGRT_NOROUTE : BGRT_ERROR; return; }
    if (bgn_build(p, &rt, &r->route)) { bgr_route_free(&rt); r->rc = BGRT_ERROR; return; }
    bgr_way_name(p, bgr_arc_way(p, rt.arcs[rt.narcs - 1]), r->dest_name, sizeof r->dest_name);
    bgr_route_free(&rt);
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
            for (int i = 0; i < BGR_PROFILES; i++) {
                size_t sl = strlen(SUFFIX[i]);
                if (l > sl && !strcasecmp(de->d_name + l - sl, SUFFIX[i])
                    && (!T.path[i][0] || strcmp(de->d_name, strrchr(T.path[i], '/') + 1) < 0)
                    && snprintf(T.path[i], sizeof T.path[i], "%s/%s", rd, de->d_name) >= (int)sizeof T.path[i])
                    T.path[i][0] = 0;
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

int bgrt_have_profile(int profile) { return profile >= 0 && profile < BGR_PROFILES && T.path[file_profile(profile)][0]; }

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
