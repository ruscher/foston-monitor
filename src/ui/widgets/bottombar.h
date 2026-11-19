#ifndef BF_WIDGET_BOTTOMBAR_H
#define BF_WIDGET_BOTTOMBAR_H
#include "lvgl.h"
lv_obj_t *bottombar_create(lv_obj_t *parent);
void bottombar_set_active(int scr);
void bottombar_open_recents(void);
void bottombar_open_power(void);
#endif
