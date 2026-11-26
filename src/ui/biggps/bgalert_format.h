/* bgalert_format.h - Big GPS road alert points (.bga), built on the PC by
 * tools/big-gps/builder/bgalert-build from OpenStreetMap nodes, read by bgalert.c.
 * A separate file so existing map packs (.bgm) stay valid.
 *
 *   header (16 bytes): "BGA1", u32 version (1), u32 count, u32 built (unix time)
 *   count entries of 12 bytes, sorted by x:
 *     u32 x, u32 y          world coordinates (Web Mercator, 2^32 per axis, like .bgm)
 *     u8 kind               BGA_*
 *     u8 maxspeed           km/h from the node's own maxspeed tag, 0 = unknown
 *     u16 dir               direction it applies to, degrees 0..359, BGA_DIR_ANY = unknown/both
 * Little endian.  Data: OpenStreetMap contributors, ODbL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGALERT_FORMAT_H
#define BGALERT_FORMAT_H
#include <stdint.h>

#define BGA_MAGIC "BGA1"
#define BGA_VERSION 1
#define BGA_DIR_ANY 0xFFFF

enum {
    BGA_SIGNAL = 1,            /* highway=traffic_signals */
    BGA_CAMERA = 2,            /* highway=speed_camera (fixed speed camera) */
    BGA_BUMP = 3,              /* traffic_calming=bump/hump/table/cushion/yes/speed_bump */
    BGA_RAIL = 4,              /* railway=level_crossing */
    BGA_KINDS
};

typedef struct {
    uint32_t x, y;
    uint8_t kind, maxspeed;
    uint16_t dir;
} bga_entry_t;

#endif
