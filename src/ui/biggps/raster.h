/* raster.h - integer rasterizer for the Big GPS map (RGB565, no floating point).
 * Coordinates are in 1/16 pixel (RAST_SUB); the caller transforms map units to that.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RASTER_H
#define RASTER_H
#include <stdint.h>

#define RAST_SHIFT 4
#define RAST_SUB   (1 << RAST_SHIFT)

typedef struct {
    uint16_t *px;
    int w, h, stride;          /* stride in pixels */
} rast_surface_t;

/* Fill polygon rings (even-odd).  xy: x,y pairs (1/16 px); ring_len[i] points per ring. */
void rast_fill(rast_surface_t *s, const int32_t *xy, const int *ring_len, int nrings, uint16_t color);

/* Polyline of width w16 (1/16 px).  Round joins when the line is 3 px or wider.
 * dash: 0 = solid, else on/off length in pixels. */
void rast_line(rast_surface_t *s, const int32_t *xy, int n, int w16, uint16_t color, int dash);

void rast_clear(rast_surface_t *s, uint16_t color);
void rast_disc(rast_surface_t *s, int32_t cx, int32_t cy, int r16, uint16_t color);

static inline uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

#endif
