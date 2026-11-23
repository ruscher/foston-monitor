/* wallpaper.h — papel de parede da área de conteúdo, calculado uma vez por
 * troca de tema: degradê radial com pontilhado ordenado (sem faixas no
 * RGB565) e, opcionalmente, o símbolo FOSTONOS como marca d'água. Fica numa
 * imagem RGB565 de 800x360 (576 KB): redesenhar é só copiar. */
#ifndef BF_WALLPAPER_H
#define BF_WALLPAPER_H
#include "theme.h"

#define BF_WALL_W BF_SCREEN_W
/* área do conteúdo; no Foston Glass a tela inteira (atrás da barra superior translúcida) */
#define BF_WALL_H (theme_glass() ? BF_SCREEN_H : BF_CONTENT_H)

bool wallpaper_build(lv_image_dsc_t *dsc, const bf_palette_t *p, lv_color_t glow, bool logo);
/* imagem do usuário recortada para preencher 800x360 (centro), escurecida um pouco no
 * tema escuro para o texto continuar legível. Guarda o resultado em $DATA/wallpaper.cache
 * (a decodificação do PNG/JPEG leva segundos no aparelho). false se não abrir. */
bool wallpaper_from_image(lv_image_dsc_t *dsc, const char *path, const bf_palette_t *p);
#endif
