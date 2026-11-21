/* bgmap_format.h - Big GPS map pack (.bgm) layout, shared by the PC builder and the device.
 *
 * World coordinates: Web Mercator scaled to 32 bits (x = 0 at 180 W, y = 0 at 85.05 N; the
 * whole world is 2^32 units on each side).  The map is cut in the standard slippy tile grid
 * at a few "data levels" (zooms); inside a tile, coordinates are integers in units of
 * 2^(32 - zoom - BGM_EXTENT_BITS) world units relative to the tile's north-west corner, so a
 * tile is 8192 units wide (with a small buffer outside, see BGM_BUFFER).
 *
 * File (little endian):
 *   bgm_header_t
 *   bgm_level_t[nlevels]
 *   tile blobs
 *   per level: bgm_index_t[ntiles] sorted by key (ty << 16 | tx)
 *   attribution text (NUL-terminated UTF-8)
 *
 * Tile blob:
 *   varint nstr, then nstr x { varint len, bytes }        (names, UTF-8)
 *   varint nfeat, varint nring, varint npts (totals, for one allocation when decoding)
 *   features sorted by kind (= draw order):
 *     u8 kind, u8 flags (BGM_F_*)
 *     [varint name index]                                  if BGM_F_NAME
 *     point: zigzag x, zigzag y
 *     line:  varint n, n x (zigzag dx, zigzag dy)          (first delta from 0,0)
 *     area:  varint nrings, per ring { varint n, n x (zigzag dx, zigzag dy) }
 *            (deltas continue across rings; even-odd fill)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BGMAP_FORMAT_H
#define BGMAP_FORMAT_H
#include <stdint.h>

#define BGM_MAGIC        0x314D4742u   /* "BGM1" */
#define BGM_VERSION      1
#define BGM_EXTENT_BITS  13
#define BGM_EXTENT       (1 << BGM_EXTENT_BITS)
#define BGM_BUFFER       256           /* tile units kept outside each edge */
#define BGM_MAX_LEVELS   8

typedef struct {
    uint32_t magic, version;
    uint32_t nlevels;
    uint32_t bbox[4];          /* world x0, y0, x1, y1 */
    uint32_t attr_off;         /* attribution text */
    uint32_t created;          /* unix time of the build */
    uint32_t data_date;        /* unix time of the OSM data (0 = unknown) */
    uint32_t reserved[6];
} bgm_header_t;                /* 64 bytes */

typedef struct {
    uint8_t zoom, pad[3];
    uint32_t ntiles;
    uint32_t index_off;
    uint32_t reserved;
} bgm_level_t;                 /* 16 bytes */

typedef struct {
    uint32_t key;              /* ty << 16 | tx */
    uint32_t off, len;
} bgm_index_t;                 /* 12 bytes */

enum {
    BGM_F_POINT = 0, BGM_F_LINE = 1, BGM_F_AREA = 2, BGM_F_GEOM = 3,
    BGM_F_NAME = 1 << 2,
    BGM_F_BRIDGE = 1 << 3,
    BGM_F_TUNNEL = 1 << 4,
    BGM_F_LINK = 1 << 5,
    BGM_F_ONEWAY = 1 << 6,
};

/* feature kinds, in draw order */
enum {
    BGK_NONE = 0,
    /* areas */
    BGK_A_RESIDENTIAL = 1, BGK_A_COMMERCIAL, BGK_A_INDUSTRIAL, BGK_A_FARM, BGK_A_FOREST,
    BGK_A_PARK, BGK_A_SAND, BGK_A_WETLAND, BGK_A_WATER, BGK_A_AEROWAY, BGK_A_PEDESTRIAN,
    BGK_A_PARKING, BGK_A_BUILDING,
    /* lines */
    BGK_L_STREAM = 20, BGK_L_RIVER, BGK_L_BOUNDARY, BGK_L_RAIL, BGK_L_PATH, BGK_L_TRACK,
    BGK_L_SERVICE, BGK_L_RESIDENTIAL, BGK_L_TERTIARY, BGK_L_SECONDARY, BGK_L_PRIMARY,
    BGK_L_TRUNK, BGK_L_MOTORWAY, BGK_L_FERRY,
    /* points (labels) */
    BGK_P_CITY = 40, BGK_P_TOWN, BGK_P_SUBURB, BGK_P_VILLAGE, BGK_P_NEIGHBOURHOOD,
    BGK_COUNT = 48
};

#endif
