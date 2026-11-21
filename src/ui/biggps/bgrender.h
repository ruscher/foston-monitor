/* bgrender.h - draw a Big GPS map view into an RGB565 surface (integer math per vertex).
 * Labels are returned to the caller (the shell draws text with its own fonts).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGRENDER_H
#define BGRENDER_H
#include <stdint.h>
#include "bgmap.h"
#include "raster.h"

#define BGR_ZOOM_MIN (5 * 256)
#define BGR_ZOOM_MAX (19 * 256)

typedef struct {
    uint32_t cx, cy;           /* world coordinates of the view centre */
    int zoom256;               /* zoom level * 256 (fractional zoom allowed) */
    int rot;                   /* map rotation, degrees clockwise (0 = north up) */
    int night;                 /* night palette */
    int ox, oy;                /* screen point that shows (cx, cy); 0,0 = surface centre */
    int detail;                /* BGR_DETAIL_*: what is drawn (0 = balanced, the default) */
} bgr_view_t;

/* level of detail (docs/performance/09-gps-map-engine.md): the economic level leaves out
 * buildings, paths, service roads, parking and the casing of minor roads until close
 * zooms; detailed draws everything the map has from its minimum zoom */
enum { BGR_DETAIL_BALANCED = 0, BGR_DETAIL_ECONOMIC = 1, BGR_DETAIL_DETAILED = 2 };

/* minimum zoom at which a kind is drawn at a level of detail */
int bgr_kind_minzoom(int kind, int detail);

typedef struct {
    int16_t x, y;              /* screen position (centre of the text) */
    int16_t angle;             /* degrees, for road names (-90..90), 0 for places */
    uint8_t kind, prio;        /* prio: lower draws first / wins */
    const char *text;          /* valid until the next bgr_render / bgm_frame */
} bgr_label_t;

typedef struct {
    unsigned tiles, feats, pts, labels;
    unsigned us_fetch, us_draw, us_clear;
    unsigned us_kind[BGK_COUNT];   /* draw time per kind (casing pass included) */
} bgr_stats_t;

/* Draw the view; returns the number of label candidates stored (sorted by prio). */
int bgr_render(bgm_pack_t *p, const bgr_view_t *v, rast_surface_t *s,
               bgr_label_t *labels, int max_labels, bgr_stats_t *st);

uint16_t bgr_background(int night);

/* Draw a polyline given in world coordinates over a rendered view (the route): casing of
 * width cw16, then the line of width w16 (1/16 px); cw16 = 0 draws no casing. */
void bgr_draw_polyline(const bgr_view_t *v, rast_surface_t *s, const uint32_t *xy, int n,
                       int w16, uint16_t color, int cw16, uint16_t casing);

/* Conversions (double math: call per GPS fix or per touch, not per vertex). */
void bgr_lonlat_to_world(double lon, double lat, uint32_t *x, uint32_t *y);
void bgr_world_to_lonlat(uint32_t x, uint32_t y, double *lon, double *lat);
void bgr_world_to_screen(const bgr_view_t *v, const rast_surface_t *s, uint32_t x, uint32_t y, int *sx, int *sy);
void bgr_screen_to_world(const bgr_view_t *v, const rast_surface_t *s, int sx, int sy, uint32_t *x, uint32_t *y);
/* Metres per pixel at the view centre. */
double bgr_metres_per_pixel(const bgr_view_t *v);

#endif
