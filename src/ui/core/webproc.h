/* webproc.h — o processo do motor do app Internet (bfweb, src/web/frontend).
 *
 * A página é desenhada pelo bfweb numa memória compartilhada (memfd) RGB565 do
 * tamanho da área da página; webproc_image() devolve um lv_image_dsc_t que aponta
 * para ela (sem cópia). Comandos vão por pipe (webproc_send), eventos voltam em
 * linhas para o callback (lidos por um lv_timer só enquanto o motor roda).
 *
 * O motor roda com prioridade baixa (nice 10: o GPS e o shell vêm antes) e com
 * limite de memória: uma página grande demais derruba só o motor (o callback
 * recebe "EXIT ..."), nunca o GPS nem o shell. */
#ifndef BF_WEBPROC_H
#define BF_WEBPROC_H
#include "lvgl.h"
#include <stdbool.h>

typedef void (*webproc_event_cb)(const char *line, void *ud);

typedef struct {
    int width, height;
    bool javascript, images;
    const char *url;          /* página inicial (NULL = about:blank) */
} webproc_opts_t;

/* diretório do motor: $BIGFOSTON_WEB_DIR ou /mnt/sd/BigFoston/web */
const char *webproc_dir(void);
bool webproc_available(void);                /* bfweb presente no cartão */
bool webproc_start(const webproc_opts_t *o, webproc_event_cb cb, void *ud, char *err, size_t errn);
void webproc_stop(void);                     /* QUIT e espera um pouco; depois SIGKILL */
bool webproc_running(void);
void webproc_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const lv_image_dsc_t *webproc_image(void);
int  webproc_pid(void);
long webproc_rss_kb(void);                   /* memória do motor (VmRSS) */
#endif
