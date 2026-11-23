/* kbd.h — teclado na tela em português: QWERTY com Ç, números na linha de cima e a
 * tecla "áã" com os acentos (Telefone, Internet).
 *
 * Dois usos:
 *   kbd_create(parent, ta)        preenche o campo ta (READY/CANCEL no próprio teclado)
 *   kbd_create(parent, NULL) +    sem campo: cada tecla vai para o callback
 *   kbd_set_sink(kb, cb, ud)      (campos de texto de uma página web, por exemplo)
 * O estilo (cores, tamanho) fica com quem cria. */
#ifndef BF_KBD_H
#define BF_KBD_H
#include "lvgl.h"

typedef enum { KBD_TEXT, KBD_BACKSPACE, KBD_ENTER, KBD_LEFT, KBD_RIGHT, KBD_CLOSE } kbd_key_t;
/* text: só em KBD_TEXT (UTF-8 de uma tecla) */
typedef void (*kbd_sink_cb)(kbd_key_t key, const char *text, void *ud);

lv_obj_t *kbd_create(lv_obj_t *parent, lv_obj_t *ta);
void kbd_set_sink(lv_obj_t *kb, kbd_sink_cb cb, void *ud);
#endif
