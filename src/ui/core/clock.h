/* clock.h — data e hora do sistema (Ajustes > Data).
 *
 * Fusos: tabela de strings TZ POSIX (o rootfs não tem zoneinfo). Fusos com
 * regra de horário de verão mudam sozinhos; nos demais o "horário de verão"
 * é manual (+1 h). O TZ vale para o shell e para os programas que ele abre,
 * e vai para /etc/profile.d/tz.sh (sessões SSH).
 *
 * Origem da hora: o boot (carimbo do PC ou do relógio do Windows CE no DTB,
 * gravado por /etc/init.d/S05clock em /run/bigfoston-clock), o ajuste
 * manual ou o NTP. O Linux não tem driver de RTC: a hora acertada aqui vale
 * até desligar.
 *
 * Atualização automática: o ntpd do BusyBox ("-n -q", 12 s por servidor)
 * numa thread, ao iniciar e depois a cada 60 min (10 min após falha). No
 * simulador nada disso mexe no relógio do PC. */
#ifndef BF_CLOCK_H
#define BF_CLOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    const char *id;       /* estável (settings.conf) */
    const char *name;     /* cidades */
    const char *posix;    /* TZ POSIX */
    int  utc_min;         /* deslocamento padrão em minutos (−180 = UTC−3) */
    bool rules;           /* tem regra de horário de verão */
} clock_zone_t;

typedef enum { CLK_UNSET, CLK_SYSTEM, CLK_BOOT, CLK_MANUAL, CLK_NTP } clock_src_t;

int                 clock_zone_count(void);
const clock_zone_t *clock_zone(int i);
int                 clock_zone_index(const char *id);   /* 0 se desconhecido */
const clock_zone_t *clock_zone_current(void);
void                clock_utc_label(int minutes, char *out, size_t n);   /* "UTC-03:00" */

void clock_init(void);           /* aplica o fuso e lê a origem do boot */
void clock_apply_tz(void);       /* após mudar fuso/horário de verão */
bool clock_valid(void);          /* ano >= 2024 */
bool clock_can_set(void);        /* alvo real com privilégio de root */
/* Acerta o relógio pela hora local (no fuso atual). */
bool clock_set_local(const struct tm *local, char *err, size_t n);

clock_src_t clock_source(time_t *when);    /* origem e quando */
const char *clock_source_detail(void);     /* servidor NTP da última sincronização */

void clock_sync_now(void);       /* pede uma sincronização (em segundo plano) */
bool clock_sync_busy(void);
/* resultado da última tentativa: 0 nenhuma, 1 ok, -1 falha (err preenchido) */
int  clock_sync_result(time_t *when, char *err, size_t n);
void clock_tick(void);           /* 1 s: agenda as sincronizações automáticas */
#endif
