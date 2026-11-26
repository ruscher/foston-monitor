/* bgspeed.h - road speed limits from a .bgk file (bgspeed_format.h): the limit of each stretch
 * of a route, and of the road at a point.  Mapped, not read.  No LVGL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGSPEED_H
#define BGSPEED_H
#include <stdint.h>
#include "bgspeed_format.h"

typedef struct bgk_pack bgk_pack_t;

bgk_pack_t *bgk_open(const char *path, char *err, int errlen);
void bgk_close(bgk_pack_t *p);
uint32_t bgk_count(const bgk_pack_t *p);
uint32_t bgk_built(const bgk_pack_t *p);

/* The limit (km/h, 0 = unknown) of the road at (x, y) going with bearing (degrees; -1 = any):
 * the nearest piece within 8 m whose direction matches within 35 degrees (its forward limit)
 * or is opposite within 35 (its backward limit). */
int bgk_at(const bgk_pack_t *p, uint32_t x, uint32_t y, int bearing);

/* For each segment of a route polyline (npts points, xy pairs), the limit of the road it runs
 * on (bgk_at at the segment's middle, with its bearing) into out[npts - 1].  Returns how many
 * segments got a limit. */
int bgk_route(const bgk_pack_t *p, const uint32_t *xy, int npts, uint8_t *out);

#endif
