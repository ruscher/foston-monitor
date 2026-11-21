/* imgview.h — visualizador de imagem em tela cheia com zoom e arrasto.
 * JPEG, PNG, BMP, GIF e SVG (core/imgload): reduzidos para caber; o zoom vai
 * até a resolução original (SVG re-rasterizado em cada nível, até 2048 px).
 * GIF de até 1200 px e 2 MB toca animado (sem zoom).
 * Com imgview_set_nav, mostra ◀ ▶ e deslizar troca de imagem (Galeria). */
#ifndef BF_IMGVIEW_H
#define BF_IMGVIEW_H
#include <stdbool.h>

typedef void (*imgview_nav_cb_t)(int dir, void *ud);   /* dir: -1 anterior, +1 próxima */

bool imgview_open(const char *path);
void imgview_close(void);                  /* também desliga a navegação */
bool imgview_is_open(void);
/* chamar antes de imgview_open; vale até imgview_close */
void imgview_set_nav(imgview_nav_cb_t cb, void *ud);
#endif
