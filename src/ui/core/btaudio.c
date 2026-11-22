/* btaudio.c — ver btaudio.h. */
#define _DEFAULT_SOURCE
#include "btaudio.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RING_PATH "/run/bluealsa-ring"
#define RING_N 2048
#define RING_MAGIC 0x31524642u   /* 'BFR1' */

struct bf_ring {                 /* igual ao patch do bluealsa-aplay */
    uint32_t magic, rate;
    volatile uint32_t w, hold;
    int16_t s[RING_N];
};

static struct bf_ring *ring;
static long next_try;

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static bool ring_map(void)
{
    if(ring) return true;
    long now = mono_ms();
    if(now < next_try) return false;
    next_try = now + 2000;                      /* sem o arquivo: tenta de novo em 2 s */
    int fd = open(RING_PATH, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;
    void *m = mmap(NULL, sizeof *ring, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if(m == MAP_FAILED) return false;
    if(((struct bf_ring *)m)->magic != RING_MAGIC) { munmap(m, sizeof *ring); return false; }
    ring = m;
    return true;
}

bool btaudio_live(void)
{
    static uint32_t last_w;
    static long last_move;
    if(!ring_map()) return false;
    long now = mono_ms();
    uint32_t w = ring->w;
    if(w != last_w) { last_w = w; last_move = now; }
    return last_move && now - last_move < 400;
}

bool btaudio_samples(int16_t *out, int n)
{
    if(!ring_map() || n > RING_N) return false;
    uint32_t w = ring->w;
    for(int i = 0; i < n; i++) out[i] = ring->s[(w - (uint32_t)n + (uint32_t)i) & (RING_N - 1)];
    return true;
}

void btaudio_hold(bool on)
{
    FILE *f = fopen("/run/bluealsa-aplay.pid", "r");
    if(!f) return;
    int pid = 0;
    if(fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if(pid > 1) kill((pid_t)pid, on ? SIGUSR1 : SIGUSR2);
}
