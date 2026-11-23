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

/* o que pausar (settings()->perf_svc) */
#define PM_SVC_GPS   (1u << 0)        /* bfgps */
#define PM_SVC_BT    (1u << 1)        /* Bluetooth inteiro: chip, bluetoothd, áudio, agente */
#define PM_SVC_PHONE (1u << 2)        /* bfphone (chamadas pelo celular) */
#define PM_SVC_CRON  (1u << 3)        /* crond (tarefas agendadas) */

/* svc: grupos PM_SVC_*; prio: a thread que chama (a da interface) ganha nice -5 */
void       perfmode_enter(unsigned svc, bool prio);
/* podem ser chamadas a qualquer momento, inclusive no meio de uma troca: a troca em curso
 * termina e, se o pedido mudou, a contrária começa em seguida (o estado sempre chega ao último
 * pedido) */
void       perfmode_leave(void);
pm_state_t perfmode_state(void);
/* na inicialização: um shell anterior parou serviços e não religou (travou, foi atualizado)?
 * religa agora (estado PM_LEAVING); true se havia algo a religar */
bool       perfmode_recover(void);
/* grupos PM_SVC_* parados por este modo agora (as telas mostram "pausado" em vez de
 * "indisponível"); o bit liga antes do stop e desliga depois do start */
unsigned   perfmode_paused(void);
/* passo atual de 0 a total (para a barra) */
void       perfmode_progress(int *done, int *total);
#endif
