/* bgnav.h - turn-by-turn guidance on a route (bgroute.h): maneuver list and live tracking
 * of the GPS position along the route.  No LVGL; double math only per route build and per
 * GPS fix (once a second), never per pixel.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGNAV_H
#define BGNAV_H
#include <stdint.h>
#include "bgroute.h"

typedef enum {
    BGN_DEPART, BGN_STRAIGHT, BGN_SLIGHT_RIGHT, BGN_RIGHT, BGN_SHARP_RIGHT, BGN_UTURN,
    BGN_SHARP_LEFT, BGN_LEFT, BGN_SLIGHT_LEFT, BGN_KEEP_RIGHT, BGN_KEEP_LEFT,
    BGN_RAMP_RIGHT, BGN_RAMP_LEFT, BGN_ROUNDABOUT, BGN_ARRIVE
} bgn_type_t;

typedef struct {
    uint8_t type;              /* bgn_type_t */
    uint8_t exit;              /* roundabout exit number (1..) */
    uint32_t at_dm;            /* distance from the route start */
    uint32_t x, y;             /* world coordinates of the maneuver */
    char name[64];             /* road taken (name or ref), may be empty */
} bgn_man_t;

typedef struct {
    uint32_t *xy;              /* route polyline (world coordinates, x,y pairs) */
    uint32_t *cum_dm;          /* distance from the start at each point */
    uint32_t *cum_ds;          /* travel time from the start at each point */
    int npts;
    bgn_man_t *man;
    int nman;
    uint32_t total_dm, total_ds;
    int profile;
    /* live tracking */
    int seg;                   /* current segment (xy[seg] -> xy[seg+1]) */
    int off_count;
} bgn_route_t;

typedef struct {
    int valid;                 /* position matched to the route */
    uint32_t x, y;             /* matched point on the route */
    uint32_t progress_dm, remain_dm, remain_ds;
    int next;                  /* index of the next maneuver in man[] */
    uint32_t next_dm;          /* distance to it */
    double cross_m;            /* distance from the route */
    int off_route;             /* left the route: recalculate */
    int arrived;
} bgn_state_t;

/* Build the polyline and the maneuvers.  0 on success. */
int bgn_build(bgr_pack_t *p, const bgr_route_t *r, bgn_route_t *out);
void bgn_free(bgn_route_t *r);

/* Track one GPS fix (world coordinates; heading in degrees or -1). */
void bgn_update(bgn_route_t *r, uint32_t x, uint32_t y, double speed_kmh, double heading, bgn_state_t *st);

/* Text for a maneuver in Portuguese (for the screen and the voice), e.g.
 * "Vire à direita na Rua X", "Na rotatória, pegue a 2ª saída". */
const char *bgn_text(const bgn_man_t *m, char *buf, int n);
/* Distance as spoken/shown in Portuguese: "300 m", "1,2 km". */
const char *bgn_dist_text(uint32_t dm, char *buf, int n);

#endif
