/* gamepad.h — controle de jogo (Bluetooth ou USB) para usar a interface.
 *
 * O core/hwkbd.c acha o aparelho e passa cada evento do evdev para cá. Cada controle ganha um
 * mapa no formato do SDL GameControllerDB (core/padmap.c), procurado nesta ordem:
 *   1. o banco do usuário: controles registrados no SC Controller (gamepad_user_db);
 *   2. os bancos dos apps instalados (<dados>/apps/<app>/gamecontrollerdb.txt);
 *   3. linhas embutidas (DualShock 4 e DualSense pelo Bluetooth sem o driver da Sony);
 *   4. o mapa padrão do Linux pelos códigos (BTN_SOUTH = A...).
 * Com o mapa, os botões viram teclas (entregues como as do teclado físico) e o analógico
 * esquerdo move um cursor que toca a tela:
 *   analógico esquerdo  cursor          A (✕)   toca onde está o cursor (segurar: arrastar)
 *   analógico direito   rola            B (○)   voltar (Esc)
 *   direcional          setas           Y (△)   Enter (confirmar)
 *   LB / RB             página ↑ / ↓    X (□)   espaço
 *   LT / RT             volume − / +    Guia/PS início
 *   Start / Options     tocar/pausar
 * Parado por GAMEPAD_IDLE_MIN minutos, o controle Bluetooth é desconectado: o do PlayStation
 * nunca desliga sozinho fora do console e gastaria a bateria; o botão PS o liga de novo.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_GAMEPAD_H
#define BF_GAMEPAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "padmap.h"

#define GAMEPAD_MAX 2
#define GAMEPAD_IDLE_MIN 10
#define GP_NABS 0x40

/* faixa de um eixo (struct input_absinfo), por código ABS_* */
typedef struct { int32_t min, max; bool have; } gp_axis_t;
typedef struct { uint16_t bus, vendor, product, version; } gp_id_t;

/* de onde veio o mapa */
enum { GPM_DEFAULT, GPM_BUILTIN, GPM_DB, GPM_USER };

/* um controle novo (addr: endereço Bluetooth "AA:BB:...", "" = USB). Devolve o número
 * (0..GAMEPAD_MAX-1) ou -1 sem lugar */
int  gamepad_attach(const char *name, const gp_id_t *id, const pm_bits_t *bits,
                    const gp_axis_t abs[GP_NABS], const char *addr);
void gamepad_detach(int slot);
/* um evento do evdev (EV_KEY, EV_ABS; os outros são ignorados) */
void gamepad_event(int slot, uint16_t type, uint16_t code, int32_t value);
int  gamepad_count(void);

/* ------------------------------------------------ app SC Controller */

typedef struct {
    char name[64], guid[33], addr[18], map_name[64];
    int  map_from;                  /* GPM_* */
    bool bt;                        /* pelo Bluetooth (o barramento, mesmo sem endereço) */
    bool btn[PC__NBTN];
    int16_t ax[PC__NAXIS];          /* analógicos -1000..1000, gatilhos 0..1000 */
    uint32_t events;                /* eventos recebidos desde que conectou */
} gp_info_t;

bool gamepad_info(int slot, gp_info_t *out);
const pm_bits_t *gamepad_bits(int slot);
/* captura (testador e registro): o controle não mexe na interface; obs recebe cada evento cru
 * (NULL = nenhum) e o estado mapeado continua em gamepad_info */
typedef void (*gamepad_obs_t)(int slot, uint16_t type, uint16_t code, int32_t value);
void gamepad_capture(bool on, gamepad_obs_t obs);
/* registro: um valor cru do eixo normalizado (-1000..1000) e o valor atual do eixo */
int  gamepad_axis_norm(int slot, uint16_t code, int32_t value);
int  gamepad_axis_now(int slot, uint16_t code);
/* procura os mapas de novo (app instalado ou removido, controle registrado) */
void gamepad_remap(void);
/* o banco do usuário: <dados>/controles/gamecontrollerdb.txt */
void gamepad_user_db(char *out, size_t n);

/* cursor (testes): posição e se está à vista */
bool gamepad_cursor(int32_t *x, int32_t *y);
#endif
