/* padmap.h — mapa de controles de jogo no formato do SDL GameControllerDB.
 *
 * Cada controle tem um GUID (barramento, fabricante, produto e versão do evdev) e uma linha
 *   GUID,Nome,a:b1,b:b2,leftx:a0,dpup:h0.1,...,platform:Linux,
 * que diz qual botão (bN), eixo (aN, +aN/-aN metade, aN~ invertido) ou direcional (hH.M) do
 * aparelho é cada controle padrão. Os números seguem a ordem do SDL no Linux: botões de
 * BTN_JOYSTICK até KEY_MAX e depois de 0 até BTN_JOYSTICK; eixos de ABS_X em diante sem os
 * direcionais (ABS_HAT*). O banco vem do app SC Controller (gamecontrollerdb.txt, do projeto
 * SDL_GameControllerDB) e o registro de controle grava linhas novas no do usuário.
 *
 * Sem LVGL: testado em tests/test_padmap.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_PADMAP_H
#define BF_PADMAP_H
#include <stdbool.h>
#include <stdint.h>

/* controles padrão (os nomes do SDL) */
typedef enum {
    PC_A, PC_B, PC_X, PC_Y, PC_BACK, PC_GUIDE, PC_START, PC_LSTICK, PC_RSTICK, PC_LSHOULDER,
    PC_RSHOULDER, PC_DPUP, PC_DPDOWN, PC_DPLEFT, PC_DPRIGHT, PC_MISC1, PC_TOUCHPAD,
    PC__NBTN,
    PC_LEFTX = PC__NBTN, PC_LEFTY, PC_RIGHTX, PC_RIGHTY, PC_LTRIGGER, PC_RTRIGGER,
    PC__N
} pad_ctl_t;
#define PC__NAXIS (PC__N - PC__NBTN)

const char *padmap_ctl_name(int c);            /* "a", "leftx"... */

/* de onde vem um controle */
enum { PS_NONE, PS_BUTTON, PS_AXIS, PS_HAT };
typedef struct {
    uint8_t kind;          /* PS_* */
    uint8_t idx;           /* bN / aN / hN */
    int8_t  half;          /* eixo: +1 metade positiva, -1 negativa, 0 inteiro */
    bool    invert;        /* eixo: "~" */
    uint8_t mask;          /* direcional: 1 cima, 2 direita, 4 baixo, 8 esquerda */
} pm_src_t;

typedef struct {
    char guid[33];
    char name[64];
    pm_src_t src[PC__N];
    pm_src_t half[PC__NAXIS][2];   /* "+leftx:b3" / "-leftx:b2": botão para metade de eixo */
} padmap_t;

/* GUID do SDL 2 para o barramento/fabricante/produto/versão do EVIOCGID (32 hex + '\0') */
void padmap_guid(uint16_t bus, uint16_t vendor, uint16_t product, uint16_t version, char out[33]);

/* lê uma linha do banco; false se inválida ou de outro sistema */
bool padmap_parse(const char *line, padmap_t *m);
/* escreve a linha (sem '\n'); devolve o tamanho ou -1 se não coube */
int  padmap_format(const padmap_t *m, char *out, int n);

/* procura o GUID nos arquivos (o primeiro achado vence: passe o do usuário primeiro). Como o
 * SDL: o GUID exato (sem o CRC) ou, se não houver, uma linha com a versão zerada */
bool padmap_find(const char *const *files, int nfiles, const char *guid, padmap_t *out);

/* ---------------------------------------------------- aplicado a um aparelho */

/* bits do EVIOCGBIT do aparelho (EV_KEY e EV_ABS) */
typedef struct {
    unsigned long key[(0x2ff + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    unsigned long abs[(0x3f + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
} pm_bits_t;

void pm_bits_set(pm_bits_t *b, bool key, unsigned code);
bool pm_bits_test(const pm_bits_t *b, bool key, unsigned code);

/* números do SDL <-> códigos do evdev (-1 / 0xffff se não existe) */
int  pm_button_code(const pm_bits_t *b, int idx);
int  pm_axis_code(const pm_bits_t *b, int idx);
int  pm_hat_code(const pm_bits_t *b, int idx);      /* ABS_HATnX do direcional idx */
int  pm_button_index(const pm_bits_t *b, unsigned code);
int  pm_axis_index(const pm_bits_t *b, unsigned code);
int  pm_hat_index(const pm_bits_t *b, unsigned code);   /* code = ABS_HATnX ou ABS_HATnY */

/* mapa padrão do Linux pelos códigos (BTN_SOUTH = A, ABS_RX/RY analógico direito,
 * ABS_Z/RZ gatilhos, ABS_HAT0 direcional): para controles fora do banco */
void padmap_linux_default(const pm_bits_t *b, padmap_t *m);
#endif
