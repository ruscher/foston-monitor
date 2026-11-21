/* osd.h — aviso no centro da tela (ícone + título + linha), que some sozinho.
 * Usado pelos eventos de dispositivo (core/devwatch.c) e por qualquer tela.
 * Não captura toques: o que está embaixo continua respondendo. */
#ifndef BF_OSD_H
#define BF_OSD_H
#include "lvgl.h"

#define OSD_MS 2500

/* icon: imagem A8 (recolorida de branco) num círculo da cor dada. Um aviso novo
 * substitui o anterior. */
void osd_show(const lv_image_dsc_t *icon, uint32_t color, const char *title, const char *sub);
#endif
