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
#include <pthread.h>
#include <stdbool.h>
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
static int full_out_h;                                  /* altura da área (sem o CLIP) */
static atomic_int show = 1;
static const uint8_t *dirty_rows;                       /* relay: linhas de origem que mudaram */
static atomic_int force_full = 1;                        /* redesenho inteiro no próximo quadro */
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
    atomic_store(&force_full, 1);
    return w > 0 && h > 0 ? 0 : -1;
}

void video_show(int on) { atomic_store(&show, on); if(on) atomic_store(&force_full, 1); }

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

/* ---- área de visão: sem faixas pretas, sem esticar, com zoom ----
 * O Sunshine põe faixas pretas DENTRO do vídeo quando a tela do computador não tem a proporção
 * pedida (o desktop com dois monitores, por exemplo). A cada 15 quadros as bordas são varridas e
 * as linhas/colunas pretas (brilho <= 24 em toda a amostra) ficam de fora. O que sobra é ampliado
 * COBRINDO a área de exibição (sem esticar: o excesso das bordas fica de fora), com zoom
 * (100..400 %) e centro móvel. O mesmo recorte converte o toque em posição no vídeo (video_map). */
static pthread_mutex_t vp_mx = PTHREAD_MUTEX_INITIALIZER;
static int fr_w, fr_h;                                 /* tamanho do quadro */
static int ct_x, ct_y, ct_w, ct_h;                     /* conteúdo, sem as faixas pretas */
static double vp_x, vp_y, vp_w, vp_h;                  /* recorte mostrado, em pontos do quadro */
static int zoom_pct = 100;
static double cen_x = -1, cen_y = -1;                  /* centro pedido (-1: o do conteúdo) */
static int passthrough;                                /* relay: o quadro já vem cortado e no tamanho */

void video_passthrough(int on) { passthrough = on; }

static bool black_row(const AVFrame *f, int y, int x0, int x1)
{
    const uint8_t *r = f->data[0] + y * f->linesize[0];
    for(int x = x0; x < x1; x += 4) if(r[x] > 24) return false;
    return true;
}

static bool black_col(const AVFrame *f, int x, int y0, int y1)
{
    for(int y = y0; y < y1; y += 4) if(f->data[0][y * f->linesize[0] + x] > 24) return false;
    return true;
}

static void autocrop(const AVFrame *f)
{
    int w = f->width, h = f->height, t = 0, b = h - 1, l = 0, r = w - 1;
    while(t < h / 4 && black_row(f, t, 0, w)) t++;
    while(b > h * 3 / 4 && black_row(f, b, 0, w)) b--;
    while(l < w / 4 && black_col(f, l, t, b)) l++;
    while(r > w * 3 / 4 && black_col(f, r, t, b)) r--;
    if(t >= h / 4 && b <= h * 3 / 4) { t = 0; b = h - 1; }   /* quadro todo escuro: não corta */
    pthread_mutex_lock(&vp_mx);
    ct_x = l & ~1; ct_y = t & ~1; ct_w = (r - ct_x + 1) & ~1; ct_h = (b - ct_y + 1) & ~1;
    pthread_mutex_unlock(&vp_mx);
}

/* com vp_mx: o recorte para a área W x H */
static void viewport(int W, int H)
{
    if(ct_w <= 0 || ct_h <= 0) { ct_x = ct_y = 0; ct_w = fr_w; ct_h = fr_h; }
    double vw = ct_w, vh = vw * H / W;                   /* cobre: corta o lado que sobra */
    if(vh > ct_h) { vh = ct_h; vw = vh * W / H; }
    vw = vw * 100 / zoom_pct;
    vh = vh * 100 / zoom_pct;
    double cx = cen_x < 0 ? ct_x + ct_w / 2.0 : cen_x, cy = cen_y < 0 ? ct_y + ct_h / 2.0 : cen_y;
    double x = cx - vw / 2, y = cy - vh / 2;
    if(x < ct_x) x = ct_x;
    if(y < ct_y) y = ct_y;
    if(x + vw > ct_x + ct_w) x = ct_x + ct_w - vw;
    if(y + vh > ct_y + ct_h) y = ct_y + ct_h - vh;
    if(cen_x >= 0) { cen_x = x + vw / 2; cen_y = y + vh / 2; }   /* o centro fica onde coube */
    vp_x = x; vp_y = y; vp_w = vw; vp_h = vh;
}

void video_zoom(int pct)
{
    pthread_mutex_lock(&vp_mx);
    if(pct < 100) pct = 100;
    if(pct > 400) pct = 400;
    if(pct == 100) cen_x = cen_y = -1;
    else if(cen_x < 0) { cen_x = vp_x + vp_w / 2; cen_y = vp_y + vp_h / 2; }
    zoom_pct = pct;
    pthread_mutex_unlock(&vp_mx);
}

/* arrastar com zoom: a imagem acompanha o dedo (dx, dy em pontos da área l x a) */
void video_pan(int dx, int dy, int aw, int ah)
{
    pthread_mutex_lock(&vp_mx);
    if(zoom_pct > 100 && aw > 0 && ah > 0) {
        if(cen_x < 0) { cen_x = vp_x + vp_w / 2; cen_y = vp_y + vp_h / 2; }
        cen_x -= dx * vp_w / aw;
        cen_y -= dy * vp_h / ah;
    }
    pthread_mutex_unlock(&vp_mx);
}

/* um ponto da área (ax, ay em l x a) em pontos do quadro; 0 sem quadro ainda */
int video_map(int ax, int ay, int aw, int ah, int *fx, int *fy, int *fw, int *fh)
{
    pthread_mutex_lock(&vp_mx);
    int ok = fr_w > 0 && aw > 0 && ah > 0;
    if(ok) {
        *fx = (int)(vp_x + (ax + 0.5) * vp_w / aw);
        *fy = (int)(vp_y + (ay + 0.5) * vp_h / ah);
        *fw = fr_w; *fh = fr_h;
    }
    pthread_mutex_unlock(&vp_mx);
    return ok;
}

/* Ampliação suave e barata (24/11/2026: a bilinear por ponto custava ~85 ms por quadro 800x450 no
 * ARM11 e derrubava 28 qps para 7):
 *  1. a cor é convertida uma vez por ponto de ORIGEM, só no trecho do recorte;
 *  2. cada linha de origem é ampliada na horizontal uma vez (as duas últimas ficam guardadas);
 *  3. o meio-termo entre dois pontos é a média no próprio RGB565 ((a & 0xF7DE) + (b & 0xF7DE)) >> 1,
 *     sem multiplicação: posições a 1/4..3/4 entre dois pontos de origem viram a média, as outras
 *     o mais próximo;
 *  4. as linhas de saída vão ao framebuffer (sem cache) dois pontos por escrita de 32 bits. */
#define AVG16(a, b) (uint16_t)((((a) & 0xF7DEu) >> 1) + (((b) & 0xF7DEu) >> 1))
#define AVG32(a, b) ((((a) & 0xF7DEF7DEu) >> 1) + (((b) & 0xF7DEF7DEu) >> 1))

static int16_t hx0[1024];                              /* ponto de origem de cada x de saída */
static uint8_t hmode[1024];                            /* 0: o ponto hx0; 1: média com o seguinte */
static int hsx0, hsx1;                                 /* trecho de origem usado */
static uint32_t hl[2][512];                            /* duas linhas já ampliadas (pares de pontos) */
static int hl_row[2] = { -1, -1 };

/* posição de origem (em 1/256) da saída i de n, num trecho [o, o + len) */
static int src_pos(int i, int n, double o, double len)
{
    return (int)((o + (i + 0.5) * len / n - 0.5) * 256);
}

static void sm_tables(int w, int W, double sx, double sw)
{
    hsx0 = w; hsx1 = 0;
    for(int x = 0; x < W; x++) {
        int p = src_pos(x, W, sx, sw);
        if(p < 0) p = 0;
        int x0 = p >> 8, fr = p & 255;
        if(x0 >= w - 1) { x0 = w - 1; fr = 0; }
        if(fr >= 192) { x0++; fr = 0; }
        hx0[x] = (int16_t)x0;
        hmode[x] = fr >= 64;
        if(x0 < hsx0) hsx0 = x0;
        if(x0 + 1 > hsx1) hsx1 = x0 + 1;
    }
    hsx0 &= ~1;
    if(hsx1 > w - 1) hsx1 = w - 1;
}

static const uint32_t *hline(const AVFrame *f, int sy, int slot, int W)
{
    if(hl_row[slot] == sy) return hl[slot];
    if(hl_row[slot ^ 1] == sy) return hl[slot ^ 1];
    static uint16_t src[1032];
    const uint8_t *yy = f->data[0] + sy * f->linesize[0];
    const uint8_t *u = f->data[1] + (sy / 2) * f->linesize[1], *v = f->data[2] + (sy / 2) * f->linesize[2];
    for(int x = hsx0; x <= hsx1; x += 2) {
        int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
        src[x] = px(yy[x], rv, g, bu);
        src[x + 1] = px(yy[x + 1], rv, g, bu);
    }
    src[hsx1 + 1] = src[hsx1];
    uint16_t *o = (uint16_t *)hl[slot];
    for(int x = 0; x < W; x++) {
        int x0 = hx0[x];
        o[x] = hmode[x] ? AVG16(src[x0], src[x0 + 1]) : src[x0];
    }
    hl_row[slot] = sy;
    return hl[slot];
}

static void blit_smooth(const AVFrame *f)
{
    static unsigned long n;
    int w = f->width, h = f->height, W = out_w & ~1, H = out_h;
    if(w > 1022 || W > 1024 || w < 2 || h < 2) return;
    if(passthrough) {                                    /* relay: o quadro inteiro, sem corte nem zoom */
        pthread_mutex_lock(&vp_mx);
        fr_w = w; fr_h = h; ct_x = ct_y = 0; ct_w = w; ct_h = h; zoom_pct = 100; cen_x = cen_y = -1;
        pthread_mutex_unlock(&vp_mx);
    } else if(n++ % 15 == 0 || w != fr_w || h != fr_h) {
        if(w != fr_w || h != fr_h) { pthread_mutex_lock(&vp_mx); fr_w = w; fr_h = h; ct_w = 0; pthread_mutex_unlock(&vp_mx); }
        autocrop(f);
    }
    pthread_mutex_lock(&vp_mx);
    viewport(W, full_out_h);                            /* a proporção da área inteira, não a cortada */
    double sx = vp_x, sy0 = vp_y, sw = vp_w, sh = vp_h;
    pthread_mutex_unlock(&vp_mx);
    sm_tables(w, W, sx, sw);
    hl_row[0] = hl_row[1] = -1;
    int words = W / 2;
    for(int y = 0; y < H; y++) {
        int p = src_pos(y, full_out_h, sy0, sh);
        if(p < 0) p = 0;
        int y0 = p >> 8, fr = p & 255;
        if(y0 >= h - 1) { y0 = h - 1; fr = 0; }
        if(fr >= 192) { y0++; fr = 0; }
        uint32_t *d = (uint32_t *)(fb + (out_y + y) * fb_stride + out_x);
        const uint32_t *a = hline(f, y0, y0 & 1, W);
        if(fr >= 64) {
            const uint32_t *b = hline(f, y0 + 1, (y0 + 1) & 1, W);
            for(int i = 0; i < words; i++) d[i] = AVG32(a[i], b[i]);
        } else {
            for(int i = 0; i < words; i++) d[i] = a[i];
        }
    }
}

/* Ampliação 2x suave e rápida (relay: o PC manda exatamente a metade da área): sem tabelas de
 * posição. Cada linha de origem vira uma linha dobrada em palavras de 32 bits (ponto, média com o
 * seguinte); as linhas de saída pares copiam, as ímpares são a média de duas linhas dobradas.
 * Medido no FOSTON: a genérica 320 -> 800x450 gastava ~23 ms por quadro. */

static void blit2x_smooth(const AVFrame *f)
{
    static uint32_t dl[2][512], avg[512];               /* linhas dobradas e a média (com cache) */
    int w = f->width, h = f->height, words = w;          /* w pontos de origem -> w palavras */
    if(w > 512 || w < 2 || h < 2) return;
    int H = out_h;                                       /* linhas a pintar (o CLIP pode cortar) */
    const uint8_t *dr = atomic_exchange(&force_full, 0) ? NULL : dirty_rows;
    int cur = 0;
    for(int sy = 0; sy < h; sy++) {
        /* a linha dobrada sy entra nas saídas 2sy-1, 2sy e 2sy+1 */
        bool here = !dr || dr[sy], before = sy > 0 && (!dr || dr[sy - 1]), after = sy + 1 < h && dr && dr[sy + 1];
        if(!here && !before && !after) { cur ^= 1; continue; }
        uint32_t *o = dl[cur];
        const uint8_t *yy = f->data[0] + sy * f->linesize[0];
        const uint8_t *u = f->data[1] + (sy / 2) * f->linesize[1], *v = f->data[2] + (sy / 2) * f->linesize[2];
        uint16_t prev = 0;
        for(int x = 0; x < w; x += 2) {
            int rv = t_rv[v[x / 2]], g = t_gu[u[x / 2]] + t_gv[v[x / 2]], bu = t_bu[u[x / 2]];
            uint16_t a = px(yy[x], rv, g, bu), b = px(yy[x + 1], rv, g, bu);
            if(x) o[x - 1] |= (uint32_t)AVG16(prev, a) << 16;   /* o meio-termo do par anterior */
            o[x] = (uint32_t)a | (uint32_t)AVG16(a, b) << 16;
            o[x + 1] = b;
            prev = b;
        }
        o[w - 1] |= (uint32_t)prev << 16;               /* a borda repete o último ponto */
        int oy = sy * 2;
        if(sy > 0 && (here || before) && oy - 1 < H) {  /* linha ímpar anterior: média das duas */
            const uint32_t *a = dl[cur ^ 1];
            for(int i = 0; i < words; i++) avg[i] = AVG32(a[i], o[i]);
            memcpy(fb + (out_y + oy - 1) * fb_stride + out_x, avg, (size_t)words * 4);   /* em rajada */
        }
        if(here && oy < H) memcpy(fb + (out_y + oy) * fb_stride + out_x, o, (size_t)words * 4);
        cur ^= 1;
    }
    int last = h * 2 - 1;                                /* a última linha ímpar repete a última */
    if(last < H && (!dr || dr[h - 1])) memcpy(fb + (out_y + last) * fb_stride + out_x, dl[cur ^ 1], (size_t)words * 4);
}

static atomic_int smooth = 1;
static atomic_int clip_rows;                         /* linhas de baixo que não são desenhadas (barra) */

void video_smooth(int on) { atomic_store(&smooth, on); }
void video_clip(int rows) { atomic_store(&clip_rows, rows < 0 ? 0 : rows); atomic_store(&force_full, 1); }

static void blit(const AVFrame *f)
{
    if(!fb || !atomic_load(&show) || f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) return;
    int full_h = out_h, c = atomic_load(&clip_rows);
    full_out_h = full_h;
    if(c > 0 && c < out_h) out_h -= c;                  /* a barra do rodapé por cima: não pinta ali */
    bool aligned = !(((uintptr_t)(fb + out_y * fb_stride + out_x) | (uintptr_t)(fb_stride * 2)) & 3);
    if(atomic_load(&smooth) && aligned && passthrough && f->width * 2 == out_w && f->height * 2 == full_h)
        blit2x_smooth(f);                                /* relay: metade exata da área */
    else if(atomic_load(&smooth) && aligned) blit_smooth(f);
    else {
        if(aligned && f->width * 2 == out_w && f->height * 2 == full_h && !c) blit2x(f);
        else blit_scaled(f);
    }
    out_h = full_h;
}

static int want_lf;

void video_loop_filter(int on) { want_lf = on; }

int video_setup(int w, int h)
{
    (void)w; (void)h;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!c) return -1;
    vc = avcodec_alloc_context3(c);
    if(!vc) return -1;
    vc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    /* filtro de desblocagem: −20..25 % de quadros/s no ARM11 (medido), mas sem ele os blocos do
     * H.264 aparecem; ligado nas qualidades média e alta */
    if(!want_lf) { vc->flags2 |= AV_CODEC_FLAG2_FAST; vc->skip_loop_filter = AVDISCARD_ALL; }
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

/* relay: desenha um quadro YUV 4:2:0 contíguo (Y, depois U e V) já no tamanho da área */
void video_draw_yuv(const uint8_t *buf, int w, int h, const uint8_t *dirty)
{
    dirty_rows = dirty;
    static AVFrame *f;
    if(!f) f = av_frame_alloc();
    if(!f) return;
    f->width = w;
    f->height = h;
    f->format = AV_PIX_FMT_YUV420P;
    f->data[0] = (uint8_t *)buf;
    f->data[1] = (uint8_t *)buf + w * h;
    f->data[2] = (uint8_t *)buf + w * h + (w / 2) * ((h + 1) / 2);   /* altura ímpar: (h + 1) / 2 */
    f->linesize[0] = w;
    f->linesize[1] = f->linesize[2] = w / 2;
    blit(f);
    atomic_fetch_add(&n_dec, 1);
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
