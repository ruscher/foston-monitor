/* perfui.h — o modo performance visto pelo usuário (os serviços em si: perfmode.c).
 *
 * Liga por dois motivos: um vídeo tocando no app Internet (automático, se o ajuste deixar,
 * depois de perf_enter_s segundos) ou o botão "Ativar agora" em Ajustes > Modo performance.
 * Desliga quando o vídeo fica parado perf_exit_s segundos (o manual só pelo botão), ao sair
 * do Internet ou pela etiqueta flutuante -> Desativar.
 *
 * Mostra a janela com barra de progresso nas trocas (perf_modal) e a etiqueta flutuante
 * "Performance" no canto superior esquerdo enquanto ligado (perf_tag): um toque abre
 * Desativar / Configurar / Fechar.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_PERFUI_H
#define BF_PERFUI_H
#include <stdbool.h>

typedef enum { PERF_WHY_NONE, PERF_WHY_VIDEO, PERF_WHY_MANUAL } perf_why_t;

void       perfui_init(void);
void       perfui_video(bool playing);   /* VIDEO 1/0 do app Internet */
void       perfui_video_gone(void);      /* saiu do Internet: o vídeo acabou já */
void       perfui_manual(bool on);       /* Ajustes: Ativar/Desativar agora */
void       perfui_disable(void);         /* desliga já (manual e o vídeo atual) */
perf_why_t perfui_why(void);
bool       perfui_video_playing(void);
void       perfui_settings_changed(void);/* depois de mudar um ajuste perf_* */
void       perfui_tag_hide(bool hide);   /* tela cheia: sem nada por cima do vídeo */
void       perfui_tag_offset(int y);     /* Internet: abaixo da barra de ferramentas (0 = padrão) */
void       perfui_dialog(void);          /* o que a etiqueta abre: Desativar / Configurar / Fechar */
#endif
