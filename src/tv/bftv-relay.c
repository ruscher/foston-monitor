/* bftv-relay — a TV do FOSTON decodificada no PC (tvrelay.h). Roda no PC (serviço do usuário,
 * tools/bigfoston/tv-relay-install.sh): conecta no FOSTON quando o app TV está tocando, recebe o
 * TS da antena, decodifica o H.264 inteiro (todos os quadros, com o filtro de desblocagem, que o
 * FOSTON precisa desligar) e o HE-AAC, reduz/amplia o quadro com Lanczos para a metade do
 * retângulo do vídeo (o FOSTON amplia 2x), e manda quadros yuvz e PCM mono 32 kHz com o horário.
 *
 *   bftv-relay [--host 192.168.7.2] [--port 48122] [-v]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "tvrelay.h"
#include "yuvz.h"

static const char *host = "192.168.7.2";
static int port = TVRELAY_PORT, verbose;
static int sock = -1, pipe_r = -1, pipe_w = -1;
static atomic_int tgt_w, tgt_h, want_key, stopping;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int connect_to(void)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if(connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(s); return -1; }
    struct pollfd p = { s, POLLOUT, 0 };
    int err = 0;
    socklen_t el = sizeof err;
    if(poll(&p, 1, 1000) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(s); return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

/* lê uma mensagem inteira (bloqueante); rx_used: o que a anterior ocupava (zerado a cada conexão) */
static size_t rx_used;
static int read_msg(proto_rx_t *rx, char *type, uint8_t **data, uint32_t *len)
{
    size_t used = rx_used;
    rx_used = 0;
    if(used) { memmove(rx->buf, rx->buf + used, rx->n - used); rx->n -= used; used = 0; }
    for(;;) {
        size_t c;
        int r = proto_take(rx, type, data, len, &c);
        if(r < 0) return -1;
        if(r) { rx_used = c; return 0; }
        if(rx->n == rx->cap) return -1;
        ssize_t n = read(sock, rx->buf + rx->n, rx->cap - rx->n);
        if(n <= 0) { if(n < 0 && errno == EINTR) continue; return -1; }
        rx->n += (size_t)n;
    }
}

static void set_target(const uint8_t *d, uint32_t l)
{
    char t[64];
    snprintf(t, sizeof t, "%.*s", (int)(l < 63 ? l : 63), (const char *)d);
    int w = 0, h = 0;
    const char *s = strncmp(t, "BFTV1 ", 6) ? t : t + 6;
    if(sscanf(s, "%d %d", &w, &h) == 2 && w >= 0 && w <= 1024 && h >= 0 && h <= 1024) {
        atomic_store(&tgt_w, w & ~7);
        atomic_store(&tgt_h, h);
        atomic_store(&want_key, 1);
        if(verbose) fprintf(stderr, "relay: quadro %dx%d\n", w & ~7, h);
    }
}

/* FOSTON -> PC: o TS vai para o demux pelo cano; S e R mudam o que sai */
static void *reader(void *a)
{
    proto_rx_t *rx = a;
    char type;
    uint8_t *d;
    uint32_t l;
    while(!atomic_load(&stopping) && read_msg(rx, &type, &d, &l) == 0) {
        if(type == 'T') { if(proto_write_all(pipe_w, d, l)) break; }
        else if(type == 'S') set_target(d, l);
        else if(type == 'R') atomic_store(&want_key, 1);
    }
    atomic_store(&stopping, 1);
    close(pipe_w);                                     /* o demux vê o fim */
    pipe_w = -1;
    return NULL;
}

static int read_cb(void *op, uint8_t *buf, int size)
{
    (void)op;
    for(;;) {
        ssize_t n = read(pipe_r, buf, (size_t)size);
        if(n > 0) return (int)n;
        if(n < 0 && errno == EINTR) continue;
        return AVERROR_EOF;
    }
}

static uint8_t *msg;
static size_t msg_cap;
static int send_pts(char type, int64_t pts, const void *p, size_t n)
{
    if(8 + n > msg_cap) {
        uint8_t *m = realloc(msg, 8 + n);
        if(!m) return -1;
        msg = m;
        msg_cap = 8 + n;
    }
    tvrelay_put_pts(msg, pts);
    memcpy(msg + 8, p, n);
    return proto_send(sock, type, msg, (uint32_t)(8 + n));
}

/* som com interferência: pacotes de TS perdidos tiram trechos do áudio, e o FOSTON ficava sem som
 * para tocar (medido na Record, SNR 11: 130 pacotes perdidos e 21 cortes em 15 s). O buraco entre
 * o fim do último bloco e o horário do próximo é preenchido aqui: o último trecho repetido,
 * sumindo em 60 ms, e depois silêncio. A linha do tempo chega ao FOSTON sem falhas. */
#define FILL_FADE (TVRELAY_RATE * 60 / 1000)
static int16_t last_blk[8192];
static int last_n;
static unsigned long filled_ms;

static int fill_gap(int64_t from, int64_t gap_us)
{
    int n = (int)(gap_us * TVRELAY_RATE / 1000000);
    static int16_t buf[4096];
    int done = 0;
    while(done < n) {
        int k = n - done < 4096 ? n - done : 4096;
        for(int i = 0; i < k; i++) {
            int at = done + i;
            int g = at < FILL_FADE ? (FILL_FADE - at) * 256 / FILL_FADE : 0;
            buf[i] = last_n && g ? (int16_t)(last_blk[at % last_n] * g / 256) : 0;
        }
        if(send_pts('A', from + (int64_t)done * 1000000 / TVRELAY_RATE, buf, (size_t)k * 2)) return -1;
        done += k;
    }
    filled_ms += (unsigned long)(gap_us / 1000);
    return 0;
}

static void session(void)
{
    proto_rx_t rx = { .cap = RELAY_MAX_MSG + 16 };
    rx.buf = malloc(rx.cap);
    rx_used = 0;
    last_n = 0;
    char type;
    uint8_t *d;
    uint32_t l;
    if(!rx.buf || read_msg(&rx, &type, &d, &l) || type != 'H') { free(rx.buf); return; }
    set_target(d, l);
    int pp[2];
    if(pipe2(pp, O_CLOEXEC)) { free(rx.buf); return; }
    pipe_r = pp[0];
    pipe_w = pp[1];
    atomic_store(&stopping, 0);
    pthread_t rt;
    pthread_create(&rt, NULL, reader, &rx);
    fprintf(stderr, "relay: FOSTON conectado, decodificando no PC\n");

    AVFormatContext *fc = avformat_alloc_context();
    uint8_t *iob = av_malloc(32768);
    fc->pb = avio_alloc_context(iob, 32768, 0, NULL, read_cb, NULL, NULL);
    fc->probesize = 120000;                            /* ~2,5 s de TS 1seg, como no FOSTON */
    fc->max_analyze_duration = AV_TIME_BASE;
    AVCodecContext *vc = NULL, *ac = NULL;
    SwrContext *swr = NULL;
    struct SwsContext *sws = NULL;
    int sws_w = 0, sws_h = 0, sws_f = -1, sws_tw = 0, sws_th = 0;
    AVPacket *pk = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    yuvz_enc_t enc = { 0 };
    uint8_t *yuv = NULL;
    size_t yuv_cap = 0;
    int16_t *pcm = NULL;
    int pcm_cap = 0;
    unsigned long nv = 0, na = 0, bytes = 0;
    double t_log = now_s() + 10;
    int64_t a_next = AV_NOPTS_VALUE;
    if(avformat_open_input(&fc, NULL, av_find_input_format("mpegts"), NULL) < 0) goto out;
    avformat_find_stream_info(fc, NULL);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    if(vi >= 0) {
        const AVCodec *c = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id);
        vc = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vc, fc->streams[vi]->codecpar);
        vc->thread_count = 2;
        if(avcodec_open2(vc, c, NULL) < 0) avcodec_free_context(&vc);
    }
    if(ai >= 0) {
        const AVCodec *c = avcodec_find_decoder(fc->streams[ai]->codecpar->codec_id);
        ac = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ac, fc->streams[ai]->codecpar);
        if(avcodec_open2(ac, c, NULL) < 0) avcodec_free_context(&ac);
    }
    fprintf(stderr, "relay: vídeo %s, áudio %s\n", vc ? vc->codec->name : "-", ac ? ac->codec->name : "-");
    while(!atomic_load(&stopping) && av_read_frame(fc, pk) >= 0) {
        AVStream *st = fc->streams[pk->stream_index];
        if(pk->stream_index == vi && vc && avcodec_send_packet(vc, pk) >= 0) {
            while(avcodec_receive_frame(vc, fr) == 0) {
                int64_t ts = fr->best_effort_timestamp;
                if(ts == AV_NOPTS_VALUE) { av_frame_unref(fr); continue; }
                int64_t pts = av_rescale_q(ts, st->time_base, (AVRational){ 1, 1000000 });
                int tw = atomic_load(&tgt_w), th = atomic_load(&tgt_h);
                if(tw <= 0 || th <= 0) { tw = fr->width & ~7; th = fr->height; }
                size_t n = yuvz_size(tw, th);
                if(n > yuv_cap) { free(yuv); yuv = malloc(n); yuv_cap = yuv ? n : 0; }
                if(!yuv) break;
                int cw = tw / 2, chh = (th + 1) / 2;
                uint8_t *dst[4] = { yuv, yuv + (size_t)tw * th, yuv + (size_t)tw * th + (size_t)cw * chh, NULL };
                int dls[4] = { tw, cw, cw, 0 };
                if(!sws || sws_w != fr->width || sws_h != fr->height || sws_f != fr->format || sws_tw != tw || sws_th != th) {
                    sws_freeContext(sws);
                    sws = sws_getContext(fr->width, fr->height, fr->format, tw, th, AV_PIX_FMT_YUV420P, SWS_LANCZOS, NULL, NULL, NULL);
                    sws_w = fr->width; sws_h = fr->height; sws_f = fr->format; sws_tw = tw; sws_th = th;
                }
                if(sws) sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dls);
                const uint8_t *m;
                long ml = yuvz_encode(&enc, yuv, tw, th, atomic_exchange(&want_key, 0), &m);
                if(ml > 0 && send_pts('V', pts, m, (size_t)ml)) atomic_store(&stopping, 1);
                nv++;
                bytes += ml > 0 ? (unsigned long)ml + 13 : 0;
                av_frame_unref(fr);
            }
        } else if(pk->stream_index == ai && ac && avcodec_send_packet(ac, pk) >= 0) {
            while(avcodec_receive_frame(ac, fr) == 0) {
                if(!swr) {
                    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
                    if(swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, TVRELAY_RATE, &fr->ch_layout, fr->format,
                                           fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0)
                        swr_free(&swr);
                }
                if(!swr) { av_frame_unref(fr); continue; }
                int64_t pts = fr->pts != AV_NOPTS_VALUE ? av_rescale_q(fr->pts, st->time_base, (AVRational){ 1, 1000000 }) : a_next;
                if(pts != AV_NOPTS_VALUE) pts -= swr_get_delay(swr, 1000000);   /* o que o reamostrador segura */
                int max = (int)swr_get_out_samples(swr, fr->nb_samples);
                if(max > pcm_cap) { free(pcm); pcm = malloc((size_t)max * 2); pcm_cap = pcm ? max : 0; }
                int got = pcm ? swr_convert(swr, (uint8_t **)&pcm, max, (const uint8_t **)fr->extended_data, fr->nb_samples) : 0;
                if(got > 0 && pts != AV_NOPTS_VALUE) {
                    int64_t gap = a_next != AV_NOPTS_VALUE ? pts - a_next : 0;
                    if(gap > 30000 && gap < 1500000 && fill_gap(a_next, gap)) atomic_store(&stopping, 1);
                    if(gap < -30000 && pts + (int64_t)got * 1000000 / TVRELAY_RATE <= a_next) { av_frame_unref(fr); continue; }
                    if(send_pts('A', pts, pcm, (size_t)got * 2)) atomic_store(&stopping, 1);
                    last_n = got < (int)(sizeof last_blk / 2) ? got : (int)(sizeof last_blk / 2);
                    memcpy(last_blk, pcm, (size_t)last_n * 2);
                    a_next = pts + (int64_t)got * 1000000 / TVRELAY_RATE;
                    na++;
                    bytes += (unsigned long)got * 2 + 13;
                }
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pk);
        if(verbose && now_s() >= t_log) {
            fprintf(stderr, "relay: %.1f qps, %lu blocos de som, %.0f KB/s, %lu ms de som recompostos\n", nv / 10.0, na,
                    bytes / 10240.0, filled_ms);
            filled_ms = 0;
            nv = na = bytes = 0;
            t_log = now_s() + 10;
        }
    }
out:
    atomic_store(&stopping, 1);
    shutdown(sock, SHUT_RDWR);
    pthread_join(rt, NULL);
    close(pipe_r);
    pipe_r = -1;
    av_packet_free(&pk);
    av_frame_free(&fr);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    swr_free(&swr);
    sws_freeContext(sws);
    if(fc) { av_freep(&fc->pb->buffer); avio_context_free(&fc->pb); avformat_close_input(&fc); }
    yuvz_enc_free(&enc);
    free(yuv);
    free(pcm);
    free(rx.buf);
    fprintf(stderr, "relay: FOSTON saiu\n");
}

int main(int argc, char **argv)
{
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if(!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "uso: bftv-relay [--host IP] [--port N] [-v]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    av_log_set_level(AV_LOG_QUIET);
    for(;;) {                                          /* o app TV abre e fecha: tenta a cada 1 s */
        sock = connect_to();
        if(sock < 0) { sleep(1); continue; }
        session();
        close(sock);
        sock = -1;
    }
}
