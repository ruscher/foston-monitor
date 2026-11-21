/* bgroute_format.h - Big GPS route pack (.bgr): one file per profile (car, motorcycle,
 * bicycle, foot), built on the PC by tools/big-gps/builder/bgroute-build and read by
 * bgroute.c on the device.
 *
 * Graph model:
 *   - routing nodes (intersections and way ends) with 32-bit Web Mercator coordinates;
 *   - arcs u->v (one per direction of travel) with way, length, travel time and the
 *     geometry between u and v (modelling nodes, in the direction of the arc);
 *   - a turn-expanded graph: its nodes are the arcs; an edge a->b exists when b leaves the
 *     head of a and the turn is allowed (OSM turn restrictions, no U-turn except at dead
 *     ends); its weight is turn cost + travel time of b.  This keeps turn restrictions
 *     and turn costs inside a plain node-based contraction hierarchy;
 *   - the contraction hierarchy (RoutingKit) over the expanded graph: upward forward and
 *     backward graphs with shortcut unpacking data, indexed by CH node id;
 *   - locality on the SD card: routing nodes are numbered along a Hilbert curve and arcs
 *     follow their tail; CH node ids keep that spatial order for the lower part of the
 *     hierarchy and put the top (visited by every long query) together, in rank order;
 *   - a grid of arcs for snapping a position to the nearest road.
 *
 * File: bgr_header_t, then sections (offset/length in header.sec[]), all little endian,
 * 4-byte aligned arrays.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BGROUTE_FORMAT_H
#define BGROUTE_FORMAT_H
#include <stdint.h>

#define BGR_MAGIC   0x31524742u   /* "BGR1" */
#define BGR_VERSION 1

enum { BGR_CAR = 0, BGR_MOTO, BGR_BIKE, BGR_FOOT, BGR_PROFILES };

enum {
    BGR_S_NODE_X, BGR_S_NODE_Y,                /* u32[nodes] */
    BGR_S_FIRST_OUT,                           /* u32[nodes + 1]: arcs sorted by tail */
    BGR_S_ARC_TAIL, BGR_S_ARC_HEAD, BGR_S_ARC_WAY,   /* u32[arcs] */
    BGR_S_ARC_LEN,                             /* u32[arcs], decimetres */
    BGR_S_ARC_TIME,                            /* u32[arcs], deciseconds */
    BGR_S_GEOM_FIRST,                          /* u32[arcs + 1] into GEOM_X/Y */
    BGR_S_GEOM_X, BGR_S_GEOM_Y,                /* u32[geom] (points between tail and head) */
    BGR_S_WAY_NAME, BGR_S_WAY_REF,             /* u32[ways]: offset in STRINGS, 0xFFFFFFFF = none */
    BGR_S_WAY_INFO,                            /* u32[ways]: class (low 8 bits, BGR_W_*) | flags << 8 */
    BGR_S_STRINGS,                             /* NUL-terminated UTF-8 */
    BGR_S_EX_HEAD,                             /* u32[ex_edges]: arc entered by the expanded edge */
    BGR_S_CH_ID,                               /* u32[arcs]: expanded node (arc) -> CH node id */
    BGR_S_CHF_FIRST, BGR_S_CHF_HEAD, BGR_S_CHF_WEIGHT, BGR_S_CHF_A, BGR_S_CHF_B, BGR_S_CHF_ORIG,
    BGR_S_CHB_FIRST, BGR_S_CHB_HEAD, BGR_S_CHB_WEIGHT, BGR_S_CHB_A, BGR_S_CHB_B, BGR_S_CHB_ORIG,
    BGR_S_GRID_FIRST,                          /* u32[gw * gh + 1] */
    BGR_S_GRID_ARCS,                           /* u32[...]: arcs touching each cell */
    BGR_NSEC
};
/* CH sides (forward / backward), as RoutingKit: FIRST u32[n+1], HEAD/WEIGHT/A/B u32[m],
 * ORIG bit vector (u32 words, bit i of word i/32): set = A is an expanded edge id (B its
 * tail); clear = shortcut of backward arc A then forward arc B. */

/* way classes (instructions, road names) */
enum {
    BGR_W_OTHER = 0, BGR_W_MOTORWAY, BGR_W_TRUNK, BGR_W_PRIMARY, BGR_W_SECONDARY, BGR_W_TERTIARY,
    BGR_W_RESIDENTIAL, BGR_W_SERVICE, BGR_W_TRACK, BGR_W_PATH, BGR_W_CYCLEWAY, BGR_W_FOOTWAY,
    BGR_W_STEPS, BGR_W_FERRY
};
enum { BGR_WF_ROUNDABOUT = 1, BGR_WF_LINK = 2, BGR_WF_TOLL = 4, BGR_WF_TUNNEL = 8, BGR_WF_BRIDGE = 16 };

typedef struct {
    uint32_t off, len;
} bgr_sec_t;

typedef struct {
    uint32_t magic, version, profile;
    uint32_t nodes, arcs, ways, geom, ex_edges, ch_fwd, ch_bwd;
    uint32_t grid_x0, grid_y0;                 /* world coordinates of the grid origin */
    uint32_t grid_shift;                       /* cell = 1 << grid_shift world units */
    uint32_t grid_w, grid_h;
    uint32_t created, data_date;
    uint32_t reserved[7];
    bgr_sec_t sec[BGR_NSEC];
} bgr_header_t;

#endif
