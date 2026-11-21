/* bgroute.h - routes on a Big GPS route pack (.bgr, see bgroute_format.h).
 * The file is read with pread through a small page cache (works for packs larger than the
 * 32-bit address space); the query is a bidirectional upward search on the contraction
 * hierarchy with a hash table of visited nodes (memory follows the search, not the graph).
 * Not thread-safe: one pack per thread.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGROUTE_H
#define BGROUTE_H
#include <stddef.h>
#include <stdint.h>
#include "bgroute_format.h"

typedef struct bgr_pack bgr_pack_t;

bgr_pack_t *bgr_open(const char *path, char *err, size_t errlen);
void bgr_close(bgr_pack_t *p);
const bgr_header_t *bgr_info(const bgr_pack_t *p);

/* A position on an arc: the projection of a point on the road. */
typedef struct {
    uint32_t arc;
    uint32_t frac;             /* position along the arc, 0..65536 (by length) */
    uint32_t x, y;             /* projected point, world coordinates */
    uint32_t dist_dm;          /* distance from the point, decimetres */
    int16_t bearing;           /* direction of the arc at the projection, degrees (0 = north) */
} bgr_snap_t;

/* Nearest arcs to (x, y) within max_m metres, best first.  heading >= 0 prefers arcs going
 * that way (degrees, 0 = north); both directions of a two-way road are separate arcs. */
int bgr_snap(bgr_pack_t *p, uint32_t x, uint32_t y, int heading, int max_m, bgr_snap_t *out, int max);

typedef struct {
    uint32_t *arcs;            /* arcs in driving order (first = source arc, last = target arc) */
    int narcs;
    uint32_t time_ds;          /* travel time (deciseconds), including turn costs */
    uint32_t len_dm;           /* length (decimetres) from the source to the target position */
    uint32_t src_frac, dst_frac;
} bgr_route_t;

typedef struct {
    unsigned settled, relaxed, pages_read, us;
} bgr_qstats_t;

/* 0 = route found; -1 = no route; -2 = out of memory / read error. */
int bgr_route(bgr_pack_t *p, const bgr_snap_t *src, int nsrc, const bgr_snap_t *dst, int ndst,
              bgr_route_t *out, bgr_qstats_t *st);
void bgr_route_free(bgr_route_t *r);

/* Arc-to-arc distance (whole arcs), for checks against reference queries. */
uint32_t bgr_arc_distance(bgr_pack_t *p, uint32_t src_arc, uint32_t dst_arc, bgr_qstats_t *st);

/* Graph access (cached reads) */
uint32_t bgr_arc_tail(bgr_pack_t *p, uint32_t arc);
uint32_t bgr_arc_head(bgr_pack_t *p, uint32_t arc);
uint32_t bgr_arc_way(bgr_pack_t *p, uint32_t arc);
uint32_t bgr_arc_len_dm(bgr_pack_t *p, uint32_t arc);
uint32_t bgr_arc_time_ds(bgr_pack_t *p, uint32_t arc);
void bgr_node_xy(bgr_pack_t *p, uint32_t node, uint32_t *x, uint32_t *y);
/* Arc geometry from tail to head (tail and head included); returns the point count
 * (up to max, xy as x,y pairs). */
int bgr_arc_points(bgr_pack_t *p, uint32_t arc, uint32_t *xy, int max);
const char *bgr_way_name(bgr_pack_t *p, uint32_t way, char *buf, size_t n);   /* name or ref, "" if none */
uint32_t bgr_way_info(bgr_pack_t *p, uint32_t way);

void bgr_cache_stats(bgr_pack_t *p, unsigned *pages, unsigned *misses);

#endif
