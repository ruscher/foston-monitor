#ifndef BF_WIDGET_TOPBAR_H
#define BF_WIDGET_TOPBAR_H
#include "lvgl.h"
lv_obj_t *topbar_create(lv_obj_t *parent);
void topbar_set_title(const char *title);   /* NULL = tela inicial */
void topbar_update(void);                   /* relógio + ícones (chamado a cada 1 s) */
#endif
