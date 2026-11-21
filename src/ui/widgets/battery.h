/* battery.h — ícone de bateria (símbolo da fonte, com o nível em 5 passos).
 *
 * O ícone inteiro muda de cor:
 *   verde     carregador ou cabo USB ligado (carregando, carga completa ou
 *             ligado sem carregar), com um raio ao lado
 *   laranja   usando a bateria
 *   vermelho  usando a bateria, abaixo de 10%
 *   apagado   desconhecido (nunca inventa nível)
 *
 * Verde, laranja e vermelho são os mesmos em todos os temas de cores (o
 * laranja escurece no modo claro, para contrastar com o fundo). */
#ifndef BF_BATTERY_H
#define BF_BATTERY_H
#include "lvgl.h"
#include "../core/status.h"

lv_obj_t *battery_create(lv_obj_t *parent, const lv_font_t *font);
/* pct -1 = desconhecido */
void battery_set(lv_obj_t *bat, int pct, bf_bat_state_t st);
/* Rótulo curto do estado ("carregando", "carga completa"...); "" se descarregando. */
const char *battery_state_text(bf_bat_state_t st);
#endif
