/* hold.h — toque e toque longo (2 s) num ícone, para toque resistivo de um ponto e mouse.
 *
 * Toque rápido: tap(). Dedo parado por HOLD_MS: hold() (o mesmo toque não abre o app).
 * Depois de HOLD_RING_MS aparece um anel de progresso em volta do ícone; mexer o dedo além de
 * HOLD_SLOP px, rolar a página ou soltar depois de HOLD_TAP_MAX (e antes de HOLD_MS) cancela.
 * Sem temporizador próprio: a contagem anda nos eventos PRESSING do LVGL (a interface não fica
 * bloqueada). Funciona com qualquer entrada que gere PRESSING: toque, mouse, cursor do controle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_HOLD_H
#define BF_HOLD_H
#include "lvgl.h"
#include <stdbool.h>

#define HOLD_MS      2000
#define HOLD_RING_MS 250
#define HOLD_SLOP    18
#define HOLD_TAP_MAX 700          /* soltar depois disso (o anel já à vista) desiste: não abre */

typedef void (*hold_cb_t)(lv_obj_t *obj, void *ud);

/* ud é do chamador (não é liberado aqui) */
void hold_attach(lv_obj_t *obj, hold_cb_t tap, hold_cb_t hold, void *ud);
/* destaque de "selecionado" (o ícone do menu aberto, o app sendo movido) */
void hold_select(lv_obj_t *obj, bool on);
/* tira o destaque do último ícone segurado (o menu fechou) */
void hold_unselect_last(void);
#endif
