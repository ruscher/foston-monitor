/* voice.h — fala instruções do Big GPS com o eSpeak NG (pt-BR), num processo separado:
 * o binário vem do sistema (/usr/bin/espeak-ng) ou do cartão, os dados de voz do cartão
 * (BigFoston/big-gps/voz/espeak-ng-data). O áudio sai pelo /dev/dsp; se o Multimídia
 * estiver tocando, ele solta o dispositivo durante a fala e continua depois. */
#ifndef BF_VOICE_H
#define BF_VOICE_H
#include <stdbool.h>

bool voice_available(void);
/* Fala (não bloqueia); um aviso novo substitui o que ainda esperava a vez. */
void voice_say(const char *text);
/* Para a fala atual e esquece a fila (ao sair do mapa). */
void voice_stop(void);

#endif
