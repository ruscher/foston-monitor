/*
 * Host harness for bfp.c, driven by tests/agent/test_bfp.py.
 *   host_bfp encode  -> writes a fixed set of messages to stdout
 *   host_bfp decode  -> reads a stream on stdin, prints one line per message:
 *                       "type seq status len crc_ok sum" then "dropped N"
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bfp.h"

static void emit(uint8_t type, uint16_t seq, uint16_t status, const uint8_t *p, uint32_t len)
{
    uint8_t h[BFP_HEADER];
    bfp_header(h, type, seq, status, p, len);
    fwrite(h, 1, BFP_HEADER, stdout);
    fwrite(p, 1, len, stdout);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "encode")) {
        static uint8_t big[70000];
        uint32_t i;
        for (i = 0; i < sizeof(big); i++)
            big[i] = (uint8_t)(i * 7 + 3);
        emit(0x81, 1, 0, (const uint8_t *)"hello", 5);
        emit(0x82, 2, 0, NULL, 0);
        emit(0xC0, 65535, 3, big, sizeof(big));
        return 0;
    } else {
        size_t cap = BFP_HEADER + BFP_MAX_PAYLOAD + 4096;
        uint8_t *buf = malloc(cap), chunk[777];
        bfp_parser p;
        bfp_msg m;
        size_t n;
        bfp_parser_init(&p, buf, cap);
        while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
            size_t i = 0;
            while (i < n) {
                i += bfp_feed(&p, chunk + i, n - i);
                while (bfp_next(&p, &m)) {
                    uint32_t sum = 0, k;
                    for (k = 0; k < m.len; k++)
                        sum = sum * 31 + m.payload[k];
                    printf("%u %u %u %u %d %u\n", m.type, m.seq, m.status, m.len, m.crc_ok, sum);
                    bfp_consume(&p);
                }
            }
        }
        printf("dropped %u\n", p.dropped);
        free(buf);
        return 0;
    }
}
