/* btaudio.c — ver btaudio.h. */
#define _DEFAULT_SOURCE
#include "btaudio.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RING_PATH "/run/bluealsa-ring"
#define RING_N 2048
#define RING_MAGIC 0x31524642u   /* 'BFR1' */
#define VOICE_N 32768
#define VOICE_MAGIC 0x31564642u  /* 'BFV1' */

struct bf_ring {                 /* igual ao patch do bluealsa-aplay */
    uint32_t magic, rate;
    volatile uint32_t w, hold;
    int16_t s[RING_N];
};

struct bf_voice {                /* igual ao patch 0002 do bluealsa-aplay */
    uint32_t magic, rate;
    volatile uint32_t w, r, beat;
    uint32_t pad[3];
    int16_t s[VOICE_N];
};

static struct bf_ring *ring;
static struct bf_voice *vring;
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

/* /run, ou outra pasta nos testes */
static const char *run_dir(void)
{
    const char *d = getenv("BIGFOSTON_RUN");
    return d && *d ? d : "/run";
}

static bool voice_map(void)
{
    if(vring) return true;
    char path[512];
    snprintf(path, sizeof path, "%s/bluealsa-voice", run_dir());
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if(fd < 0) return false;
    struct stat st;
    void *m = fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof *vring
              ? mmap(NULL, sizeof *vring, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if(m == MAP_FAILED) return false;
    if(((struct bf_voice *)m)->magic != VOICE_MAGIC) { munmap(m, sizeof *vring); return false; }
    vring = m;
    return true;
}

uint32_t btaudio_say(const int16_t *pcm, uint32_t n, uint32_t rate, bool (*stopped)(void))
{
    if(!n || !voice_map()) return 0;
    /* a música está saindo agora? o bluealsa-aplay conta cada bloco que toca */
    uint32_t beat = vring->beat;
    long t0 = mono_ms();
    while(vring->beat == beat) {
        if(mono_ms() - t0 > 300) return 0;
        usleep(10000);
    }
    vring->w = vring->r;                        /* sobra de uma fala interrompida: fora */
    vring->rate = rate;
    __sync_synchronize();
    uint32_t i = 0, last_r = vring->r;
    long moved = mono_ms();
    for(;;) {
        uint32_t r = vring->r, w = vring->w;
        long now = mono_ms();
        if(stopped && stopped()) { vring->w = vring->r; return n; }
        if(r != last_r) { last_r = r; moved = now; }
        else if(w != r && now - moved > 600) {  /* a música parou: o aplay não toca mais */
            uint32_t played = i - (w - r);
            vring->w = vring->r;
            return played;
        }
        if(i == n && w == r) return n;
        uint32_t space = VOICE_N - (w - r), k = n - i < space ? n - i : space;
        if(k >= 1024 || (k && k == n - i)) {
            for(uint32_t j = 0; j < k; j++) vring->s[(w + j) & (VOICE_N - 1)] = pcm[i + j];
            __sync_synchronize();
            vring->w = w + k;
            i += k;
            continue;
        }
        usleep(10000);
    }
}
