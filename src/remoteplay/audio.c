/* audio.c — som do Big Remote Play: o Opus do Sunshine decodificado com a libopus em ponto fixo
 * (o ARM11 não tem FPU; firmware/build/opus-arm) e tocado no alto-falante pelo /dev/dsp no mesmo
 * formato da TV (src/tv/bftvd.c): mono, 32 kHz, com a mesma reamostragem 48 -> 32 kHz (3 -> 2).
 *
 * Buffer de ~190 ms (6 x 2 KB): o som chega por pacotes de 5-20 ms e a thread de áudio da
 * moonlight-common-c espera o /dev/dsp. Pacote perdido: o próprio Opus tapa (PLC). Se o
 * alto-falante estiver ocupado (música do celular, TV), a sessão segue sem som.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <Limelight.h>

#include "audio.h"

static OpusMSDecoder *dec;
static OpusDecoder *dec24;                           /* estéreo simples: mono a 24 kHz (mais barato) */
static int dsp = -1, chans, frame_samples;
static int16_t pcm[2 * 5760];                        /* até 120 ms estéreo a 48 kHz */
static int16_t outbuf[5760];
static atomic_ulong n_pkts, n_lost, n_underruns;
static atomic_int muted;

void audio_mute(int on) { atomic_store(&muted, on); }

static int dsp_open(void)
{
    for(int i = 0; i < 10 && dsp < 0; i++) {
        dsp = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
        if(dsp < 0 && errno == EBUSY) usleep(50000);
        else break;
    }
    if(dsp < 0) return -1;
    int frag = (6 << 16) | 11, fmt = AFMT_S16_LE, ch = 1, sp = 32000;
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    if(ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) || ioctl(dsp, SNDCTL_DSP_SPEED, &sp)) {
        close(dsp);
        dsp = -1;
        return -1;
    }
    return 0;
}

int audio_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc)
{
    (void)cfg;
    int err = 0;
    chans = oc->channelCount;
    frame_samples = oc->samplesPerFrame;
    if(oc->streams == 1) {
        /* o caso do Sunshine (estéreo, um fluxo): o próprio Opus mistura em mono e decodifica a
         * 24 kHz, bem mais barato no ARM11 que estéreo a 48 kHz (banda de 12 kHz) */
        dec24 = opus_decoder_create(24000, 1, &err);
        if(!dec24 || err != OPUS_OK) dec24 = NULL;
        else { chans = 1; frame_samples = oc->samplesPerFrame / 2; }
    }
    if(!dec24) {
        dec = opus_multistream_decoder_create(oc->sampleRate, oc->channelCount, oc->streams, oc->coupledStreams,
                                              oc->mapping, &err);
        if(!dec || err != OPUS_OK) { dec = NULL; return -1; }
    }
    if(dsp_open()) return 1;                           /* sem alto-falante: segue sem som */
    return 0;
}

static void write_out(int n);

/* 24 kHz mono -> 32 kHz (3 -> 4, interpolação linear): as saídas caem nas posições 0, 3/4,
 * 3/2 e 9/4 de cada trio de entrada (a, b, c; d é o primeiro do trio seguinte) */
static void play24(const int16_t *in, int frames)
{
    int n = 0;
    for(int i = 0; i + 2 < frames && n + 4 <= (int)(sizeof outbuf / 2); i += 3) {
        int a = in[i], b = in[i + 1], c = in[i + 2], d = i + 3 < frames ? in[i + 3] : c;
        outbuf[n++] = (int16_t)a;
        outbuf[n++] = (int16_t)((a + 3 * b) / 4);
        outbuf[n++] = (int16_t)((b + c) / 2);
        outbuf[n++] = (int16_t)((3 * c + d) / 4);
    }
    write_out(n);
}

/* 48 kHz com chans canais -> 32 kHz mono (3 -> 2, média linear), como o bftvd */
static void play(const int16_t *in, int frames)
{
    int n = 0;
    for(int i = 0; i + 2 < frames && n + 2 < (int)(sizeof outbuf / 2); i += 3) {
        int a = in[i * chans], b = in[(i + 1) * chans], c = in[(i + 2) * chans];
        if(chans >= 2) {
            a = (a + in[i * chans + 1]) / 2;
            b = (b + in[(i + 1) * chans + 1]) / 2;
            c = (c + in[(i + 2) * chans + 1]) / 2;
        }
        outbuf[n++] = (int16_t)((2 * a + b) / 3);
        outbuf[n++] = (int16_t)((b + 2 * c) / 3);
    }
    write_out(n);
}

static void write_out(int n)
{
    if(atomic_load(&muted)) memset(outbuf, 0, (size_t)n * 2);
    int q = 0;
    if(ioctl(dsp, SNDCTL_DSP_GETODELAY, &q) == 0 && q == 0) atomic_fetch_add(&n_underruns, 1);
    const uint8_t *p = (const uint8_t *)outbuf;
    size_t left = (size_t)n * 2;
    while(left) {
        ssize_t w = write(dsp, p, left);
        if(w < 0) { if(errno == EINTR) continue; break; }
        p += w;
        left -= (size_t)w;
    }
}

void audio_sample(char *data, int len)
{
    if(dec24) {
        atomic_fetch_add(&n_pkts, 1);
        if(!data) atomic_fetch_add(&n_lost, 1);
        int got = opus_decode(dec24, (const unsigned char *)data, data ? len : 0, pcm,
                              data ? (int)(sizeof pcm / sizeof pcm[0]) : frame_samples, 0);
        if(got > 2 && dsp >= 0) play24(pcm, got);
        return;
    }
    if(!dec) return;
    atomic_fetch_add(&n_pkts, 1);
    int max = (int)(sizeof pcm / sizeof pcm[0]) / (chans > 0 ? chans : 1);
    int got = opus_multistream_decode(dec, (const unsigned char *)data, data ? len : 0, pcm,
                                      data ? max : frame_samples, 0);
    if(!data) atomic_fetch_add(&n_lost, 1);
    if(got > 0 && dsp >= 0) play(pcm, got);
}

void audio_stats(unsigned long *pkts, unsigned long *lost, unsigned long *underruns)
{
    *pkts = atomic_exchange(&n_pkts, 0);
    *lost = atomic_exchange(&n_lost, 0);
    *underruns = atomic_exchange(&n_underruns, 0);
}

int audio_ok(void) { return dsp >= 0; }

void audio_close(void)
{
    if(dec) { opus_multistream_decoder_destroy(dec); dec = NULL; }
    if(dec24) { opus_decoder_destroy(dec24); dec24 = NULL; }
    if(dsp >= 0) { close(dsp); dsp = -1; }
}
