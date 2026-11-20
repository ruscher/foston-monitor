/*
 * BFP framing (see bfp.h). Portable C: also compiled on the host by
 * tests/agent/test_bfp.py to check it against the Python implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "bfp.h"

uint16_t bfp_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

void bfp_header(uint8_t *out, uint8_t type, uint16_t seq, uint16_t status, const uint8_t *payload, uint32_t len)
{
    out[0] = BFP_MAGIC0;
    out[1] = BFP_MAGIC1;
    out[2] = BFP_VERSION;
    out[3] = type;
    bfp_put16(out + 4, seq);
    bfp_put16(out + 6, status);
    bfp_put32(out + 8, len);
    bfp_put16(out + 12, len ? bfp_crc16(payload, len) : 0xFFFF);
    bfp_put16(out + 14, bfp_crc16(out, 14));
}

void bfp_parser_init(bfp_parser *p, uint8_t *buf, size_t cap)
{
    p->buf = buf;
    p->cap = cap;
    p->used = 0;
    p->dropped = 0;
}

size_t bfp_feed(bfp_parser *p, const uint8_t *data, size_t len)
{
    size_t room = p->cap - p->used;
    if (len > room)
        len = room;
    memcpy(p->buf + p->used, data, len);
    p->used += len;
    return len;
}

static int header_ok(const uint8_t *h)
{
    return h[0] == BFP_MAGIC0 && h[1] == BFP_MAGIC1 && h[2] == BFP_VERSION &&
           bfp_get16(h + 14) == bfp_crc16(h, 14) && bfp_get32(h + 8) <= BFP_MAX_PAYLOAD;
}

static void drop(bfp_parser *p, size_t n)
{
    memmove(p->buf, p->buf + n, p->used - n);
    p->used -= n;
    p->dropped += (uint32_t)n;
}

int bfp_next(bfp_parser *p, bfp_msg *msg)
{
    for (;;) {
        size_t skip = 0;
        uint32_t len;
        /* resynchronise on the next "BF" whose header checks out */
        while (skip < p->used && p->buf[skip] != BFP_MAGIC0)
            skip++;
        if (skip)
            drop(p, skip);
        if (p->used < BFP_HEADER)
            return 0;
        if (!header_ok(p->buf)) {
            drop(p, 1);
            continue;
        }
        len = bfp_get32(p->buf + 8);
        if (p->used < BFP_HEADER + len)
            return 0;
        msg->type = p->buf[3];
        msg->seq = bfp_get16(p->buf + 4);
        msg->status = bfp_get16(p->buf + 6);
        msg->len = len;
        msg->payload = p->buf + BFP_HEADER;
        msg->crc_ok = bfp_get16(p->buf + 12) == (len ? bfp_crc16(msg->payload, len) : 0xFFFF);
        return 1;
    }
}

void bfp_consume(bfp_parser *p)
{
    size_t n;
    if (p->used < BFP_HEADER)
        return;
    n = BFP_HEADER + bfp_get32(p->buf + 8);
    if (n > p->used)
        n = p->used;
    memmove(p->buf, p->buf + n, p->used - n);
    p->used -= n;
}
