/* cover.h — capa de álbum (JPEG da tag ID3) reduzida para a tela. */
#ifndef BF_COVER_H
#define BF_COVER_H
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COVER_PX 176

/* Decodifica um JPEG (baseline) em memória para COVER_PX x COVER_PX RGB565.
 * Retorna a imagem (buffer estático, válido até a próxima chamada) ou NULL
 * (PNG, JPEG progressivo, dados inválidos). */
const lv_image_dsc_t *cover_decode(const uint8_t *data, size_t len);

/* JPEG (baseline) reduzido para caber em max_w x max_h mantendo a proporção
 * (nunca amplia). Aloca os pixels: liberar com jpeg_free. */
bool jpeg_fit(const uint8_t *data, size_t len, unsigned max_w, unsigned max_h, lv_image_dsc_t *img);
void jpeg_free(lv_image_dsc_t *img);
/* só lê o cabeçalho: largura e altura do JPEG */
bool jpeg_size(const uint8_t *data, size_t len, unsigned *w, unsigned *h);
#endif
