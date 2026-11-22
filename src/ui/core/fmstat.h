/* fmstat.h — transmissor FM pelo bffm (src/fm/bffm.c): estado em /run/bigfoston-fm, comandos
 * rodados em segundo plano (o bffm leva até ~1,5 s para ligar), configuração e favoritos em
 * data_path("fm.conf"), temporizador de desligamento e aviso de bateria fraca (fm_poll_global,
 * a cada segundo pelo app). docs/fm-radio. */
#ifndef BF_FMSTAT_H
#define BF_FMSTAT_H
#include <stdbool.h>
#include <time.h>

#define FM_MIN 880                 /* MHz x 10 */
#define FM_MAX 1080
#define FM_MAXFAV 8

typedef enum { FM_OFF, FM_STARTING, FM_ON, FM_ERROR, FM_UNAVAILABLE } fm_state_t;

typedef struct {
    fm_state_t state;
    int freq;                      /* MHz x 10 (no hardware, se ligado) */
    bool speaker_off;              /* só no rádio (alto-falante desligado) */
    time_t since;                  /* ligado desde */
    char error[64];
} fm_info_t;

typedef struct { char name[32]; int freq; } fm_fav_t;

typedef struct {
    int freq;                      /* última frequência escolhida */
    int mode;                      /* 0 só no rádio FM, 1 FOSTON + rádio FM */
    int timer_min;                 /* desligar depois de N minutos (0 = não) */
    bool help_seen;
    fm_fav_t fav[FM_MAXFAV];
    int nfav;
} fm_conf_t;

bool fm_available(void);
void fm_read(fm_info_t *f);
bool fm_busy(void);                         /* um comando do bffm rodando */
const char *fm_last_result(void);           /* "ok ..." / "erro ..." do último comando */
/* comandos (em segundo plano): ligar na frequência com o modo de saída, desligar, mudar frequência,
 * modo de saída, tom de teste */
bool fm_on(int freq, int mode);
bool fm_off(void);
bool fm_set_freq(int freq);
bool fm_set_mode(int mode);
bool fm_tone(int secs);
void fm_tone_stop(void);
bool fm_tone_running(void);
fm_conf_t *fm_conf(void);
void fm_conf_save(void);
bool fm_freq_valid(int freq);
void fm_format(int freq, char *out, size_t n);      /* "99,5" / "99.5" conforme o idioma */
time_t fm_off_at(void);                              /* desligamento programado (0 = nenhum) */
void fm_poll_global(void);
#endif
