/* client.c — o bfrp no FOSTON com o relay do PC (relay.c): ponte entre a tela (stdin/stdout, o
 * mesmo protocolo de linhas de bfrp.c) e o relay pela rede USB. Aqui não há H.264 nem Opus:
 * o quadro chega pronto (YUV 4:2:0 + LZ4, já cortado e no tamanho da área) e vai para o
 * framebuffer pelo video.c; o som chega em PCM mono 32 kHz e vai para o /dev/dsp.
 *
 * Fica no FOSTON (não vai ao PC): RECT (a área), CLIP (a barra da tela cheia por cima) e SHOW
 * (também vai: o PC para de mandar quadros). Cada quadro desenhado é confirmado (Q).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/soundcard.h>
#include <unistd.h>

#include "proto.h"
#include "relay.h"
#include "video.h"
#include "lz4dec.h"

static int dsp = -1;
static int prof;
static double t_lz4, t_draw, t_bytes;
static int t_n;
static unsigned long n_bad;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void dsp_open(void)
{
    if(dsp >= 0) return;
    dsp = open("/dev/dsp", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(dsp < 0) return;
    int frag = (6 << 16) | 11, fmt = AFMT_S16_LE, ch = 1, sp = 32000;
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    if(ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) || ioctl(dsp, SNDCTL_DSP_SPEED, &sp)) {
        close(dsp);
        dsp = -1;
    }
}

static void dsp_close(void) { if(dsp >= 0) { close(dsp); dsp = -1; } }

/* o relay do PC conecta AQUI (o firewall do PC só libera as portas do Sunshine; uma conexão que
 * sai do PC passa): escuta só no endereço da rede USB e aceita só o PC. Prazo curto: sem relay, o
 * bfrp segue no modo direto */
static int wait_relay(const char *host, int port)
{
    int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    if(l < 0) return -1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }, pc = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if(inet_pton(AF_INET, "192.168.7.2", &a.sin_addr) != 1 || inet_pton(AF_INET, host, &pc.sin_addr) != 1 ||
       bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) { close(l); return -1; }
    int s = -1;
    for(int tries = 0; tries < 3 && s < 0; tries++) {
        struct pollfd p = { l, POLLIN, 0 };
        if(poll(&p, 1, 2500) != 1) break;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        s = accept(l, (struct sockaddr *)&from, &fl);
        if(s >= 0 && from.sin_addr.s_addr != pc.sin_addr.s_addr) { close(s); s = -1; }   /* só o PC */
    }
    close(l);
    if(s < 0) return -1;
    int rcv = 512 * 1024;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    return s;
}

static void say(const char *l) { fputs(l, stdout); fputc('\n', stdout); fflush(stdout); }

/* uma linha da tela: o que é só daqui fica, o resto vai ao relay */
static void from_shell(int s, char *l)
{
    if(!strncmp(l, "CLIP ", 5)) { video_clip(atoi(l + 5)); return; }
    if(!strncmp(l, "SHOW ", 5)) video_show(atoi(l + 5));
    if(!strncmp(l, "RECT ", 5)) {
        int x, y, w, h;
        if(sscanf(l + 5, "%d %d %d %d", &x, &y, &w, &h) == 4) video_open(x, y, w, h);
    }
    if(!strncmp(l, "START ", 6)) {                       /* START app l a qps kbps vídeo x y l a ... */
        int v[11] = { 0 };
        if(sscanf(l + 6, "%d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                  &v[8], &v[9]) >= 10 && v[5]) {
            video_open(v[6], v[7], v[8], v[9]);
            dsp_open();
        }
    }
    proto_send(s, 'K', l, (uint32_t)strlen(l));
}

static uint8_t yuv[1024 * 1024] __attribute__((aligned(4))), xbuf[1024 * 1024] __attribute__((aligned(4)));

/* XOR de n bytes (n múltiplo de 4, endereços alinhados) */
static void xor_words(uint8_t *dst, const uint8_t *x, size_t n)
{
    uint32_t *d = (uint32_t *)(void *)dst;
    const uint32_t *s = (const uint32_t *)(const void *)x;
    for(size_t i = 0; i < n / 4; i++) d[i] ^= s[i];
}

/* um quadro do relay (proto.h, relay.c): l a (u16) + flags (bit 0 delta, bit 1 soma) [+ soma u32]
 * [+ máscara dos pares de linhas, se delta] + LZ4. No delta, só os pares que mudaram vêm (em XOR,
 * em sequência: 2 linhas de brilho, a de U e a de V) e só eles são redesenhados */
static void on_video(int s, const uint8_t *d, uint32_t l)
{
    static int have_w, have_h;
    static uint8_t dirty[1024];
    if(l < 5) goto ack;
    int w = d[0] | d[1] << 8, h = d[2] | d[3] << 8, delta = d[4] & 1, with_sum = d[4] & 2;
    size_t need = yuv_size(w, h), hl = 5 + (with_sum ? 4 : 0);
    int cw = w / 2, chh = (h + 1) / 2;
    size_t ml = delta ? (size_t)(chh + 7) / 8 : 0;
    if(w <= 0 || (w & 7) || h <= 0 || w > 1024 || h > 1024 || need > sizeof yuv || l < hl + ml) goto ack;
    if(delta && (w != have_w || h != have_h)) goto miss;      /* sem o quadro base */
    uint32_t want = with_sum ? (uint32_t)d[5] | (uint32_t)d[6] << 8 | (uint32_t)d[7] << 16 | (uint32_t)d[8] << 24 : 0;
    const uint8_t *mask = d + hl, *z = d + hl + ml;
    size_t zn = l - hl - ml;
    double t0 = prof ? now_ms() : 0;
    const uint8_t *dmask = NULL;
    if(delta) {
        size_t pairs = 0, xn = 0;
        for(int r = 0; r < chh; r++)
            if(mask[r / 8] >> (r % 8) & 1) { pairs++; xn += (size_t)(2 * r + 1 < h ? 2 : 1) * w + 2 * (size_t)cw; }
        if(xn > sizeof xbuf || (xn ? lz4_block_decode(z, zn, xbuf, xn) != (long)xn : zn != 0)) goto miss;
        const uint8_t *x = xbuf;
        uint8_t *yu = yuv + (size_t)w * h, *yv = yu + (size_t)cw * chh;
        memset(dirty, 0, (size_t)h);
        for(int r = 0; r < chh; r++) {
            if(!(mask[r / 8] >> (r % 8) & 1)) continue;
            int ly = 2 * r + 1 < h ? 2 : 1;
            xor_words(yuv + (size_t)2 * r * w, x, (size_t)ly * w); x += (size_t)ly * w;
            xor_words(yu + (size_t)r * cw, x, (size_t)cw); x += cw;
            xor_words(yv + (size_t)r * cw, x, (size_t)cw); x += cw;
            dirty[2 * r] = 1;
            if(ly == 2) dirty[2 * r + 1] = 1;
        }
        dmask = dirty;
        if(!pairs && !with_sum) goto ack;                    /* nada mudou */
    } else if(lz4_block_decode(z, zn, yuv, need) != (long)need) goto miss;
    have_w = w; have_h = h;
    if(with_sum && yuv_sum(yuv, need) != want) {             /* a base divergiu: pede um quadro inteiro */
        have_w = 0;
        if(++n_bad < 20 || n_bad % 100 == 0) fprintf(stderr, "cliente: soma errada (%lu, %s)\n", n_bad, delta ? "delta" : "inteiro");
        proto_send(s, 'R', NULL, 0);
    }
    double t1 = prof ? now_ms() : 0;
    video_draw_yuv(yuv, w, h, dmask);
    if(prof) {
        t_lz4 += t1 - t0; t_draw += now_ms() - t1; t_bytes += l; t_n++;
        if(t_n == 50) {
            fprintf(stderr, "cliente: lz4 %.1f ms, desenho %.1f ms, %.1f KB por quadro, %lu somas erradas\n",
                    t_lz4 / t_n, t_draw / t_n, t_bytes / t_n / 1024, n_bad);
            t_lz4 = t_draw = t_bytes = 0; t_n = 0;
        }
    }
    goto ack;
miss:
    have_w = 0;
    proto_send(s, 'R', NULL, 0);                             /* o próximo vai inteiro */
ack:
    proto_send(s, 'Q', NULL, 0);                             /* o próximo quadro pode vir */
}

int relay_client_main(const char *host, int port)
{
    int s = wait_relay(host, port);
    if(s < 0) return -1;
    video_passthrough(1);
    if(getenv("BFRP_SMOOTH")) video_smooth(atoi(getenv("BFRP_SMOOTH")));
    prof = 1;                                            /* tempos no log (/tmp/bfrp.log) a cada 50 quadros */
    say("MODE relay");
    proto_rx_t rx = { 0 };
    rx.cap = RELAY_MAX_MSG + 16;
    rx.buf = malloc(rx.cap);
    char line[512];
    int len = 0;
    if(!rx.buf) { close(s); return -1; }
    for(;;) {
        struct pollfd p[2] = { { 0, POLLIN, 0 }, { s, POLLIN, 0 } };
        if(poll(p, 2, 1000) < 0) { if(errno == EINTR) continue; break; }
        if(p[0].revents & (POLLIN | POLLHUP)) {          /* a tela */
            ssize_t n = read(0, line + len, sizeof line - 1 - (size_t)len);
            if(n <= 0) {                                 /* a tela saiu: nada preso no PC */
                proto_send(s, 'K', "RELEASE", 7);
                proto_send(s, 'K', "STOP", 4);
                break;
            }
            len += (int)n;
            line[len] = 0;
            char *a = line, *nl;
            while((nl = strchr(a, '\n'))) {
                *nl = 0;
                if(nl > a && nl[-1] == '\r') nl[-1] = 0;
                if(*a) from_shell(s, a);
                a = nl + 1;
            }
            len = (int)strlen(a);
            memmove(line, a, (size_t)len + 1);
            if(len >= (int)sizeof line - 1) len = 0;
        }
        if(p[1].revents & (POLLIN | POLLHUP | POLLERR)) { /* o relay */
            if(rx.n == rx.cap) break;
            ssize_t n = read(s, rx.buf + rx.n, rx.cap - rx.n);
            if(n <= 0) { say("TERMINATED relay"); break; }
            rx.n += (size_t)n;
            char type;
            uint8_t *d;
            uint32_t l;
            size_t used;
            int r;
            while((r = proto_take(&rx, &type, &d, &l, &used)) == 1) {
                if(type == 'E') {
                    char ev[600];
                    size_t m = l < sizeof ev - 1 ? l : sizeof ev - 1;
                    memcpy(ev, d, m);
                    ev[m] = 0;
                    say(ev);
                    if(!strncmp(ev, "STOPPED", 7) || !strncmp(ev, "TERMINATED", 10)) dsp_close();
                } else if(type == 'V') {
                    on_video(s, d, l);
                } else if(type == 'A' && dsp >= 0) {
                    if(write(dsp, d, l) < 0 && errno != EAGAIN) dsp_close();   /* cheio: descarta (sem atraso) */
                }
                memmove(rx.buf, rx.buf + used, rx.n - used);
                rx.n -= used;
            }
            if(r < 0) { say("TERMINATED relay"); break; }
        }
    }
    dsp_close();
    close(s);
    free(rx.buf);
    return 0;
}
