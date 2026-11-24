/* perfui.h — o modo performance visto pelo usuário (os serviços em si: perfmode.c).
 *
 * Liga por três motivos: um vídeo tocando no app Internet (automático, se o ajuste deixar,
 * depois de perf_enter_s segundos), um controle de jogo conectado (perf_pad) ou o botão
 * "Ativar agora" em Ajustes > Modo performance. Desliga quando o vídeo fica parado
 * perf_exit_s segundos, quando o controle sai (o manual só pelo botão), ao sair do Internet
 * ou pela etiqueta flutuante -> Desativar.
 *
 * Com o modo ligado, o app aberto religa o que usa (perfmode_hold): Big GPS e GpsInfo o GPS,
 * Telefone o telefone e o Bluetooth, a tela Bluetooth o Bluetooth; ao sair, pausa de novo
 * depois de HOLD_RELEASE_MS. Teclado ou controle Bluetooth conectado mantém o Bluetooth.
 * Com um controle conectado, a interface (nice -5) e o bluetoothd (nice -10, leva os dados
 * do controle) ganham prioridade.
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

typedef enum { PERF_WHY_NONE, PERF_WHY_VIDEO, PERF_WHY_MANUAL, PERF_WHY_PAD, PERF_WHY_TV, PERF_WHY_PLAN, PERF_WHY_STREAM } perf_why_t;

void       perfui_init(void);
void       perfui_video(bool playing);   /* VIDEO 1/0 do app Internet */
void       perfui_video_gone(void);      /* saiu do Internet: o vídeo acabou já */
void       perfui_tv(bool open);         /* app TV aberto / fechado (perf_tv) */
void       perfui_stream(bool on);       /* imagem do Big Remote Play (perf_stream) */
void       perfui_manual(bool on);       /* Ajustes: Ativar/Desativar agora */
void       perfui_disable(void);         /* desliga já (manual e o vídeo atual) */
perf_why_t perfui_why(void);
bool       perfui_video_playing(void);
void       perfui_settings_changed(void);/* depois de mudar um ajuste perf_* */
void       perfui_tag_hide(bool hide);   /* tela cheia: sem nada por cima do vídeo */
void       perfui_tag_offset(int y);     /* Internet: abaixo da barra de ferramentas (0 = padrão) */
void       perfui_dialog(void);          /* o que a etiqueta abre: Desativar / Configurar / Fechar */
#endif
