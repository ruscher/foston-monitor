/* voice.c — ver voice.h.
 * Os trechos da frase são procurados no pacote .bgv (mapeado: só as páginas tocadas são lidas
 * do cartão), decodificados (IMA ADPCM, custo desprezível) e escritos no /dev/dsp. */
#define _DEFAULT_SOURCE
#include "btaudio.h"
#include "voice.h"
#include "player.h"
#include "webaudio.h"
#include "../biggps/bgvoice.h"
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

#define BIGGPS_DIR_DEFAULT "/mnt/sd/BigFoston/big-gps"
#define GAP_MS 70                      /* pausa entre os trechos de uma frase */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static bool started, has_text, stop_now, want_beep, busy;
static char pending[512];
#define MAXPACKS 4
static bgv_pack_t *packs[MAXPACKS];             /* o maior primeiro; um trecho vem do primeiro que o tiver */
static int npacks;

static const char *base_dir(void)
{
    const char *b = getenv("BIGFOSTON_BIGGPS");
    return b && *b ? b : BIGGPS_DIR_DEFAULT;
}

/* os .bgv da pasta voz, o de mais frases primeiro: um pacote regional (com nomes de ruas) e o
 * das frases fixas, que pode ser refeito sozinho quando entra uma frase nova */
static void open_packs(void)
{
    char dir[600], path[900], err[256];
    snprintf(dir, sizeof dir, "%s/voz", base_dir());
    DIR *dp = opendir(dir);
    if(!dp) return;
    struct dirent *de;
    while((de = readdir(dp)) && npacks < MAXPACKS) {
        size_t l = strlen(de->d_name);
        if(l < 5 || strcasecmp(de->d_name + l - 4, ".bgv") || de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        bgv_pack_t *p = bgv_open(path, err, sizeof err);
        if(!p) { fprintf(stderr, "voz: %s\n", err); continue; }
        if(npacks && bgv_rate(p) != bgv_rate(packs[0])) { fprintf(stderr, "voz: %s com outra taxa, ignorado\n", path); bgv_close(p); continue; }
        int i = npacks++;
        while(i > 0 && bgv_count(packs[i - 1]) < bgv_count(p)) { packs[i] = packs[i - 1]; i--; }
        packs[i] = p;
    }
    closedir(dp);
    for(int i = 0; i < npacks; i++) fprintf(stderr, "voz: Faber, %u frases\n", bgv_count(packs[i]));
}

static int find_clip(const char *text, bgv_clip_t *c)
{
    for(int i = 0; i < npacks; i++) if(bgv_find(packs[i], text, c)) return 1;
    return 0;
}

static pthread_once_t pack_once = PTHREAD_ONCE_INIT;
static void load_pack(void) { open_packs(); }

/* a interface e a thread da fala chamam: abre uma vez só */
static bgv_pack_t *get_pack(void)
{
    pthread_once(&pack_once, load_pack);
    return npacks ? packs[0] : NULL;
}

bool voice_available(void)
{
    return get_pack() && access("/dev/dsp", W_OK) == 0;
}

static int dsp_open(int rate, int channels)
{
    int fd = -1;
    for(int i = 0; i < 25 && fd < 0; i++) {             /* o Multimídia pode estar soltando */
        fd = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
        if(fd < 0) usleep(20000);
    }
    if(fd < 0) return -1;
    int frag = (8 << 16) | 11;                          /* 8 x 2 KB */
    ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
    int fmt = AFMT_S16_LE, ch = channels, sp = rate;
    if(ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) || ioctl(fd, SNDCTL_DSP_SPEED, &sp)) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool stop_requested(void)
{
    pthread_mutex_lock(&mu);
    bool s = stop_now;
    pthread_mutex_unlock(&mu);
    return s;
}

/* a voz sozinha no /dev/dsp: o que estiver tocando (Multimídia, app Internet, música do
 * celular) solta a saída e volta depois */
static void play_dsp(const int16_t *pcm, uint32_t n, uint32_t rate)
{
    /* primeiro o som do app Internet (modo completo): ao soltar o /dev/dsp ele devolve a
     * vez ao player e ao Bluetooth, que então são suspensos abaixo */
    webaudio_hold(true);
    bool music = player_state() != PL_STOPPED;
    if(music) player_suspend(true);
    bool bt = btaudio_live();                   /* o bluealsa-aplay solta a saída */
    if(bt) btaudio_hold(true);
    int dsp = dsp_open((int)rate, 1);
    const char *b = (const char *)pcm;
    size_t left = (size_t)n * 2;
    while(dsp >= 0 && left) {
        size_t chunk = left < 4096 ? left : 4096;
        ssize_t w = write(dsp, b, chunk);
        if(w <= 0) break;
        b += w;
        left -= (size_t)w;
        if(stop_requested()) { ioctl(dsp, SNDCTL_DSP_RESET, 0); break; }
    }
    if(dsp >= 0) close(dsp);                            /* drena o que falta tocar */
    if(music) player_suspend(false);
    if(bt) btaudio_hold(false);
    webaudio_hold(false);
}

static void speak(const char *text)
{
    bgv_pack_t *p = get_pack();
    if(!p) return;
    /* junta os trechos que o pacote tem, com uma pausa curta entre eles */
    bgv_clip_t clips[12];
    int nc = 0;
    uint32_t rate = bgv_rate(p), gap = rate * GAP_MS / 1000, total = 0;
    char part[512];
    const char *s = text;
    while(*s && nc < 12) {
        const char *bar = strchr(s, '|');
        size_t l = bar ? (size_t)(bar - s) : strlen(s);
        if(l >= sizeof part) l = sizeof part - 1;
        memcpy(part, s, l);
        part[l] = 0;
        if(find_clip(part, &clips[nc])) total += clips[nc++].nsamples + gap;
        else if(part[0]) fprintf(stderr, "voz: sem o trecho \"%s\" no pacote\n", part);
        s = bar ? bar + 1 : s + l;
    }
    if(!nc) return;
    int16_t *pcm = calloc(total, sizeof *pcm);
    if(!pcm) return;
    uint32_t o = 0;
    for(int i = 0; i < nc; i++) {
        bgv_decode(&clips[i], pcm + o);
        o += clips[i].nsamples + gap;
    }
    /* música do celular tocando: a voz entra nela, que abaixa enquanto a voz fala */
    uint32_t done = btaudio_live() ? btaudio_say(pcm, total, rate, stop_requested) : 0;
    if(done < total) play_dsp(pcm + done, total - done, rate);
    free(pcm);
}

/* 1 kHz por 100 ms, com 8 ms de rampa nas pontas (sem estalo); feito uma vez só */
#define BEEP_RATE 22050
#define BEEP_N (BEEP_RATE / 10)
static void beep(void)
{
    static int16_t pcm[BEEP_N];
    static bool made;
    if(!made) {
        int ramp = BEEP_RATE * 8 / 1000;
        for(int i = 0; i < BEEP_N; i++) {
            double g = i < ramp ? (double)i / ramp : BEEP_N - 1 - i < ramp ? (double)(BEEP_N - 1 - i) / ramp : 1.0;
            pcm[i] = (int16_t)lround(14000.0 * g * sin(2 * M_PI * 1000.0 * i / BEEP_RATE));
        }
        made = true;
    }
    uint32_t done = btaudio_live() ? btaudio_say(pcm, BEEP_N, BEEP_RATE, stop_requested) : 0;
    if(done < BEEP_N) play_dsp(pcm + done, BEEP_N - done, BEEP_RATE);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    for(;;) {
        while(!has_text && !want_beep) pthread_cond_wait(&cv, &mu);
        bool talk = has_text;
        char text[sizeof pending];
        if(talk) memcpy(text, pending, sizeof text);
        has_text = want_beep = false;
        stop_now = false;
        busy = true;
        pthread_mutex_unlock(&mu);
        if(talk) speak(text); else beep();
        pthread_mutex_lock(&mu);
        busy = false;
    }
    return NULL;
}

/* com mu travado */
static void start_worker(void)
{
    if(started) return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    started = pthread_create(&t, &at, worker, NULL) == 0;
    pthread_attr_destroy(&at);
}

void voice_beep(void)
{
    pthread_mutex_lock(&mu);
    start_worker();
    if(started && !busy && !has_text) {
        want_beep = true;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
}

void voice_say(const char *text)
{
    if(!text || !*text) return;
    pthread_mutex_lock(&mu);
    start_worker();
    snprintf(pending, sizeof pending, "%s", text);
    has_text = started;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void voice_stop(void)
{
    pthread_mutex_lock(&mu);
    has_text = want_beep = false;
    stop_now = true;
    pthread_mutex_unlock(&mu);
}
