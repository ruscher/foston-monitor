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
static bool started, has_text, stop_now;
static char pending[512];
static bgv_pack_t *pack;

static const char *base_dir(void)
{
    const char *b = getenv("BIGFOSTON_BIGGPS");
    return b && *b ? b : BIGGPS_DIR_DEFAULT;
}

/* o .bgv da pasta voz com mais frases (um pacote com nomes de ruas ganha do só com as fixas) */
static bgv_pack_t *open_pack(void)
{
    char dir[600], path[900], err[256];
    snprintf(dir, sizeof dir, "%s/voz", base_dir());
    DIR *dp = opendir(dir);
    if(!dp) return NULL;
    bgv_pack_t *best = NULL;
    struct dirent *de;
    while((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if(l < 5 || strcasecmp(de->d_name + l - 4, ".bgv") || de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        bgv_pack_t *p = bgv_open(path, err, sizeof err);
        if(!p) { fprintf(stderr, "voz: %s\n", err); continue; }
        if(!best || bgv_count(p) > bgv_count(best)) { bgv_close(best); best = p; }
        else bgv_close(p);
    }
    closedir(dp);
    if(best) fprintf(stderr, "voz: Faber, %u frases\n", bgv_count(best));
    return best;
}

static pthread_once_t pack_once = PTHREAD_ONCE_INIT;
static void load_pack(void) { pack = open_pack(); }

/* a interface e a thread da fala chamam: abre uma vez só */
static bgv_pack_t *get_pack(void)
{
    pthread_once(&pack_once, load_pack);
    return pack;
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
        if(bgv_find(p, part, &clips[nc])) total += clips[nc++].nsamples + gap;
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
    /* primeiro o som do app Internet (modo completo): ao soltar o /dev/dsp ele devolve a
     * vez ao player e ao Bluetooth, que então são suspensos abaixo */
    webaudio_hold(true);
    bool music = player_state() != PL_STOPPED;
    if(music) player_suspend(true);
    bool bt = btaudio_live();                   /* música do celular: o bluealsa-aplay solta a saída */
    if(bt) btaudio_hold(true);
    int dsp = dsp_open((int)rate, 1);
    const char *b = (const char *)pcm;
    size_t left = (size_t)total * 2;
    while(dsp >= 0 && left) {
        size_t chunk = left < 4096 ? left : 4096;
        ssize_t w = write(dsp, b, chunk);
        if(w <= 0) break;
        b += w;
        left -= (size_t)w;
        pthread_mutex_lock(&mu);
        bool stop = stop_now;
        pthread_mutex_unlock(&mu);
        if(stop) { ioctl(dsp, SNDCTL_DSP_RESET, 0); break; }
    }
    if(dsp >= 0) close(dsp);                            /* drena o que falta tocar */
    free(pcm);
    if(music) player_suspend(false);
    if(bt) btaudio_hold(false);
    webaudio_hold(false);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    for(;;) {
        while(!has_text) pthread_cond_wait(&cv, &mu);
        char text[sizeof pending];
        memcpy(text, pending, sizeof text);
        has_text = false;
        stop_now = false;
        pthread_mutex_unlock(&mu);
        speak(text);
        pthread_mutex_lock(&mu);
    }
    return NULL;
}

void voice_say(const char *text)
{
    if(!text || !*text) return;
    pthread_mutex_lock(&mu);
    if(!started) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 128 * 1024);
        started = pthread_create(&t, &at, worker, NULL) == 0;
        pthread_attr_destroy(&at);
    }
    snprintf(pending, sizeof pending, "%s", text);
    has_text = started;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void voice_stop(void)
{
    pthread_mutex_lock(&mu);
    has_text = false;
    stop_now = true;
    pthread_mutex_unlock(&mu);
}
