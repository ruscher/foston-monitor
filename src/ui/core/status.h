/* status.h — provedor de estado do hardware para a barra superior e Ajustes.
 *
 * Duas implementações:
 *   - "linux": lê sysfs/proc (alvo, ou PC com --real-status);
 *   - "sim":   valores SIMULADOS para o simulador de PC (status.simulated = true,
 *              e a interface mostra o selo "SIM").
 * Regra de honestidade: valor desconhecido é BF_UNKNOWN / -1 e a interface
 * mostra "—". Nunca inventar porcentagem de bateria. */
#ifndef BF_STATUS_H
#define BF_STATUS_H

#include <stdbool.h>

typedef enum { BF_UNKNOWN = -1, BF_NO = 0, BF_YES = 1 } bf_tri_t;

typedef struct {
    bool     simulated;      /* true => valores falsos (simulador) */
    int      battery_pct;    /* 0..100 ou -1 (desconhecido) */
    bf_tri_t battery_present;
    bf_tri_t charging;
    bf_tri_t ext_power;      /* alimentação externa (DC/USB) */
    int      volume_pct;     /* -1 = sem mixer */
    bf_tri_t muted;
    bf_tri_t usb;            /* cabo USB conectado/configurado */
    bf_tri_t bluetooth;      /* adaptador presente e ligado */
    bf_tri_t network;        /* alguma interface não-loopback com IPv4 */
    char     ip[48];
    bf_tri_t gps_fix;
    int      gps_sats;       /* -1 = desconhecido */
} bf_status_t;

typedef struct {
    const char *name;                 /* "linux" ou "sim" */
    bool simulated;
    void (*read)(bf_status_t *out);
    /* Luz posterior: get retorna 0..100 ou -1 se não suportado. */
    int  (*backlight_get)(void);
    bool (*backlight_set)(int pct);
    const char *(*backlight_device)(void);
    /* Volume: get retorna 0..100 ou -1 se não há placa de som. */
    int  (*volume_get)(void);
    bool (*volume_set)(int pct);
} bf_status_provider_t;

extern const bf_status_provider_t bf_status_linux;
extern const bf_status_provider_t bf_status_sim;

void status_select(const bf_status_provider_t *p);
const bf_status_provider_t *status_provider(void);
/* Leitura em cache: atualiza no máximo 1x por segundo. */
const bf_status_t *status_get(void);
void status_refresh(void);

#endif
