/* bgmap.h - read a Big GPS map pack (.bgm, see bgmap_format.h) with a cache of decoded tiles.
 * Plain C, no floating point in the per-tile work; used by the shell and the bench tools.
 * Not thread-safe: one pack is used from one thread.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGMAP_H
#define BGMAP_H
#include <stddef.h>
#include <stdint.h>
#include "bgmap_format.h"

typedef struct {
    uint8_t kind, flags;
    uint16_t name;             /* index in tile->names, 0xFFFF = none */
    uint16_t nrings;
    uint32_t ring;             /* first ring in tile->ring_start */
    int16_t bx0, by0, bx1, by1;    /* bounding box in tile units */
} bgm_feat_t;

typedef struct bgm_tile {
    int level, tx, ty;
    uint32_t used;             /* frame counter of the last use (cache) */
    size_t mem;
    int nfeat, nring, nname;
    bgm_feat_t *feat;
    uint32_t *ring_start;      /* nring + 1 entries, index in pts (pairs) */
    int16_t *pts;              /* x, y pairs in tile units */
    const char **names;
    int kind_first[BGK_COUNT + 1];  /* features of kind k: [kind_first[k], kind_first[k+1]) */
    struct bgm_tile *next;     /* hash chain */
} bgm_tile_t;

typedef struct bgm_pack bgm_pack_t;

bgm_pack_t *bgm_open(const char *path, char *err, size_t errlen);
/* Attach another file of the same map (tiles must not overlap; a tile is taken from the
 * first file that has it).  0 on success. */
int bgm_add(bgm_pack_t *p, const char *path, char *err, size_t errlen);
void bgm_close(bgm_pack_t *p);
const bgm_header_t *bgm_header(const bgm_pack_t *p);
const char *bgm_attribution(const bgm_pack_t *p);
int bgm_nlevels(const bgm_pack_t *p);
int bgm_level_zoom(const bgm_pack_t *p, int level);
int bgm_level_for_zoom(const bgm_pack_t *p, int zoom);  /* highest level with zoom <= zoom */

/* Start a frame: tiles fetched from now on are kept until the next frame. */
void bgm_frame(bgm_pack_t *p);
/* Decoded tile (NULL if the pack has no data there); valid until the next bgm_frame(). */
const bgm_tile_t *bgm_tile(bgm_pack_t *p, int level, int tx, int ty);
void bgm_set_cache_limit(bgm_pack_t *p, size_t bytes);

typedef struct {
    unsigned hits, misses, empty, bytes_read, decode_us;
    size_t cache_bytes;
} bgm_stats_t;
void bgm_get_stats(bgm_pack_t *p, bgm_stats_t *st, int reset);

#endif
