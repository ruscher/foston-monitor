/* hwkbd.h — teclados físicos e controles de jogo (Bluetooth ou USB) em qualquer tela.
 *
 * Lê os /dev/input/event* que têm teclas de letra ou são controles de jogo (aparecem e somem a
 * qualquer hora: o teclado Bluetooth dorme e reconecta). Os controles vão para o
 * core/gamepad.c, que os transforma em cursor e teclas. Traduz cada tecla pelo layout de
 * Ajustes > Teclado e controle (ABNT2, EUA ou EUA internacional, com acentos por tecla
 * morta) e entrega:
 *   1. ao campo de texto ativo: o do teclado da tela aberto ou o campo tocado por último
 *      (Enter = o OK do teclado da tela, Esc = fechar o teclado);
 *   2. Enter/Esc a um diálogo aberto (botão principal / fechar);
 *   3. à tela atual, se ela registrou um tratador (Terminal: o shell; Internet: a página);
 *   4. atalhos gerais: Esc ou Alt+← volta, setas e PgUp/PgDn rolam a página, teclas de
 *      volume e de música.
 * A primeira tecla com a tela apagada ou escurecida só acende a tela, como o toque.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_HWKBD_H
#define BF_HWKBD_H
#include <stdbool.h>
#include <stdint.h>
#include "kbdmap.h"               /* HK_*, BF_KBD_* */

typedef struct {
    uint16_t code;                /* KEY_* de <linux/input.h> (o teclado numérico já traduzido) */
    uint8_t  mods;                /* HK_* */
    bool     repeat;              /* repetição automática (tecla segurada) */
    char     text[KBDMAP_TEXT];   /* UTF-8 do que a tecla escreve ("" = tecla especial ou
                                     atalho com Ctrl/Alt/Meta) */
} hwkey_t;

/* true = a tela usou a tecla */
typedef bool (*hwkbd_handler_t)(const hwkey_t *k);

void hwkbd_init(void);
/* laço principal (main.c): os fds a vigiar (o /dev/input e cada teclado) e o que fazer
 * depois do poll(); n_max >= 1 + HWKBD_MAX */
#define HWKBD_MAX 4
struct pollfd;
int  hwkbd_poll_fds(struct pollfd *pf, int n_max);
void hwkbd_poll_done(const struct pollfd *pf, int n);

/* a tela atual quer as teclas que não foram para um campo de texto (NULL ao sair) */
void hwkbd_set_handler(hwkbd_handler_t h);
/* uso da interface por teclado ou controle: true = a tela estava apagada ou escurecida e só
 * acendeu (o evento não vale) */
bool hwkbd_activity(void);
/* entrega uma tecla já traduzida */
void hwkbd_dispatch(const hwkey_t *k);
/* um evento cru (KEY_*, 1 apertou / 2 repetiu / 0 soltou), como do evdev: testes e o FIFO de
 * controle ("key CÓDIGO VALOR") */
void hwkbd_feed(uint16_t code, int value);
/* nome da tecla especial no protocolo do Internet ("enter", "left"...) ou NULL */
const char *hwkbd_key_name(uint16_t code);

int  hwkbd_count(void);                 /* teclados conectados agora */
const char *hwkbd_name(int i);
bool hwkbd_is_bt(int i);
bool hwkbd_is_pad(int i);               /* controle de jogo (core/gamepad.c) */
bool hwkbd_bt_connected(void);          /* algum pelo Bluetooth (o modo performance o mantém) */
int  hwkbd_layout(void);                /* o efetivo: BF_KBD_ABNT2, _US ou _US_INTL */
#endif
