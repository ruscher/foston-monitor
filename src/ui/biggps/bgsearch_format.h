/* bgsearch_format.h - Big GPS search index (.bgs), built on the PC by
 * tools/big-gps/builder/bgsearch-build and read by bgsearch.c on the device.
 *
 * Entries: streets (ways of the same name in the same neighbourhood merged), places, points
 * of interest by category and addresses with a house number; each with a display name, a
 * context (neighbourhood / town) and a position.  Words are normalized (bgs_norm: lower
 * case, no accents, letters and digits only) and indexed for prefix search.
 *
 * File (little endian, 4-byte aligned sections):
 *   bgs_header_t
 *   bgs_entry_t[nentries]
 *   tokens: bgs_token_t[ntokens] sorted by text (strcmp of the normalized word)
 *   postings: u32 entry ids, per token sorted by entry id
 *   strings: NUL-terminated UTF-8 (names, contexts) and normalized words
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BGSEARCH_FORMAT_H
#define BGSEARCH_FORMAT_H
#include <stddef.h>
#include <stdint.h>

#define BGS_MAGIC   0x31534742u   /* "BGS1" */
#define BGS_VERSION 1

enum { BGS_T_PLACE = 0, BGS_T_STREET, BGS_T_POI, BGS_T_ADDRESS };

/* point of interest categories */
enum {
    BGS_C_NONE = 0, BGS_C_FUEL, BGS_C_HOSPITAL, BGS_C_PHARMACY, BGS_C_POLICE, BGS_C_SCHOOL,
    BGS_C_UNIVERSITY, BGS_C_FOOD, BGS_C_BANK, BGS_C_MARKET, BGS_C_MALL, BGS_C_HOTEL,
    BGS_C_ATTRACTION, BGS_C_PARKING, BGS_C_BUS, BGS_C_AIRPORT, BGS_C_WORSHIP, BGS_C_GOV,
    BGS_C_SHOP, BGS_C_COUNT
};

typedef struct {
    uint32_t magic, version;
    uint32_t nentries, ntokens, npostings, strings_len;
    uint32_t off_entries, off_tokens, off_postings, off_strings;
    uint32_t created, reserved[5];
} bgs_header_t;

typedef struct {
    uint32_t x, y;             /* world coordinates (32-bit Web Mercator) */
    uint32_t name, context;    /* offsets in strings (context 0xFFFFFFFF = none) */
    uint8_t type, cat;         /* BGS_T_*, BGS_C_* */
    uint16_t rank;             /* importance (place size, road class), higher = better */
} bgs_entry_t;

typedef struct {
    uint32_t text;             /* offset of the normalized word in strings */
    uint32_t first, count;     /* postings[first .. first + count) */
} bgs_token_t;

/* Normalize UTF-8 text into lower-case ASCII words separated by single spaces (accents of
 * Portuguese/Spanish letters removed, other characters become spaces).  Returns the length. */
static inline size_t bgs_norm(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int space = 1;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && o + 1 < n) {
        unsigned c = *s++;
        char a = 0;
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') a = (char)(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) a = (char)c;
        } else if ((c == 0xC3) && *s) {           /* Latin-1 supplement */
            unsigned d = *s++ | 0x40;             /* fold upper case into lower case range */
            if (d >= 0xE0 && d <= 0xE5) a = 'a';
            else if (d == 0xE7) a = 'c';
            else if (d >= 0xE8 && d <= 0xEB) a = 'e';
            else if (d >= 0xEC && d <= 0xEF) a = 'i';
            else if (d == 0xF1) a = 'n';
            else if ((d >= 0xF2 && d <= 0xF6) || d == 0xF8) a = 'o';
            else if (d >= 0xF9 && d <= 0xFC) a = 'u';
        } else {
            while ((*s & 0xC0) == 0x80) s++;      /* other multibyte character: separator */
        }
        if (a) {
            out[o++] = a;
            space = 0;
        } else if (!space) {
            out[o++] = ' ';
            space = 1;
        }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o;
}

/* words not indexed (too common to help) */
static inline int bgs_stopword(const char *w, size_t len)
{
    static const char *const sw[] = { "de", "da", "do", "das", "dos", "e", "a", "o", "em", "na", "no" };
    for (size_t i = 0; i < sizeof sw / sizeof sw[0]; i++) {
        size_t l = 0;
        while (sw[i][l]) l++;
        if (l == len) {
            size_t k = 0;
            while (k < len && sw[i][k] == w[k]) k++;
            if (k == len) return 1;
        }
    }
    return 0;
}

#endif
