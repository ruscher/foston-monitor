/* bfsnoop — grava o tráfego HCI do Bluetooth em formato btsnoop (o btmon do PC lê:
 * btmon -r arquivo), pelo canal de monitoração do kernel. Para diagnóstico.
 *
 *   bfsnoop ARQUIVO [SEGUNDOS]     (padrão: até Ctrl+C / SIGTERM)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_HCI 1
#define HCI_CHANNEL_MONITOR 2
#define HCI_DEV_NONE 0xffff

struct sockaddr_hci { unsigned short family, dev, channel; };
struct mon_hdr { uint16_t opcode, index, len; } __attribute__((packed));

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "uso: bfsnoop ARQUIVO [SEGUNDOS]\n"); return 2; }
    int secs = argc > 2 ? atoi(argv[2]) : 0;
    int s = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if(s < 0) { perror("socket"); return 1; }
    struct sockaddr_hci a = { AF_BT, HCI_DEV_NONE, HCI_CHANNEL_MONITOR };
    if(bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind (monitor)"); return 1; }
    FILE *f = fopen(argv[1], "wb");
    if(!f) { perror(argv[1]); return 1; }
    /* cabeçalho btsnoop versão 1, tipo 2001 (monitor do Linux: o btmon entende) */
    static const uint8_t hdr[16] = { 'b', 't', 's', 'n', 'o', 'o', 'p', 0, 0, 0, 0, 1, 0, 0, 0x07, 0xd1 };
    fwrite(hdr, 1, sizeof hdr, f);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    time_t end = secs ? time(NULL) + secs : 0;
    uint8_t buf[2048];
    unsigned long n = 0;
    while(!quit && (!end || time(NULL) < end)) {
        struct pollfd p = { s, POLLIN, 0 };
        if(poll(&p, 1, 500) <= 0) continue;
        ssize_t r = recv(s, buf, sizeof buf, 0);
        if(r < (ssize_t)sizeof(struct mon_hdr)) continue;
        struct mon_hdr *m = (struct mon_hdr *)buf;
        uint32_t len = (uint32_t)r - sizeof *m;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        /* microssegundos desde 0000-01-01 (época do btsnoop) */
        uint64_t ts = ((uint64_t)tv.tv_sec + 62168256000ULL) * 1000000ULL + (uint64_t)tv.tv_usec;
        uint8_t rec[24];
        be32(rec, len);
        be32(rec + 4, len);
        be32(rec + 8, ((uint32_t)m->index << 16) | m->opcode);   /* flags: índice e opcode do monitor */
        be32(rec + 12, 0);
        be32(rec + 16, (uint32_t)(ts >> 32));
        be32(rec + 20, (uint32_t)ts);
        fwrite(rec, 1, sizeof rec, f);
        fwrite(buf + sizeof *m, 1, len, f);
        n++;
    }
    fclose(f);
    fprintf(stderr, "bfsnoop: %lu pacotes em %s\n", n, argv[1]);
    return 0;
}
