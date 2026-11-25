/* svgimg.h — SVG rasterizado (nanosvg) para uma imagem RGB565 do LVGL. */
#ifndef BF_SVGIMG_H
#define BF_SVGIMG_H
#include "lvgl.h"
#include <stdbool.h>

typedef struct svgdoc svgdoc_t;

/* carrega e interpreta (não rasteriza); NULL se inválido */
svgdoc_t *svg_open(const char *path);
void      svg_close(svgdoc_t *d);
/* tamanho natural do desenho, em px */
void      svg_size(const svgdoc_t *d, int *w, int *h);
/* rasteriza com a escala dada (1.0 = natural) sobre o fundo bg; a imagem
 * nunca passa de max_px pixels de lado. Libera a anterior em img. */
bool      svg_render(svgdoc_t *d, float scale, lv_color_t bg, int max_px, lv_image_dsc_t *img);
void      svg_image_free(lv_image_dsc_t *img);
/* ícone de app: o desenho cabe em box x box (com margem), só a forma (A8, a opacidade de
 * cada pixel), para ser pintado com image_recolor como os ícones do sistema */
bool      svg_render_mask(const char *path, int box, lv_image_dsc_t *img);
/* um SVG em texto (nanosvg altera a cópia) rasterizado com w px de largura, como máscara A8;
 * a altura segue a proporção do desenho */
bool      svg_render_mask_str(const char *svg, int w, lv_image_dsc_t *img);
#endif
