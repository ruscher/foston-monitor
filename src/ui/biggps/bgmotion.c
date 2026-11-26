/* bgmotion.c - display position between GPS fixes (bgmotion.h).
 * A handful of double operations per call (a few calls per second): no per-pixel math.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgmotion.h"
#include <math.h>
#include <string.h>

#ifndef TAU_POS
#define TAU_POS 0.9            /* s: how fast the display catches up with the estimate */
#endif
#define TAU_CAM 0.70           /* s: camera turns slower than the vehicle */
#define CAM_MAX_DPS 90.0       /* degrees per second at most */
#define SNAP_M 80.0            /* farther than this from the estimate: jump (reroute, teleport) */
#define BACK_M 15.0            /* ahead of the estimate by less than this: wait, never reverse */
#define STILL_M 6.0            /* standing still: ignore wander smaller than this */
#define LOOK_DM 60.0           /* heading from the route +-6 m around the vehicle */

double bgmo_angle_diff(double a, double b)
{
    double d = fmod(b - a, 360.0);
    if (d <= -180) d += 360;
    if (d > 180) d -= 360;
    return d;
}

static double norm360(double a)
{
    a = fmod(a, 360.0);
    return a < 0 ? a + 360 : a;
}

static double bearing(double ax, double ay, double bx, double by)
{
    return norm360(atan2(bx - ax, -(by - ay)) * 180 / M_PI);   /* world y grows southwards */
}

void bgmo_reset(bgmo_t *m)
{
    memset(m, 0, sizeof *m);
    m->course = m->head = -1;
}

void bgmo_set_route(bgmo_t *m, const uint32_t *xy, const uint32_t *cum_dm, int npts)
{
    m->xy = npts >= 2 ? xy : NULL;
    m->cum_dm = npts >= 2 ? cum_dm : NULL;
    m->npts = npts >= 2 ? npts : 0;
    m->fix_on_route = 0;
}

static void seg_point(const bgmo_t *m, double s, double *x, double *y, int *seg)
{
    int n = m->npts;
    if (s <= 0) { *x = m->xy[0]; *y = m->xy[1]; *seg = 0; return; }
    if (s >= m->cum_dm[n - 1]) { *x = m->xy[2 * n - 2]; *y = m->xy[2 * n - 1]; *seg = n - 2; return; }
    int lo = 0, hi = n - 1;                       /* cum_dm[lo] <= s < cum_dm[hi] */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (m->cum_dm[mid] <= s) lo = mid; else hi = mid;
    }
    double span = (double)m->cum_dm[hi] - m->cum_dm[lo], f = span > 0 ? (s - m->cum_dm[lo]) / span : 0;
    double ax = m->xy[2 * lo], ay = m->xy[2 * lo + 1];
    double dx = (double)(int32_t)(m->xy[2 * hi] - m->xy[2 * lo]), dy = (double)(int32_t)(m->xy[2 * hi + 1] - m->xy[2 * lo + 1]);
    *x = ax + f * dx;
    *y = ay + f * dy;
    *seg = lo;
}

void bgmo_route_point(const bgmo_t *m, double s, double look_dm, double *x, double *y, double *head)
{
    int seg, sa, sb;
    double ax, ay, bx, by;
    seg_point(m, s, x, y, &seg);
    seg_point(m, s - look_dm, &ax, &ay, &sa);
    seg_point(m, s + look_dm, &bx, &by, &sb);
    if (fabs(bx - ax) + fabs(by - ay) < 1) {      /* both ends clamped to one point */
        ax = m->xy[2 * seg]; ay = m->xy[2 * seg + 1];
        bx = m->xy[2 * seg + 2]; by = m->xy[2 * seg + 3];
    }
    *head = fabs(bx - ax) + fabs(by - ay) < 1 ? -1 : bearing(ax, ay, bx, by);
}

void bgmo_fix(bgmo_t *m, uint64_t t_us, uint32_t x, uint32_t y, double speed_kmh, double course,
              int on_route, uint32_t progress_dm)
{
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y / 4294967296.0)));
    int was_on = m->fix_on_route;
    m->mpu = 40075016.686 / 4294967296.0 * cos(lat);
    m->fix_us = t_us;
    m->fix_x = x;
    m->fix_y = y;
    m->speed = speed_kmh >= 3 ? speed_kmh / 3.6 : 0;
    if (course >= 0) m->course = norm360(course);
    m->fix_on_route = on_route && m->xy;
    if (m->fix_on_route) m->fix_s = progress_dm;
    m->has_fix = 1;
    m->stale = 0;
    if (!m->has_disp) {
        m->has_disp = 1;
        m->t_us = t_us;
        m->x = x;
        m->y = y;
        m->s = m->fix_s;
        m->head = m->speed > 0 ? m->course : -1;
        m->cam = m->head >= 0 ? m->head : 0;
        if (m->fix_on_route) bgmo_route_point(m, m->s, LOOK_DM, &m->x, &m->y, &m->head);
        if (m->head >= 0) m->cam = m->head;
        return;
    }
    if (m->fix_on_route && !was_on) m->s = m->fix_s;   /* joined the route: start from the fix */
}

int bgmo_step(bgmo_t *m, uint64_t t_us)
{
    if (!m->has_disp) return 0;
    double dt = t_us > m->t_us ? (double)(t_us - m->t_us) / 1e6 : 0;
    if (dt > 0.5) dt = 0.5;
    m->t_us = t_us;
    uint64_t since = t_us > m->fix_us ? t_us - m->fix_us : 0;
    m->stale = since > BGMO_STALE_US;
    int predicting = since < BGMO_PRED_US && m->speed > 0;
    double el = (double)(since < BGMO_PRED_US ? since : BGMO_PRED_US) / 1e6;
    double pred_m = m->speed * el;
    if (pred_m > BGMO_PRED_M) pred_m = BGMO_PRED_M;
    double k = 1 - exp(-dt / TAU_POS);
    double ox = m->x, oy = m->y, oh = m->head, oc = m->cam;

    if (m->fix_on_route && m->xy) {
        double end = m->cum_dm[m->npts - 1], target = m->fix_s + pred_m * 10, s = m->s;
        if (target > end) target = end;                       /* never past the destination */
        if (fabs(target - s) > SNAP_M * 10) {
            s = target;
        } else {
            double adv = predicting ? m->speed * 10 * dt : 0;
            double ns = s + adv;
            if (ns > end) ns = end;
            ns += (target - ns) * k;
            if (ns < s && s - target < BACK_M * 10) ns = s;   /* a bit ahead: pause, never reverse */
            s = ns;
        }
        m->s = s;
        double h;
        bgmo_route_point(m, s, LOOK_DM, &m->x, &m->y, &h);
        if (h >= 0 && (m->speed > 0 || m->head < 0)) m->head = h;
    } else {
        double tx = m->fix_x, ty = m->fix_y, vx = 0, vy = 0;
        if (m->speed > 0 && m->course >= 0 && m->mpu > 0) {
            double r = m->course * M_PI / 180;
            vx = sin(r) * m->speed / m->mpu;
            vy = -cos(r) * m->speed / m->mpu;
            tx += vx * pred_m / m->speed;
            ty += vy * pred_m / m->speed;
        }
        double dx = tx - m->x, dy = ty - m->y, dist_m = sqrt(dx * dx + dy * dy) * m->mpu;
        if (dist_m > SNAP_M) {
            m->x = tx;
            m->y = ty;
        } else if (m->speed > 0 || dist_m > STILL_M) {        /* standing: ignore the wander */
            if (predicting) { m->x += vx * dt; m->y += vy * dt; }
            m->x += (tx - m->x) * k;
            m->y += (ty - m->y) * k;
        }
        if (m->speed > 0 && m->course >= 0) m->head = m->course;
    }

    if (m->head >= 0) {
        double d = bgmo_angle_diff(m->cam, m->head), step = d * (1 - exp(-dt / TAU_CAM)), lim = CAM_MAX_DPS * dt;
        if (step > lim) step = lim;
        if (step < -lim) step = -lim;
        if (fabs(d) < 0.05) step = d;
        m->cam = norm360(m->cam + step);
    }
    return fabs(m->x - ox) > 0.5 || fabs(m->y - oy) > 0.5 || fabs(bgmo_angle_diff(oc, m->cam)) > 0.05
        || (oh < 0) != (m->head < 0) || (oh >= 0 && fabs(bgmo_angle_diff(oh, m->head)) > 0.05);
}
