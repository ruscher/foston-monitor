/* page.h — blocos de página reutilizados pelas telas. */
#ifndef BF_PAGE_H
#define BF_PAGE_H
#include "lvgl.h"
#include <stdbool.h>

/* Contêiner de página: coluna rolável com padding padrão. */
lv_obj_t *page_create(lv_obj_t *parent);
/* Cabeçalho: ícone (símbolo de fonte) em círculo + título + subtítulo. */
lv_obj_t *page_header(lv_obj_t *page, const char *symbol, lv_color_t color,
                      const char *title, const char *subtitle);
/* Aviso honesto: "Não disponível neste hardware ainda" + o que falta. */
lv_obj_t *page_unavailable(lv_obj_t *page, const char *missing);
/* Linha "chave  valor" dentro de um cartão. Retorna o rótulo do valor. */
lv_obj_t *page_kv(lv_obj_t *parent, const char *key, const char *value);
/* Linha com rótulo + switch. Retorna o switch. */
lv_obj_t *page_switch_row(lv_obj_t *parent, const char *label, bool on,
                          lv_event_cb_t cb, void *ud);
/* Selo "valor simulado" quando o provedor de status é o simulador. */
void page_sim_note(lv_obj_t *parent);
/* Ícone redondo com símbolo (usado nas listas). */
lv_obj_t *page_round_icon(lv_obj_t *parent, const char *symbol, lv_color_t color, int size);
/* Cartão com slider grande (0..100) + valor em %. Retorna o slider;
 * o rótulo do valor é atualizado automaticamente. */
lv_obj_t *page_slider_card(lv_obj_t *parent, const char *label, const char *sym_lo,
                           const char *sym_hi, int min, int value, lv_event_cb_t cb, void *ud);
/* Cartão de escolha (uma de várias): título, explicação embaixo e ✓ no escolhido. */
lv_obj_t *page_choice(lv_obj_t *parent, const char *symbol, const char *title, const char *sub, bool selected,
                      lv_event_cb_t cb, void *ud);
#endif
