/* ctxmenu.h — menu de contexto (toque longo num ícone) e lista de escolha (Enviar para o
 * grupo): cartão sobre um fundo escurecido, perto do ícone e sempre dentro da tela. Toque
 * fora, Voltar ou Esc fecham. No Foston Glass, com o vidro azul do tema.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_CTXMENU_H
#define BF_CTXMENU_H
#include "lvgl.h"
#include <stdbool.h>

typedef struct {
    const char *symbol;              /* símbolo da fonte, ou NULL */
    const lv_image_dsc_t *img;       /* ou um ícone A8 num quadrado da cor color */
    uint32_t color;
    const char *text;
    const char *note;                /* texto pequeno à direita ("atual", "3 apps") ou NULL */
    int id;
    bool danger, current;
} ctx_item_t;

typedef void (*ctx_cb_t)(int id, void *ud);       /* id -1: fechado sem escolha */

/* anchor NULL: centralizado. Só um menu por vez (abrir outro fecha o anterior sem callback) */
lv_obj_t *ctxmenu_open(lv_obj_t *anchor, const char *title, const char *subtitle, const ctx_item_t *items,
                       int n, ctx_cb_t cb, void *ud);
bool ctxmenu_is_open(void);
/* fecha como um toque fora (callback com -1); true se havia menu (Esc / Voltar do teclado) */
bool ctxmenu_close(void);
/* escolhe a opção id como num toque (testes, FIFO de controle) */
void ctxmenu_pick(int id);
#endif
