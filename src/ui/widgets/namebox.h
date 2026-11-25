/* namebox.h — janela para digitar um nome (Novo grupo, Renomear grupo): título, campo, aviso
 * de erro, Cancelar / confirmar e o teclado pt-BR com acentos (widgets/kbd.c). O teclado
 * físico também digita (core/hwkbd.c acha o campo na camada de cima); Enter confirma, Esc
 * cancela.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_NAMEBOX_H
#define BF_NAMEBOX_H
#include "lvgl.h"
#include <stdbool.h>

/* confirmar: NULL fecha a janela; um texto é o erro, mostrado com a janela ainda aberta.
 * text NULL: cancelado */
typedef const char *(*namebox_cb_t)(const char *text, void *ud);

void namebox_open(const char *symbol, const char *title, const char *label, const char *initial,
                  const char *ok_text, int max_chars, namebox_cb_t cb, void *ud);
bool namebox_is_open(void);
void namebox_close(void);                  /* cancela (callback com NULL) */
/* testes: digita e confirma como pelo teclado */
const char *namebox_demo_submit(const char *text);
/* testes: põe o texto no campo e devolve o que ficou (o campo corta no limite) */
const char *namebox_demo_set(const char *text);
#endif
