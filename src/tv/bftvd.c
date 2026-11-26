/* bftvd — TV digital 1seg do FOSTON: sintoniza, demultiplexa, decodifica e mostra.
 *
 *   bftvd [-f FW.inp] [-r X,Y,ESCALA] [-s STATE] [-w GRAVACAO.ts] [-W PASTA] MHZ   assistir (e gravar)
 *   bftvd [-r X,Y,ESCALA] [-s STATE] -p ARQUIVO.ts                       tocar uma gravação
 *   bftvd [-f FW.inp] [-s STATE] -S CANAIS.tsv                           buscar canais (UHF 14-69)
 *   bftvd -O                                                             desligar o sintonizador
 *
 * Threads: o sintonizador (siano.c) enche um anel de TS; o demux do ffmpeg lê dele; o vídeo
 * H.264 (320x180, 15 qps) é decodificado sem o filtro de desblocagem (~50 % de CPU a menos de
 * 70 %) e convertido para RGB565 ampliado 2x direto no framebuffer, na hora do relógio do áudio;
 * o áudio HE-AAC (LOAS/LATM) vai pela fdk-aac (ponto fixo: 14 % de CPU em vez de 59 %),
 * reamostrado de 48 para 32 kHz mono, no /dev/dsp. Estado em STATE (padrão /run/bigfoston-tv).
 * Termina com SIGTERM/SIGINT (o sintonizador fica ligado para a troca de canal: -O desliga).
 * -r X,Y,L,A / -F X,Y,L,A: retângulo do vídeo e o da tela cheia; SIGUSR2 vai para a tela cheia,
 * SIGWINCH volta.
 * SIGUSR1 começa/para de gravar o TS em PASTA/tv-AAAAMMDD-HHMMSS-cNN.ts sem parar a imagem.
 * Com o PC na rede USB (bftv-relay, tvrelay.h), o PC decodifica: o bftvd manda o TS e só desenha
 * os quadros prontos na hora do relógio do som; se o PC sai, volta a decodificar aqui.
 * BFTV_RELAY=0 desliga.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include "siano.h"
#include "tvrelay.h"
#include "yuvz.h"

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
static int ladder;                                        /* degrau de decodificação (run_player) */
static unsigned long audio_underruns;
static unsigned long frames_shown, frames_late, frames_thin;
static volatile int relay_on;                  /* o PC está decodificando */

static FILE *rec;                              /* -w: o TS também vai para o cartão */
static unsigned long rec_bytes;
static const char *rec_dir;                    /* -W: pasta das gravações (SIGUSR1) */
static char rec_name[256];
static volatile sig_atomic_t rec_toggle;
static void on_usr1(int s) { (void)s; rec_toggle = 1; }
static const char *play_path;                  /* -p: toca um arquivo em vez da antena */
static double play_pos, play_dur;

/* pacotes TS perdidos (contador de continuidade): falha do sinal ou do sintonizador, não da CPU */
static unsigned long ts_lost;
static uint8_t cc_last[8192];

static void cc_check(const uint8_t *d, int n)
{
    for(int i = 0; i + 188 <= n; i += 188) {
        const uint8_t *p = d + i;
        if(p[0] != 0x47) return;
        int pid = (p[1] & 0x1f) << 8 | p[2];
        if(pid == 0x1fff || !(p[3] & 0x10)) continue;          /* nulo ou sem carga */
        int c = p[3] & 15, l = cc_last[pid];
        if(l & 0x80 && c != ((l + 1) & 15) && c != (l & 15)) ts_lost++;
        cc_last[pid] = (uint8_t)(0x80 | c);
    }
}

static void on_data(const hdr_t *h, const uint8_t *d, int n)
{
    if(h->type == 607 || h->type == 729 || h->type == 693) {
        cc_check(d, n);
        ring_put(d, n);
        if(rec && n > 0) {
            if(fwrite(d, 1, (size_t)n, rec) != (size_t)n) { fclose(rec); rec = NULL; }   /* cartão cheio */
            else rec_bytes += (unsigned long)n;
        }
    } else store(h, d, n);
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
    fprintf(f, "ts_kbps=%.0f\nlost=%lu\ndrops=%lu\nfps=%.1f\nlate=%lu\nshown=%lu\nthin=%lu\nskip=%d\nunderruns=%lu\nrelay=%d\n", ts_kbps, ts_lost, ts_drops, fps_shown, frames_late, frames_shown, frames_thin, ladder, audio_underruns, relay_on);
    if(rec) fprintf(f, "rec_kb=%lu\nrec_file=%s\n", rec_bytes / 1024, rec_name);
    if(play_path) fprintf(f, "file=%s\npos=%.0f\ndur=%.0f\n", play_path, play_pos, play_dur);
    fclose(f);
    rename(tmp, state_path);
}

static void *tuner_thread(void *a)
{
    (void)a;
    /* a FIFO do chip transborda se esta thread atrasa (TS perdido = imagem travada) */
    setpriority(PRIO_PROCESS, 0, -15);
    pthread_setname_np(pthread_self(), "tv-antena");
    on_msg = on_data;
    double next_stats = mono() + 1, t_start = mono();
    int idle_ms = getenv("BFTV_IDLE_MS") ? atoi(getenv("BFTV_IDLE_MS")) : 3;
    unsigned long last_bytes = 0;
    while(!quit) {
        if(rec_toggle) {                                /* gravar / parar (SIGUSR1) */
            rec_toggle = 0;
            if(rec) { fclose(rec); rec = NULL; sync(); }
            else if(rec_dir) {
                time_t now = time(NULL);
                struct tm tm;
                localtime_r(&now, &tm);
                char ts[32];
                strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
                int ch = (int)((freq_mhz - 473.142857) / 6 + 14.5);
                snprintf(rec_name, sizeof rec_name, "%s/tv-%s-c%02d.ts", rec_dir, ts, ch);
                rec_bytes = 0;
                rec = fopen(rec_name, "wb");
            }
            write_state(last_stats.valid && last_stats.demod_locked ? "tocando" : "sem_sinal");
        }
        unsigned long before = ts_bytes;
        poll_read();
        if(ts_bytes == before) msleep(idle_ms);     /* nada chegando: não gira à toa */
        if(mono() >= next_stats) {
            stats();                                   /* on_data passa a resposta a store: o TS não para */
            unsigned long b = ts_bytes;                  /* taxa do TS no intervalo */
            ts_kbps = (b - last_bytes) * 8 / 1000.0;   /* por segundo */
            last_bytes = b;
            static int ever_locked;
            int lk = last_stats.valid && last_stats.demod_locked;
            ever_locked |= lk;
            /* antes de travar a 1ª vez (até 6 s) ainda é sintonia, não falta de sinal */
            write_state(lk ? (frames_shown ? "tocando" : mono() - t_start < 20 ? "abrindo" : "sem_imagem")
                           : (!ever_locked && mono() - t_start < 6) ? "sintonizando" : "sem_sinal");
            next_stats = mono() + 1;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ vídeo: YUV420 -> RGB565 2x */

static uint16_t *fb;
static int fb_stride = 800, out_x = 80, out_y = 60, out_w, out_h;    /* out_w 0: 2x do quadro */
static int alt_x, alt_y, alt_w, alt_h;          /* -F: retângulo da tela cheia (SIGUSR2 alterna) */
static volatile sig_atomic_t want_full;        /* SIGUSR2: tela cheia; SIGWINCH: volta à janela */
static int is_full;
static void on_usr2(int s) { (void)s; want_full = 1; }
static void on_winch(int s) { (void)s; want_full = 0; }
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

static int fb_w = 800, fb_h = 480;
static int getenv_no64;                                   /* BFTV_NO64=1: o caminho de 32 bits (A/B) */

/* qualquer tamanho: converte cada linha de origem uma vez e estica (vizinho mais próximo) */
static void blit_scaled(const AVFrame *f, int ox, int oy, int W, int H)
{
    static uint16_t line[1024];
    static int xmap[1024];
    int w = f->width, h = f->height;
    if(w > 1024) w = 1024;
    if(ox < 0) ox = 0;
    if(oy < 0) oy = 0;
    if(W > fb_w - ox) W = fb_w - ox;
    if(H > fb_h - oy) H = fb_h - oy;
    if(W <= 0 || H <= 0 || W > 1024 || w <= 0 || h <= 0) return;
    for(int x = 0; x < W; x++) xmap[x] = x * w / W;
    int last = -1;
    for(int y = 0; y < H; y++) {
        int sy = y * h / H;
        if(sy != last) {
            const uint8_t *yy = f->data[0] + sy * f->linesize[0];
            const uint8_t *u = f->data[1] + (sy / 2) * f->linesize[1], *v = f->data[2] + (sy / 2) * f->linesize[2];
            for(int x = 0; x + 1 < w; x += 2) {
                int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
                line[x] = px(yy[x], rv, g, bu);
                line[x + 1] = px(yy[x + 1], rv, g, bu);
            }
            last = sy;
        }
        uint16_t *d = fb + (oy + y) * fb_stride + ox;
        for(int x = 0; x < W; x++) d[x] = line[xmap[x]];
    }
}

static void blit(const AVFrame *f)
{
    if(want_full != is_full && alt_w > 0) {            /* tela cheia <-> janela */
        is_full = want_full;
        int t;
        t = out_x; out_x = alt_x; alt_x = t;
        t = out_y; out_y = alt_y; alt_y = t;
        t = out_w; out_w = alt_w; alt_w = t;
        t = out_h; out_h = alt_h; alt_h = t;
    }
    if(out_w > 0 && (out_w != f->width * 2 || out_h != f->height * 2)) { blit_scaled(f, out_x, out_y, out_w, out_h); return; }
    int w = f->width, h = f->height;
    if(w > (fb_w - out_x) / 2) w = (fb_w - out_x) / 2 & ~1;  /* nunca fora do retângulo da tela */
    if(h > (fb_h - out_y) / 2) h = (fb_h - out_y) / 2 & ~1;
    if(w <= 0 || h <= 0) return;
    if(!getenv_no64 && !(((uintptr_t)(fb + out_y * fb_stride + out_x) | (uintptr_t)(fb_stride * 2)) & 7)) {
        /* alinhado em 8: quatro pontos por escrita de 64 bits (STRD), metade das escritas de 32 */
        for(int y = 0; y < h; y += 2) {
            const uint8_t *y0 = f->data[0] + y * f->linesize[0], *y1 = y0 + f->linesize[0];
            const uint8_t *u = f->data[1] + (y / 2) * f->linesize[1], *v = f->data[2] + (y / 2) * f->linesize[2];
            uint64_t *d0 = (uint64_t *)(void *)(fb + (out_y + y * 2) * fb_stride + out_x);
            uint64_t *d1 = d0 + fb_stride / 4, *d2 = d1 + fb_stride / 4, *d3 = d2 + fb_stride / 4;
            for(int x = 0; x < w; x += 2) {
                int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
                uint32_t a = px(y0[x], rv, g, bu), b = px(y0[x + 1], rv, g, bu);
                uint32_t c = px(y1[x], rv, g, bu), d = px(y1[x + 1], rv, g, bu);
                uint64_t ab = (a | a << 16) | (uint64_t)(b | b << 16) << 32;
                uint64_t cd = (c | c << 16) | (uint64_t)(d | d << 16) << 32;
                d0[x / 2] = d1[x / 2] = ab;
                d2[x / 2] = d3[x / 2] = cd;
            }
        }
        return;
    }
    if(!(((uintptr_t)(fb + out_y * fb_stride + out_x) | (uintptr_t)(fb_stride * 2)) & 3)) {
        /* alinhado: dois pontos por escrita de 32 bits (o framebuffer não tem cache; metade das
         * escritas no barramento) */
        for(int y = 0; y < h; y += 2) {
            const uint8_t *y0 = f->data[0] + y * f->linesize[0], *y1 = y0 + f->linesize[0];
            const uint8_t *u = f->data[1] + (y / 2) * f->linesize[1], *v = f->data[2] + (y / 2) * f->linesize[2];
            uint32_t *d0 = (uint32_t *)(fb + (out_y + y * 2) * fb_stride + out_x);
            uint32_t *d1 = d0 + fb_stride / 2, *d2 = d1 + fb_stride / 2, *d3 = d2 + fb_stride / 2;
            for(int x = 0; x < w; x += 2) {
                int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
                uint32_t a = px(y0[x], rv, g, bu), b = px(y0[x + 1], rv, g, bu);
                uint32_t c = px(y1[x], rv, g, bu), d = px(y1[x + 1], rv, g, bu);
                a |= a << 16; b |= b << 16; c |= c << 16; d |= d << 16;
                d0[x] = d1[x] = a;
                d0[x + 1] = d1[x + 1] = b;
                d2[x] = d3[x] = c;
                d2[x + 1] = d3[x + 1] = d;
            }
        }
        return;
    }
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
    struct fb_var_screeninfo vi;
    int fd2 = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if(fd2 >= 0) {
        if(ioctl(fd2, FBIOGET_VSCREENINFO, &vi) == 0) { fb_w = (int)vi.xres; fb_h = (int)vi.yres; }
        close(fd2);
    }
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
    /* 6 x 2 KB (~190 ms). Medido (24/11/2026, mesmo canal): 12 x 4 KB (~750 ms) mostrou 10 qps
     * contra 15,6: com o buffer grande o decodificador corre muito à frente do som, a fila de
     * quadros enche e o relógio (GETODELAY) anda em degraus. BFTV_FRAG=0xNNNNMMMM para testar. */
    int frag = (6 << 16) | 11, fmt = AFMT_S16_LE, ch = 1, sp = 32000;
    /* com o PC decodificando, o relógio vem do horário de cada bloco e o buffer maior (12 x 2 KB,
     * ~380 ms) só segura mais: um aperto de CPU (interrupções do USB) não deixa o som faltar */
    if(relay_on) frag = (12 << 16) | 11;
    if(getenv("BFTV_FRAG")) frag = (int)strtol(getenv("BFTV_FRAG"), NULL, 0);
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
    if(audio_pts >= 0) {                                       /* o buffer secou: o som falhou */
        int q = 0;
        if(ioctl(dsp, SNDCTL_DSP_GETODELAY, &q) == 0 && q == 0) audio_underruns++;
    }
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

/* 2 s de vídeo mesmo a 30 qps: em alguns canais o vídeo vem no fluxo bem à frente do áudio; com
 * fila curta o decodificador parava esperando o mostrador, que esperava o áudio que não chegava;
 * o mostrador seguia sozinho a 15 qps, o decodificador consumia o TS à metade da velocidade de um
 * canal de 30 qps e o anel do TS transbordava (drops). 64 quadros 320x180 = 5,5 MB. */
#define QN 128                                 /* 4 s: com o PC, o vídeo chega até ~1,2 s antes do som */
static AVFrame *q[QN];
static double q_pts[QN];
static int q_r, q_w;
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;

/* último quadro atrasado (o mostrador marca; a escada de decodificação lê) */
static volatile double late_at = -100;

static volatile int show_stop;                 /* o modo muda (PC -> aqui): este mostrador sai */
static double blit_s;                          /* tempo desenhando (relay: vai para o log) */
static void *show_thread(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "tv-tela");
    /* o desenho tem hora marcada; descomprimir o próximo quadro não (a fila tem 2 s) */
    setpriority(PRIO_PROCESS, 0, -12);
    double t0 = mono();
    unsigned long n0 = 0;
    while(!quit && !show_stop) {
        pthread_mutex_lock(&q_mx);
        if(q_r == q_w) { pthread_mutex_unlock(&q_mx); usleep(5000); continue; }
        AVFrame *f = q[q_r % QN];
        double pts = q_pts[q_r % QN];
        pthread_mutex_unlock(&q_mx);
        double ac = audio_clock(), now = mono();
        /* relógio do áudio parado (sem áudio chegando): o vídeo segue no próprio ritmo */
        static double last_ac = -1, ac_moved, free_next;
        if(ac != last_ac) { last_ac = ac; ac_moved = now; }
        bool stalled = ac < 0 || now - ac_moved > 0.4;
        if(stalled) {
            if(now < free_next) { usleep(5000); continue; }
            free_next = now + 1.0 / 15;
            ac = -1;
        }
        if(ac >= 0 && pts > ac + 0.02 && pts < ac + 2) { usleep(5000); continue; }   /* cedo: espera */
        /* atrasado: descarta. No degrau 2 (só I e P, ~5 qps) o decodificador segura os quadros
         * para reordenar (até ~0,9 s): mostrar com atraso é melhor que não mostrar nenhum */
        static double last_blit;
        if(ac >= 0 && pts < ac - (ladder >= 2 ? 1.0 : 0.25)) { frames_late++; late_at = now; }
        else if(ladder >= 1 && now - last_blit < 0.06) frames_thin++;   /* no aperto, até ~15 qps na tela */
        else { double b0 = mono(); blit(f); blit_s += mono() - b0; frames_shown++; last_blit = now; }
        pthread_mutex_lock(&q_mx);
        if(!relay_on) av_frame_unref(f);               /* com o PC a vaga guarda o buffer (sem malloc) */
        q_r++;
        pthread_mutex_unlock(&q_mx);
        double t = mono();
        if(t - t0 >= 2) { fps_shown = (frames_shown - n0) / (t - t0); n0 = frames_shown; t0 = t; }
    }
    return NULL;
}

/* ------------------------------------------------------------ PC decodificando (tvrelay.h) */

static int listen_fd = -1, relay_fd = -1;
static FILE *play_fp;                                     /* gravação indo para o PC (-p com o relay) */
static double play_start;                                 /* horário do começo da gravação */
static volatile int play_done;
static volatile int play_sent;                            /* o arquivo todo já foi */
static double relay_t0;                                   /* começo do modo PC (para o log) */
static volatile int relay_stop;
static pthread_mutex_t tx_mx = PTHREAD_MUTEX_INITIALIZER;
static int win_w, win_h, full_w, full_h;                 /* -r e -F como vieram (blit troca os dois) */

static int relay_send(char type, const void *d, uint32_t n)
{
    pthread_mutex_lock(&tx_mx);
    int r = proto_send(relay_fd, type, d, n);
    pthread_mutex_unlock(&tx_mx);
    return r;
}

static int relay_listen(void)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(TVRELAY_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if(bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) { close(s); return -1; }
    return s;
}

/* o PC tenta a cada 1 s enquanto a TV toca: espera até ms */
static int relay_accept(int ms)
{
    struct pollfd p = { listen_fd, POLLIN, 0 };
    if(poll(&p, 1, ms) != 1) return -1;
    int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if(c < 0) return -1;
    int one = 1, buf = 256 * 1024;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(c, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    return c;
}

/* o tamanho que o PC manda: a metade do retângulo (o blit amplia 2x); 0 0 = o do canal */
static void relay_target(int full, char *t, size_t n)
{
    int w = full && full_w > 0 ? full_w : win_w, h = full && full_w > 0 ? full_h : win_h;
    snprintf(t, n, "%d %d", w > 0 ? w / 2 : 0, h > 0 ? h / 2 : 0);
}

/* anel do som do PC (relay_audio) */
#define AR (TVRELAY_RATE * 16)                            /* o PC pode mandar uns segundos de uma vez ao abrir */
static unsigned long ar_dropped;                          /* amostras que não couberam (não deve acontecer) */
#define AB 2048
static int16_t ar[AR];
static unsigned ar_w, ar_r;                               /* amostras (contadores) */
static struct { unsigned at; double pts; } ab[AB];        /* começo de cada bloco no anel */
static unsigned ab_w, ab_r;
static pthread_mutex_t aq_mx = PTHREAD_MUTEX_INITIALIZER;

/* TS do anel para o PC */
static void *relay_tx(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "tv-ts-pc");
    /* o começo da cadeia: sem o TS o PC não tem o que decodificar, e som e imagem param juntos
     * (medido: 21 cortes de som com esta thread abaixo do desenho). Gasta 2-3 % */
    setpriority(PRIO_PROCESS, 0, -14);
    static uint8_t buf[16384];
    size_t sent = 0;
    /* a trava abaixo só vê o som que já voltou; sem limite de taxa o laço mandava o arquivo inteiro
     * antes da primeira resposta (medido: 16 s de som na fila e amostras perdidas). No máximo 2x a
     * taxa do arquivo */
    double rate = 60000, next_send = 0;
    if(play_fp && play_dur > 1) {
        long cur = ftell(play_fp);
        if(fseek(play_fp, 0, SEEK_END) == 0) { rate = ftell(play_fp) / play_dur; fseek(play_fp, cur, SEEK_SET); }
    }
    while(play_fp && !quit && !relay_stop) {               /* gravação: no ritmo do som, ~2 s na frente */
        pthread_mutex_lock(&aq_mx);
        unsigned ahead = ar_w - ar_r, have_audio = ar_w != 0;
        pthread_mutex_unlock(&aq_mx);
        pthread_mutex_lock(&q_mx);
        int vq = q_w - q_r;
        pthread_mutex_unlock(&q_mx);
        /* antes do primeiro som o PC ainda analisa o fluxo (~200 KB): mais que isso viria de uma vez */
        if(ahead > TVRELAY_RATE * 2 || (!have_audio && (vq > 60 || sent >= 256 * 1024))) { usleep(20000); continue; }
        double now = mono();
        if(have_audio && now < next_send) { usleep((useconds_t)((next_send - now) * 1e6)); continue; }
        size_t n = fread(buf, 1, sizeof buf, play_fp);
        sent += n;
        next_send = (next_send > now ? next_send : now) + n / (2 * rate);
        if(n == 0) { play_sent = 1; return NULL; }
        if(relay_send('T', buf, (uint32_t)n)) { relay_stop = 1; return NULL; }
    }
    while(!quit && !relay_stop) {
        pthread_mutex_lock(&r_mx);
        if(r_head == r_tail) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 200000000;
            if(t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&r_cv, &r_mx, &t);
        }
        int n = (int)(r_head - r_tail);
        if(n > (int)sizeof buf) n = (int)sizeof buf;
        for(int i = 0; i < n; i++) buf[i] = ring[(r_tail + (size_t)i) % RING];
        r_tail += (size_t)n;
        pthread_mutex_unlock(&r_mx);
        if(n > 0 && relay_send('T', buf, (uint32_t)n)) { relay_stop = 1; break; }
    }
    return NULL;
}

/* som do PC: anel fixo de amostras (nada de malloc/free por bloco: a thread do som disputava
 * a trava do alocador com as de prioridade menor e esperava; medido: o som secava com 0,3-0,5 s
 * na fila, em pares a cada ~2 s) e o horário de cada bloco à parte. Reserva de 300 ms antes de
 * começar e depois de secar. */

static void audio_push(const uint8_t *d, int ns, double pts)
{
    pthread_mutex_lock(&aq_mx);
    if(ar_w - ar_r + (unsigned)ns <= AR && ab_w - ab_r < AB) {
        for(int i = 0; i < ns; i++) ar[(ar_w + (unsigned)i) % AR] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
        ab[ab_w % AB].at = ar_w;
        ab[ab_w % AB].pts = pts;
        ab_w++;
        ar_w += (unsigned)ns;
    } else {
        ar_dropped += (unsigned long)ns;
    }
    pthread_mutex_unlock(&aq_mx);
}

static void *relay_audio(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "tv-som");
    /* o som não pode faltar: no aperto, quem cede é o vídeo (um quadro a menos) */
    setpriority(PRIO_PROCESS, 0, -15);
    static int16_t out[1024];
    int primed = 0;
    while(!quit && !relay_stop) {
        pthread_mutex_lock(&aq_mx);
        unsigned have = ar_w - ar_r;
        if(!primed && have < TVRELAY_RATE * 3 / 10) { pthread_mutex_unlock(&aq_mx); usleep(10000); continue; }
        primed = 1;
        if(!have) { pthread_mutex_unlock(&aq_mx); primed = 0; continue; }
        unsigned n = have < 1024 ? have : 1024;
        for(unsigned i = 0; i < n; i++) out[i] = ar[(ar_r + i) % AR];
        unsigned start = ar_r;
        ar_r += n;
        while(ab_r + 1 < ab_w && ab[(ab_r + 1) % AB].at <= start) ab_r++;   /* o bloco onde start cai */
        double pts = ab_r < ab_w ? ab[ab_r % AB].pts + (double)(start - ab[ab_r % AB].at) / TVRELAY_RATE : -1;
        pthread_mutex_unlock(&aq_mx);
        if(dsp >= 0) {
            int q = 0;
            if(audio_pts >= 0 && ioctl(dsp, SNDCTL_DSP_GETODELAY, &q) == 0 && q == 0) {
                audio_underruns++;
                fprintf(stderr, "bftvd: o som secou em %.1f s (fila %u ms)\n", mono() - relay_t0, have * 1000 / TVRELAY_RATE);
            }
            const uint8_t *p = (const uint8_t *)out;
            size_t left = (size_t)n * 2;
            while(left && !quit && !relay_stop) {
                ssize_t w = write(dsp, p, left);
                if(w < 0) { if(errno == EINTR) continue; break; }
                p += w;
                left -= (size_t)w;
            }
            if(pts >= 0) audio_pts = pts + n / (double)TVRELAY_RATE;
        }
    }
    return NULL;
}

/* um quadro pronto na fila do mostrador. Cada vaga guarda o buffer dela (o mostrador não o
 * solta com o PC): só aloca quando o tamanho muda. Nunca espera: o som vem atrás na mesma
 * conexão (medido: 12 cortes esperando vaga numa troca para a tela cheia); fila cheia, este
 * quadro fica fora. */
static void relay_frame(const yuvz_dec_t *d, double pts)
{
    pthread_mutex_lock(&q_mx);
    int full = q_w - q_r >= QN;
    AVFrame *f = full ? NULL : q[q_w % QN];
    pthread_mutex_unlock(&q_mx);
    if(full) { frames_late++; return; }
    if(f->width != d->w || f->height != d->h || !f->data[0]) {
        av_frame_unref(f);
        f->width = d->w;
        f->height = d->h;
        f->format = AV_PIX_FMT_YUV420P;
        if(av_frame_get_buffer(f, 32) < 0) { av_frame_unref(f); return; }
    }
    int cw = d->w / 2, chh = (d->h + 1) / 2;
    const uint8_t *u = d->yuv + (size_t)d->w * d->h, *v = u + (size_t)cw * chh;
    for(int y = 0; y < d->h; y++) memcpy(f->data[0] + y * f->linesize[0], d->yuv + (size_t)y * d->w, (size_t)d->w);
    for(int y = 0; y < chh; y++) {
        memcpy(f->data[1] + y * f->linesize[1], u + (size_t)y * cw, (size_t)cw);
        memcpy(f->data[2] + y * f->linesize[2], v + (size_t)y * cw, (size_t)cw);
    }
    pthread_mutex_lock(&q_mx);
    q_pts[q_w % QN] = pts;
    q_w++;
    pthread_mutex_unlock(&q_mx);
}

static void queues_clear(void)
{
    pthread_mutex_lock(&q_mx);
    q_r = q_w;
    for(int i = 0; i < QN; i++) av_frame_unref(q[i]);         /* o modo daqui volta a usar ref/unref */
    pthread_mutex_unlock(&q_mx);
    pthread_mutex_lock(&aq_mx);
    ar_r = ar_w;
    ab_r = ab_w;
    pthread_mutex_unlock(&aq_mx);
}

/* 0: terminou (SIGTERM); -1: o PC saiu (decodifica aqui daí em diante) */
static int relay_play(void)
{
    char t[48], h[64];
    relay_target(0, t, sizeof t);
    snprintf(h, sizeof h, "BFTV1 %s", t);
    relay_stop = 0;
    if(relay_send('H', h, (uint32_t)strlen(h))) { close(relay_fd); relay_fd = -1; return -1; }
    relay_on = 1;
    relay_t0 = mono();
    write_state(play_path ? "tocando" : "abrindo");
    double next_state = 0, quiet_since = 0;
    for(int i = 0; i < QN; i++) if(!q[i]) q[i] = av_frame_alloc();
    dsp_open();
    pthread_t st, tx, au;
    show_stop = 0;
    pthread_create(&st, NULL, show_thread, NULL);
    pthread_create(&tx, NULL, relay_tx, NULL);
    pthread_create(&au, NULL, relay_audio, NULL);
    /* ordem no aperto: som e antena (-15) > receber do PC (-13: o som vem nesta conexão, e esta
     * thread nunca espera) > desenhar (-12: um quadro atrasado é descartado) > o resto */
    setpriority(PRIO_PROCESS, 0, -13);
    proto_rx_t rx = { .cap = RELAY_MAX_MSG + 16 };
    rx.buf = malloc(rx.cap);
    yuvz_dec_t dec = { 0 };
    int last_full = want_full;
    unsigned long n_v = 0, n_q = 0, n_a = 0, shown0 = frames_shown;
    double next_log = mono() + 60;
    while(rx.buf && !quit && !relay_stop) {
        if(mono() >= next_log) {                           /* no log: o que chegou e o que saiu */
            unsigned long ns = frames_shown - shown0;
            fprintf(stderr, "bftvd: relay 60 s: %lu quadros chegaram, %lu na fila, %lu mostrados (%.1f ms cada), %lu blocos de som\n",
                    n_v, n_q, ns, ns ? blit_s * 1000 / ns : 0, n_a);
            blit_s = 0;
            n_v = n_q = n_a = 0;
            shown0 = frames_shown;
            next_log = mono() + 60;
        }
        if(want_full != last_full) {                       /* tela cheia / janela: outro tamanho */
            last_full = want_full;
            fprintf(stderr, "bftvd: %s em %.1f s\n", last_full ? "tela cheia" : "janela", mono() - relay_t0);
            relay_target(last_full, t, sizeof t);
            if(relay_send('S', t, (uint32_t)strlen(t))) break;
        }
        if(play_path) {                                    /* gravação: posição para a barra e o fim */
            double now = mono();
            if(audio_pts >= 0) play_pos = audio_pts - play_start;
            if(now >= next_state) { write_state("tocando"); next_state = now + 1; }
            pthread_mutex_lock(&aq_mx);
            int a_empty = ar_w == ar_r;
            pthread_mutex_unlock(&aq_mx);
            pthread_mutex_lock(&q_mx);
            int v_empty = q_w == q_r;
            pthread_mutex_unlock(&q_mx);
            if(!play_sent || !a_empty || !v_empty) quiet_since = 0;
            else if(!quiet_since) quiet_since = now;
            else if(now - quiet_since > 1.5) { play_done = 1; break; }   /* tudo tocado */
        }
        struct pollfd p = { relay_fd, POLLIN, 0 };
        if(poll(&p, 1, 200) <= 0) continue;
        if(rx.n == rx.cap) break;
        ssize_t n = read(relay_fd, rx.buf + rx.n, rx.cap - rx.n);
        if(n <= 0) { if(n < 0 && errno == EINTR) continue; break; }
        rx.n += (size_t)n;
        char type;
        uint8_t *d;
        uint32_t l;
        size_t used = 0, c;
        for(;;) {
            proto_rx_t view = { rx.buf + used, rx.n - used, rx.cap - used };
            int r = proto_take(&view, &type, &d, &l, &c);
            if(r <= 0) { if(r < 0) relay_stop = 1; break; }
            used += c;
            if(l < 8) continue;
            double pts = tvrelay_get_pts(d) / 1e6;
            if(type == 'V') {
                int k = yuvz_decode(&dec, d + 8, l - 8);
                n_v++;
                if(k < 0) relay_send('R', NULL, 0);        /* sem a base: o próximo vem inteiro */
                else if(k > 0) { relay_frame(&dec, pts); n_q++; }
            } else if(type == 'A' && l > 9) {
                n_a++;
                audio_push(d + 8, (int)((l - 8) / 2), pts);
            }
        }
        if(used) { memmove(rx.buf, rx.buf + used, rx.n - used); rx.n -= used; }
    }
    int lost = !quit && !play_done;
    relay_stop = 1;
    shutdown(relay_fd, SHUT_RDWR);
    pthread_join(tx, NULL);
    pthread_join(au, NULL);
    show_stop = 1;
    pthread_join(st, NULL);
    show_stop = 0;
    queues_clear();
    close(relay_fd);
    relay_fd = -1;
    free(rx.buf);
    yuvz_dec_free(&dec);
    relay_on = 0;
    audio_pts = -1;
    if(dsp >= 0) { close(dsp); dsp = -1; }             /* aqui o buffer volta ao curto (dsp_open) */
    if(lost) fprintf(stderr, "bftvd: o PC saiu; decodificando aqui\n");
    if(ar_dropped) fprintf(stderr, "bftvd: %lu amostras de som não couberam no anel\n", ar_dropped);
    return lost ? -1 : 0;
}

/* ------------------------------------------------------------ busca de canais */

/* UHF 14-69 no Brasil: centro = 473 + 6 (canal - 14) + 1/7 MHz */
static double ch_mhz(int ch) { return 473.0 + 6.0 * (ch - 14) + 1.0 / 7.0; }

static char sdt_name[96];
static uint8_t sdt_sec[1024];
static int sdt_len, sdt_want;

/* nome do serviço (descritor 0x48) de uma seção SDT; o 1seg (tipo 0xC0) tem preferência */
static void sdt_parse(const uint8_t *s, int n)
{
    if(n < 11 || s[0] != 0x42) return;
    int end = 3 + ((s[1] & 0x0f) << 8 | s[2]) - 4;              /* sem o CRC */
    if(end > n) end = n;
    char best[96] = "";
    int best_1seg = 0;
    for(int i = 11; i + 5 <= end;) {
        int dl = (s[i + 3] & 0x0f) << 8 | s[i + 4];
        int j = i + 5, dend = j + dl;
        if(dend > end) break;
        while(j + 2 <= dend) {
            int tag = s[j], len = s[j + 1];
            if(j + 2 + len > dend) break;
            if(tag == 0x48 && len >= 3) {
                int type = s[j + 2], pl = s[j + 3];
                if(4 + pl < len + 2) {
                    int nl = s[j + 4 + pl];
                    const uint8_t *nm = s + j + 5 + pl;
                    if(j + 5 + pl + nl <= dend && (!best[0] || (type == 0xc0 && !best_1seg))) {
                        /* ABNT NBR 15606-1: ISO 8859-15 (o 1º byte pode escolher a tabela) -> UTF-8 */
                        size_t o = 0;
                        for(int k = 0; k < nl && o + 3 < sizeof best; k++) {
                            uint8_t c = nm[k];
                            if(c < 0x20 || (c >= 0x7f && c < 0xa0)) continue;
                            if(c < 0x80) best[o++] = (char)c;
                            else { best[o++] = (char)(0xc0 | c >> 6); best[o++] = (char)(0x80 | (c & 0x3f)); }
                        }
                        best[o] = 0;
                        while(o && best[o - 1] == ' ') best[--o] = 0;
                        best_1seg = type == 0xc0;
                    }
                }
            }
            j += 2 + len;
        }
        i = dend;
    }
    if(best[0]) snprintf(sdt_name, sizeof sdt_name, "%s", best);
}

static void scan_data(const hdr_t *h, const uint8_t *d, int n)
{
    if(!(h->type == 607 || h->type == 729 || h->type == 693)) { store(h, d, n); return; }
    for(int p = 0; p + 188 <= n; p += 188) {
        const uint8_t *t = d + p;
        if(t[0] != 0x47 || ((t[1] & 0x1f) << 8 | t[2]) != 0x11) continue;
        int off = 4;
        if(t[3] & 0x20) off += 1 + t[4];                       /* campo de adaptação */
        if(off >= 188 || !(t[3] & 0x10)) continue;
        if(t[1] & 0x40) {                                      /* início de seção */
            off += 1 + t[off];
            if(off + 3 > 188) continue;
            sdt_len = 0;
            sdt_want = 3 + ((t[off + 1] & 0x0f) << 8 | t[off + 2]);
        } else if(!sdt_want) continue;
        int k = 188 - off;
        if(sdt_len + k > (int)sizeof sdt_sec) k = (int)sizeof sdt_sec - sdt_len;
        memcpy(sdt_sec + sdt_len, t + off, (size_t)k);
        sdt_len += k;
        if(sdt_len >= sdt_want) { if(sdt_sec[0] == 0x42) sdt_parse(sdt_sec, sdt_want); sdt_want = 0; }
    }
}

static int found;
static int scan_ch;

static void scan_state(const char *st)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    fprintf(f, "state=%s\nscan_ch=%d\nscan_found=%d\n", st, scan_ch, found);
    fclose(f);
    rename(tmp, state_path);
}

static int scan(const char *out)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *o = fopen(tmp, "w");
    if(!o) { scan_state("erro_arquivo"); return 1; }
    for(scan_ch = 14; scan_ch <= 69 && !quit; scan_ch++) {
        scan_state("busca");
        double f = ch_mhz(scan_ch);
        on_msg = store;
        if(set_mode_tune(f, 1)) continue;
        int locked = 0;
        for(int i = 0; i < 6 && !quit && !locked; i++) {        /* trava em ~0,5-1 s quando há sinal */
            msleep(300);
            if(!stats() && last_stats.valid && last_stats.demod_locked) locked = 1;
        }
        if(!locked) continue;
        sdt_name[0] = 0;
        sdt_want = 0;
        on_msg = scan_data;
        long end = now_ms() + 4000;
        while(now_ms() < end && !sdt_name[0] && !quit) { if(!poll_read()) msleep(2); }
        on_msg = store;
        stats();
        fprintf(o, "%d\t%.3f\t%s\t%d\t%d\n", scan_ch, f, sdt_name, last_stats.snr, last_stats.rssi);
        fflush(o);
        found++;
    }
    fclose(o);
    if(quit) { unlink(tmp); scan_state("parado"); return 1; }
    rename(tmp, out);
    scan_ch = 0;
    scan_state("busca_fim");
    return 0;
}

/* Escada de decodificação. Há canais 1seg em H.264 Main com quadros B a 30 qps (SBT: 85 % B,
 * em pirâmide); o ARM11 não decodifica todos com o som, o sintonizador e o desenho (CPU a 100 %,
 * o som picota e os quadros chegam atrasados). Medido com 15 s do SBT:
 *   0 tudo                              456 quadros  100 % do custo
 *   1 sem os que não são referência      401          76 %
 *   2 sem os quadros B (só I e P)         69          38 %
 * Sobe um degrau quando o TS se acumula (> 48 KB, ~1 s atrás do ar, sem diminuir), o som está a ponto de acabar
 * (< 30 ms no buffer) ou um quadro chegou atrasado no último segundo; desce depois de 8 s sem
 * aperto, e o dobro (até 64 s) cada vez que precisou subir de novo logo depois de descer. Nos degraus 1 e 2 a tela mostra até
 * ~15 qps (cada quadro ampliado custa CPU; 15 qps é o padrão do 1seg). Só troca num quadro-chave: pular B
 * de referência no meio do grupo deixaria os seguintes sem referência (imagem quebrada).
 * Canais de 15 qps em Baseline (TV Brasil) não têm B: ficam no 0 com folga. */
static double ladder_ok = -1;

static int audio_buffered_ms(void)
{
    int bytes = 0;
    if(dsp < 0 || ioctl(dsp, SNDCTL_DSP_GETODELAY, &bytes) < 0) return 1000;
    return bytes / 64;                                         /* 64 bytes por ms */
}

/* ------------------------------------------------------------ decodificando aqui (sem o PC)
 * Três threads: a leitura do TS só distribui os pacotes (nunca espera o vídeo); o som (nice
 * -15) decodifica o AAC e toca; o vídeo decodifica com o que sobra e, atrasado em relação ao
 * relógio do som, pula até o próximo quadro-chave. Antes era uma thread só: o som esperava cada
 * quadro de vídeo e secava (medido na Globo: 10-12 qps e 95 cortes em 36 s). */

static volatile int local_switch;                         /* o PC conectou: a decodificação daqui para */
#define PKN 1024
typedef struct {
    AVPacket *p[PKN];
    int r, w, eof;
    pthread_mutex_t mx;
    pthread_cond_t cv;
} pktq_t;
static pktq_t apq = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static pktq_t vpq = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static int pktq_len(pktq_t *q) { pthread_mutex_lock(&q->mx); int n = q->w - q->r; pthread_mutex_unlock(&q->mx); return n; }

/* 0: entrou; -1: cheia (o pacote fica com quem chamou) */
static int pktq_put(pktq_t *q, AVPacket *pk)
{
    pthread_mutex_lock(&q->mx);
    if(q->w - q->r >= PKN) { pthread_mutex_unlock(&q->mx); return -1; }
    AVPacket *d = q->p[q->w % PKN];
    if(!d) d = q->p[q->w % PKN] = av_packet_alloc();
    av_packet_move_ref(d, pk);
    q->w++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mx);
    return 0;
}

/* 1: pacote em out; 0: acabou (eof ou quit) */
static int pktq_get(pktq_t *q, AVPacket *out)
{
    pthread_mutex_lock(&q->mx);
    while(q->r == q->w && !q->eof && !quit && !local_switch) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 100000000;
        if(t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&q->cv, &q->mx, &t);
    }
    int ok = q->r != q->w && !quit && !local_switch;
    if(ok) av_packet_move_ref(out, q->p[q->r++ % PKN]);
    pthread_mutex_unlock(&q->mx);
    return ok;
}

static void pktq_reset(pktq_t *q)
{
    pthread_mutex_lock(&q->mx);
    for(; q->r < q->w; q->r++) av_packet_unref(q->p[q->r % PKN]);
    q->eof = 0;
    pthread_mutex_unlock(&q->mx);
}

static void ladder_step(AVCodecContext *vc)
{
    static const enum AVDiscard lv[3] = { AVDISCARD_DEFAULT, AVDISCARD_NONREF, AVDISCARD_BIDIR };
    static double hold = 8, went_down = -100;
    double now = mono();
    /* atrás: pacotes de vídeo acumulando (mais de ~0,5 s) e sem diminuir */
    static int prev_len;
    int len = pktq_len(&vpq);
    bool behind = len > 15 && len + 2 >= prev_len;
    prev_len = len;
    bool tight = behind || (audio_pts >= 0 && audio_buffered_ms() < 30) || now - late_at < 1;
    if(ladder_ok < 0) ladder_ok = now;
    if(tight) {
        if(ladder < 2) {
            ladder++;
            if(now - went_down < 20 && hold < 64) hold *= 2;   /* desceu cedo demais: espera mais */
        }
        ladder_ok = now;
    } else if(ladder > 0 && now - ladder_ok > hold) {
        ladder--;
        ladder_ok = went_down = now;
    }
    vc->skip_frame = lv[ladder];
}

static HANDLE_AACDECODER l_aac;
static AVCodecContext *l_vc;
static AVRational l_atb, l_vtb;
static unsigned long video_skipped;                       /* pacotes pulados até um quadro-chave */

/* buraco no áudio (pacotes perdidos no ar ou na gravação): silêncio pelo tempo que falta, para o
 * relógio do som seguir o tempo real e o vídeo ficar no lugar (sem isto a gravação da Record de
 * 38 s tocava em 27 s, pulando o vídeo dos buracos; e antes do áudio em thread própria, 167 cortes) */
static unsigned long gap_ms;
static void audio_gap(double from, double to)
{
    static const int16_t z[1024];
    long n = (long)((to - from) * 32000);
    while(n > 0 && dsp >= 0 && !quit) {
        long k = n < 1024 ? n : 1024;
        if(write(dsp, z, (size_t)k * 2) < 0 && errno != EINTR) break;
        n -= k;
    }
    gap_ms += (unsigned long)((to - from) * 1000);
    audio_pts = to;
}

static void *local_audio(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "tv-som");
    setpriority(PRIO_PROCESS, 0, -15);
    static INT_PCM pcm[8192];
    AVPacket *pk = av_packet_alloc();
    while(pktq_get(&apq, pk)) {
        double pts = pk->pts == AV_NOPTS_VALUE ? -1 : pk->pts * av_q2d(l_atb);
        UCHAR *b = pk->data;
        UINT sz = (UINT)pk->size, valid = sz;
        int first = 1;
        while(valid > 0 && !quit) {
            if(aacDecoder_Fill(l_aac, &b, &sz, &valid) != AAC_DEC_OK) break;
            b = pk->data + (pk->size - valid);
            sz = valid;
            while(aacDecoder_DecodeFrame(l_aac, pcm, 8192, 0) == AAC_DEC_OK) {
                CStreamInfo *si = aacDecoder_GetStreamInfo(l_aac);
                double fp = first && pts >= 0 ? pts : audio_pts;
                first = 0;
                if(audio_pts >= 0 && fp > audio_pts + 0.03 && fp < audio_pts + 3) audio_gap(audio_pts, fp);
                audio_out(pcm, si->frameSize, si->numChannels, fp);
            }
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk);
    if(gap_ms) fprintf(stderr, "bftvd: %lu ms de silêncio nos buracos do áudio\n", gap_ms);
    return NULL;
}

static void *local_video(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "tv-video");
    int skip_ok = !getenv("BFTV_NOSKIP"), need_key = 0;
    AVPacket *pk = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    while(pktq_get(&vpq, pk)) {
        int key = (pk->flags & AV_PKT_FLAG_KEY) != 0;
        double pts = pk->pts == AV_NOPTS_VALUE ? -1 : pk->pts * av_q2d(l_vtb);
        double ac = audio_clock();
        /* atrasado em relação ao som: o que sair daqui já passou da hora; pula até o próximo
         * quadro-chave (decodificar P sem as referências daria lixo na tela) */
        if(skip_ok && !key && !need_key && ac >= 0 && pts >= 0 && pts < ac - 1.0) need_key = 1;
        if(need_key) {
            if(!key) { video_skipped++; av_packet_unref(pk); continue; }
            need_key = 0;
            avcodec_flush_buffers(l_vc);
        }
        if(skip_ok && key) ladder_step(l_vc);
        avcodec_send_packet(l_vc, pk);
        av_packet_unref(pk);
        while(avcodec_receive_frame(l_vc, fr) == 0) {
            double fp = fr->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : fr->best_effort_timestamp * av_q2d(l_vtb);
            for(;;) {                                      /* fila cheia: espera o mostrador */
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
            av_frame_unref(fr);
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pk);
    return NULL;
}

/* demux já aberto: decodifica e mostra até o fim ou SIGTERM */
static void run_player(AVFormatContext *fc, pthread_t *st)
{
    double next_state = 0, start_s = fc->start_time == AV_NOPTS_VALUE ? 0 : fc->start_time / (double)AV_TIME_BASE;
    int vi = quit ? -1 : av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = quit ? -1 : av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    l_vc = NULL;
    if(vi >= 0) {
        const AVCodec *c = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
        l_vc = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(l_vc, fc->streams[vi]->codecpar);
        l_vc->skip_loop_filter = AVDISCARD_ALL;            /* -30 % de CPU, perda pequena em 320x180 */
        l_vc->flags2 |= AV_CODEC_FLAG2_FAST;
        if(avcodec_open2(l_vc, c, NULL) < 0) { avcodec_free_context(&l_vc); vi = -1; }
        else l_vtb = fc->streams[vi]->time_base;
    }
    l_aac = ai >= 0 ? aacDecoder_Open(TT_MP4_LOAS, 1) : NULL;
    if(l_aac) {
        /* a saída é mono: a mistura no decodificador pula o estéreo paramétrico (HE-AAC v2);
         * QMF real (baixo consumo) no SBR; sem o limitador (o volume vem do mixer) */
        aacDecoder_SetParam(l_aac, AAC_PCM_MAX_OUTPUT_CHANNELS, 1);
        aacDecoder_SetParam(l_aac, AAC_QMF_LOWPOWER, 1);
        aacDecoder_SetParam(l_aac, AAC_PCM_LIMITER_ENABLE, 0);
        l_atb = fc->streams[ai]->time_base;
        dsp_open();
    }
    for(int i = 0; i < QN; i++) if(!q[i]) q[i] = av_frame_alloc();
    show_stop = 0;
    pthread_create(st, NULL, show_thread, NULL);
    pktq_reset(&apq);
    pktq_reset(&vpq);
    pthread_t at, vt;
    int have_a = l_aac != NULL, have_v = l_vc != NULL;
    if(have_a) pthread_create(&at, NULL, local_audio, NULL);
    if(have_v) pthread_create(&vt, NULL, local_video, NULL);
    write_state(vi < 0 ? "sem_video" : play_path ? "tocando" : "abrindo");
    setpriority(PRIO_PROCESS, 0, -14);                     /* a leitura só distribui: barata */

    AVPacket *pk = av_packet_alloc();
    double next_accept = mono() + 0.5;
    while(!quit && av_read_frame(fc, pk) >= 0) {
        if(!play_path && listen_fd >= 0 && mono() >= next_accept) {   /* o PC chegou depois? */
            next_accept = mono() + 0.5;
            if((relay_fd = relay_accept(0)) >= 0) { local_switch = 1; av_packet_unref(pk); break; }
        }
        if(pk->stream_index == ai && have_a) {
            /* gravação: o arquivo chega todo de uma vez; o som (tocado no tempo real) dá o ritmo,
             * com até ~2 s na frente. Ao vivo a antena já dá o ritmo e isto nunca espera */
            while(!quit && pktq_len(&apq) > 100) usleep(10000);
            if(pktq_put(&apq, pk)) av_packet_unref(pk);
        } else if(pk->stream_index == vi && have_v) {
            if(pktq_put(&vpq, pk)) { video_skipped++; av_packet_unref(pk); }   /* nunca espera o vídeo */
        }
        av_packet_unref(pk);
        if(play_path) {                                    /* arquivo: posição para a barra do shell */
            if(audio_pts >= 0) play_pos = audio_pts - start_s;
            double t = mono();
            if(t >= next_state) { write_state("tocando"); next_state = t + 1; }
        }
    }
    av_packet_free(&pk);
    /* fim do arquivo: as threads esvaziam o que ficou e saem */
    pthread_mutex_lock(&apq.mx); apq.eof = 1; pthread_cond_signal(&apq.cv); pthread_mutex_unlock(&apq.mx);
    pthread_mutex_lock(&vpq.mx); vpq.eof = 1; pthread_cond_signal(&vpq.cv); pthread_mutex_unlock(&vpq.mx);
    while(play_path && !quit && (pktq_len(&apq) || pktq_len(&vpq))) {
        if(audio_pts >= 0) play_pos = audio_pts - start_s;
        usleep(100000);
    }
    if(have_a) pthread_join(at, NULL);
    if(have_v) pthread_join(vt, NULL);
    avcodec_free_context(&l_vc);
    if(l_aac) { aacDecoder_Close(l_aac); l_aac = NULL; }
    if(video_skipped) fprintf(stderr, "bftvd: %lu pacotes de vídeo pulados até um quadro-chave\n", video_skipped);
}

static int play_file(void)
{
    AVFormatContext *fc = NULL;
    if(avformat_open_input(&fc, play_path, NULL, NULL) < 0) { write_state("erro_arquivo"); return 1; }
    avformat_find_stream_info(fc, NULL);
    play_dur = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
    play_start = fc->start_time == AV_NOPTS_VALUE ? 0 : fc->start_time / (double)AV_TIME_BASE;
    write_state("tocando");
    /* com o PC na rede USB, ele decodifica a gravação também */
    if((!getenv("BFTV_RELAY") || atoi(getenv("BFTV_RELAY"))) && (listen_fd = relay_listen()) >= 0 &&
       (relay_fd = relay_accept(1500)) >= 0 && (play_fp = fopen(play_path, "rb"))) {
        fprintf(stderr, "bftvd: o PC decodifica a gravação (relay)\n");
        int r = relay_play();
        fclose(play_fp);
        play_fp = NULL;
        if(r == 0 || quit) {
            int done = !quit || play_done;
            if(dsp >= 0) close(dsp);
            avformat_close_input(&fc);
            write_state(done ? "fim" : "parado");
            return 0;
        }
        /* o PC saiu no meio: toca daqui, do começo */
        av_seek_frame(fc, -1, fc->start_time == AV_NOPTS_VALUE ? 0 : fc->start_time, AVSEEK_FLAG_BACKWARD);
    } else if(relay_fd >= 0) { close(relay_fd); relay_fd = -1; }
    pthread_t st;
    run_player(fc, &st);
    int done = !quit;
    quit = 1;
    pthread_join(st, NULL);
    if(dsp >= 0) close(dsp);
    avformat_close_input(&fc);
    write_state(done ? "fim" : "parado");
    return 0;
}

/* ------------------------------------------------------------ principal */

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_QUIET);                     /* avisos de quadro com erro: nada de log */
    int opt, off = 0;
    const char *scan_out = NULL, *rec_path = NULL;
    while((opt = getopt(argc, argv, "f:r:F:s:S:w:W:p:O")) != -1) {
        if(opt == 'f') fw_path = optarg;
        else if(opt == 's') state_path = optarg;
        else if(opt == 'r') {                          /* X,Y,2 (o dobro do quadro) ou X,Y,L,A */
            int a = 0, b = 0;
            if(sscanf(optarg, "%d,%d,%d,%d", &out_x, &out_y, &a, &b) == 4) { out_w = a; out_h = b; }
        }
        else if(opt == 'F') sscanf(optarg, "%d,%d,%d,%d", &alt_x, &alt_y, &alt_w, &alt_h);
        else if(opt == 'S') scan_out = optarg;
        else if(opt == 'w') rec_path = optarg;
        else if(opt == 'W') rec_dir = optarg;
        else if(opt == 'p') play_path = optarg;
        else if(opt == 'O') off = 1;
    }
    if(!off && !scan_out && !play_path && optind >= argc) {
        fprintf(stderr, "uso: bftvd [-f FW] [-r X,Y,2] [-s STATE] [-w REC.ts] MHZ | -p ARQ.ts | -S CANAIS.tsv | -O\n");
        return 2;
    }
    if(optind < argc) freq_mhz = atof(argv[optind]);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    signal(SIGWINCH, on_winch);
    /* acima da interface (nice -5 com o modo performance ou um controle): a TV não perde a CPU
     * para o shell nem para o Bluetooth; as threads criadas depois herdam */
    setpriority(PRIO_PROCESS, 0, -10);
    if(!play_path && map()) return 1;
    if(off) { power_off(); return 0; }
    tables();
    if(!scan_out && fb_open()) { fprintf(stderr, "sem /dev/fb0\n"); return 1; }
    win_w = out_w; win_h = out_h; full_w = alt_w; full_h = alt_h;
    getenv_no64 = getenv("BFTV_NO64") && atoi(getenv("BFTV_NO64"));
    if(play_path) return play_file();

    /* sintonizador: liga e carrega o firmware se ainda não respondeu */
    int play_div = getenv("BFTV_CLKDIV") ? atoi(getenv("BFTV_CLKDIV")) : 7;
    clk_div = play_div;
    spi_setup();
    on_msg = store;
    if(send_msg(668, 150, NULL, 0) || wait_msg(669, 500)) {
        write_state("ligando");
        power_on();
        clk_div = 15;
        spi_setup();
        if(fw_download(fw_path)) {                         /* travado de antes: desliga, liga e tenta de novo */
            power_off();
            msleep(300);
            power_on();
            spi_setup();
            if(fw_download(fw_path)) { write_state("erro_firmware"); return 1; }
        }
        /* o download é em clk 15; tocar nele deixava a leitura do TS (espera ativa no SPI) com o
         * dobro de CPU na primeira abertura depois de ligar */
        clk_div = play_div;
        spi_setup();
    }
    if(scan_out) return scan(scan_out);
    /* o PC conecta enquanto sintoniza (ele tenta a cada 1 s) */
    if(!getenv("BFTV_RELAY") || atoi(getenv("BFTV_RELAY"))) listen_fd = relay_listen();
    write_state("sintonizando");
    if(set_mode_tune(freq_mhz, 1)) { write_state("erro_sintonia"); return 1; }
    if(rec_path) { rec = fopen(rec_path, "wb"); snprintf(rec_name, sizeof rec_name, "%s", rec_path); }
    pthread_t tt;
    pthread_create(&tt, NULL, tuner_thread, NULL);

    /* PC ou aqui, e troca no meio: o PC que conecta depois (ele tenta a cada 1 s; numa troca de
     * canal pode chegar depois da espera inicial) assume; o PC que sai devolve para cá */
    if(listen_fd >= 0) relay_fd = relay_accept(1500);
    AVFormatContext *fc = NULL;
    while(!quit) {
        if(relay_fd >= 0) {
            fprintf(stderr, "bftvd: o PC decodifica (relay)\n");
            if(relay_play() == 0 || quit) break;
        }
        if(!fc) {                                      /* demux daqui (uma vez; segue lendo o anel) */
            fc = avformat_alloc_context();
            uint8_t *iob = av_malloc(32768);
            fc->pb = avio_alloc_context(iob, 32768, 0, NULL, ring_read, NULL, NULL);
            fc->probesize = 120000;                    /* ~2,5 s de TS 1seg: abre o canal mais rápido */
            fc->max_analyze_duration = AV_TIME_BASE;
            const AVInputFormat *ts = av_find_input_format("mpegts");
            if(avformat_open_input(&fc, NULL, ts, NULL) < 0) { write_state("erro_fluxo"); quit = 1; break; }
            avformat_find_stream_info(fc, NULL);
        }
        pthread_t st;
        local_switch = 0;
        run_player(fc, &st);
        show_stop = 1;
        pthread_join(st, NULL);
        show_stop = 0;
        if(!local_switch) break;                       /* fim ou SIGTERM */
        queues_clear();                                /* o PC chegou: daqui para lá */
        if(dsp >= 0) { close(dsp); dsp = -1; }
        audio_pts = -1;
        ladder = 0;
    }
    quit = 1;
    pthread_join(tt, NULL);
    if(dsp >= 0) close(dsp);
    if(rec) { fclose(rec); rec = NULL; }
    write_state("parado");
    return 0;
}
