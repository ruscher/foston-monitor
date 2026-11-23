/* bftv — TV digital (ISDB-T 1seg): ferramenta de teste do sintonizador (siano.c).
 *
 *   bftv on | off | boot FW.inp | version | tune MHZ [SEG] | dump MHZ SEGUNDOS OUT.ts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "siano.h"

static FILE *ts_out;
static unsigned long ts_bytes, ts_msgs;

static void ts_msg(const hdr_t *h, const uint8_t *d, int n)
{
    if(h->type == 607 || h->type == 729 || h->type == 693) {
        ts_msgs++;
        if(ts_out && n > 0) ts_bytes += fwrite(d, 1, (size_t)n, ts_out);
    } else store(h, d, n);
}

int main(int argc, char **argv)
{
    if(argc < 2) {
        fprintf(stderr, "uso: bftv on|off|boot FW.inp|version|tune MHZ [SEG]|dump MHZ SEGUNDOS OUT.ts\n");
        return 2;
    }
    if(map()) return 1;
    const char *c = argv[1];
    if(!strcmp(c, "on")) { power_on(); printf("ligado\n"); return 0; }
    if(!strcmp(c, "off")) { power_off(); printf("desligado\n"); return 0; }
    if(!strcmp(c, "boot")) {
        if(argc < 3) return 2;
        power_on();
        spi_setup();
        return fw_download(argv[2]) ? 1 : 0;
    }
    clk_div = 7;
    spi_setup();
    if(!strcmp(c, "version")) return version() ? 1 : 0;
    if(!strcmp(c, "tune") && argc > 2) {
        if(set_mode_tune(atof(argv[2]), argc > 3 ? atoi(argv[3]) : 1)) return 1;
        for(int i = 0; i < 5; i++) {
            msleep(1000);
            if(stats()) { printf("sem estatísticas\n"); continue; }
            printf("RF %d, demodulador %d, SNR %d dB, RSSI %d dBm, potência %d dBm, desvio %d Hz, %u Hz\n",
                   last_stats.rf_locked, last_stats.demod_locked, last_stats.snr, last_stats.rssi,
                   last_stats.in_band_pwr, last_stats.carrier_offset, last_stats.frequency);
        }
        return 0;
    }
    if(!strcmp(c, "dump") && argc > 4) {
        if(set_mode_tune(atof(argv[2]), 1)) return 1;
        ts_out = fopen(argv[4], "wb");
        if(!ts_out) { perror(argv[4]); return 1; }
        on_msg = ts_msg;
        uint32_t pid = 0;                        /* PAT */
        send_msg(601, 150, &pid, 4);
        long end = now_ms() + atol(argv[3]) * 1000;
        while(now_ms() < end) { poll_read(); msleep(2); }
        fclose(ts_out);
        printf("%lu mensagens de dados, %lu bytes gravados\n", ts_msgs, ts_bytes);
        return 0;
    }
    fprintf(stderr, "comando desconhecido\n");
    return 2;
}
