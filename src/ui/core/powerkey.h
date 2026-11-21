/* powerkey.h — botão de energia (KEY_POWER do kernel) e modo Hibernar.
 *
 * Toque curto no botão: abre o diálogo de energia (Cancelar, Hibernar,
 * Reiniciar, Desligar); com ele aberto, fecha. Hibernar apaga a tela (sem
 * suspender o processador, que este kernel não suporta); o botão ou um toque
 * na tela acordam. */
#ifndef BF_POWERKEY_H
#define BF_POWERKEY_H
#include "lvgl.h"
#include <stdbool.h>

void powerkey_init(void);        /* começa a ler o botão (nada no simulador) */
void powerkey_press(void);       /* o que um toque no botão faz (também para testes) */
void powerkey_hibernate(void);
void powerkey_wake(void);
bool powerkey_sleeping(void);
#endif
