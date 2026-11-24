/* video.c — vídeo do Big Remote Play: H.264 do Sunshine decodificado com o FFmpeg mínimo da TV
 * (firmware/build/ffmpeg-tv, o mesmo decodificador do bftvd) e desenhado direto no framebuffer
 * em RGB565, num retângulo que o shell deixa preto (como a TV em janela).
 *
 * Baixa latência: sem fila própria (a moonlight-common-c já entrega um quadro por vez na thread
 * dela), AV_CODEC_FLAG_LOW_DELAY, sem o filtro de desblocagem (-30 % de CPU no ARM11) e quadro
 * com erro pede um quadro-chave novo (DR_NEED_IDR). A ampliação 2x usa escrita de 32 bits; os
 * outros tamanhos, vizinho mais próximo. Conversão de cor: as tabelas do bftvd (src/tv).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <Limelight.h>

#include "video.h"

static uint16_t *fb;
static int fb_stride = 800, fb_w = 800, fb_h = 480;
static int out_x, out_y, out_w, out_h;
static atomic_int show = 1;
static AVCodecContext *vc;
static AVPacket *pk;
static AVFrame *fr;
static uint8_t *buf;
static size_t buf_n;
static int16_t t_rv[256], t_gu[256], t_gv[256], t_bu[256];
static uint8_t clip[1024];

/* estatística (lida pela thread principal) */
static atomic_ulong n_dec, n_err;
static atomic_ullong dec_us;

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

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

int video_open(int x, int y, int w, int h)
{
    tables();
    if(!fb) {
        int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
        if(fd < 0) return -1;
        struct fb_fix_screeninfo fx;
        struct fb_var_screeninfo vi;
        if(ioctl(fd, FBIOGET_FSCREENINFO, &fx) < 0) { close(fd); return -1; }
        if(ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0) { fb_w = (int)vi.xres; fb_h = (int)vi.yres; }
        size_t off = fx.smem_start & 0xfff;                   /* simplefb não alinhado (docs) */
        uint8_t *m = mmap(NULL, fx.smem_len + off, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if(m == MAP_FAILED) return -1;
        fb = (uint16_t *)(m + off);
        fb_stride = (int)fx.line_length / 2;
    }
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(w > fb_w - x) w = fb_w - x;
    if(h > fb_h - y) h = fb_h - y;
    out_x = x; out_y = y; out_w = w; out_h = h;
    return w > 0 && h > 0 ? 0 : -1;
}

void video_show(int on) { atomic_store(&show, on); }

static void blit2x(const AVFrame *f)
{
    int w = f->width, h = f->height;
    if(w > out_w / 2) w = out_w / 2 & ~1;
    if(h > out_h / 2) h = out_h / 2 & ~1;
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
            d0[x] = d1[x] = a; d0[x + 1] = d1[x + 1] = b;
            d2[x] = d3[x] = c; d2[x + 1] = d3[x + 1] = d;
        }
    }
}

static void blit_scaled(const AVFrame *f)
{
    static uint16_t line[1024];
    static int xmap[1024];
    int w = f->width, h = f->height, W = out_w, H = out_h;
    if(w > 1024 || W > 1024 || w <= 0 || h <= 0) return;
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
        uint16_t *d = fb + (out_y + y) * fb_stride + out_x;
        for(int x = 0; x < W; x++) d[x] = line[xmap[x]];
    }
}

static void blit(const AVFrame *f)
{
    if(!fb || !atomic_load(&show) || f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) return;
    bool aligned = !(((uintptr_t)(fb + out_y * fb_stride + out_x) | (uintptr_t)(fb_stride * 2)) & 3);
    if(aligned && f->width * 2 == out_w && f->height * 2 == out_h) blit2x(f);
    else blit_scaled(f);
}

int video_setup(int w, int h)
{
    (void)w; (void)h;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!c) return -1;
    vc = avcodec_alloc_context3(c);
    if(!vc) return -1;
    vc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    vc->flags2 |= AV_CODEC_FLAG2_FAST;
    vc->skip_loop_filter = AVDISCARD_ALL;
    vc->thread_count = 1;
    if(avcodec_open2(vc, c, NULL) < 0) return -1;
    pk = av_packet_alloc();
    fr = av_frame_alloc();
    return pk && fr ? 0 : -1;
}

int video_submit(PDECODE_UNIT du)
{
    if(!vc) return DR_OK;
    size_t need = (size_t)du->fullLength + AV_INPUT_BUFFER_PADDING_SIZE;
    if(need > buf_n) {
        uint8_t *nb = realloc(buf, need);
        if(!nb) return DR_NEED_IDR;
        buf = nb;
        buf_n = need;
    }
    size_t n = 0;
    for(PLENTRY e = du->bufferList; e; e = e->next) {
        if(n + (size_t)e->length > (size_t)du->fullLength) break;
        memcpy(buf + n, e->data, (size_t)e->length);
        n += (size_t)e->length;
    }
    memset(buf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    uint64_t t0 = now_us();
    pk->data = buf;
    pk->size = (int)n;
    if(avcodec_send_packet(vc, pk) < 0) { atomic_fetch_add(&n_err, 1); return DR_NEED_IDR; }
    int got = 0;
    while(avcodec_receive_frame(vc, fr) == 0) {
        if(fr->decode_error_flags) { atomic_fetch_add(&n_err, 1); av_frame_unref(fr); return DR_NEED_IDR; }
        blit(fr);
        av_frame_unref(fr);
        got = 1;
    }
    if(got) {
        atomic_fetch_add(&n_dec, 1);
        atomic_fetch_add(&dec_us, now_us() - t0);
    }
    return DR_OK;
}

void video_stats(unsigned long *frames, unsigned long *errors, unsigned long long *us)
{
    *frames = atomic_exchange(&n_dec, 0);
    *errors = atomic_exchange(&n_err, 0);
    *us = atomic_exchange(&dec_us, 0);
}

void video_close(void)
{
    if(vc) avcodec_free_context(&vc);
    if(pk) av_packet_free(&pk);
    if(fr) av_frame_free(&fr);
}
