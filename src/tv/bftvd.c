/* bftvd — TV digital 1seg do FOSTON: sintoniza, demultiplexa, decodifica e mostra.
 *
 *   bftvd [-f FW.inp] [-r X,Y,ESCALA] [-s STATE] MHZ
 *
 * Threads: o sintonizador (siano.c) enche um anel de TS; o demux do ffmpeg lê dele; o vídeo
 * H.264 (320x180, 15 qps) é decodificado sem o filtro de desblocagem (~50 % de CPU a menos de
 * 70 %) e convertido para RGB565 ampliado 2x direto no framebuffer, na hora do relógio do áudio;
 * o áudio HE-AAC (LOAS/LATM) vai pela fdk-aac (ponto fixo: 14 % de CPU em vez de 59 %),
 * reamostrado de 48 para 32 kHz mono, no /dev/dsp. Estado em STATE (padrão /run/bigfoston-tv).
 * Termina com SIGTERM/SIGINT (desliga o sintonizador).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <fdk-aac/aacdecoder_lib.h>
#include "siano.h"

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ------------------------------------------------------------ anel de TS */

#define RING (1 << 20)
static uint8_t ring[RING];
static size_t r_head, r_tail;                  /* escrita / leitura (contadores) */
static pthread_mutex_t r_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_cv = PTHREAD_COND_INITIALIZER;
static unsigned long ts_bytes, ts_drops;

static void ring_put(const uint8_t *d, int n)
{
    pthread_mutex_lock(&r_mx);
    if(r_head + (size_t)n - r_tail > RING) { ts_drops++; pthread_mutex_unlock(&r_mx); return; }
    for(int i = 0; i < n; i++) ring[(r_head + (size_t)i) % RING] = d[i];
    r_head += (size_t)n;
    ts_bytes += (unsigned long)n;
    pthread_cond_signal(&r_cv);
    pthread_mutex_unlock(&r_mx);
}

static int ring_read(void *op, uint8_t *buf, int size)
{
    (void)op;
    pthread_mutex_lock(&r_mx);
    while(r_head == r_tail && !quit) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 1;
        pthread_cond_timedwait(&r_cv, &r_mx, &t);
    }
    if(quit) { pthread_mutex_unlock(&r_mx); return AVERROR_EOF; }
    int n = (int)(r_head - r_tail);
    if(n > size) n = size;
    for(int i = 0; i < n; i++) buf[i] = ring[(r_tail + (size_t)i) % RING];
    r_tail += (size_t)n;
    pthread_mutex_unlock(&r_mx);
    return n;
}

/* ------------------------------------------------------------ sintonizador */

static const char *fw_path = "/mnt/sd/BigFoston/tv/isdbt_nova_12mhz_b0.inp";
static double freq_mhz;
static const char *state_path = "/run/bigfoston-tv";
static double fps_shown, ts_kbps;
static unsigned long frames_shown, frames_late;

static void on_data(const hdr_t *h, const uint8_t *d, int n)
{
    if(h->type == 607 || h->type == 729 || h->type == 693) ring_put(d, n);
    else store(h, d, n);
}

static void write_state(const char *st)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    fprintf(f, "state=%s\nfreq=%.3f\n", st, freq_mhz);
    if(last_stats.valid)
        fprintf(f, "locked=%d\nsnr=%d\nrssi=%d\n", last_stats.demod_locked, last_stats.snr, last_stats.rssi);
    fprintf(f, "ts_kbps=%.0f\ndrops=%lu\nfps=%.1f\nlate=%lu\n", ts_kbps, ts_drops, fps_shown, frames_late);
    fclose(f);
    rename(tmp, state_path);
}

static void *tuner_thread(void *a)
{
    (void)a;
    on_msg = on_data;
    double next_stats = mono() + 1;
    unsigned long last_bytes = 0;
    while(!quit) {
        unsigned long before = ts_bytes;
        poll_read();
        if(ts_bytes == before) msleep(3);           /* nada chegando: não gira à toa */
        if(mono() >= next_stats) {
            on_msg = store;
            stats();
            on_msg = on_data;
            unsigned long b = ts_bytes;                  /* taxa do TS no intervalo */
            ts_kbps = (b - last_bytes) * 8 / 1000.0 / 2;
            last_bytes = b;
            write_state(last_stats.valid && last_stats.demod_locked ? "tocando" : "sem_sinal");
            next_stats = mono() + 2;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ vídeo: YUV420 -> RGB565 2x */

static uint16_t *fb;
static int fb_stride = 800, out_x = 80, out_y = 60, scale2 = 1;
static int16_t t_rv[256], t_gu[256], t_gv[256], t_bu[256];
static uint8_t clip[1024];

static void tables(void)
{
    for(int i = 0; i < 256; i++) {
        t_rv[i] = (int16_t)((1.402 * (i - 128)));
        t_gu[i] = (int16_t)((0.344 * (i - 128)));
        t_gv[i] = (int16_t)((0.714 * (i - 128)));
        t_bu[i] = (int16_t)((1.772 * (i - 128)));
    }
    for(int i = 0; i < 1024; i++) { int v = i - 384; clip[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
}

static inline uint16_t px(int y, int rv, int g, int bu)
{
    y = (y - 16) * 298 >> 8;                                  /* faixa de vídeo 16..235 */
    int r = clip[y + rv + 384], gg = clip[y - g + 384], b = clip[y + bu + 384];
    return (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | b >> 3);
}

static void blit(const AVFrame *f)
{
    int w = f->width, h = f->height;
    for(int y = 0; y < h; y += 2) {
        const uint8_t *y0 = f->data[0] + y * f->linesize[0], *y1 = y0 + f->linesize[0];
        const uint8_t *u = f->data[1] + (y / 2) * f->linesize[1], *v = f->data[2] + (y / 2) * f->linesize[2];
        uint16_t *d0 = fb + (out_y + y * 2) * fb_stride + out_x;
        uint16_t *d1 = d0 + fb_stride, *d2 = d1 + fb_stride, *d3 = d2 + fb_stride;
        for(int x = 0; x < w; x += 2) {
            int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
            uint16_t a = px(y0[x], rv, g, bu), b = px(y0[x + 1], rv, g, bu);
            uint16_t c = px(y1[x], rv, g, bu), d = px(y1[x + 1], rv, g, bu);
            int o = x * 2;
            d0[o] = d0[o + 1] = d1[o] = d1[o + 1] = a;
            d0[o + 2] = d0[o + 3] = d1[o + 2] = d1[o + 3] = b;
            d2[o] = d2[o + 1] = d3[o] = d3[o + 1] = c;
            d2[o + 2] = d2[o + 3] = d3[o + 2] = d3[o + 3] = d;
        }
    }
}

static int fb_open(void)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if(fd < 0) return -1;
    struct fb_fix_screeninfo fx;
    if(ioctl(fd, FBIOGET_FSCREENINFO, &fx) < 0) { close(fd); return -1; }
    size_t off = fx.smem_start & 0xfff;                       /* simplefb não alinhado (docs) */
    uint8_t *m = mmap(NULL, fx.smem_len + off, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(m == MAP_FAILED) return -1;
    fb = (uint16_t *)(m + off);
    fb_stride = (int)fx.line_length / 2;
    return 0;
}

/* ------------------------------------------------------------ áudio */

static int dsp = -1;
static double audio_pts = -1;                                 /* fim do que foi escrito (s) */

static int dsp_open(void)
{
    for(int i = 0; i < 20 && dsp < 0; i++) {
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

static double audio_clock(void)
{
    if(dsp < 0 || audio_pts < 0) return -1;
    int bytes = 0;
    ioctl(dsp, SNDCTL_DSP_GETODELAY, &bytes);
    return audio_pts - bytes / 64000.0;                       /* 32 kHz mono 16 bits */
}

/* 48 kHz -> 32 kHz (3 -> 2, média linear), mistura para mono */
static void audio_out(const INT_PCM *pcm, int frames, int chans, double pts)
{
    static int16_t out[4096];
    int n = 0;
    for(int i = 0; i + 2 < frames && n + 2 < (int)(sizeof out / 2); i += 3) {
        int a = pcm[i * chans], b = pcm[(i + 1) * chans], c = pcm[(i + 2) * chans];
        if(chans == 2) { a = (a + pcm[i * 2 + 1]) / 2; b = (b + pcm[(i + 1) * 2 + 1]) / 2; c = (c + pcm[(i + 2) * 2 + 1]) / 2; }
        out[n++] = (int16_t)((2 * a + b) / 3);
        out[n++] = (int16_t)((b + 2 * c) / 3);
    }
    if(dsp < 0) return;
    const uint8_t *p = (const uint8_t *)out;
    size_t left = (size_t)n * 2;
    while(left && !quit) {
        ssize_t w = write(dsp, p, left);
        if(w < 0) { if(errno == EINTR) continue; break; }
        p += w;
        left -= (size_t)w;
    }
    audio_pts = pts + frames / 48000.0;
}

/* ------------------------------------------------------------ fila de quadros (mostrar na hora) */

#define QN 6
static AVFrame *q[QN];
static double q_pts[QN];
static int q_r, q_w;
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;

static void *show_thread(void *a)
{
    (void)a;
    double t0 = mono();
    unsigned long n0 = 0;
    while(!quit) {
        pthread_mutex_lock(&q_mx);
        if(q_r == q_w) { pthread_mutex_unlock(&q_mx); usleep(5000); continue; }
        AVFrame *f = q[q_r % QN];
        double pts = q_pts[q_r % QN];
        pthread_mutex_unlock(&q_mx);
        double ac = audio_clock();
        if(ac >= 0 && pts > ac + 0.02 && pts < ac + 2) { usleep(5000); continue; }   /* cedo: espera */
        if(ac >= 0 && pts < ac - 0.25) frames_late++;          /* atrasado: descarta */
        else { blit(f); frames_shown++; }
        pthread_mutex_lock(&q_mx);
        av_frame_unref(f);
        q_r++;
        pthread_mutex_unlock(&q_mx);
        double t = mono();
        if(t - t0 >= 2) { fps_shown = (frames_shown - n0) / (t - t0); n0 = frames_shown; t0 = t; }
    }
    return NULL;
}

/* ------------------------------------------------------------ principal */

int main(int argc, char **argv)
{
    int opt;
    while((opt = getopt(argc, argv, "f:r:s:")) != -1) {
        if(opt == 'f') fw_path = optarg;
        else if(opt == 's') state_path = optarg;
        else if(opt == 'r') sscanf(optarg, "%d,%d,%d", &out_x, &out_y, &scale2);
    }
    if(optind >= argc) { fprintf(stderr, "uso: bftvd [-f FW] [-r X,Y,2] [-s STATE] MHZ\n"); return 2; }
    freq_mhz = atof(argv[optind]);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
    if(map()) return 1;
    tables();
    if(fb_open()) { fprintf(stderr, "sem /dev/fb0\n"); return 1; }

    /* sintonizador: liga e carrega o firmware se ainda não respondeu */
    clk_div = 7;
    spi_setup();
    on_msg = store;
    if(send_msg(668, 150, NULL, 0) || wait_msg(669, 500)) {
        write_state("ligando");
        power_on();
        clk_div = 15;
        spi_setup();
        if(fw_download(fw_path)) { write_state("erro_firmware"); return 1; }
    }
    write_state("sintonizando");
    if(set_mode_tune(freq_mhz, 1)) { write_state("erro_sintonia"); return 1; }
    pthread_t tt, st;
    pthread_create(&tt, NULL, tuner_thread, NULL);

    /* demux */
    AVFormatContext *fc = avformat_alloc_context();
    uint8_t *iob = av_malloc(32768);
    fc->pb = avio_alloc_context(iob, 32768, 0, NULL, ring_read, NULL, NULL);
    fc->probesize = 400000;
    fc->max_analyze_duration = 2 * AV_TIME_BASE;
    const AVInputFormat *ts = av_find_input_format("mpegts");
    if(avformat_open_input(&fc, NULL, ts, NULL) < 0) { write_state("erro_fluxo"); quit = 1; }
    if(!quit) avformat_find_stream_info(fc, NULL);
    int vi = quit ? -1 : av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = quit ? -1 : av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    AVCodecContext *vc = NULL;
    if(vi >= 0) {
        const AVCodec *c = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
        vc = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vc, fc->streams[vi]->codecpar);
        vc->skip_loop_filter = AVDISCARD_ALL;              /* -30 % de CPU, perda pequena em 320x180 */
        vc->flags2 |= AV_CODEC_FLAG2_FAST;
        if(avcodec_open2(vc, c, NULL) < 0) vi = -1;
    }
    HANDLE_AACDECODER aac = ai >= 0 ? aacDecoder_Open(TT_MP4_LOAS, 1) : NULL;
    if(aac) dsp_open();
    for(int i = 0; i < QN; i++) q[i] = av_frame_alloc();
    pthread_create(&st, NULL, show_thread, NULL);
    write_state(vi >= 0 ? "tocando" : "sem_video");

    AVPacket *pk = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    static INT_PCM pcm[8192];
    while(!quit && av_read_frame(fc, pk) >= 0) {
        AVStream *s = fc->streams[pk->stream_index];
        double pts = pk->pts == AV_NOPTS_VALUE ? -1 : pk->pts * av_q2d(s->time_base);
        if(pk->stream_index == ai && aac) {
            UCHAR *b = pk->data;
            UINT sz = (UINT)pk->size, valid = sz;
            int first = 1;
            while(valid > 0 && !quit) {
                if(aacDecoder_Fill(aac, &b, &sz, &valid) != AAC_DEC_OK) break;
                b = pk->data + (pk->size - valid);
                sz = valid;
                while(aacDecoder_DecodeFrame(aac, pcm, 8192, 0) == AAC_DEC_OK) {
                    CStreamInfo *si = aacDecoder_GetStreamInfo(aac);
                    double fp = first && pts >= 0 ? pts : audio_pts;
                    first = 0;
                    audio_out(pcm, si->frameSize, si->numChannels, fp);
                }
            }
        } else if(pk->stream_index == vi && vc) {
            avcodec_send_packet(vc, pk);
            while(avcodec_receive_frame(vc, fr) == 0) {
                double fp = fr->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : fr->best_effort_timestamp * av_q2d(s->time_base);
                for(;;) {                                  /* fila cheia: espera o mostrador */
                    pthread_mutex_lock(&q_mx);
                    int full = q_w - q_r >= QN;
                    if(!full) {
                        av_frame_move_ref(q[q_w % QN], fr);
                        q_pts[q_w % QN] = fp;
                        q_w++;
                    }
                    pthread_mutex_unlock(&q_mx);
                    if(!full || quit) break;
                    usleep(5000);
                }
            }
        }
        av_packet_unref(pk);
    }
    quit = 1;
    pthread_join(st, NULL);
    pthread_join(tt, NULL);
    if(dsp >= 0) close(dsp);
    write_state("parado");
    return 0;
}
