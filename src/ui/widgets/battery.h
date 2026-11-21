/* battery.h — ícone de bateria desenhado (contorno, nível, raio).
 *
 *   descarregando        contorno e nível na cor do texto; vermelho <= 15%
 *   carregando           nível âmbar e um raio por cima
 *   carga completa       bateria verde, com o raio (carregador ligado)
 *   ligado sem carregar  nível cinza com o raio (fonte fraca, ex. USB do PC)
 *   desconhecido         só o contorno, apagado (nunca inventa nível)
 *
 * As cores vêm da paleta (verde, âmbar e vermelho são os mesmos em todos os
 * temas), então o verde de "carga completa" não se confunde com o destaque. */
#ifndef BF_BATTERY_H
#define BF_BATTERY_H
#include "lvgl.h"
#include "../core/status.h"

/* w x h é o corpo; o polo fica à direita (w/10 de largura). font: o do raio. */
lv_obj_t *battery_create(lv_obj_t *parent, int w, int h, const lv_font_t *font);
/* pct -1 = desconhecido */
void battery_set(lv_obj_t *bat, int pct, bf_bat_state_t st);
/* Rótulo curto do estado ("carregando", "carga completa"...); "" se descarregando. */
const char *battery_state_text(bf_bat_state_t st);
#endif
