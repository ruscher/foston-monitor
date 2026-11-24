/* padview.h — um controle de jogo desenhado (SC Controller: testar, registrar, lista).
 *
 * A silhueta (corpo e pegadas) é um SVG rasterizado uma vez por tamanho, com sombra e contorno;
 * botões, direcional, analógicos, gatilhos e touchpad são objetos do LVGL por cima, nas
 * posições de um DualShock (desenho de 400 x 250). No PlayStation os botões de frente têm as
 * formas △ ○ ✕ □ desenhadas com linhas; nos outros, A B X Y nas cores do Xbox.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_PADVIEW_H
#define BF_PADVIEW_H
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* w = largura em px; a altura é padview_height(w) */
lv_obj_t *padview_create(lv_obj_t *parent, int w, bool sony);
int       padview_height(int w);
/* estado ao vivo: btn[PC__NBTN], ax[PC__NAXIS] (core/padmap.h) */
void      padview_state(lv_obj_t *pv, const bool *btn, const int16_t *ax);
/* o controle da vez pisca (registro); analógicos ganham uma seta; -1 = nenhum */
void      padview_focus(lv_obj_t *pv, int ctl);
/* já ensinado (registro): fica verde */
void      padview_done(lv_obj_t *pv, int ctl, bool done);
#endif
