/* phonestat.h — telefone pelo bfphone (src/bt/bfphone.c): o shell lê o estado em
 * /run/bigfoston-phone e manda comandos pelo FIFO /run/bigfoston-phone.cmd. */
#ifndef BF_PHONESTAT_H
#define BF_PHONESTAT_H
#include <stdbool.h>
#include <time.h>

#define PH_MAXCALLS 6

/* estado da chamada (AT+CLCC) */
enum { PH_ACTIVE = 0, PH_HELD = 1, PH_DIALING = 2, PH_ALERTING = 3, PH_INCOMING = 4, PH_WAITING = 5 };

typedef struct { int idx, dir, stat; char num[48], name[96]; } ph_call_t;

typedef struct {
    bool available;                   /* bfphone rodando */
    bool link;                        /* canal de chamadas (HFP) com o celular */
    char dev[64], op[48], data[96];   /* nome do celular, operadora, pasta dos dados no cartão */
    int signal, battery, service;     /* 0-5 / 0-5 / 0-1; -1 = desconhecido */
    bool voice_phone, sco;
    bool voice_foston;                /* o FOSTON já sabe fazer a voz da ligação */
    bool voice_on;                    /* voz da ligação no FOSTON agora (DSP ligado) */
    bool mute, rec;                   /* microfone do FOSTON mudo; gravando a ligação */
    int vol;                          /* volume da ligação 1-10 */
    ph_call_t call[PH_MAXCALLS];
    int ncall;
    time_t since;                     /* início da ligação ativa (0 = nenhuma) */
    char sync[200];                   /* idle | andamento | ok ... | erro ... */
} ph_info_t;

void ph_read(ph_info_t *p);
bool ph_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* pasta dos dados do último celular conectado ("" se nenhum) */
const char *ph_last_data(void);
/* chamada a cada segundo pelo app: aviso de chamada recebida em qualquer tela */
void ph_poll_global(void);

#endif
