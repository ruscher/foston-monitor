/* bgicons.h - maneuver icons (bgnav.h types) drawn into an RGB565 surface.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGICONS_H
#define BGICONS_H
#include "bgnav.h"
#include "raster.h"

/* fg: arrow, dim: secondary parts (other branch, roundabout ring), bg: background */
void bgn_icon(rast_surface_t *s, int type, int exit, uint16_t fg, uint16_t dim, uint16_t bg);

#endif
