/* webaudio.h — som do modo completo do app Internet (Chrome do PC).
 *
 * O som tem conexão própria (porta WEBAUDIO_PORT, só a rede USB), separada da imagem: lida
 * por uma thread de prioridade maior, não espera a interface desenhar nem os quadros de
 * vídeo na mesma conexão. O PC só entra com o código de uso único que o FOSTON manda pela
 * conexão principal já autorizada (webaudio_set_token). Conteúdo: PCM S16_LE estéreo a
 * 32 kHz, contínuo enquanto a página toca algo.
 *
 * Saída: buffer em anel e outra thread escrevendo no /dev/dsp. O /dev/dsp só fica aberto
 * enquanto chega som (1,5 s sem nada: solta), a música do Multimídia é pausada quando o
 * som da web começa e a voz do Big GPS tem prioridade (webaudio_hold).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_WEBAUDIO_H
#define BF_WEBAUDIO_H

#include <stdbool.h>
#include <stddef.h>

#define WEBAUDIO_RATE     32000
#define WEBAUDIO_CHANNELS 2
#define WEBAUDIO_PORT     7072

bool webaudio_listen(void);                  /* abre a porta (app Internet aberto) */
void webaudio_close(void);                   /* fecha a porta e a conexão, descarta o som */
void webaudio_set_token(const char *token);  /* NULL: nenhuma conexão de som aceita */
void webaudio_hold(bool on);                 /* outra parte precisa do /dev/dsp */
bool webaudio_playing(void);

#endif
