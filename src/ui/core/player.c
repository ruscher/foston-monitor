/* player.c — motor de reprodução de MP3 (ver player.h).
 *
 * Decodificação: libmpg123 (no FOSTON, o decodificador arm_nofpu em ponto
 * fixo), sempre em S16. Saída: OSS (/dev/dsp, emulação do kernel sobre o
 * ALSA do MT3351) com fragmentos curtos (~90 ms) para que pausa, avanço e o
 * espectro acompanhem o som. Espectro: FFT inteira de 256 pontos (Q15) sobre
 * as últimas amostras escritas, agrupada em PL_BANDS faixas logarítmicas.
 * ARMv6 sem VFP: nada de ponto flutuante no caminho de cada quadro. */
#define _DEFAULT_SOURCE
#include "player.h"
#include <string.h>

#ifndef BIGFOSTON_HAVE_MPG123
bool       player_available(void) { return false; }
bool       player_open(const char *p) { (void)p; return false; }
void       player_toggle(void) {}
void       player_suspend(bool on) { (void)on; }
void       player_stop(void) {}
void       player_seek_ms(int ms) { (void)ms; }
int        player_pos_ms(void) { return 0; }
int        player_len_ms(void) { return 0; }
pl_state_t player_state(void) { return PL_STOPPED; }
bool       player_take_finished(void) { return false; }
void       player_tags(pl_tags_t *t) { memset(t, 0, sizeof *t); }
void       player_info(pl_info_t *i) { memset(i, 0, sizeof *i); }
void       player_spectrum(uint8_t out[PL_BANDS]) { memset(out, 0, PL_BANDS); }
bool       player_cover(const uint8_t **d, size_t *l, const char **m) { (void)d; (void)l; (void)m; return false; }
#else

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include "btaudio.h"
#include <mpg123.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FFT_N    256
#define FFT_LOG  8
#define RING     1024              /* amostras mono recentes (potência de 2) */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thr;
static bool thr_running;
static volatile bool quit_req, paused, finished;
static volatile bool suspend_req;   /* outra parte (voz do Big GPS) precisa do /dev/dsp */
static volatile int seek_req_ms = -1;
static volatile int pos_ms, len_ms;
static int dsp = -1;
static pl_tags_t tags;
static pl_info_t info;
static uint8_t *cover;
static size_t cover_len;
static char cover_mime[32];

static int16_t ring[RING];
static volatile unsigned ring_w;

static bool inited;

static void init_once(void)
{
    if(inited) return;
    mpg123_init();
    inited = true;
}

bool player_available(void) { return true; }

/* ---------------- saída OSS ---------------- */

static bool dsp_open(int rate, int channels)
{
    if(dsp >= 0) close(dsp);
    dsp = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
    if(dsp < 0) return false;
    int frag = (4 << 16) | 12;     /* 4 fragmentos de 4 KB: ~90 ms a 44,1 kHz estéreo */
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    int fmt = AFMT_S16_LE, ch = channels, sp = rate;
    if(ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) ||
       ioctl(dsp, SNDCTL_DSP_SPEED, &sp)) {
        close(dsp);
        dsp = -1;
        return false;
    }
    return true;
}

/* atraso do que já foi escrito e ainda não saiu no alto-falante */
static int dsp_delay_ms(int rate, int channels)
{
    int bytes = 0;
    if(dsp < 0 || ioctl(dsp, SNDCTL_DSP_GETODELAY, &bytes) || rate <= 0) return 0;
    return (int)((long long)bytes * 1000 / (rate * channels * 2));
}

/* ---------------- tags ---------------- */

static void copy_tag(char *dst, size_t n, const mpg123_string *s1, const char *s2, size_t n2)
{
    dst[0] = 0;
    if(s1 && s1->fill && s1->p) snprintf(dst, n, "%s", s1->p);
    else if(s2 && s2[0]) {
        size_t l = n2 < n - 1 ? n2 : n - 1;
        memcpy(dst, s2, l);
        dst[l] = 0;
        for(int i = (int)l - 1; i >= 0 && (dst[i] == ' ' || !dst[i]); i--) dst[i] = 0;
    }
}

static void read_tags(mpg123_handle *mh)
{
    mpg123_id3v1 *v1 = NULL;
    mpg123_id3v2 *v2 = NULL;
    memset(&tags, 0, sizeof tags);
    if(mpg123_id3(mh, &v1, &v2) != MPG123_OK) return;
    copy_tag(tags.title, sizeof tags.title, v2 ? v2->title : NULL, v1 ? v1->title : NULL, 30);
    copy_tag(tags.artist, sizeof tags.artist, v2 ? v2->artist : NULL, v1 ? v1->artist : NULL, 30);
    copy_tag(tags.album, sizeof tags.album, v2 ? v2->album : NULL, v1 ? v1->album : NULL, 30);
    copy_tag(tags.year, sizeof tags.year, v2 ? v2->year : NULL, v1 ? v1->year : NULL, 4);
    copy_tag(tags.genre, sizeof tags.genre, v2 ? v2->genre : NULL, NULL, 0);

    /* capa: a "frente" (tipo 3) se houver, senão a primeira imagem */
    free(cover);
    cover = NULL;
    cover_len = 0;
    cover_mime[0] = 0;
    if(!v2 || !v2->pictures) return;
    mpg123_picture *pic = &v2->picture[0];
    for(size_t i = 0; i < v2->pictures; i++)
        if(v2->picture[i].type == mpg123_id3_pic_front_cover) { pic = &v2->picture[i]; break; }
    if(!pic->size || pic->size > 4 * 1024 * 1024 || !(cover = malloc(pic->size))) return;
    memcpy(cover, pic->data, pic->size);
    cover_len = pic->size;
    if(pic->mime_type.fill && pic->mime_type.p) snprintf(cover_mime, sizeof cover_mime, "%s", pic->mime_type.p);
}

bool player_cover(const uint8_t **d, size_t *l, const char **m)
{
    if(!cover) return false;
    *d = cover;
    *l = cover_len;
    *m = cover_mime;
    return true;
}

/* ---------------- thread de decodificação ---------------- */

static void *decode_thread(void *arg)
{
    mpg123_handle *mh = arg;
    static unsigned char buf[4608 * 2];
    long rate = 0;
    bool suspended = false;
    int ch = 0, enc = 0;

    mpg123_getformat(mh, &rate, &ch, &enc);
    while(!quit_req) {
        if(seek_req_ms >= 0) {
            int ms = seek_req_ms;
            seek_req_ms = -1;
            off_t s = mpg123_seek(mh, (off_t)((long long)ms * rate / 1000), SEEK_SET);
            if(dsp >= 0) ioctl(dsp, SNDCTL_DSP_RESET, 0);
            if(s >= 0) pos_ms = (int)((long long)s * 1000 / rate);
        }
        if(suspend_req) {                        /* solta o /dev/dsp até player_suspend(false) */
            if(dsp >= 0) { ioctl(dsp, SNDCTL_DSP_RESET, 0); close(dsp); dsp = -1; suspended = true; }
            usleep(20000);
            continue;
        }
        if(suspended) {
            suspended = false;
            pthread_mutex_lock(&lock);
            info.audio_out = dsp_open((int)rate, ch);
            pthread_mutex_unlock(&lock);
        }
        if(paused) {
            usleep(20000);
            continue;
        }
        size_t done = 0;
        int r = mpg123_read(mh, buf, sizeof buf, &done);
        if(r == MPG123_NEW_FORMAT) {
            mpg123_getformat(mh, &rate, &ch, &enc);
            pthread_mutex_lock(&lock);
            info.rate = (int)rate;
            info.channels = ch;
            info.audio_out = dsp_open((int)rate, ch);
            pthread_mutex_unlock(&lock);
            continue;
        }
        if(done) {
            /* amostras para o espectro (mono = média L/R) */
            const int16_t *s = (const int16_t *)buf;
            size_t frames = done / (2 * (size_t)ch);
            unsigned w = ring_w;
            for(size_t i = 0; i < frames; i++) {
                int v = ch == 2 ? (s[2 * i] + s[2 * i + 1]) / 2 : s[i];
                ring[w++ & (RING - 1)] = (int16_t)v;
            }
            ring_w = w;
            if(dsp >= 0) {
                const unsigned char *p = buf;
                size_t left = done;
                while(left && !quit_req) {
                    ssize_t n = write(dsp, p, left);
                    if(n < 0) { if(errno == EINTR) continue; break; }
                    p += n;
                    left -= (size_t)n;
                }
            }
            else {
                /* sem saída de áudio (simulador, ou /dev/dsp ocupado por outro
                 * programa): mantém o ritmo real e tenta reabrir a cada ~1 s */
                usleep((useconds_t)((long long)frames * 1000000 / rate));
                static unsigned retry;
                if(++retry % 40 == 0) {
                    pthread_mutex_lock(&lock);
                    info.audio_out = dsp_open((int)rate, ch);
                    pthread_mutex_unlock(&lock);
                }
            }
            off_t t = mpg123_tell(mh);
            int d = dsp_delay_ms((int)rate, ch);
            int ms = t >= 0 ? (int)((long long)t * 1000 / rate) - d : pos_ms;
            pos_ms = ms < 0 ? 0 : ms;
            struct mpg123_frameinfo fi;
            if(mpg123_info(mh, &fi) == MPG123_OK) info.bitrate_kbps = fi.bitrate;
        }
        if(r == MPG123_DONE) {
            if(dsp >= 0) ioctl(dsp, SNDCTL_DSP_SYNC, 0);  /* deixa o fim tocar */
            finished = true;
            break;
        }
        if(r != MPG123_OK && r != MPG123_NEW_FORMAT && !done) {
            finished = true;    /* arquivo com erro: passa adiante */
            break;
        }
    }
    mpg123_close(mh);
    mpg123_delete(mh);
    pthread_mutex_lock(&lock);
    if(dsp >= 0) { close(dsp); dsp = -1; }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void player_stop(void)
{
    if(thr_running) {
        quit_req = true;
        paused = false;
        pthread_join(thr, NULL);
        thr_running = false;
    }
    quit_req = false;
    finished = false;
    paused = false;
    pos_ms = 0;
    memset(ring, 0, sizeof ring);
}

bool player_open(const char *path)
{
    player_stop();
    init_once();
    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if(!mh) return false;
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_PICTURE, 0);
    mpg123_format_none(mh);
    static const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for(unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
        mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    if(mpg123_open(mh, path) != MPG123_OK) { mpg123_delete(mh); return false; }

    long rate = 0;
    int ch = 0, enc = 0;
    if(mpg123_getformat(mh, &rate, &ch, &enc) != MPG123_OK || rate <= 0) {
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }
    read_tags(mh);
    struct mpg123_frameinfo fi;
    memset(&info, 0, sizeof info);
    if(mpg123_info(mh, &fi) == MPG123_OK) {
        info.bitrate_kbps = fi.bitrate;
        info.vbr = fi.vbr != MPG123_CBR;
        info.layer = fi.layer;
        info.version = fi.version;
    }
    struct stat st;
    info.file_bytes = stat(path, &st) ? 0 : st.st_size;
    info.rate = (int)rate;
    info.channels = ch;
    off_t len = mpg123_length(mh);
    len_ms = len > 0 ? (int)((long long)len * 1000 / rate) : 0;
    info.audio_out = dsp_open((int)rate, ch);

    quit_req = finished = paused = false;
    pos_ms = 0;
    seek_req_ms = -1;
    if(pthread_create(&thr, NULL, decode_thread, mh)) {
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }
    thr_running = true;
    return true;
}

void player_toggle(void) { if(thr_running) paused = !paused; }
void player_suspend(bool on) { suspend_req = on; }
void player_seek_ms(int ms) { if(thr_running) seek_req_ms = ms < 0 ? 0 : ms; }
int  player_pos_ms(void) { return pos_ms; }
int  player_len_ms(void) { return len_ms; }

pl_state_t player_state(void)
{
    if(!thr_running || finished) return PL_STOPPED;
    return paused ? PL_PAUSED : PL_PLAYING;
}

bool player_take_finished(void)
{
    if(!finished) return false;
    player_stop();       /* junta a thread que terminou */
    return true;
}

void player_tags(pl_tags_t *t) { *t = tags; }

void player_info(pl_info_t *i)
{
    pthread_mutex_lock(&lock);
    *i = info;
    pthread_mutex_unlock(&lock);
}

/* ---------------- espectro (FFT inteira) ---------------- */

static int16_t tw_cos[FFT_N / 2], tw_sin[FFT_N / 2], win[FFT_N];
static uint8_t band_edge[PL_BANDS + 1];   /* bins [edge[b], edge[b+1]) da faixa b */
static bool tw_ready;

static void tw_init(void)
{
    /* uma vez só: ponto flutuante emulado é aceitável aqui */
    for(int i = 0; i < FFT_N / 2; i++) {
        tw_cos[i] = (int16_t)lrint(32767.0 * cos(2 * M_PI * i / FFT_N));
        tw_sin[i] = (int16_t)lrint(-32767.0 * sin(2 * M_PI * i / FFT_N));
    }
    for(int i = 0; i < FFT_N; i++)   /* janela de Hann */
        win[i] = (int16_t)lrint(32767.0 * (0.5 - 0.5 * cos(2 * M_PI * i / (FFT_N - 1))));
    /* bordas logarítmicas de 1 a 128 (bin 1 = rate/256, ~172 Hz a 44,1 kHz) */
    int prev = 1;
    band_edge[0] = 1;
    for(int b = 1; b <= PL_BANDS; b++) {
        int e = (int)lrint(pow(128.0, (double)b / PL_BANDS));
        if(e <= prev) e = prev + 1;
        if(e > 128) e = 128;
        band_edge[b] = (uint8_t)e;
        prev = e;
    }
    band_edge[PL_BANDS] = 128;
    tw_ready = true;
}

static void fft(int32_t *re, int32_t *im)
{
    for(int i = 1, j = 0; i < FFT_N; i++) {       /* bit-reversal */
        int bit = FFT_N >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if(i < j) {
            int32_t t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for(int len = 2; len <= FFT_N; len <<= 1) {
        int step = FFT_N / len;
        for(int i = 0; i < FFT_N; i += len) {
            for(int k = 0; k < len / 2; k++) {
                int32_t c = tw_cos[k * step], s = tw_sin[k * step];
                int32_t *ar = &re[i + k], *ai = &im[i + k];
                int32_t *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                int32_t tr = (int32_t)(((int64_t)*br * c - (int64_t)*bi * s) >> 15);
                int32_t ti = (int32_t)(((int64_t)*br * s + (int64_t)*bi * c) >> 15);
                /* >>1 por estágio: sem estouro, escala 1/N */
                *br = (*ar - tr) >> 1; *bi = (*ai - ti) >> 1;
                *ar = (*ar + tr) >> 1; *ai = (*ai + ti) >> 1;
            }
        }
    }
}

/* log2 inteiro com 4 bits de fração */
static int ilog2_q4(uint32_t v)
{
    if(!v) return 0;
    int n = 31 - __builtin_clz(v);
    uint32_t frac = n >= 4 ? (v >> (n - 4)) & 15 : (v << (4 - n)) & 15;
    return n * 16 + (int)frac;
}

void player_spectrum(uint8_t out[PL_BANDS])
{
    static uint8_t level[PL_BANDS];
    static int32_t re[FFT_N], im[FFT_N];
    if(!tw_ready) tw_init();

    bool live = player_state() == PL_PLAYING;
    static int16_t bt[FFT_N];
    /* música do celular: as amostras que o bluealsa-aplay já decodificou (core/btaudio) */
    bool bt_live = !live && btaudio_live() && btaudio_samples(bt, FFT_N);
    if(live || bt_live) {
        unsigned w = ring_w;
        for(int i = 0; i < FFT_N; i++) {
            int32_t s = bt_live ? bt[i] : ring[(w - FFT_N + (unsigned)i) & (RING - 1)];
            re[i] = (s * win[i]) >> 15;
            im[i] = 0;
        }
        fft(re, im);
    }
    live = live || bt_live;
    for(int b = 0; b < PL_BANDS; b++) {
        int target = 0;
        if(live) {
            uint32_t peak = 0;
            for(int k = band_edge[b]; k < band_edge[b + 1] && k < 128; k++) {
                uint32_t a = (uint32_t)(re[k] < 0 ? -re[k] : re[k]);
                uint32_t c = (uint32_t)(im[k] < 0 ? -im[k] : im[k]);
                uint32_t m = a > c ? a + c / 2 : c + a / 2;   /* |z| aproximado */
                if(m > peak) peak = m;
            }
            /* ~6 dB por unidade de log2; 0..100 cobrindo ~60 dB */
            int db = ilog2_q4(peak) - 16 * 4;
            target = db * 100 / (16 * 10);
            if(target < 0) target = 0;
            if(target > 100) target = 100;
        }
        if(target > level[b]) level[b] = (uint8_t)target;               /* sobe rápido */
        else level[b] = (uint8_t)(level[b] > 6 ? level[b] - 6 : 0);    /* cai devagar */
        out[b] = level[b];
    }
}
#endif
