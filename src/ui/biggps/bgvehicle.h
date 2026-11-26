/* bgvehicle.h - the "you are here" marker of the Big GPS, drawn once (4x4 supersampling) into a
 * small ARGB8888 bitmap and only rotated/scaled by LVGL afterwards: top views of a car (four
 * styles), motorcycle, bicycle and walker, plus the plain arrow and dot.  Front points up.
 * No LVGL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGVEHICLE_H
#define BGVEHICLE_H
#include <stdint.h>

#define BGVEH_SIZE 48              /* bitmap side; the shape is centred at (24, 24) */

enum {
    BGVEH_ARROW, BGVEH_DOT, BGVEH_SEDAN, BGVEH_COMPACT, BGVEH_SUV, BGVEH_SPORT,
    BGVEH_MOTO, BGVEH_BIKE, BGVEH_WALKER, BGVEH_MODELS,
    /* not offered as markers: the destination pin (48x48, tip at (24, 46), always red) and the
     * search magnifier (draw at 32x32, white, tinted by the theme) */
    BGVEH_PIN = 100, BGVEH_MAGNIFIER
};

#define BGVEH_COLORS 8
extern const uint32_t bgveh_color[BGVEH_COLORS];   /* 0xRRGGBB, [0] = the system blue */

/* Models offered for a navigation profile (BGR_CAR, BGR_MOTO, BGR_BIKE, BGR_FOOT); the first is
 * the default.  Returns how many. */
int bgveh_models(int profile, const int **models);

/* Draw model in color into px (BGVEH_SIZE * BGVEH_SIZE, ARGB8888, premultiplied: no).
 * grey: the "position uncertain" look (desaturated, translucent). */
void bgveh_draw(uint32_t *px, int model, uint32_t color, int grey);
/* The same on a side x side bitmap (the model's coordinates are not scaled). */
void bgveh_draw_n(uint32_t *px, int side, int model, uint32_t color, int grey);

#endif
