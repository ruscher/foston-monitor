/* fpbench.c - cost of double / float / fixed-point math on the device (soft-float ARMv6).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define N 2000000
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void)
{
    volatile double vd = 1.0000001; volatile float vf = 1.0001f; volatile int32_t vi = 3;
    double t, d = 0.5; float f = 0.5f; int64_t acc = 0; int32_t x = 12345;
    t = now(); for (int i = 0; i < N; i++) d = d * vd + 0.25; t = now() - t;
    printf("double mul+add: %.1f ns/op (%g)\n", t / N * 1e9, d);
    t = now(); for (int i = 0; i < N; i++) f = f * vf + 0.25f; t = now() - t;
    printf("float  mul+add: %.1f ns/op (%g)\n", t / N * 1e9, f);
    t = now(); for (int i = 0; i < N; i++) { x = (int32_t)(((int64_t)x * vi) >> 1) + 7; acc += x; } t = now() - t;
    printf("int32 fixed mul+shift+add: %.1f ns/op (%lld)\n", t / N * 1e9, (long long)acc);
    t = now(); for (int i = 0; i < N / 10; i++) d += sqrt(vd + i); t = now() - t;
    printf("double sqrt: %.1f ns/op\n", t / (N / 10) * 1e9);
    t = now(); for (int i = 0; i < N / 20; i++) d += sin(vd * i) ; t = now() - t;
    printf("double sin: %.1f ns/op (%g)\n", t / (N / 20) * 1e9, d);
    return 0;
}
