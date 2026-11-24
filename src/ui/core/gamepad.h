/* gamepad.h — controle de jogo (Bluetooth ou USB) para usar a interface.
 *
 * O core/hwkbd.c acha o aparelho e passa cada evento do evdev para cá. Os botões viram teclas
 * (entregues como as do teclado físico) e o analógico esquerdo move um cursor que toca a tela:
 *   analógico esquerdo  cursor          ✕ / A   toca onde está o cursor (segurar: arrastar)
 *   analógico direito   rola            ○ / B   voltar (Esc)
 *   direcional          setas           △ / Y   Enter (confirmar)
 *   L1 / R1             página ↑ / ↓    □ / X   espaço
 *   L2 / R2             volume − / +    PS      início
 *   Options / Start     tocar/pausar
 * O controle do PlayStation sem o driver do kernel (hid-generic) manda os botões em outra
 * ordem (□ ✕ ○ △ L1 R1 L2 R2 Share Options L3 R3 PS) e o analógico direito em Z/RZ: o
 * gamepad_attach() recebe isso em sony_generic.
 * Parado por GAMEPAD_IDLE_MIN minutos, o controle Bluetooth é desconectado: o do PlayStation
 * nunca desliga sozinho fora do console e gastaria a bateria; o botão PS o liga de novo.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_GAMEPAD_H
#define BF_GAMEPAD_H
#include <stdbool.h>
#include <stdint.h>

#define GAMEPAD_MAX 2
#define GAMEPAD_IDLE_MIN 10

/* faixa de um eixo (struct input_absinfo) */
typedef struct { int32_t min, max, flat; bool have; } gp_axis_t;

/* eixos que importam, na ordem: X, Y, Z, RX, RY, RZ, HAT0X, HAT0Y */
enum { GPA_X, GPA_Y, GPA_Z, GPA_RX, GPA_RY, GPA_RZ, GPA_HX, GPA_HY, GPA__N };

/* um controle novo; addr = endereço Bluetooth ("AA:BB:..."; "" = USB). Devolve o número
 * (0..GAMEPAD_MAX-1) ou -1 sem lugar */
int  gamepad_attach(bool sony_generic, const gp_axis_t ax[GPA__N], const char *addr);
void gamepad_detach(int slot);
/* um evento do evdev (EV_KEY, EV_ABS; EV_SYN é ignorado) */
void gamepad_event(int slot, uint16_t type, uint16_t code, int32_t value);
int  gamepad_count(void);

/* cursor (testes): posição e se está à vista */
bool gamepad_cursor(int32_t *x, int32_t *y);
#endif
