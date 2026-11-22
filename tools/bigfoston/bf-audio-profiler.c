/* bf-audio-profiler — mede o caminho do áudio Bluetooth no FOSTON sem atrapalhar
 * (docs/bluetooth-audio-performance/02-cpu-profiling.md).
 *
 *   bf-audio-profiler [-t SEGUNDOS] [-i MS] [-v]
 *
 * Amostra a cada -i ms (padrão 50) só lendo arquivos do /proc (sem criar processos):
 *  - bytes recebidos e interrupções da UART do Bluetooth (ttyS1);
 *  - estado do PCM de saída (card0 pcm0p): entradas em XRUN, fechamentos e aberturas;
 *  - CPU total (usr/sys/irq/softirq/ocioso) e por processo de áudio/BT/GPS/shell.
 * No fim: buracos na recepção da UART (> 200 ms sem bytes), maior buraco, taxa média,
 * cortes do PCM e a divisão da CPU. -v imprime a linha do tempo da recepção.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PCM_STATUS "/proc/asound/card0/pcm0p/sub0/status"
#define MAXP 16

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int read_file(const char *p, char *b, size_t n)
{
    FILE *f = fopen(p, "r");
    if(!f) { b[0] = 0; return 0; }
    size_t k = fread(b, 1, n - 1, f);
    fclose(f);
    b[k] = 0;
    return (int)k;
}

static unsigned long uart_rx(void)
{
    char b[1024];
    read_file("/proc/tty/driver/serial", b, sizeof b);
    char *l = strstr(b, "\n1:");
    char *r = l ? strstr(l, "rx:") : NULL;
    return r ? strtoul(r + 3, NULL, 10) : 0;
}

static unsigned long uart_irqs(void)
{
    char b[4096];
    read_file("/proc/interrupts", b, sizeof b);
    char *t = strstr(b, "ttyS1");
    if(!t) return 0;
    while(t > b && t[-1] != '\n') t--;
    char *c = strchr(t, ':');
    return c ? strtoul(c + 1, NULL, 10) : 0;
}

/* 0 fechado, 1 preparado, 2 tocando, 3 XRUN, 4 outro */
static int pcm_state(void)
{
    char b[256];
    read_file(PCM_STATUS, b, sizeof b);
    if(!strncmp(b, "closed", 6)) return 0;
    if(strstr(b, "state: PREPARED")) return 1;
    if(strstr(b, "state: RUNNING")) return 2;
    if(strstr(b, "state: XRUN")) return 3;
    return 4;
}

typedef struct { unsigned long long usr, sys, idle, irq, sirq, tot; } cpu_t;

static void cpu_read(cpu_t *c)
{
    char b[256];
    read_file("/proc/stat", b, sizeof b);
    unsigned long long u, n, s, i, w, q, sq;
    if(sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &q, &sq) != 7) { memset(c, 0, sizeof *c); return; }
    c->usr = u + n; c->sys = s; c->idle = i + w; c->irq = q; c->sirq = sq;
    c->tot = u + n + s + i + w + q + sq;
}

static const char *watch[] = { "bluealsa", "bluealsa-aplay", "bluetoothd", "bfgps", "bigfoston-shell", "bfbt-agent",
                               "mpg123", "espeak-ng", NULL };
typedef struct { int pid; char name[32]; unsigned long long ut, st, ut0, st0; } proc_t;
static proc_t procs[MAXP];
static int nprocs;

static void procs_find(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    while(d && (e = readdir(d)) && nprocs < MAXP) {
        int pid = atoi(e->d_name);
        if(pid <= 0) continue;
        char p[64], comm[64];
        snprintf(p, sizeof p, "/proc/%d/comm", pid);
        if(!read_file(p, comm, sizeof comm)) continue;
        comm[strcspn(comm, "\n")] = 0;
        for(int i = 0; watch[i]; i++)
            if(!strcmp(comm, watch[i])) {
                procs[nprocs].pid = pid;
                snprintf(procs[nprocs].name, sizeof procs[nprocs].name, "%s", comm);
                nprocs++;
            }
    }
    if(d) closedir(d);
}

static void proc_times(proc_t *p, unsigned long long *ut, unsigned long long *st)
{
    char path[64], b[512];
    snprintf(path, sizeof path, "/proc/%d/stat", p->pid);
    *ut = *st = 0;
    if(!read_file(path, b, sizeof b)) return;
    char *r = strrchr(b, ')');                     /* o nome pode ter espaços */
    if(!r) return;
    unsigned long long v[13];
    int k = 0;
    char *s = r + 2;
    for(char *tok = strtok(s, " "); tok && k < 13; tok = strtok(NULL, " "), k++) v[k] = strtoull(tok, NULL, 10);
    if(k == 13) { *ut = v[11]; *st = v[12]; }      /* campos 14 e 15 */
}

int main(int argc, char **argv)
{
    int secs = 20, ivl = 50, verbose = 0;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-i") && i + 1 < argc) ivl = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "uso: %s [-t SEGUNDOS] [-i MS] [-v]\n", argv[0]); return 2; }
    }
    if(ivl < 10) ivl = 10;
    procs_find();
    for(int i = 0; i < nprocs; i++) proc_times(&procs[i], &procs[i].ut0, &procs[i].st0);
    cpu_t c0, c1;
    cpu_read(&c0);
    unsigned long rx0 = uart_rx(), irq0 = uart_irqs(), rx_prev = rx0;
    long t0 = now_ms(), last_rx_t = t0, end = t0 + secs * 1000L;
    int st_prev = pcm_state(), xruns = 0, closes = 0, opens = 0, gaps = 0;
    long max_gap = 0, gap_total = 0;
    struct timespec sl = { 0, ivl * 1000000L };
    if(verbose) printf("ms\tbytes\tpcm\n");
    while(now_ms() < end) {
        nanosleep(&sl, NULL);
        long t = now_ms();
        unsigned long rx = uart_rx();
        int st = pcm_state();
        if(st != st_prev) {
            if(st == 3) xruns++;
            if(st == 0) closes++;
            if(st_prev == 0) opens++;
        }
        if(rx != rx_prev) {
            long g = t - last_rx_t;
            if(g > 200) { gaps++; gap_total += g; }
            if(g > max_gap) max_gap = g;
            last_rx_t = t;
        }
        if(verbose) printf("%ld\t%lu\t%c\n", t - t0, rx - rx_prev, "fPRXo"[st]);
        rx_prev = rx;
        st_prev = st;
    }
    long dt = now_ms() - t0;
    cpu_read(&c1);
    double tot = (double)(c1.tot - c0.tot);
    if(tot <= 0) tot = 1;
    printf("duração %.1f s, amostra a cada %d ms\n", dt / 1000.0, ivl);
    printf("UART ttyS1: %.1f KB/s, %.0f interrupções/s (%.1f bytes cada)\n", (rx_prev - rx0) / 1024.0 / (dt / 1000.0),
           (uart_irqs() - irq0) / (dt / 1000.0), uart_irqs() > irq0 ? (double)(rx_prev - rx0) / (uart_irqs() - irq0) : 0.0);
    printf("recepção: %d buracos > 200 ms (%.1f s no total), maior %ld ms\n", gaps, gap_total / 1000.0, max_gap);
    printf("PCM: %d XRUN, %d fechamentos, %d aberturas\n", xruns, closes, opens);
    printf("CPU: usr %.0f%%  sys %.0f%%  irq %.0f%%  softirq %.0f%%  ocioso %.0f%%\n", 100 * (c1.usr - c0.usr) / tot,
           100 * (c1.sys - c0.sys) / tot, 100 * (c1.irq - c0.irq) / tot, 100 * (c1.sirq - c0.sirq) / tot,
           100 * (c1.idle - c0.idle) / tot);
    double proc_sum = 0;
    for(int i = 0; i < nprocs; i++) {
        unsigned long long ut, st;
        proc_times(&procs[i], &ut, &st);
        double u = 100 * (ut - procs[i].ut0) / tot, s = 100 * (st - procs[i].st0) / tot;
        proc_sum += u + s;
        printf("  %-16s usr %4.1f%%  sys %4.1f%%\n", procs[i].name, u, s);
    }
    double busy = 100 * (tot - (c1.idle - c0.idle)) / tot;
    printf("  fora desses processos (kernel, outros): %.0f%%\n", busy - proc_sum);
    return 0;
}
