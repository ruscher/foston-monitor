/* bgalert.h - road alert points (traffic signals, fixed speed cameras, speed bumps, level
 * crossings) from a .bga file (bgalert_format.h): in a map view, and along a route.
 * The file is mapped, not read: opening costs nothing and only the pages a query touches
 * are loaded.  No LVGL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGALERT_H
#define BGALERT_H
#include <stdint.h>
#include "bgalert_format.h"

typedef struct bga_pack bga_pack_t;

bga_pack_t *bga_open(const char *path, char *err, int errlen);
void bga_close(bga_pack_t *p);
uint32_t bga_count(const bga_pack_t *p);
uint32_t bga_built(const bga_pack_t *p);

/* Entries inside the box (world coordinates, inclusive), up to max; returns how many. */
int bga_query(const bga_pack_t *p, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
              const bga_entry_t **out, int max);

/* An alert met along a route. */
typedef struct {
    uint32_t at_dm;            /* distance from the route start */
    uint8_t kind, maxspeed;
    uint32_t x, y;
} bga_hit_t;

/* Alerts on a route polyline (xy pairs, cum_dm distances), in route order.  Points on the
 * road itself (signals, bumps, level crossings) must lie within 6 m of the route; speed
 * cameras within 25 m, and when the camera has a direction it must face the way the route
 * goes (within 60 degrees).  The same kind within 40 m counts once.  Returns a malloc'd
 * array (caller frees) and its length in *n. */
bga_hit_t *bga_route(const bga_pack_t *p, const uint32_t *xy, const uint32_t *cum_dm, int npts, int *n);

#endif
