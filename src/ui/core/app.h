/* app.h — montagem da interface (barras + conteúdo) e ações globais. */
#ifndef BF_APP_H
#define BF_APP_H
#include "lvgl.h"
#include <stdbool.h>

void app_init(lv_display_t *disp);
void app_rebuild(void);            /* após trocar tema/língua (assíncrono) */
void app_rebuild_now(void);
void app_apply_hw_settings(void);  /* brilho/volume salvos -> hardware */
void app_power(bool reboot);
lv_display_t *app_display(void);
lv_obj_t    *app_content(void);   /* área de conteúdo (800x360) */
bool         app_shutting_down(void);
lv_obj_t   *app_bye_screen(bool reboot);   /* tela "Desligando/Reiniciando" */   /* desligar/reiniciar em andamento */
#endif
