/* bgspeed_format.h - Big GPS road speed limits (.bgk), built on the PC by
 * tools/big-gps/builder/bgspeed-build from the OpenStreetMap maxspeed tags (only numeric values
 * on the way itself: nothing is guessed from the road type), read by bgspeed.c.  A file of its
 * own, like the alerts and the lanes, so the route packs stay as they are.
 *
 * One entry per way segment (consecutive nodes), split so no piece is longer than
 * BGK_MAX_PIECE_M, in the way's node order:
 *   header (16 bytes): "BGK1", u32 version (1), u32 count, u32 built (unix time)
 *   count entries of 20 bytes, sorted by min(xa, xb):
 *     u32 xa, ya, xb, yb    the piece (world coordinates, as .bgm)
 *     u8 fwd, bwd           km/h along the way's direction and against it; 0 = unknown
 *     u16 0
 * Little endian.  Data: OpenStreetMap contributors, ODbL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGSPEED_FORMAT_H
#define BGSPEED_FORMAT_H
#include <stdint.h>

#define BGK_MAGIC "BGK1"
#define BGK_VERSION 1
#define BGK_MAX_PIECE_M 200

typedef struct {
    uint32_t xa, ya, xb, yb;
    uint8_t fwd, bwd;
    uint16_t pad;
} bgk_entry_t;

#endif
