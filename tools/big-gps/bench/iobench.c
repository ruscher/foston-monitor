/* iobench.c - SD card read speed on the device: sequential and random 4 KiB reads.
 *   iobench FILE [random_reads]   (drop the page cache first: echo 3 > /proc/sys/vm/drop_caches)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    static char buf[1 << 16];
    if (argc < 2) return 2;
    int n = argc > 2 ? atoi(argv[2]) : 500;
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(argv[1]); return 1; }
    unsigned seed = 12345;
    double t = now();
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        off_t off = ((off_t)(seed >> 4) % (st.st_size / 4096)) * 4096;
        if (pread(fd, buf, 4096, off) != 4096) { perror("pread"); return 1; }
    }
    t = now() - t;
    printf("aleatória 4 KiB: %d leituras, %.2f ms/leitura\n", n, t / n * 1e3);
    long long tot = 0;
    ssize_t r;
    lseek(fd, 0, SEEK_SET);
    t = now();
    while ((r = read(fd, buf, sizeof buf)) > 0) tot += r;
    t = now() - t;
    printf("sequencial: %lld bytes, %.2f MB/s\n", tot, tot / t / 1e6);
    return 0;
}
