/* bgvoice.h - pre-generated voice pack (.bgv): spoken phrases made on the PC with the Piper
 * Faber voice (tools/big-gps/make-faber-voice.py), looked up by their text and played on the
 * device.  The neural model does not run on the FOSTON (docs/big-gps-2/22-piper-faber.md).
 *
 * File (little endian):
 *   header 24 bytes: "BGV1", u32 version (1), u32 count, u32 rate (Hz), u32 keys_off, u32 data_off
 *   count index entries of 16 bytes, sorted by key bytes:
 *     u32 key_off (from keys_off), u16 key_len, u16 0, u32 data_off (from data_off), u32 nsamples
 *   keys: normalized UTF-8 text (bgv_normalize), no terminator
 *   data per clip: i16 first predictor, u8 step index, u8 0, then IMA ADPCM nibbles
 *     (mono, low nibble first), (nsamples + 1) / 2 bytes
 * No LVGL; the encoder is here too so the PC tool and the tests share it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGVOICE_H
#define BGVOICE_H
#include <stddef.h>
#include <stdint.h>

#define BGV_MAGIC "BGV1"
#define BGV_VERSION 1

typedef struct bgv_pack bgv_pack_t;
typedef struct {
    const uint8_t *data;       /* predictor, index, nibbles */
    uint32_t nsamples;
} bgv_clip_t;

bgv_pack_t *bgv_open(const char *path, char *err, int errlen);
void bgv_close(bgv_pack_t *p);
uint32_t bgv_count(const bgv_pack_t *p);
uint32_t bgv_rate(const bgv_pack_t *p);
/* Clip for a phrase (any case/spacing; normalized here).  0 when the pack does not have it. */
int bgv_find(const bgv_pack_t *p, const char *text, bgv_clip_t *out);

/* Lowercase (ASCII and Latin-1 letters), single spaces, no leading/trailing spaces or
 * punctuation (, . ; : ! ? …).  Same rule as make-faber-voice.py. */
void bgv_normalize(const char *in, char *out, size_t n);

/* IMA ADPCM.  decode writes nsamples samples. */
void bgv_decode(const bgv_clip_t *c, int16_t *out);
/* encode: writes 4 + (n + 1) / 2 bytes to out (header + nibbles). */
size_t bgv_encode(const int16_t *in, uint32_t n, uint8_t *out);

#endif
