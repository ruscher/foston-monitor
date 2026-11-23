/* perfmode.h — modo performance: enquanto um vídeo toca no app Internet, os serviços que não
 * ajudam a ver o vídeo param (GPS, Bluetooth, telefone, tarefas agendadas) e voltam depois.
 *
 * Só para o que estava rodando (o que o usuário desligou continua desligado) e volta na ordem
 * inversa. Parar leva ~3 s e voltar ~6 s (o Bluetooth carrega o firmware do chip): numa
 * thread, com o progresso para a janela da interface. O shell também ganha prioridade
 * (nice -5) enquanto o modo está ligado.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_PERFMODE_H
#define BF_PERFMODE_H
#include <stdbool.h>

typedef enum { PM_OFF, PM_ENTERING, PM_ON, PM_LEAVING } pm_state_t;

void       perfmode_enter(void);
void       perfmode_leave(void);       /* também no meio da entrada: volta assim que ela terminar */
pm_state_t perfmode_state(void);
/* passo atual de 0 a total (para a barra) */
void       perfmode_progress(int *done, int *total);
#endif
