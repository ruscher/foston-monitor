#ifndef BF_TILE_H
#define BF_TILE_H
#include "lvgl.h"
/* Ícone grande da tela inicial: quadrado com degradê + rótulo. */
lv_obj_t *tile_create(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *label,
                      uint32_t color, lv_event_cb_t cb, void *ud);
/* o mesmo, com um texto curto (a inicial de um WebApp) no lugar do ícone */
lv_obj_t *tile_create_text(lv_obj_t *parent, const char *text, const char *label,
                           uint32_t color, lv_event_cb_t cb, void *ud);
#endif
