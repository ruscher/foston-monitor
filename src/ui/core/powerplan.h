/* powerplan.h — plano de energia (Ajustes > Potência) e efeitos visuais (Ajustes > Desempenho).
 *
 * O FOSTON não muda a frequência da CPU (468 MHz fixos, sem cpufreq): o plano age no que
 * gasta CPU e bateria e dá para controlar. Cada plano grava um conjunto de ajustes:
 *
 *                     Economia            Equilibrado          Desempenho
 *   brilho            até 50 %            como está            como está
 *   escurecer         1 min               nunca                nunca
 *   sons de toque     não                 sim                  sim
 *   efeitos           mínimos             completos            mínimos
 *   modo performance  sempre (GPS e       com vídeo, TV ou     sempre (GPS e tarefas) e
 *                     tarefas pausados)   controle             com vídeo, TV ou controle
 *   mapa do Big GPS   econômico           automático           econômico
 *
 * Com o modo sempre ligado só GPS e tarefas pausam fora dos apps que os usam: Bluetooth e
 * telefone continuam (o celular precisa conectar e as chamadas, chegar). Os ajustes podem ser
 * mudados um a um depois; o plano continua marcado.
 *
 * Efeitos: completos (animações e papel de parede), reduzidos (sem animações), mínimos (sem
 * animações e sem papel de parede, fundo liso: menos a desenhar em cada quadro).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_POWERPLAN_H
#define BF_POWERPLAN_H

void powerplan_apply(int plan);      /* BF_PLAN_* */
void fx_level_apply(int level);      /* BF_FX_* */
#endif
