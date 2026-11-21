/* bgrecv.c - receive one file over a plain TCP connection on the USB network (the
 * device's CPU makes SSH encryption the bottleneck, ~1 MB/s).  Used by
 * tools/big-gps/install-sd.py: copied to /tmp over SSH and run for one transfer.
 *
 *   bgrecv ADDR PORT SIZE DEST
 *
 * Listens on ADDR:PORT (the USB link address), accepts a single connection (60 s
 * timeout), writes exactly SIZE bytes to DEST, fsyncs it and prints "sha256 <hex>".
 * Exits 1 on any error (the caller then removes DEST).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "sha256.h"

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "uso: bgrecv ENDEREÇO PORTA TAMANHO DESTINO\n"); return 2; }
    unsigned long long size = strtoull(argv[3], NULL, 10);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
    if (inet_pton(AF_INET, argv[1], &a.sin_addr) != 1) { fprintf(stderr, "endereço inválido\n"); return 2; }
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("listen"); return 1; }
    printf("pronto\n");
    fflush(stdout);
    struct timeval tv = { 60, 0 };
    setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int s = accept(ls, NULL, NULL);
    close(ls);                                      /* one connection only */
    if (s < 0) { perror("accept"); return 1; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int fd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { perror(argv[4]); return 1; }
    static unsigned char buf[1 << 16];
    sha256_ctx c;
    sha256_init(&c);
    unsigned long long got = 0;
    while (got < size) {
        size_t want = size - got < sizeof buf ? (size_t)(size - got) : sizeof buf;
        ssize_t r = read(s, buf, want);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { fprintf(stderr, "conexão encerrada após %llu de %llu bytes\n", got, size); return 1; }
        sha256_update(&c, buf, (size_t)r);
        for (ssize_t o = 0; o < r;) {
            ssize_t w = write(fd, buf + o, (size_t)(r - o));
            if (w <= 0) { perror("write"); return 1; }
            o += w;
        }
        got += (unsigned long long)r;
    }
    if (fsync(fd) || close(fd)) { perror("fsync"); return 1; }
    close(s);
    uint8_t d[32];
    char hex[65];
    sha256_final(&c, d);
    sha256_hex(d, hex);
    printf("sha256 %s\n", hex);
    return 0;
}
