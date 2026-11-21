/* bigscreen.h — cliente do Big Screen Monitor Display (FSP v1) no shell.
 *
 * O serviço do PC (big-screen-monitor-display, driver "foston_bigfoston")
 * conecta por TCP em 192.168.7.2:BS_PORT (rede USB) e fala o mesmo protocolo
 * do cliente bsmd-ce do Windows CE: retângulos (raw/RLE/deflate/fill) num
 * quadro sombra, PRESENT mostra, toques voltam como FSP_TOUCH. O núcleo do
 * protocolo é third_party/fsp (cópia sem alterações do bsmd).
 *
 * Tudo roda na thread do LVGL: bs_poll() é chamada por um lv_timer. */
#ifndef BF_BIGSCREEN_H
#define BF_BIGSCREEN_H
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define BS_PORT 7707

typedef enum { BS_OFF, BS_WAITING, BS_CONNECTED } bs_state_t;

typedef struct {
    bs_state_t state;
    char host[64];          /* nome do PC (HELLO) */
    char peer[48];          /* endereço do PC */
    uint32_t frames;        /* PRESENT recebidos nesta conexão */
    uint32_t bytes;
    bool quit_requested;    /* o PC pediu para fechar o cliente (BYE 1) */
} bs_status_t;

/* Começa a escutar; err recebe a explicação em caso de falha. */
bool bs_start(char *err, size_t n);
void bs_stop(void);
const bs_status_t *bs_status(void);
/* Atende a rede. Devolve true se um quadro novo foi apresentado. */
bool bs_poll(void);
/* Quadro do PC, 800x480 RGB565 (válido entre bs_start e bs_stop). */
const lv_image_dsc_t *bs_image(void);
/* Área alterada pelo último PRESENT (coordenadas da tela). */
lv_area_t bs_last_dirty(void);
/* Confirma ao PC que o quadro já está na tela (FSP_PRESENTED). */
void bs_presented(uint32_t blit_ms);
/* Toque no painel: FSP_TOUCH_DOWN/MOVE/UP (respeita o intervalo do TOUCHCFG). */
void bs_touch(int kind, int x, int y);
#endif
