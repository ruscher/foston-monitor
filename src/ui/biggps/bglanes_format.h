/* bglanes_format.h - Big GPS lane guidance (.bgl), built on the PC by
 * tools/big-gps/builder/bglanes-build from the OpenStreetMap turn:lanes tags, read by
 * bglanes.c.  A file of its own (like the alerts) so the route packs stay as they are.
 *
 * One entry per way end where the lanes apply: the junction a driver reaches and the
 * bearing they arrive with.  turn:lanes and turn:lanes:forward apply at the way's last node
 * (arriving along the way), turn:lanes:backward at its first node (arriving against it).
 *
 *   header (16 bytes): "BGL1", u32 version (1), u32 count, u32 built (unix time)
 *   count entries of 28 bytes, sorted by x:
 *     u32 x, u32 y          the junction (world coordinates, as .bgm)
 *     u16 bearing           arriving direction, degrees 0..359 (0 = north)
 *     u8 nlanes (1..8), u8 0
 *     u16 lane[8]           BGL_* bits per lane, left to right; 0 = no marking
 * Little endian.  Data: OpenStreetMap contributors, ODbL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGLANES_FORMAT_H
#define BGLANES_FORMAT_H
#include <stdint.h>

#define BGL_MAGIC "BGL1"
#define BGL_VERSION 1
#define BGL_MAXLANES 8

enum {
    BGL_LEFT = 1, BGL_SLIGHT_LEFT = 2, BGL_SHARP_LEFT = 4, BGL_THROUGH = 8,
    BGL_RIGHT = 16, BGL_SLIGHT_RIGHT = 32, BGL_SHARP_RIGHT = 64, BGL_REVERSE = 128,
    BGL_MERGE_LEFT = 256, BGL_MERGE_RIGHT = 512,
};

typedef struct {
    uint32_t x, y;
    uint16_t bearing;
    uint8_t n, pad;
    uint16_t lane[BGL_MAXLANES];
} bgl_entry_t;

#endif
