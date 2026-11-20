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
#endif
