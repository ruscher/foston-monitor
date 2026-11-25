/* webaudio.c — ver webaudio.h.
 *
 * Duas threads, as duas com prioridade acima da interface (nice -8; o shell roda como root):
 *   rede: aceita a conexão de som (com o código certo) e põe o que chega no anel;
 *   saída: escreve o anel no /dev/dsp, no ritmo do som.
 *
 * Latência: o som começa com 200 ms no anel; se o anel passar de 700 ms (o relógio do PC
 * anda um pouco mais rápido que o do FOSTON), o excesso mais antigo é descartado até
 * 400 ms. Com os ~125 ms do próprio /dev/dsp (8 fragmentos de 2 KB), o som fica ~0,3 s
 * atrás do Chrome; a imagem também chega com atraso parecido.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "webaudio.h"
#include "player.h"
#include "btaudio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/soundcard.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define BPS      (WEBAUDIO_RATE * WEBAUDIO_CHANNELS * 2)   /* bytes por segundo */
#define FRAME    (WEBAUDIO_CHANNELS * 2)
#define MS(x)    (BPS / 1000 * (x) / FRAME * FRAME)
#define RING     (BPS)                                     /* 1 s */
#define PREBUF   MS(200)
#define MAXLAT   MS(700)
#define TRIMTO   MS(400)
#define IDLE_MS  1500

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint8_t ring[RING];
static size_t rd, used;
static bool hold, flush_req, play_on;
static volatile bool live;                 /* /dev/dsp aberto pelo som da web */

static int lfd = -1;
static pthread_t net_thr;
static bool net_on;
static volatile bool net_quit;
static char token[40];
static unsigned token_gen;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void boost(void)
{
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -8);
}

/* ------------------------------------------------------------------ saída */

static int dsp_open(void)
{
    int fd = -1;
    for(int i = 0; i < 25 && fd < 0; i++) {   /* o Multimídia ou a voz podem estar soltando */
        fd = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
        if(fd < 0) usleep(20000);
    }
    if(fd < 0) return -1;
    int frag = (8 << 16) | 11;                  /* 8 x 2 KB: ~125 ms */
    ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
    int fmt = AFMT_S16_LE, ch = WEBAUDIO_CHANNELS, sp = WEBAUDIO_RATE;
    if(ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) || ioctl(fd, SNDCTL_DSP_SPEED, &sp)
       || fmt != AFMT_S16_LE || ch != WEBAUDIO_CHANNELS) {
        close(fd);
        return -1;
    }
    return fd;
}

/* o player segura o /dev/dsp mesmo pausado: suspende enquanto a web toca (e pausa a
 * música, como um celular faz ao abrir um vídeo; ela não volta sozinha) */
static bool took_player, took_bt;

static void acquire(void)
{
    pl_state_t st = player_state();
    if(st != PL_STOPPED) {
        if(st == PL_PLAYING) player_toggle();
        player_suspend(true);
        took_player = true;
    }
    if(btaudio_live()) { btaudio_hold(true); took_bt = true; }
}

static void release(void)
{
    if(took_player) player_suspend(false);
    if(took_bt) btaudio_hold(false);
    took_player = took_bt = false;
}

static void *play_worker(void *arg)
{
    (void)arg;
    boost();
    static uint8_t buf[2048];
    int dsp = -1;
    long last = 0;
    pthread_mutex_lock(&mu);
    for(;;) {
        if(hold || flush_req) {
            if(flush_req) { rd = used = 0; flush_req = false; }
            if(dsp >= 0) {
                pthread_mutex_unlock(&mu);
                ioctl(dsp, SNDCTL_DSP_RESET, 0);
                close(dsp);
                dsp = -1;
                live = false;
                release();
                pthread_mutex_lock(&mu);
            }
            if(hold) { rd = used = 0; pthread_cond_wait(&cv, &mu); }
            continue;
        }
        if(dsp < 0) {
            if(used < PREBUF) { pthread_cond_wait(&cv, &mu); continue; }
            pthread_mutex_unlock(&mu);
            acquire();
            dsp = dsp_open();
            pthread_mutex_lock(&mu);
            if(dsp < 0) {                       /* sem saída: descarta e tenta mais tarde */
                rd = used = 0;
                pthread_mutex_unlock(&mu);
                release();
                sleep(1);
                pthread_mutex_lock(&mu);
                continue;
            }
            live = true;
            last = now_ms();
        }
        if(used == 0) {
            if(now_ms() - last > IDLE_MS) {     /* acabou o som: o /dev/dsp volta para os outros */
                pthread_mutex_unlock(&mu);
                close(dsp);                      /* drena o que falta */
                dsp = -1;
                live = false;
                release();
                pthread_mutex_lock(&mu);
                continue;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000L;
            if(ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&cv, &mu, &ts);
            continue;
        }
        size_t n = used < sizeof buf ? used : sizeof buf;
        if(n > RING - rd) n = RING - rd;
        memcpy(buf, ring + rd, n);
        rd = (rd + n) % RING;
        used -= n;
        last = now_ms();
        pthread_mutex_unlock(&mu);
        for(size_t off = 0; off < n; ) {        /* bloqueia no ritmo do som */
            ssize_t w = write(dsp, buf + off, n - off);
            if(w < 0 && errno == EINTR) continue;
            if(w <= 0) break;
            off += (size_t)w;
        }
        pthread_mutex_lock(&mu);
    }
    return NULL;
}

/* com o mutex */
static void ring_put(const uint8_t *p, size_t n)
{
    if(!play_on) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        play_on = pthread_create(&t, &a, play_worker, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    if(hold || !play_on || !n) return;
    if(n > RING) { p += n - RING; n = RING; }
    if(used + n > RING) {                       /* cheio: perde o mais antigo */
        size_t d = used + n - RING;
        rd = (rd + d) % RING;
        used -= d;
    }
    size_t wpos = (rd + used) % RING;
    size_t a = n < RING - wpos ? n : RING - wpos;
    memcpy(ring + wpos, p, a);
    memcpy(ring, p + a, n - a);
    used += n;
    if(live && used > MAXLAT) {                 /* o relógio do PC adiantou: volta para TRIMTO */
        size_t d = used - TRIMTO;
        rd = (rd + d) % RING;
        used -= d;
    }
    pthread_cond_signal(&cv);
}

static void ring_flush(void)
{
    pthread_mutex_lock(&mu);
    if(play_on) { flush_req = true; pthread_cond_signal(&cv); }
    pthread_mutex_unlock(&mu);
}

/* ------------------------------------------------------------------ rede */

/* lê a linha do código (até 2 s); true se é o código atual */
static bool check_token(int c, unsigned *gen)
{
    char line[64];
    size_t n = 0;
    long end = now_ms() + 2000;
    while(n < sizeof line - 1) {
        struct pollfd p = { c, POLLIN, 0 };
        long left = end - now_ms();
        if(left <= 0 || poll(&p, 1, (int)left) != 1) return false;
        ssize_t r = recv(c, line + n, 1, 0);
        if(r <= 0) return false;
        if(line[n] == '\n') break;
        n++;
    }
    line[n] = 0;
    pthread_mutex_lock(&mu);
    bool ok = token[0] && !strcmp(line, token);
    *gen = token_gen;
    if(ok) token[0] = 0;                        /* uso único */
    pthread_mutex_unlock(&mu);
    return ok;
}

static void *net_worker(void *arg)
{
    (void)arg;
    boost();
    static uint8_t buf[8192];
    while(!net_quit) {
        struct pollfd lp = { lfd, POLLIN, 0 };
        if(poll(&lp, 1, 200) != 1) continue;
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if(c < 0) continue;
        unsigned gen;
        if(!check_token(c, &gen)) { close(c); continue; }
        size_t carry = 0;                       /* pedaço de amostra que ficou para o próximo recv */
        while(!net_quit) {
            pthread_mutex_lock(&mu);
            bool same = gen == token_gen;
            pthread_mutex_unlock(&mu);
            if(!same) break;                    /* a conexão principal caiu */
            struct pollfd p = { c, POLLIN, 0 };
            int pr = poll(&p, 1, 200);
            if(pr == 0) continue;
            if(pr < 0) { if(errno == EINTR) continue; break; }
            ssize_t r = recv(c, buf + carry, sizeof buf - carry, 0);
            if(r <= 0) break;
            size_t have = carry + (size_t)r, whole = have - have % FRAME;
            pthread_mutex_lock(&mu);
            ring_put(buf, whole);
            pthread_mutex_unlock(&mu);
            carry = have - whole;
            memmove(buf, buf + whole, carry);
        }
        close(c);
        ring_flush();
    }
    return NULL;
}

bool webaudio_listen(void)
{
    if(lfd >= 0) return true;
    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(lfd < 0) return false;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(WEBAUDIO_PORT) };
    /* como a conexão principal: só a rede USB; no simulador, só o próprio PC */
    const char *addr = getenv("BIGFOSTON_WEB_REMOTE_ADDR");
    inet_pton(AF_INET, addr && *addr ? addr : "192.168.7.2", &a.sin_addr);
    if(bind(lfd, (struct sockaddr *)&a, sizeof a)) {
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if(bind(lfd, (struct sockaddr *)&a, sizeof a)) { close(lfd); lfd = -1; return false; }
    }
    if(listen(lfd, 1)) { close(lfd); lfd = -1; return false; }
    net_quit = false;
    net_on = pthread_create(&net_thr, NULL, net_worker, NULL) == 0;
    if(!net_on) { close(lfd); lfd = -1; return false; }
    return true;
}

void webaudio_close(void)
{
    webaudio_set_token(NULL);
    if(net_on) {
        net_quit = true;
        pthread_join(net_thr, NULL);            /* acorda em até 200 ms */
        net_on = false;
    }
    if(lfd >= 0) close(lfd);
    lfd = -1;
    ring_flush();
}

void webaudio_set_token(const char *t)
{
    pthread_mutex_lock(&mu);
    snprintf(token, sizeof token, "%s", t ? t : "");
    token_gen++;
    pthread_mutex_unlock(&mu);
}

void webaudio_hold(bool on)
{
    pthread_mutex_lock(&mu);
    hold = on;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    if(on) for(int i = 0; i < 25 && live; i++) usleep(20000);   /* espera soltar o /dev/dsp */
}

bool webaudio_playing(void) { return live; }
