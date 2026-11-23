/* voice.c — ver voice.h.
 * Um processo espeak-ng por aviso (fork + execv, texto como argumento: nada passa por
 * shell); o WAV sai do pipe e vai para o /dev/dsp enquanto é sintetizado (o eSpeak NG gera
 * ~10x mais rápido que o tempo real no aparelho, docs/big-gps/09-voz.md). */
#define _DEFAULT_SOURCE
#include "btaudio.h"
#include "voice.h"
#include "player.h"
#include "webaudio.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BIGGPS_DIR_DEFAULT "/mnt/sd/BigFoston/big-gps"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static bool started, has_text, stop_now;
static char pending[512];
static pid_t child = -1;

static const char *base_dir(void)
{
    const char *b = getenv("BIGFOSTON_BIGGPS");
    return b && *b ? b : BIGGPS_DIR_DEFAULT;
}

/* motor: $BIGGPS_ESPEAK, o do sistema ou o do cartão */
static bool engine(char *out, size_t n)
{
    const char *e = getenv("BIGGPS_ESPEAK");
    if(e && *e && access(e, X_OK) == 0) { snprintf(out, n, "%s", e); return true; }
    if(access("/usr/bin/espeak-ng", X_OK) == 0) { snprintf(out, n, "%s", "/usr/bin/espeak-ng"); return true; }
    snprintf(out, n, "%s/voz/espeak-ng", base_dir());
    return access(out, X_OK) == 0;
}

static bool data_dir(char *out, size_t n)
{
    struct stat st;
    char probe[600];
    snprintf(out, n, "%s/voz", base_dir());
    snprintf(probe, sizeof probe, "%s/espeak-ng-data/phondata", out);
    return stat(probe, &st) == 0;
}

bool voice_available(void)
{
    char e[600], d[600];
    return engine(e, sizeof e) && data_dir(d, sizeof d) && access("/dev/dsp", W_OK) == 0;
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while(got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if(r <= 0) return (int)got;
        got += (size_t)r;
    }
    return (int)got;
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
    char eng[600], dir[600], pathopt[640];
    if(!engine(eng, sizeof eng) || !data_dir(dir, sizeof dir)) return;
    snprintf(pathopt, sizeof pathopt, "--path=%s", dir);
    int pfd[2];
    if(pipe(pfd)) return;
    /* primeiro o som do app Internet (modo completo): ao soltar o /dev/dsp ele devolve a
     * vez ao player e ao Bluetooth, que então são suspensos abaixo */
    webaudio_hold(true);
    /* o player segura o /dev/dsp também pausado: suspende sempre que está carregado */
    bool music = player_state() != PL_STOPPED;
    if(music) player_suspend(true);
    bool bt = btaudio_live();                   /* música do celular: o bluealsa-aplay solta a saída */
    if(bt) btaudio_hold(true);
    pid_t pid = fork();
    if(pid == 0) {
        dup2(pfd[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if(nul >= 0) dup2(nul, 2);
        close(pfd[0]);
        char *argv[] = { eng, pathopt, "-v", "pt-br", "-s", "165", "--stdout", (char *)text, NULL };
        execv(eng, argv);
        _exit(127);
    }
    close(pfd[1]);
    if(pid < 0) { close(pfd[0]); if(music) player_suspend(false); if(bt) btaudio_hold(false); webaudio_hold(false); return; }
    pthread_mutex_lock(&mu);
    child = pid;
    pthread_mutex_unlock(&mu);
    uint8_t hdr[44];
    int dsp = -1;
    if(read_full(pfd[0], hdr, sizeof hdr) == (int)sizeof hdr && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        int channels = hdr[22] | hdr[23] << 8;
        int rate = hdr[24] | hdr[25] << 8 | hdr[26] << 16 | hdr[27] << 24;
        if(channels >= 1 && channels <= 2 && rate >= 8000 && rate <= 48000) dsp = dsp_open(rate, channels);
    }
    static char buf[4096];
    ssize_t r;
    while((r = read(pfd[0], buf, sizeof buf)) > 0) {
        if(dsp < 0) continue;                           /* sem áudio: só esvazia o pipe */
        char *p = buf;
        while(r > 0) {
            ssize_t w = write(dsp, p, (size_t)r);
            if(w <= 0) { r = 0; break; }
            p += w;
            r -= w;
        }
        pthread_mutex_lock(&mu);
        bool stop = stop_now;
        pthread_mutex_unlock(&mu);
        if(stop) { ioctl(dsp, SNDCTL_DSP_RESET, 0); break; }
    }
    close(pfd[0]);
    if(dsp >= 0) close(dsp);                            /* drena o que falta tocar */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    pthread_mutex_lock(&mu);
    child = -1;
    pthread_mutex_unlock(&mu);
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
    if(child > 0) kill(child, SIGTERM);
    pthread_mutex_unlock(&mu);
}
