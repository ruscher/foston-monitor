/* bgpersp.h - the tilted ("2.5D") navigation view by row remapping, no 3D hardware: the engine
 * draws an ordinary top-down frame of a larger area (deeper ahead, wider at the far end) and
 * each screen row takes one source row, scaled horizontally around the centre column — a
 * pinhole camera looking down at the ground.  Scale is 1 at the vehicle's row, so the
 * vehicle and the road under it look as in 2D; rows above it show ground farther ahead,
 * compressed; the horizon is above the top of the screen (never shown).
 *
 *   screen row y, ground distance ahead Z:  y = yh + Z0 * (y0 - yh) / Z,   Z0 = y0 - yh
 *   screen x:  x = cx + (sx - cx) * Z0 / Z         (sx, Z: source column and depth)
 *
 * Integers only per pixel (16.16 steps from tables built once).  No LVGL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGPERSP_H
#define BGPERSP_H
#include <stdint.h>

#define BGP_MAXH 600

typedef struct {
    int w, h;                  /* screen */
    int y0, yh;                /* vehicle row; horizon row (negative: above the screen) */
    int sw, sh;                /* source frame to draw (top-down, same zoom as the vehicle row) */
    int scx, sy0;              /* the vehicle in the source: column (= centre) and row */
    int32_t row[BGP_MAXH];     /* per screen row: source row (relative to sy0, negative = ahead) */
    int32_t u0[BGP_MAXH], du[BGP_MAXH];   /* 16.16 source column at x = 0 and step per pixel */
} bgp_t;

/* Set up for a w x h screen, the vehicle at row y0 and the horizon at yh (< 0).  Returns 0, or
 * -1 when the source would be unreasonably large (more than 4x the screen). */
int bgp_init(bgp_t *p, int w, int h, int y0, int yh);

/* Screen from source (RGB565, sw x sh, stride sw).  dx, dy: the source moved by this many
 * pixels since it was drawn (the vehicle advanced), applied before projecting.  Source
 * pixels that do not exist are filled with bg. */
void bgp_remap(const bgp_t *p, const uint16_t *src, uint16_t *dst, int dst_stride, int dx, int dy, uint16_t bg);

/* A source point to the screen; 0 when it is behind the camera or beyond the far edge. */
int bgp_project(const bgp_t *p, int sx, int sy, int *x, int *y);
/* A screen point to the source (for touches); 0 when above the far edge. */
int bgp_unproject(const bgp_t *p, int x, int y, int *sx, int *sy);

#endif
