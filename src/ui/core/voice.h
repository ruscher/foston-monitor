/* voice.h — a voz do Big GPS: Piper pt_BR-faber-medium, a única voz da navegação. As frases são
 * geradas no PC (tools/big-gps/make-faber-voice.py) e ficam no cartão em
 * BigFoston/big-gps/voz (arquivos .bgv); o aparelho só as toca (o modelo neural não roda no FOSTON,
 * docs/big-gps-2/22-piper-faber.md). Sem o pacote não há voz — nunca outra voz no lugar.
 * O áudio sai pelo /dev/dsp; o Multimídia e a música do celular soltam o dispositivo durante a
 * fala e continuam depois. */
#ifndef BF_VOICE_H
#define BF_VOICE_H
#include <stdbool.h>

/* Há pacote de voz e saída de som. */
bool voice_available(void);
/* Fala (não bloqueia); um aviso novo substitui o que ainda esperava a vez. O texto vem em
 * trechos separados por '|' ("Em 300 metros|vire à direita|na Rua X"), cada um procurado no
 * pacote; trecho que o pacote não tem (um nome de rua, em geral) é pulado. */
void voice_say(const char *text);
/* Um bipe curto (aviso de velocidade acima do limite de um radar); pulado se a voz estiver
 * falando ou com uma frase na fila. Não bloqueia. */
void voice_beep(void);
/* Para a fala atual e esquece a fila (ao sair do mapa). */
void voice_stop(void);

#endif
