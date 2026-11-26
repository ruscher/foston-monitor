/* bglanes.h - lane guidance from a .bgl file (bglanes_format.h): the lanes at the junction of
 * a maneuver and which of them lead the right way.  Mapped, not read.  No LVGL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGLANES_H
#define BGLANES_H
#include <stdint.h>
#include "bglanes_format.h"

typedef struct bgl_pack bgl_pack_t;

bgl_pack_t *bgl_open(const char *path, char *err, int errlen);
void bgl_close(bgl_pack_t *p);
uint32_t bgl_count(const bgl_pack_t *p);

/* The lanes of the junction at (x, y) (within 25 m) for a driver arriving with bearing
 * (degrees, within 35 degrees); NULL when the data has none. */
const bgl_entry_t *bgl_find(const bgl_pack_t *p, uint32_t x, uint32_t y, int bearing);

/* Lanes that fit a maneuver (bgn_type_t), as bits (bit i = lane i from the left); 0 when
 * none clearly does — then nothing should be shown (only the general direction).  Unmarked
 * lanes count as straight on.  For keep/ramp maneuvers without a marked turn, the straight
 * lanes on that side of a marked turn count. */
unsigned bgl_fit(const bgl_entry_t *e, int maneuver);

/* The arrow to draw for lane i: the maneuver's direction when the lane fits, else the lane's
 * own main direction (bgn_type_t). */
int bgl_arrow(const bgl_entry_t *e, int i, int maneuver, unsigned fit);

#endif
