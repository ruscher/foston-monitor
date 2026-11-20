/* dialog.h — diálogo modal (camada superior) com botões grandes. */
#ifndef BF_DIALOG_H
#define BF_DIALOG_H
#include "lvgl.h"

typedef void (*dialog_cb_t)(int button, void *ud);  /* button = índice ou -1 (fechado) */

/* btns: textos dos botões (da esquerda p/ direita). primary_idx/danger_idx
 * destacam um botão (-1 = nenhum). Retorna o contêiner do cartão para o
 * chamador acrescentar conteúdo extra, se quiser. */
lv_obj_t *dialog_open(const char *symbol, lv_color_t sym_color, const char *title,
                      const char *body, const char *const *btns, int n_btns,
                      int primary_idx, int danger_idx, dialog_cb_t cb, void *ud);
void dialog_close_all(void);
#endif
