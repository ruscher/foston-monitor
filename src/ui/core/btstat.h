/* btstat.h — Bluetooth pelo bfbt-agent (src/bt/bfbt-agent.c): o shell não fala D-Bus;
 * lê o estado em /run/bigfoston-bt e manda comandos pelo FIFO /run/bigfoston-bt.cmd. */
#ifndef BF_BTSTAT_H
#define BF_BTSTAT_H
#include <stdbool.h>

#define BT_MAXDEV 32

typedef struct {
    char addr[18], icon[24], name[64];
    bool paired, connected, trusted;
    int rssi;                         /* 0 = desconhecido */
} bt_dev_t;

typedef struct {
    bool available;                   /* bfbt-agent rodando (estado recente) */
    bool enabled;                     /* marca "ligado" (o usuário ligou) */
    bool adapter, powered, discoverable, discovering;
    char name[64];
    bt_dev_t dev[BT_MAXDEV];
    int ndev;
    bool req;                         /* pedido de pareamento esperando resposta */
    char req_kind[16], req_code[8], req_addr[18], req_name[64];
    bool player;                      /* música de um celular conectado (AVRCP) */
    char pl_status[16], pl_title[96], pl_artist[96], pl_dev[64], pl_album[96];
    unsigned pl_dur, pl_pos;          /* ms; 0 = desconhecido */
    long pl_pos_at;                   /* CLOCK_MONOTONIC (ms) em que pl_pos valia */
    bool streaming;                   /* transporte A2DP ativo (áudio chegando) */
    char res_op[16], res_state[16], res_err[96];   /* resultado do último comando */
} bt_info_t;

void bt_read(bt_info_t *b);
/* posição atual da música do celular (extrapolada enquanto toca), ms */
unsigned bt_position_ms(const bt_info_t *b);
bool bt_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* chamada a cada segundo pelo app: abre o diálogo de pareamento em qualquer tela */
void bt_poll_global(void);

#endif
