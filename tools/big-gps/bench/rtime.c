/* rtime.c - run a command on the device and report wall time, CPU time and peak RSS (wait4).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct timespec a, b;
    struct rusage ru;
    int st;
    if (argc < 2) return 2;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pid_t p = fork();
    if (p == 0) { execvp(argv[1], argv + 1); perror(argv[1]); _exit(127); }
    wait4(p, &st, 0, &ru);
    clock_gettime(CLOCK_MONOTONIC, &b);
    fprintf(stderr, "rtime: rc=%d real=%.2fs user=%.2fs sys=%.2fs maxrss=%ldkB\n",
            WIFEXITED(st) ? WEXITSTATUS(st) : -1, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9,
            ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, ru.ru_maxrss);
    return 0;
}
