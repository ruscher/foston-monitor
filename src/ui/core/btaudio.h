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
#endif
