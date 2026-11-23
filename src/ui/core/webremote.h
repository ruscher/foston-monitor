/* webremote.h — modo completo do app Internet: o Chromium do PC navega e o FOSTON
 * mostra (tools/bigfoston/bfweb-pc.py).
 *
 * Enquanto o app Internet está aberto, o FOSTON escuta em 192.168.7.2:7071 (só a rede
 * USB; no simulador 127.0.0.1). O PC se liga, manda a chave dele e, na primeira vez,
 * a tela pergunta se aquele PC pode mostrar páginas (as chaves aceitas ficam em
 * <dados>/web/pcs.tsv). Depois disso, comandos e eventos são os mesmos do motor local
 * (src/web/frontend/protocol.md) e a imagem chega em blocos RGB565 comprimidos.
 *
 * Eventos extras para o callback: "PAIR nome" (pedir autorização), "REMOTE 1 nome"
 * (modo completo ativo), "REMOTE 0" (o PC saiu: voltar ao motor local). */
#ifndef BF_WEBREMOTE_H
#define BF_WEBREMOTE_H
#include "lvgl.h"
#include <stdbool.h>

typedef void (*webremote_cb)(const char *line, void *ud);

#define WEBREMOTE_PORT 7071

bool webremote_listen(int width, int height, webremote_cb cb, void *ud);
void webremote_stop(void);
bool webremote_active(void);                 /* PC autorizado e ligado */
/* muda o tamanho da imagem (tela cheia: 800x480) e pede ao PC o mesmo tamanho; quem mostra
 * a imagem precisa chamar lv_image_set_src de novo */
bool webremote_resize(int w, int h);
void webremote_set_start_url(const char *url);   /* página que o PC abre ao ligar */
void webremote_pair_answer(bool allow);      /* resposta ao "PAIR" */
void webremote_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const lv_image_dsc_t *webremote_image(void);
#endif
