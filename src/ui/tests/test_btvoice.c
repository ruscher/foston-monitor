/* test_btvoice.c - the Big GPS voice mixed into the phone's music: core/btaudio.c (the shell
 * side) against bf_voice_mix taken from the bluealsa-aplay patch itself
 * (firmware/buildroot/patches/bluez-alsa/0002-aplay-bigfoston-voice-mix.patch, extracted by
 * the Makefile into bfvoice_mix.inc).  A thread plays the part of bluealsa-aplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../core/btaudio.h"
#include "bfvoice_mix.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MUSIC 8000                       /* música constante, para conferir o ganho */
#define VOICE 10000
#define MAXF (44100 * 6)

static struct bf_voice *V;
static int rate = 44100;
static volatile int run, stop_after = -1; /* blocos até a "música parar" */
static int16_t out[MAXF];                /* canal esquerdo do que saiu */
static int nout;

static void *aplay(void *arg)
{
    (void)arg;
    int16_t buf[2 * 1024];
    int blocks = 0;
    while (run) {
        if (stop_after >= 0 && blocks >= stop_after) { usleep(5000); continue; }
        for (int i = 0; i < 2048; i++) buf[i] = MUSIC;
        bf_voice_mix(V, buf, 2048, 2, (unsigned)rate);
        for (int f = 0; f < 1024 && nout < MAXF; f++) {
            CHECK(buf[2 * f] == buf[2 * f + 1], "canais diferentes");
            out[nout++] = buf[2 * f];
        }
        blocks++;
        usleep(4000);                    /* mais rápido que o tempo real: o teste não demora */
    }
    return NULL;
}

static pthread_t th;
static void start(int r, int after)
{
    rate = r;
    stop_after = after;
    nout = 0;
    run = 1;
    pthread_create(&th, NULL, aplay, NULL);
}
static void finish(void)
{
    run = 0;
    pthread_join(th, NULL);
}

static int count_near(int v, int tol)
{
    int k = 0;
    for (int i = 0; i < nout; i++) k += abs(out[i] - v) <= tol;
    return k;
}

static volatile int want_stop;
static bool stopped(void) { return want_stop; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    setenv("BIGFOSTON_RUN", dir, 1);
    char path[512];
    snprintf(path, sizeof path, "%s/bluealsa-voice", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, sizeof *V)) { perror(path); return 1; }
    V = mmap(NULL, sizeof *V, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    V->magic = BF_VOICE_MAGIC;

    const uint32_t vrate = 22050, n = vrate / 2;          /* meio segundo de voz */
    int16_t *pcm = malloc(n * sizeof *pcm);
    for (uint32_t i = 0; i < n; i++) pcm[i] = VOICE;

    /* 1. música tocando a 44,1 kHz: a voz entra inteira, a música abaixa e volta */
    for (int pass = 0; pass < 2; pass++) {
        int r = pass ? 48000 : 44100;
        start(r, -1);
        usleep(50000);
        uint32_t got = btaudio_say(pcm, n, vrate, stopped);
        usleep(1200000);                 /* espera o fim da rampa de volta */
        finish();
        CHECK(got == n, "%d Hz: tocou %u de %u", r, got, n);
        int ducked = MUSIC * BF_DUCK / 256 + VOICE;
        int mixed = count_near(ducked, 2), expect = r / 2;
        CHECK(abs(mixed - expect) < r / 10, "%d Hz: %d quadros com a voz e a música baixa (esperado ~%d)", r, mixed, expect);
        CHECK(out[nout - 1] == MUSIC, "%d Hz: a música não voltou ao volume (%d)", r, out[nout - 1]);
        int over = 0;
        for (int i = 0; i < nout; i++) over |= out[i] > ducked + 2 || out[i] < MUSIC * BF_DUCK / 256 - 2;
        CHECK(!over, "%d Hz: amostra fora da faixa esperada", r);
        /* nenhum salto maior que um passo da rampa fora da entrada da voz */
        int jumps = 0;
        for (int i = 1; i < nout; i++) jumps += abs(out[i] - out[i - 1]) > MUSIC / 256 + 1;
        CHECK(jumps <= 2, "%d Hz: %d saltos (estalos) na saída", r, jumps);
        printf("  %d Hz: %d quadros com voz, %d saltos\n", r, mixed, jumps);
    }

    /* 2. sem música saindo (o aplay parado): devolve 0 logo, para a voz ir pelo /dev/dsp */
    start(44100, 0);
    uint32_t got = btaudio_say(pcm, n, vrate, stopped);
    finish();
    CHECK(got == 0, "sem música: tocou %u (esperado 0)", got);

    /* 3. a música para no meio da fala: devolve o que tocou, e o resto fica para o /dev/dsp */
    int16_t *longv = malloc(vrate * 3 * sizeof *longv);
    for (uint32_t i = 0; i < vrate * 3; i++) longv[i] = VOICE;
    start(44100, 40);
    got = btaudio_say(longv, vrate * 3, vrate, stopped);
    finish();
    CHECK(got > 0 && got < vrate * 3, "música parou no meio: tocou %u de %u", got, vrate * 3);
    CHECK(V->w == V->r, "sobrou voz na fila (%u)", V->w - V->r);
    printf("  música parou: %u de %u amostras tocadas\n", got, vrate * 3);

    /* 4. a fala é interrompida (voice_stop): para logo e a fila esvazia */
    start(44100, -1);
    want_stop = 1;
    got = btaudio_say(longv, vrate * 3, vrate, stopped);
    usleep(50000);
    finish();
    CHECK(got == vrate * 3, "interrompida: %u", got);
    CHECK(V->w == V->r, "interrompida: sobrou voz na fila");
    free(pcm);
    free(longv);
    remove(path);
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
