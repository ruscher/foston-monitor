/* btaudio.h — o áudio Bluetooth que o bluealsa-aplay toca (patch BigFoston do BlueALSA):
 * amostras recentes em mono para o espectro (/run/bluealsa-ring, sem decodificar o SBC de
 * novo) e soltar a saída de som para a voz do Big GPS (SIGUSR1/SIGUSR2). */
#ifndef BF_BTAUDIO_H
#define BF_BTAUDIO_H
#include <stdbool.h>
#include <stdint.h>

/* áudio chegando agora (o anel andou nos últimos ~400 ms) */
bool btaudio_live(void);
/* as n amostras mais recentes (n <= 2048); false sem o anel */
bool btaudio_samples(int16_t *out, int n);
/* true: o bluealsa-aplay solta o dispositivo e descarta o áudio; false: volta a tocar */
void btaudio_hold(bool on);
/* Toca a voz (mono, rate Hz) junto com a música do celular, que abaixa enquanto ela fala
 * (patch 0002 do bluealsa-aplay, /run/bluealsa-voice). Bloqueia até a voz sair. Devolve
 * quantas amostras foram tocadas: 0 quando o bluealsa-aplay não está tocando música agora,
 * menos que n quando a música parou no meio (o resto fica para o /dev/dsp). stopped() true
 * descarta o que falta e devolve n. */
uint32_t btaudio_say(const int16_t *pcm, uint32_t n, uint32_t rate, bool (*stopped)(void));
#endif
