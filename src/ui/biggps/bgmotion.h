/* bgmotion.h - where to draw the vehicle between two GPS fixes.
 *
 * The receiver gives about one position per second; the screen can show many more frames.
 * bgmotion keeps a display position that glides toward the last real fix instead of
 * jumping: on a route it moves along the route geometry (never across a block), off a
 * route it dead-reckons with speed and course.  Prediction is bounded in time and distance
 * (BGMO_PRED_US, BGMO_PRED_M) and stops when fixes stop: the vehicle never drives on by
 * itself.  Standing still, small GPS noise is ignored.  The camera heading follows the
 * vehicle heading with its own, slower smoothing and always turns the short way.
 *
 * No LVGL, no clock of its own (the caller passes microseconds): tests drive it with
 * synthetic tracks (tests/test_bgmotion.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGMOTION_H
#define BGMOTION_H
#include <stdint.h>

#define BGMO_PRED_US 1500000ull    /* predict at most 1.5 s past the last fix */
#define BGMO_PRED_M  45.0          /* ... and at most 45 m */
#define BGMO_STALE_US 3000000ull   /* no fix for 3 s: position uncertain */

typedef struct {
    /* route being followed (borrowed; NULL when there is none) */
    const uint32_t *xy, *cum_dm;
    int npts;
    /* last fix */
    int has_fix, fix_on_route;
    uint64_t fix_us;
    double fix_x, fix_y;           /* world coordinates */
    double fix_s;                  /* on the route: decimetres from the start */
    double speed;                  /* m/s (0 below 3 km/h) */
    double course;                 /* degrees, -1 unknown */
    double mpu;                    /* metres per world unit at the fix */
    /* what is drawn */
    int has_disp;
    uint64_t t_us;
    double x, y, s;                /* display position (world) and route distance (dm) */
    double head;                   /* vehicle heading, degrees 0..360, -1 unknown */
    double cam;                    /* camera heading, degrees 0..360 */
    int stale;
} bgmo_t;

void bgmo_reset(bgmo_t *m);
/* Route to glide along (NULL/0 to drop it).  The arrays must outlive the use. */
void bgmo_set_route(bgmo_t *m, const uint32_t *xy, const uint32_t *cum_dm, int npts);
/* A real (or simulated) position.  on_route/progress_dm come from bgn_update. */
void bgmo_fix(bgmo_t *m, uint64_t t_us, uint32_t x, uint32_t y, double speed_kmh, double course,
              int on_route, uint32_t progress_dm);
/* Advance the display state to t_us.  Returns 1 when it moved or turned since the last call. */
int bgmo_step(bgmo_t *m, uint64_t t_us);
/* Point at route distance s (dm), clamped to the route; heading of the route around it,
 * averaged over +-look_dm so corners turn smoothly. */
void bgmo_route_point(const bgmo_t *m, double s, double look_dm, double *x, double *y, double *head);
/* Shortest signed difference b - a in degrees (-180..180]. */
double bgmo_angle_diff(double a, double b);

#endif
