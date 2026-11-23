/* glass.h — peças do Foston Glass (tela inicial "vidro"): paleta azul-lavanda fixa (o vidro
 * fica sobre o papel de parede, em qualquer tema), cartões translúcidos, ícones em degradê e
 * barras de sinal. Só transparência simples e degradê vertical: nada de desfoque (o ARM1176
 * não tem GPU; um desfoque por quadro custaria a tela inteira). */
#ifndef BF_GLASS_H
#define BF_GLASS_H
#include "lvgl.h"
#include <stdbool.h>

#define GL_BLUE     0x285C95
#define GL_DEEP     0x153C6A
#define GL_LIGHT    0x73B8EE
#define GL_SKY      0x91D7F5
#define GL_LAVENDER 0xB5A7EC
#define GL_LAVBLUE  0x788ED6
#define GL_WHITE    0xF8FBFF
#define GL_TEXT2    0xD5E5FA
#define GL_ACTIVE   0x91E4F2

/* cartão de vidro em (x, y) do pai, w x h, raio r */
lv_obj_t *glass_card(lv_obj_t *parent, int x, int y, int w, int h, int r);
lv_obj_t *glass_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
/* quadrado arredondado em degradê (top -> bottom) com o ícone em branco no centro;
 * icon NULL: o chamador desenha o conteúdo */
lv_obj_t *glass_tile(lv_obj_t *parent, int size, int radius, uint32_t top, uint32_t bottom,
                     const lv_image_dsc_t *icon, const char *symbol, const lv_font_t *font);
/* barras de sinal (0..4 acesas); níveis < 0: escondidas */
lv_obj_t *glass_bars(lv_obj_t *parent);
void glass_bars_set(lv_obj_t *bars, int level);
#endif
