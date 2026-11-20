/* screens.h — definições de todas as telas. */
#ifndef BF_SCREENS_H
#define BF_SCREENS_H

#include "../core/nav.h"

extern const bf_screen_def_t scr_home_def, scr_settings_def, scr_sysinfo_def, scr_run_def,
       scr_monitor_def, scr_calibrate_def, scr_backlight_def, scr_volume_def,
       scr_language_def, scr_date_def, scr_power_def, scr_reset_def, scr_ssh_def,
       scr_advanced_def, scr_feature_def, scr_music_def, scr_files_def, scr_term_def;

/* Argumento de SCR_FEATURE: funções ainda sem implementação/hardware. */
typedef enum {
    FEAT_MULTIMEDIA, FEAT_TV, FEAT_BLUETOOTH, FEAT_BOOK, FEAT_GPS, FEAT_FILES,
    FEAT_TERMINAL, FEAT_BIGSCREEN, FEAT_FM, FEAT_USB, FEAT_GPSINFO,
    FEAT_COUNT
} bf_feature_t;

/* Argumento de SCR_RUN */
#define RUN_ARG_NORMAL   0
#define RUN_ARG_TERMINAL 1

/* Ganchos usados pelo modo --screenshot */
void scr_home_show_page(int page);
void scr_run_demo_keyboard(void);
void scr_run_demo_command(const char *cmd);
void scr_calibrate_demo_step(int step);
void scr_music_demo(int step);   /* 0 tocar a 1ª faixa, 1 lista, 2 tela cheia, 3 parar */
void music_play_file(const char *path);   /* Arquivos -> Multimídia */
void scr_files_demo(const char *path);    /* NULL: locais */
void scr_term_demo_type(const char *s);   /* digita no shell do Terminal */

#endif
