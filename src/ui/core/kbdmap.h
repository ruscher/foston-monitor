/* kbdmap.h — o que cada tecla de um teclado físico escreve: layouts ABNT2, EUA e EUA
 * internacional, acentos por tecla morta (´ ` ^ ~ ¨, e ' " no internacional; ´+c = ç),
 * AltGr, Caps Lock e o teclado numérico (com Num Lock números, sem ele navegação).
 * Sem LVGL nem dispositivos: core/hwkbd.c lê o teclado e entrega o resultado.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_KBDMAP_H
#define BF_KBDMAP_H
#include <stdbool.h>
#include <stdint.h>

#define HK_SHIFT 0x01
#define HK_CTRL  0x02
#define HK_ALT   0x04             /* Alt da esquerda (no layout dos EUA, os dois) */
#define HK_ALTGR 0x08             /* Alt da direita (AltGr) */
#define HK_META  0x10             /* tecla Windows/Super */

/* layout (settings()->kbd_layout; AUTO = pelo idioma) */
enum { BF_KBD_AUTO, BF_KBD_ABNT2, BF_KBD_US, BF_KBD_US_INTL, BF_KBD__N };

#define KBDMAP_TEXT 12

typedef struct {
    bool caps, numlock;
    uint32_t dead;                /* acento esperando a letra (0 = nenhum) */
} kbdmap_t;

/* *code: KEY_* apertado; sai trocado se o teclado numérico sem Num Lock virar navegação
 * (KP8 -> KEY_UP, KPENTER -> KEY_ENTER) e 0 quando a tecla só cancelou um acento.
 * text: o que escrever em UTF-8 ("" = tecla especial, atalho com Ctrl/Alt/Meta ou acento
 * esperando a letra). lay: BF_KBD_ABNT2, _US ou _US_INTL. */
void kbdmap_translate(kbdmap_t *st, int lay, uint16_t *code, uint8_t mods, bool repeat, char text[KBDMAP_TEXT]);
#endif
