/* bgicons.c - maneuver icons for the guidance banner, drawn with the integer rasterizer.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgicons.h"
#include <math.h>

#define S RAST_SUB

static void head(rast_surface_t *s, double x, double y, double ang, double size, uint16_t c)
{
    /* arrowhead with its tip at (x, y) pointing along ang (degrees, 0 = up, clockwise) */
    double r = ang * M_PI / 180, ux = sin(r), uy = -cos(r), px = -uy, py = ux;
    int32_t q[6] = {
        (int32_t)(x * S), (int32_t)(y * S),
        (int32_t)((x - ux * size + px * size * 0.75) * S), (int32_t)((y - uy * size + py * size * 0.75) * S),
        (int32_t)((x - ux * size - px * size * 0.75) * S), (int32_t)((y - uy * size - py * size * 0.75) * S),
    };
    int three = 3;
    rast_fill(s, q, &three, 1, c);
}

static void line(rast_surface_t *s, const double *p, int n, double w, uint16_t c)
{
    int32_t q[32];
    for (int i = 0; i < n && i < 16; i++) { q[2 * i] = (int32_t)(p[2 * i] * S); q[2 * i + 1] = (int32_t)(p[2 * i + 1] * S); }
    rast_line(s, q, n, (int)(w * S), c, 0);
}

void bgn_icon(rast_surface_t *s, int type, int exit, uint16_t fg, uint16_t dim, uint16_t bg)
{
    rast_clear(s, bg);
    double W = s->w, H = s->h, cx = W / 2, w = W * 0.13, hs = W * 0.22;
    double bottom = H * 0.92, mid = H * 0.52, len = H * 0.36;
    double ang = 0;
    switch (type) {
    case BGN_SLIGHT_RIGHT: case BGN_RAMP_RIGHT: case BGN_KEEP_RIGHT: ang = 40; break;
    case BGN_RIGHT: ang = 90; break;
    case BGN_SHARP_RIGHT: ang = 135; break;
    case BGN_SLIGHT_LEFT: case BGN_RAMP_LEFT: case BGN_KEEP_LEFT: ang = -40; break;
    case BGN_LEFT: ang = -90; break;
    case BGN_SHARP_LEFT: ang = -135; break;
    }
    if (type == BGN_ARRIVE) {
        /* map pin */
        double r = W * 0.24;
        int32_t q[6] = { (int32_t)((cx - r * 0.85) * S), (int32_t)(H * 0.42 * S), (int32_t)((cx + r * 0.85) * S), (int32_t)(H * 0.42 * S),
                         (int32_t)(cx * S), (int32_t)(H * 0.92 * S) };
        int three = 3;
        rast_disc(s, (int32_t)(cx * S), (int32_t)(H * 0.36 * S), (int)(r * S), fg);
        rast_fill(s, q, &three, 1, fg);
        rast_disc(s, (int32_t)(cx * S), (int32_t)(H * 0.36 * S), (int)(r * 0.42 * S), bg);
        return;
    }
    if (type == BGN_ROUNDABOUT) {
        double r = W * 0.2, cy = H * 0.45;
        /* ring */
        rast_disc(s, (int32_t)(cx * S), (int32_t)(cy * S), (int)((r + w / 2) * S), dim);
        rast_disc(s, (int32_t)(cx * S), (int32_t)(cy * S), (int)((r - w / 2) * S), bg);
        /* entry from the bottom, exit at an angle by exit number (right-hand traffic: counter-clockwise) */
        double e = exit <= 1 ? 90 : exit == 2 ? 0 : exit == 3 ? -90 : -150;
        double p1[4] = { cx, bottom, cx, cy + r };
        line(s, p1, 2, w, fg);
        double er = e * M_PI / 180, ex = cx + sin(er) * r, ey = cy - cos(er) * r;
        double ox = cx + sin(er) * (r + H * 0.2), oy = cy - cos(er) * (r + H * 0.2);
        /* arc from the entry (bottom, 180°) counter-clockwise to the exit angle */
        double a0 = 180, a1 = e < 180 ? e : e - 360;
        if (a1 > a0) a1 -= 360;
        double arc[32];
        int na = 0;
        for (int k = 0; k <= 10 && na < 16; k++) {
            double a = (a0 + (a1 - a0) * k / 10) * M_PI / 180;
            arc[2 * na] = cx + sin(a) * r;
            arc[2 * na + 1] = cy - cos(a) * r;
            na++;
        }
        line(s, arc, na, w, fg);
        double p2[4] = { ex, ey, ox, oy };
        line(s, p2, 2, w, fg);
        head(s, ox + sin(er) * hs * 0.6, oy - cos(er) * hs * 0.6, e, hs, fg);
        return;
    }
    if (type == BGN_UTURN) {
        /* up, around to the left, down (right-hand traffic) */
        double r = W * 0.17, top = H * 0.32, rx = cx + r * 0.6, lx = rx - 2 * r;
        double p[32];
        int np = 0;
        p[np++] = rx; p[np++] = bottom;
        for (int k = 0; k <= 10; k++) {
            double a = M_PI * k / 10;
            p[np++] = rx - r + cos(a) * r;
            p[np++] = top - sin(a) * r;
        }
        line(s, p, np / 2, w, fg);
        double p2[4] = { lx, top, lx, H * 0.62 };
        line(s, p2, 2, w, fg);
        head(s, lx, H * 0.62 + hs, 180, hs, fg);
        return;
    }
    if (type == BGN_KEEP_LEFT || type == BGN_KEEP_RIGHT) {
        /* the other branch, dimmed */
        double r = -ang * M_PI / 180;
        double po[6] = { cx, bottom, cx, mid, cx + sin(r) * len, mid - cos(r) * len };
        line(s, po, 3, w * 0.8, dim);
    }
    if (ang == 0) {
        double p[4] = { cx, bottom, cx, H * 0.08 + hs };
        line(s, p, 2, w, fg);
        head(s, cx, H * 0.08, 0, hs, fg);
        return;
    }
    double r = ang * M_PI / 180, ex = cx + sin(r) * len, ey = mid - cos(r) * len;
    double p[6] = { cx, bottom, cx, mid, ex, ey };
    line(s, p, 3, w, fg);
    head(s, ex + sin(r) * hs * 0.7, ey - cos(r) * hs * 0.7, ang, hs, fg);
}
