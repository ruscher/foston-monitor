/* relay.c — o bfrp no PC (build do PC, -DBFRP_RELAY): o trabalho pesado sai do FOSTON.
 *
 * O ARM11 do FOSTON leva ~35 ms para decodificar UM quadro H.264 de 320x180 (medido): nem com a
 * imagem mínima ele passava de ~28 qps, e o som (Opus) tomava mais 15 % de CPU. No relay o PC é o
 * cliente do Sunshine (no próprio PC, 127.0.0.1): pede 1280x720 a 60 qps, decodifica, corta as
 * faixas pretas, aplica o zoom, reduz com o redutor do FFmpeg (área: nítido) exatamente ao tamanho
 * da tela do FOSTON e manda pela rede USB YUV 4:2:0 + LZ4; o FOSTON só descomprime e desenha
 * (client.c). O som vai decodificado: PCM mono 32 kHz.
 *
 * Fluxo: um quadro por vez (o FOSTON confirma com Q); a thread de envio sempre manda o MAIS NOVO,
 * então não há fila e o atraso não cresce. A decodificação nunca espera a rede.
 *
 * Implementa a mesma interface de video.h e audio.h que o FOSTON usa (bfrp.c não muda).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <lz4.h>
#include <lz4hc.h>
#include <opus/opus_multistream.h>
#include <Limelight.h>

#include "video.h"
#include "audio.h"
#include "proto.h"
#include "relay.h"

/* ------------------------------------------------------------ rede */

static int cfd = -1;
static pthread_mutex_t tx_mx = PTHREAD_MUTEX_INITIALIZER;

void relay_attach(int fd) { pthread_mutex_lock(&tx_mx); cfd = fd; pthread_mutex_unlock(&tx_mx); }

int relay_send(char type, const void *data, uint32_t len)
{
    pthread_mutex_lock(&tx_mx);
    int r = cfd >= 0 ? proto_send(cfd, type, data, len) : -1;
    pthread_mutex_unlock(&tx_mx);
    return r;
}

/* ------------------------------------------------------------ área de visão (como video.c) */

static pthread_mutex_t vp_mx = PTHREAD_MUTEX_INITIALIZER;
static int area_w = 800, area_h = 384;               /* área do vídeo no FOSTON */
static int target_w = 320;                            /* largura do quadro enviado */
static int fr_w, fr_h, ct_x, ct_y, ct_w, ct_h;
static double vp_x, vp_y, vp_w, vp_h;
static int zoom_pct = 100;
static double cen_x = -1, cen_y = -1;
static atomic_int show = 1;

void relay_set_target(int w) { if(w >= 160 && w <= 800) target_w = w & ~7; }   /* múltiplo de 8: a cor em palavras de 32 bits */

int video_open(int x, int y, int w, int h)
{
    (void)x; (void)y;
    pthread_mutex_lock(&vp_mx);
    if(w > 0 && h > 0) { area_w = w; area_h = h; }
    pthread_mutex_unlock(&vp_mx);
    return 0;
}

void video_show(int on) { atomic_store(&show, on); }
void video_loop_filter(int on) { (void)on; }
void video_smooth(int on) { (void)on; }
void video_clip(int rows) { (void)rows; }

/* Faixa preta: quase toda escura. Tolera uns pontos claros (menos de 1 em 32 amostras): um
 * contador de FPS ou um ícone no canto da faixa (monitor ultrawide com o jogo em 16:9 no meio)
 * não pode fazer a faixa inteira aparecer e o lado oposto do jogo ser cortado. */
static bool black_row(const AVFrame *f, int y)
{
    const uint8_t *r = f->data[0] + y * f->linesize[0];
    int bright = 0, n = 0;
    for(int x = 0; x < f->width; x += 4, n++) bright += r[x] > 24;
    return bright * 32 < n;
}

static bool black_col(const AVFrame *f, int x, int y0, int y1)
{
    int bright = 0, n = 0;
    for(int y = y0; y < y1; y += 4, n++) bright += f->data[0][y * f->linesize[0] + x] > 24;
    return bright * 32 < n;
}

static void autocrop(const AVFrame *f)
{
    int w = f->width, h = f->height, t = 0, b = h - 1, l = 0, r = w - 1;
    while(t < h / 4 && black_row(f, t)) t++;
    while(b > h * 3 / 4 && black_row(f, b)) b--;
    if(t >= h / 4 && b <= h * 3 / 4) return;            /* quadro (quase) todo preto: fica o corte de antes */
    while(l < w / 4 && black_col(f, l, t, b)) l++;
    while(r > w * 3 / 4 && black_col(f, r, t, b)) r--;
    if(l >= w / 4 && r <= w * 3 / 4) return;
    pthread_mutex_lock(&vp_mx);
    ct_x = l & ~1; ct_y = t & ~1; ct_w = (r - ct_x + 1) & ~1; ct_h = (b - ct_y + 1) & ~1;
    pthread_mutex_unlock(&vp_mx);
}

static void viewport(void)                            /* com vp_mx */
{
    if(ct_w <= 0 || ct_h <= 0) { ct_x = ct_y = 0; ct_w = fr_w; ct_h = fr_h; }
    double vw = ct_w, vh = vw * area_h / area_w;
    if(vh > ct_h) { vh = ct_h; vw = vh * area_w / area_h; }
    vw = vw * 100 / zoom_pct;
    vh = vh * 100 / zoom_pct;
    double cx = cen_x < 0 ? ct_x + ct_w / 2.0 : cen_x, cy = cen_y < 0 ? ct_y + ct_h / 2.0 : cen_y;
    double x = cx - vw / 2, y = cy - vh / 2;
    if(x < ct_x) x = ct_x;
    if(y < ct_y) y = ct_y;
    if(x + vw > ct_x + ct_w) x = ct_x + ct_w - vw;
    if(y + vh > ct_y + ct_h) y = ct_y + ct_h - vh;
    if(cen_x >= 0) { cen_x = x + vw / 2; cen_y = y + vh / 2; }
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

/* ------------------------------------------------------------ vídeo */

static AVCodecContext *vc;
static AVPacket *pk;
static AVFrame *fr;
static uint8_t *abuf;
static size_t abuf_n;
static struct SwsContext *sws;
static int sws_sw, sws_sh, sws_dw, sws_dh;

/* quadro pronto para o FOSTON (o mais novo) e a thread que manda */
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *q_yuv;
static size_t q_cap;
static int q_w, q_h;
static bool q_ready, tx_run;
/* quadros a caminho sem confirmação: com 2, o PC prepara e manda o próximo enquanto o FOSTON
 * desenha o atual (com 1, cada lado esperava o outro) */
#define IN_FLIGHT 2
static int in_flight;
static pthread_t tx_th;
static atomic_ulong n_dec, n_sent, n_acked, n_err;
static atomic_ullong bytes_sent;

static atomic_int want_key;
void relay_key(void) { atomic_store(&want_key, 1); }

void relay_ack(void)
{
    pthread_mutex_lock(&q_mx);
    if(in_flight > 0) in_flight--;
    atomic_fetch_add(&n_acked, 1);
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mx);
}

/* Menos bytes pelo USB (no FOSTON o controlador USB custa ~40 % da CPU a 1,3 MB/s, medido) e
 * menos trabalho no FOSTON (ARM11 a 468 MHz):
 *  - quantização: brilho com 6 bits e cor com 5 (a tela RGB565 não mostra mais que isso) deixa
 *    as áreas lisas repetidas; com histerese contra o quadro anterior (uma variação menor que
 *    um degrau fica como estava: o ruído do vídeo não "muda" a linha);
 *  - delta por pares de linhas: um par (2 linhas de brilho + a linha de U e a de V que elas
 *    usam) só vai se mudou, em XOR com o anterior; o FOSTON aplica e redesenha só esses pares;
 *  - quadro inteiro a cada 60, quando o tamanho muda, quando o FOSTON pede (R) ou quando quase
 *    tudo mudou (sai menor e o FOSTON não precisa aplicar o XOR);
 *  - LZ4 HC: comprime mais no PC, descomprime igual no FOSTON.
 * Cabeçalho: l a (u16) + flags (bit 0 delta, bit 1 soma) [+ soma u32] [+ máscara dos pares,
 * 1 bit por par, se delta] + LZ4. */
static uint8_t *z_tmp;
static size_t z_tmp_cap;

static int compress_into(const uint8_t *src, size_t n, uint8_t **out, size_t *cap)
{
    size_t zmax = (size_t)LZ4_compressBound((int)n);
    if(zmax > *cap) { free(*out); *out = malloc(zmax); *cap = *out ? zmax : 0; }
    if(!*out) return -1;
    return LZ4_compress_HC((const char *)src, (char *)*out, (int)n, (int)zmax, 4);
}

static void *tx_thread(void *a)
{
    (void)a;
    static uint8_t *local, *prev, *x, *zk, *zd;
    static size_t local_cap, prev_cap, x_cap, zk_cap, zd_cap;
    static int prev_w, prev_h, since_key;
    static bool have_prev;
    static unsigned long nfr;
    pthread_mutex_lock(&q_mx);
    while(tx_run) {
        if(!q_ready || in_flight >= IN_FLIGHT) { pthread_cond_wait(&q_cv, &q_mx); continue; }
        int w = q_w, h = q_h;
        size_t n = yuv_size(w, h);
        if(n > local_cap) { free(local); local = malloc(n); local_cap = local ? n : 0; }
        if(n > prev_cap) { free(prev); prev = malloc(n); prev_cap = prev ? n : 0; have_prev = false; }
        if(n > x_cap) { free(x); x = malloc(n); x_cap = x ? n : 0; }
        if(!local || !prev || !x) { q_ready = false; continue; }
        memcpy(local, q_yuv, n);
        q_ready = false;
        in_flight++;
        pthread_mutex_unlock(&q_mx);

        bool delta = have_prev && prev_w == w && prev_h == h && (h + 1) / 2 <= 2048 && since_key < 60 && !atomic_exchange(&want_key, 0);
        size_t ny = (size_t)w * h;
        for(size_t i = 0; i < n; i++) {                  /* 6 bits de brilho, 5 de cor (arredondado) */
            bool luma = i < ny;
            int raw = local[i];
            if(delta) {                                  /* histerese: menos de um degrau, fica */
                int d = raw - prev[i];
                if(d < 0) d = -d;
                if(d <= (luma ? 3 : 6)) { local[i] = prev[i]; continue; }
            }
            int v = raw + (luma ? 2 : 4);
            if(v > 255) v = 255;
            local[i] = (uint8_t)(v & (luma ? 0xFC : 0xF8));
        }

        int cw = w / 2, chh = (h + 1) / 2;
        uint8_t mask[256];                               /* até 2048 pares */
        size_t xn = 0;
        int dirty = 0;
        if(delta && chh <= 2048) {                       /* os pares que mudaram, em XOR, em sequência */
            memset(mask, 0, (size_t)(chh + 7) / 8);
            const uint8_t *lu = local + ny, *lv = lu + (size_t)cw * chh, *pu = prev + ny, *pv = pu + (size_t)cw * chh;
            for(int r = 0; r < chh; r++) {
                int y0 = 2 * r, ly = y0 + 1 < h ? 2 : 1;
                size_t ro = (size_t)y0 * w, co = (size_t)r * cw;
                if(!memcmp(local + ro, prev + ro, (size_t)ly * w) && !memcmp(lu + co, pu + co, (size_t)cw) &&
                   !memcmp(lv + co, pv + co, (size_t)cw)) continue;
                mask[r / 8] |= (uint8_t)(1 << (r % 8));
                dirty++;
                for(size_t i = 0; i < (size_t)ly * w; i++) x[xn++] = local[ro + i] ^ prev[ro + i];
                for(int i = 0; i < cw; i++) x[xn++] = lu[co + i] ^ pu[co + i];
                for(int i = 0; i < cw; i++) x[xn++] = lv[co + i] ^ pv[co + i];
            }
        }
        memcpy(prev, local, n);
        have_prev = true; prev_w = w; prev_h = h;

        int ok = 0, zn = -1;
        const uint8_t *z = NULL;
        bool sent_delta = false;
        if(delta && dirty == 0) zn = 0, sent_delta = true;   /* nada mudou: só o cabeçalho */
        else if(delta) {
            zn = compress_into(x, xn, &zd, &zd_cap);
            z = zd;
            sent_delta = zn > 0;
            if(dirty * 10 > chh * 8) {                   /* quase tudo mudou: o inteiro sai melhor? */
                int kn = compress_into(local, n, &zk, &zk_cap);
                if(kn > 0 && (zn <= 0 || kn < zn + zn / 4)) { zn = kn; z = zk; sent_delta = false; }
            }
        }
        if(!delta || (zn <= 0 && !sent_delta)) { zn = compress_into(local, n, &zk, &zk_cap); z = zk; sent_delta = false; }
        if(!sent_delta) since_key = 0;
        since_key++;

        if(zn >= 0) {
            size_t ml = sent_delta ? (size_t)(chh + 7) / 8 : 0;
            bool with_sum = !sent_delta || nfr++ % 30 == 0; /* conferência: inteiros e 1 a cada 30 */
            size_t hl = 5 + (with_sum ? 4 : 0) + ml;
            size_t total = hl + (size_t)zn;
            if(total > z_tmp_cap) { free(z_tmp); z_tmp = malloc(total); z_tmp_cap = z_tmp ? total : 0; }
            if(z_tmp) {
                uint8_t *o = z_tmp;
                o[0] = (uint8_t)w; o[1] = (uint8_t)(w >> 8); o[2] = (uint8_t)h; o[3] = (uint8_t)(h >> 8);
                o[4] = (uint8_t)((sent_delta ? 1 : 0) | (with_sum ? 2 : 0));
                o += 5;
                if(with_sum) { uint32_t sum = yuv_sum(local, n); for(int i = 0; i < 4; i++) *o++ = (uint8_t)(sum >> 8 * i); }
                if(ml) { memcpy(o, mask, ml); o += ml; }
                if(zn) memcpy(o, z, (size_t)zn);
                if(relay_send('V', z_tmp, (uint32_t)total) == 0) {
                    ok = 1;
                    atomic_fetch_add(&n_sent, 1);
                    atomic_fetch_add(&bytes_sent, (unsigned long long)total + 5);
                }
            }
        }
        pthread_mutex_lock(&q_mx);
        if(!ok) { if(in_flight > 0) in_flight--; have_prev = false; }   /* não chegou: o próximo vai inteiro */
    }
    pthread_mutex_unlock(&q_mx);
    return NULL;
}

int video_setup(int w, int h)
{
    (void)w; (void)h;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!c) return -1;
    vc = avcodec_alloc_context3(c);
    vc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    vc->thread_count = 1;                                /* um quadro por vez: menor atraso */
    if(avcodec_open2(vc, c, NULL) < 0) return -1;
    pk = av_packet_alloc();
    fr = av_frame_alloc();
    pthread_mutex_lock(&q_mx);
    q_ready = false;
    in_flight = 0;
    tx_run = true;
    pthread_mutex_unlock(&q_mx);
    pthread_create(&tx_th, NULL, tx_thread, NULL);
    return 0;
}

/* recorta (área de visão) e reduz para o tamanho do FOSTON; deixa como o quadro mais novo */
static void handoff(const AVFrame *f)
{
    static unsigned long nf;
    if(nf++ % 15 == 0 || f->width != fr_w || f->height != fr_h) {
        if(f->width != fr_w || f->height != fr_h) {
            pthread_mutex_lock(&vp_mx); fr_w = f->width; fr_h = f->height; ct_w = 0; pthread_mutex_unlock(&vp_mx);
        }
        autocrop(f);
    }
    pthread_mutex_lock(&vp_mx);
    viewport();
    int sx = (int)vp_x & ~1, sy = (int)vp_y & ~1, sw = (int)(vp_w + 0.5) & ~1, sh = (int)(vp_h + 0.5) & ~1;
    /* metade exata da área: o FOSTON amplia 2x pelo caminho rápido (video.c, blit2x_smooth);
     * uma largura menor pedida vale (mais leve, pelo caminho genérico) */
    int dw = area_w / 2 & ~7, dh = area_h / 2;
    if(target_w < dw) { dw = target_w; dh = (target_w * area_h / area_w) & ~1; }
    pthread_mutex_unlock(&vp_mx);
    if(sw < 16 || sh < 16 || dh < 16) return;
    if(sx + sw > f->width) sw = (f->width - sx) & ~1;
    if(sy + sh > f->height) sh = (f->height - sy) & ~1;
    if(!sws || sw != sws_sw || sh != sws_sh || dw != sws_dw || dh != sws_dh) {
        sws_freeContext(sws);
        /* área: a redução nítida (média de todos os pontos de origem); bicúbica ao ampliar com zoom */
        sws = sws_getContext(sw, sh, (enum AVPixelFormat)f->format, dw, dh, AV_PIX_FMT_YUV420P,
                             dw < sw ? SWS_AREA : SWS_BICUBIC, NULL, NULL, NULL);
        sws_sw = sw; sws_sh = sh; sws_dw = dw; sws_dh = dh;
        if(!sws) return;
    }
    const uint8_t *src[3] = { f->data[0] + sy * f->linesize[0] + sx,
                              f->data[1] + (sy / 2) * f->linesize[1] + sx / 2,
                              f->data[2] + (sy / 2) * f->linesize[2] + sx / 2 };
    size_t n = yuv_size(dw, dh);
    pthread_mutex_lock(&q_mx);
    if(n > q_cap) { free(q_yuv); q_yuv = malloc(n); q_cap = q_yuv ? n : 0; }
    if(q_yuv) {
        /* altura ímpar (225 = 450 / 2): a cor tem (h + 1) / 2 linhas */
        uint8_t *dst[3] = { q_yuv, q_yuv + dw * dh, q_yuv + dw * dh + (dw / 2) * ((dh + 1) / 2) };
        int dl[3] = { dw, dw / 2, dw / 2 };
        sws_scale(sws, src, f->linesize, 0, sh, dst, dl);
        q_w = dw; q_h = dh;
        q_ready = true;                                  /* substitui o anterior, se ainda não foi */
        pthread_cond_signal(&q_cv);
    }
    pthread_mutex_unlock(&q_mx);
}

int video_submit(PDECODE_UNIT du)
{
    if(!vc) return DR_OK;
    size_t need = (size_t)du->fullLength + AV_INPUT_BUFFER_PADDING_SIZE;
    if(need > abuf_n) { uint8_t *nb = realloc(abuf, need); if(!nb) return DR_NEED_IDR; abuf = nb; abuf_n = need; }
    size_t n = 0;
    for(PLENTRY e = du->bufferList; e; e = e->next) {
        if(n + (size_t)e->length > (size_t)du->fullLength) break;
        memcpy(abuf + n, e->data, (size_t)e->length);
        n += (size_t)e->length;
    }
    memset(abuf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pk->data = abuf;
    pk->size = (int)n;
    if(avcodec_send_packet(vc, pk) < 0) { atomic_fetch_add(&n_err, 1); return DR_NEED_IDR; }
    while(avcodec_receive_frame(vc, fr) == 0) {
        atomic_fetch_add(&n_dec, 1);
        if(atomic_load(&show)) handoff(fr);
        av_frame_unref(fr);
    }
    return DR_OK;
}

/* VSTATS do relay: quadros que o FOSTON desenhou (confirmações) */
void video_stats(unsigned long *frames, unsigned long *errors, unsigned long long *us)
{
    *frames = atomic_exchange(&n_acked, 0);
    *errors = atomic_exchange(&n_err, 0);
    *us = 0;
}

void relay_stats(unsigned long *decoded, unsigned long *sent, unsigned long long *bytes)
{
    *decoded = atomic_exchange(&n_dec, 0);
    *sent = atomic_exchange(&n_sent, 0);
    *bytes = atomic_exchange(&bytes_sent, 0);
}

void video_close(void)
{
    pthread_mutex_lock(&q_mx);
    bool was = tx_run;
    tx_run = false;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mx);
    if(was) pthread_join(tx_th, NULL);
    if(vc) avcodec_free_context(&vc);
    if(pk) av_packet_free(&pk);
    if(fr) av_frame_free(&fr);
    sws_freeContext(sws);
    sws = NULL;
}

/* ------------------------------------------------------------ som */

static OpusMSDecoder *adec;
static int achans;
static int16_t pcm[2 * 5760], mono[5760];
static atomic_int muted;
static atomic_ulong a_pkts, a_lost;

int audio_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc)
{
    (void)cfg;
    int err = 0;
    achans = oc->channelCount;
    adec = opus_multistream_decoder_create(oc->sampleRate, oc->channelCount, oc->streams, oc->coupledStreams,
                                           oc->mapping, &err);
    return adec && err == OPUS_OK ? 0 : -1;
}

/* 48 kHz -> 32 kHz mono (3 -> 2, média linear), o formato do /dev/dsp do FOSTON */
void audio_sample(char *data, int len)
{
    if(!adec) return;
    atomic_fetch_add(&a_pkts, 1);
    if(!data) atomic_fetch_add(&a_lost, 1);
    int got = opus_multistream_decode(adec, (const unsigned char *)data, data ? len : 0, pcm,
                                      (int)(sizeof pcm / sizeof pcm[0]) / achans, 0);
    if(got <= 2 || atomic_load(&muted) || !atomic_load(&show)) return;
    int n = 0;
    for(int i = 0; i + 2 < got && n + 2 <= (int)(sizeof mono / 2); i += 3) {
        int a = pcm[i * achans], b = pcm[(i + 1) * achans], c = pcm[(i + 2) * achans];
        if(achans >= 2) {
            a = (a + pcm[i * achans + 1]) / 2;
            b = (b + pcm[(i + 1) * achans + 1]) / 2;
            c = (c + pcm[(i + 2) * achans + 1]) / 2;
        }
        mono[n++] = (int16_t)((2 * a + b) / 3);
        mono[n++] = (int16_t)((b + 2 * c) / 3);
    }
    relay_send('A', mono, (uint32_t)n * 2);
}

void audio_mute(int on) { atomic_store(&muted, on); }
int audio_ok(void) { return adec != NULL; }

void audio_stats(unsigned long *pkts, unsigned long *lost, unsigned long *underruns)
{
    *pkts = atomic_exchange(&a_pkts, 0);
    *lost = atomic_exchange(&a_lost, 0);
    *underruns = 0;
}

void audio_close(void)
{
    if(adec) { opus_multistream_decoder_destroy(adec); adec = NULL; }
}
