/* bgrouter.h - route calculation in a background thread (the first route on a cold SD card
 * takes about a second; the interface must keep responding).  Route packs are found in
 * <dir>/rotas as *-carro.bgr, *-moto.bgr, *-bicicleta.bgr and *-ape.bgr; a pack that avoids
 * tolls is *-carro-sem-pedagio.bgr (or -moto-sem-pedagio.bgr).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGROUTER_H
#define BGROUTER_H
#include <stdint.h>
#include "bgnav.h"

typedef struct {
    unsigned id;               /* echoed in the result */
    int profile;               /* BGR_CAR .. BGR_FOOT */
    uint32_t sx, sy;           /* start (world) */
    int heading;               /* degrees, -1 = unknown (prefers the road going that way) */
    uint32_t dx, dy;           /* destination (world) */
    int avoid_toll;            /* use the "sem pedágio" pack when there is one */
} bgrt_req_t;

enum { BGRT_OK = 0, BGRT_NOROUTE = -1, BGRT_ERROR = -2, BGRT_NOFILE = -3, BGRT_NOSTART = -4, BGRT_NODEST = -5 };

typedef struct {
    unsigned id;
    int rc;                    /* BGRT_* */
    bgn_route_t route;         /* owned by the caller after bgrt_poll (bgn_free) */
    unsigned ms;               /* calculation time */
    char dest_name[64];        /* road at the destination */
    int toll_arcs;             /* arcs of toll ways on the route */
    int toll_avoided;          /* computed with a pack that avoids tolls (toll_arcs > 0: no toll-free way) */
} bgrt_res_t;

int bgrt_start(const char *dir);
void bgrt_stop(void);
int bgrt_have_profile(int profile);
int bgrt_have_toll_free(int profile);   /* a "sem pedágio" pack exists for the profile */
/* Replaces any pending request. */
void bgrt_request(const bgrt_req_t *r);
/* 1 = a result was copied to *out. */
int bgrt_poll(bgrt_res_t *out);

#endif
