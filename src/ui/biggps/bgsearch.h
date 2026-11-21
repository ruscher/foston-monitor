/* bgsearch.h - offline search on a Big GPS index (.bgs, see bgsearch_format.h): prefix
 * search of every word typed (any order, no accents needed), ranked by importance and
 * distance; nearby points of interest by category.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGSEARCH_H
#define BGSEARCH_H
#include <stddef.h>
#include <stdint.h>
#include "bgsearch_format.h"

typedef struct bgs_index bgs_index_t;

typedef struct {
    char name[96], context[64];
    uint32_t x, y;
    uint8_t type, cat;
    uint32_t dist_m;           /* from the reference point */
} bgs_result_t;

bgs_index_t *bgs_open(const char *path, char *err, size_t errlen);
void bgs_close(bgs_index_t *ix);
uint32_t bgs_count(const bgs_index_t *ix);

/* Results for the text, best first; (x, y) = reference point (world), for distance. */
int bgs_query(bgs_index_t *ix, const char *text, uint32_t x, uint32_t y, bgs_result_t *out, int max);
/* Points of interest of a category nearest to (x, y), within max_m metres. */
int bgs_nearby(bgs_index_t *ix, int cat, uint32_t x, uint32_t y, uint32_t max_m, bgs_result_t *out, int max);

#endif
