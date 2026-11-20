/* imgview.h — visualizador de imagem em tela cheia com zoom e arrasto.
 * SVG: re-rasterizado em cada nível de zoom (nanosvg), até 2048 px de lado.
 * JPEG: reduzido para caber; o zoom vai até a resolução original. */
#ifndef BF_IMGVIEW_H
#define BF_IMGVIEW_H
#include <stdbool.h>

bool imgview_open(const char *path);      /* .svg ou .jpg/.jpeg */
void imgview_close(void);
bool imgview_is_open(void);
#endif
