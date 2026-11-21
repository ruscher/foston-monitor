/* screens.h — definições de todas as telas. */
#ifndef BF_SCREENS_H
#define BF_SCREENS_H

#include "../core/nav.h"
#include <stddef.h>

extern const bf_screen_def_t scr_home_def, scr_settings_def, scr_sysinfo_def, scr_run_def,
       scr_monitor_def, scr_calibrate_def, scr_backlight_def, scr_volume_def,
       scr_language_def, scr_date_def, scr_power_def, scr_reset_def, scr_ssh_def,
       scr_advanced_def, scr_feature_def, scr_music_def, scr_files_def, scr_term_def, scr_book_def,
       scr_appearance_def, scr_bigscreen_def, scr_gallery_def, scr_gps_def, scr_map_def;

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
void scr_gallery_demo(int step); /* 0 espera as miniaturas, 1 abre a 1ª, 2 fecha */
void scr_music_demo(int step);   /* 0 tocar a 1ª faixa, 1 lista, 2 tela cheia, 3 parar */
void scr_map_demo(int step);     /* Big GPS: 0 destino e rota, 1 simular, 2 avançar 60 s, 3-5 busca */
void music_play_file(const char *path);   /* Arquivos -> Multimídia */
enum { MUSIC_TOGGLE, MUSIC_PREV, MUSIC_NEXT };
bool music_has_queue(void);               /* há faixas na fila do Multimídia */
void music_command(int cmd);              /* MUSIC_*: Painel da Início */
/* faixa atual: título (tag ou nome do arquivo) e "artista — álbum"; false sem faixa */
bool music_now(char *title, size_t tn, char *sub, size_t sn, int *idx);
/* Apps da Início (tabela em scr_home.c; a ordem só cresce no fim, é a dos bits
 * de settings()->home_apps) */
int         home_app_count(void);
const char *home_app_name(int i);
const lv_image_dsc_t *home_app_icon(int i);
uint32_t    home_app_color(int i);
bool        home_app_is_settings(int i);
void        home_app_open(int i);
void scr_files_demo(const char *path);    /* NULL: locais */
void scr_term_demo_type(const char *s);   /* digita no shell do Terminal */
void scr_book_demo(const char *path);     /* NULL: biblioteca */
void book_open_file(const char *path);     /* Arquivos -> Livros */

#endif
