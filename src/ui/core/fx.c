/* fx.c — ver fx.h. */
#define _DEFAULT_SOURCE
#include "fx.h"
#include "settings.h"
#include "status.h"
#include "player.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

#define RATE 22050

/* ---------------------------------------------------------------- sons */

#define CLICK_N   (RATE * 22 / 1000)
#define TOGGLE_N  (RATE * 70 / 1000)
#define STARTUP_N (RATE * 900 / 1000)
static int16_t snd_click[CLICK_N], snd_toggle[TOGGLE_N], snd_startup[STARTUP_N];
static atomic_int playing;

/* seno inteiro por tabela (o alvo não usa a FPU no caminho do áudio) */
static int16_t sine_tab[256];

static void sine_init(void)
{
    /* parábola de Bhaskara por quarto de onda: erro < 0,2 %, só inteiros */
    for(int i = 0; i < 256; i++) {
        int x = i & 127;                         /* 0..127 = 0..pi */
        long num = 16L * x * (128 - x);
        long den = 5L * 128 * 128 - 4L * x * (128 - x);
        long v = num * 32767 / den;
        sine_tab[i] = (int16_t)(i < 128 ? v : -v);
    }
}

/* soma um tom com ataque curto e decaimento exponencial aproximado */
static void tone(int16_t *buf, int n, int start, int len, int freq, int amp)
{
    uint32_t ph = 0, inc = (uint32_t)(((uint64_t)freq << 32) / RATE);
    int att = RATE / 500;                         /* 2 ms */
    for(int i = 0; i < len && start + i < n; i++) {
        int env = i < att ? amp * i / att : amp * (len - i) / len;
        env = env * (len - i) / len;              /* curva mais rápida no fim */
        int v = buf[start + i] + sine_tab[ph >> 24] * env / 32767;
        buf[start + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        ph += inc;
    }
}

static void sounds_init(void)
{
    sine_init();
    memset(snd_click, 0, sizeof snd_click);
    tone(snd_click, CLICK_N, 0, CLICK_N, 1900, 9000);
    tone(snd_click, CLICK_N, 0, CLICK_N / 2, 3800, 3000);
    memset(snd_toggle, 0, sizeof snd_toggle);
    tone(snd_toggle, TOGGLE_N, 0, TOGGLE_N / 2, 1320, 8000);
    tone(snd_toggle, TOGGLE_N, TOGGLE_N / 2, TOGGLE_N / 2, 1760, 8000);
    /* acorde ascendente (dó, mi, sol, dó) */
    memset(snd_startup, 0, sizeof snd_startup);
    static const int f[] = { 523, 659, 784, 1047 };
    for(int k = 0; k < 4; k++)
        tone(snd_startup, STARTUP_N, k * RATE * 120 / 1000, STARTUP_N - k * RATE * 120 / 1000, f[k], 6500);
}

typedef struct { const int16_t *pcm; int n; } job_t;

static void *play_thread(void *arg)
{
    job_t *j = arg;
    int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        int frag = (4 << 16) | 10;                /* 4 x 1 KB: pouca latência */
        ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
        int fmt = AFMT_S16_LE, ch = 1, sp = RATE;
        if(!ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) && !ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) &&
           !ioctl(fd, SNDCTL_DSP_SPEED, &sp)) {
            const char *p = (const char *)j->pcm;
            size_t left = (size_t)j->n * 2;
            while(left) {
                ssize_t w = write(fd, p, left);
                if(w <= 0) break;
                p += w;
                left -= (size_t)w;
            }
        }
        close(fd);                                /* drena o que falta tocar */
    }
    atomic_store(&playing, 0);
    return NULL;
}

bool fx_audio_present(void) { return access("/dev/dsp", W_OK) == 0; }

static bool quiet;                   /* modo performance: sem sons de toque nem animações */
static bool awake;                   /* vídeo tocando: a tela não escurece sem toque */

void fx_quiet(bool on) { quiet = on; }
void fx_keep_awake(bool on) { awake = on; }

void fx_sound(fx_sound_t s)
{
    const bf_settings_t *cfg = settings();
    if(quiet) return;
    if(s == FX_STARTUP ? !cfg->boot_sound : !cfg->sounds) return;
    if(player_state() != PL_STOPPED) return;      /* o Multimídia é dono do /dev/dsp */
    int expected = 0;
    if(!atomic_compare_exchange_strong(&playing, &expected, 1)) return;   /* um som por vez */
    static job_t job;
    job.pcm = s == FX_CLICK ? snd_click : s == FX_TOGGLE ? snd_toggle : snd_startup;
    job.n = s == FX_CLICK ? CLICK_N : s == FX_TOGGLE ? TOGGLE_N : STARTUP_N;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if(pthread_create(&t, &at, play_thread, &job)) atomic_store(&playing, 0);
    pthread_attr_destroy(&at);
}

/* ------------------------------------------------------ toque e inatividade */

static lv_obj_t *dim_cover;

static void undim_cb(lv_event_t *e)
{
    (void)e;
    const bf_status_provider_t *p = status_provider();
    if(p->backlight_get() >= 0) p->backlight_set(settings()->backlight);
    lv_indev_wait_release(lv_indev_active());     /* o toque só acende a tela */
    if(dim_cover) { lv_obj_delete_async(dim_cover); dim_cover = NULL; }
}

void fx_tick(void)
{
    int min = settings()->dim_min;
    const bf_status_provider_t *p = status_provider();
    if(awake || dim_cover || min <= 0 || p->backlight_get() < 0) return;
    if(lv_display_get_inactive_time(NULL) < (uint32_t)min * 60000u) return;
    p->backlight_set(5);
    dim_cover = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(dim_cover);
    lv_obj_set_size(dim_cover, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(dim_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dim_cover, undim_cb, LV_EVENT_PRESSED, NULL);
}

static bool is_interactive(lv_obj_t *o)
{
    if(!o || !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return false;
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) return false;   /* ex.: painel do Big Screen */
    if(lv_obj_check_type(o, &lv_button_class) || lv_obj_check_type(o, &lv_switch_class) ||
       lv_obj_check_type(o, &lv_buttonmatrix_class) || lv_obj_check_type(o, &lv_keyboard_class))
        return true;
    return lv_obj_get_event_count(o) > 0;         /* objetos com ação própria */
}

static void indev_clicked(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_param(e);
    if(!is_interactive(o) || o == dim_cover) return;
    bool toggle = lv_obj_check_type(o, &lv_switch_class) || lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE);
    fx_sound(toggle ? FX_TOGGLE : FX_CLICK);
}

void fx_init(void)
{
    sounds_init();
    for(lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if(lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER)
            lv_indev_add_event_cb(in, indev_clicked, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------- animações */

lv_anim_enable_t fx_anim(void) { return settings()->anim && !quiet ? LV_ANIM_ON : LV_ANIM_OFF; }

static void tx_cb(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void ty_cb(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }

void fx_slide_in(lv_obj_t *obj, int dx, int dy, uint32_t delay_ms, uint32_t ms)
{
    if(!settings()->anim) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if(dx) {
        lv_obj_set_style_translate_x(obj, dx, 0);
        lv_anim_set_exec_cb(&a, tx_cb);
        lv_anim_set_values(&a, dx, 0);
        lv_anim_start(&a);
    }
    if(dy) {
        lv_obj_set_style_translate_y(obj, dy, 0);
        lv_anim_set_exec_cb(&a, ty_cb);
        lv_anim_set_values(&a, dy, 0);
        lv_anim_start(&a);
    }
}
