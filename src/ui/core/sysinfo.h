/* sysinfo.h — leitura de /proc, uname, statvfs e getifaddrs (sem ponto flutuante). */
#ifndef BF_SYSINFO_H
#define BF_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include "status.h"

typedef struct { unsigned long long total, idle; } bf_cpu_sample_t;

bool sysinfo_cpu_sample(bf_cpu_sample_t *s);
/* Uso de CPU entre duas amostras em décimos de % (0..1000). */
int  sysinfo_cpu_permille(const bf_cpu_sample_t *a, const bf_cpu_sample_t *b);
bool sysinfo_mem(unsigned long *total_kb, unsigned long *avail_kb);
bool sysinfo_loadavg(char *buf, size_t n);          /* "0.12  0.30  0.25" */
int  sysinfo_proc_count(void);
long sysinfo_uptime(void);                          /* segundos ou -1 */
void sysinfo_os_pretty(char *buf, size_t n);        /* PRETTY_NAME de /etc/os-release */
void sysinfo_cpu_model(char *buf, size_t n, int *cores);
bool sysinfo_disk(const char *path, unsigned long long *total, unsigned long long *avail);
/* Primeira interface não-loopback ativa. Campos vazios se não houver. */
void sysinfo_net(char *ifname, size_t nif, char *ip, size_t nip, char *mac, size_t nmac);
bf_tri_t sysinfo_ssh_listening(void);               /* porta 22 em LISTEN (tcp/tcp6) */
void sysinfo_hostname(char *buf, size_t n);
void sysinfo_user(char *buf, size_t n);

void fmt_bytes(unsigned long long b, char *buf, size_t n);   /* "1,5 GB" */
void fmt_duration(long sec, char *buf, size_t n);            /* "2d 3h 4min" */

#endif
