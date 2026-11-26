/* bgpersp.c - see bgpersp.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgpersp.h"
#include <string.h>

int bgp_init(bgp_t *p, int w, int h, int y0, int yh)
{
    if (h > BGP_MAXH || h < 2 || w < 2 || yh >= 0 || y0 <= 0 || y0 >= h) return -1;
    memset(p, 0, sizeof *p);
    p->w = w; p->h = h; p->y0 = y0; p->yh = yh;
    int64_t z0 = y0 - yh;
    /* depth of the top row and of the bottom row, in source pixels from the camera */
    int64_t ztop = z0 * z0 / (0 - yh), zbot = z0 * z0 / (h - 1 - yh);
    int ahead = (int)(ztop - z0), behind = (int)(z0 - zbot);
    int sw = (int)((int64_t)w * ztop / z0) + 4;
    sw = (sw + 7) & ~7;
    p->sw = sw;
    p->sh = ahead + behind + 4;
    p->scx = sw / 2;
    p->sy0 = ahead + 2;
    if ((int64_t)p->sw * p->sh > (int64_t)4 * w * h) return -1;
    for (int y = 0; y < h; y++) {
        int64_t z = z0 * z0 / (y - yh);               /* depth of this row */
        p->row[y] = (int32_t)(z0 - z);                /* rows ahead are negative */
        /* source column = scx + (x - w/2) * z / z0, as 16.16 */
        int64_t step = (z << 16) / z0;
        p->du[y] = (int32_t)step;
        p->u0[y] = (int32_t)(((int64_t)p->scx << 16) - (int64_t)(w / 2) * step);
    }
    return 0;
}

void bgp_remap(const bgp_t *p, const uint16_t *src, uint16_t *dst, int dst_stride, int dx, int dy, uint16_t bg)
{
    for (int y = 0; y < p->h; y++) {
        uint16_t *d = dst + (size_t)y * dst_stride;
        int sy = p->sy0 + p->row[y] - dy;
        if (sy < 0 || sy >= p->sh) {
            for (int x = 0; x < p->w; x++) d[x] = bg;
            continue;
        }
        const uint16_t *s = src + (size_t)sy * p->sw;
        int32_t u = p->u0[y] - dx * 65536, du = p->du[y];
        int x = 0;
        /* left and right ends outside the source: background; the middle has no test */
        while (x < p->w && (u < 0 || (u >> 16) >= p->sw)) {
            if (u >= 0 && (u >> 16) >= p->sw) break;
            d[x++] = bg; u += du;
        }
        int32_t ulast = ((int32_t)p->sw << 16) - 1;
        int n = du > 0 ? (int)((ulast - u) / du) + 1 : p->w;
        if (n > p->w - x) n = p->w - x;
        if (n < 0) n = 0;
        for (int i = 0; i < n; i++) { d[x + i] = s[u >> 16]; u += du; }
        for (x += n; x < p->w; x++) d[x] = bg;
    }
}

int bgp_project(const bgp_t *p, int sx, int sy, int *x, int *y)
{
    int64_t z0 = p->y0 - p->yh, z = z0 - (sy - p->sy0);   /* depth of that source row */
    if (z <= 0) return 0;
    int64_t yy = p->yh + z0 * z0 / z;
    if (yy < -32) return 0;
    *y = (int)yy;
    *x = (int)(p->w / 2 + (int64_t)(sx - p->scx) * z0 / z);
    return 1;
}

int bgp_unproject(const bgp_t *p, int x, int y, int *sx, int *sy)
{
    if (y <= p->yh) return 0;
    int64_t z0 = p->y0 - p->yh, z = z0 * z0 / (y - p->yh);
    *sy = (int)(p->sy0 + (z0 - z));
    *sx = (int)(p->scx + (int64_t)(x - p->w / 2) * z / z0);
    return *sy >= 0;
}
