/* bf-perf-monitor — amostrador leve de desempenho do BigFoston OS.
 *
 * Lê só /proc (stat, meminfo, vmstat, diskstats e o stat de cada thread) a cada
 * intervalo e imprime uma linha por amostra; no fim (ou com Ctrl-C) imprime o
 * resumo. Custo medido no aparelho: ver docs/performance/03-cpu-profiling.md.
 *
 *   bf-perf-monitor [-i ms] [-n amostras] [-t N] [-q]
 *     -i  intervalo (padrão 1000 ms)
 *     -n  número de amostras (padrão: até Ctrl-C)
 *     -t  mostra as N threads que mais usaram CPU em cada amostra (padrão 4, 0 = não)
 *     -q  só o resumo
 *
 * Colunas: cpu% usr sys io irq+sirq, trocas de contexto/s, interrupções/s,
 * MemAvailable, Anon, Cached (MB), faltas de página menores/maiores por s,
 * swap in/out (páginas/s), leitura/escrita do cartão (KB/s) e ms de I/O. */
#define _GNU_SOURCE
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXT 512

typedef struct { unsigned long long v[10]; } cpu_t;
typedef struct { int tid; char name[20]; unsigned long long t; } th_t;
typedef struct { unsigned long long rd, wr, ioms; } disk_t;

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static void read_cpu(cpu_t *c, unsigned long long *ctxt, unsigned long long *intr)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    memset(c, 0, sizeof *c);
    if(!f) return;
    while(fgets(line, sizeof line, f)) {
        if(!strncmp(line, "cpu ", 4))
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu", &c->v[0], &c->v[1], &c->v[2], &c->v[3],
                   &c->v[4], &c->v[5], &c->v[6], &c->v[7]);
        else if(!strncmp(line, "ctxt ", 5)) *ctxt = strtoull(line + 5, NULL, 10);
        else if(!strncmp(line, "intr ", 5)) *intr = strtoull(line + 5, NULL, 10);
    }
    fclose(f);
}

static long mem_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static void read_file(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t r = f ? fread(buf, 1, n - 1, f) : 0;
    buf[r] = 0;
    if(f) fclose(f);
}

static unsigned long long vm(const char *buf, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\n%s ", key);
    const char *p = strstr(buf, k);
    return p ? strtoull(p + strlen(k), NULL, 10) : 0;
}

static void read_disk(disk_t *d)
{
    FILE *f = fopen("/proc/diskstats", "r");
    char line[256], name[32];
    unsigned maj, min;
    unsigned long long x[11];
    memset(d, 0, sizeof *d);
    if(!f) return;
    while(fgets(line, sizeof line, f)) {
        if(sscanf(line, "%u %u %31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &maj, &min, name,
                  &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8], &x[9]) == 13 &&
           !strcmp(name, "mmcblk0")) {
            d->rd = x[2] / 2;      /* setores -> KB */
            d->wr = x[6] / 2;
            d->ioms = x[9];
        }
    }
    fclose(f);
}

static int read_threads(th_t *t, int max)
{
    int n = 0;
    DIR *pd = opendir("/proc");
    struct dirent *pe;
    if(!pd) return 0;
    while((pe = readdir(pd)) && n < max) {
        if(pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;
        char path[300];
        snprintf(path, sizeof path, "/proc/%s/task", pe->d_name);
        DIR *td = opendir(path);
        if(!td) continue;
        struct dirent *te;
        while((te = readdir(td)) && n < max) {
            if(te->d_name[0] < '0' || te->d_name[0] > '9') continue;
            char sp[600], buf[512];
            snprintf(sp, sizeof sp, "/proc/%s/task/%s/stat", pe->d_name, te->d_name);
            read_file(sp, buf, sizeof buf);
            char *l = strchr(buf, '('), *r = strrchr(buf, ')');
            if(!l || !r) continue;
            th_t *x = &t[n];
            x->tid = atoi(te->d_name);
            int len = (int)(r - l - 1);
            if(len > (int)sizeof x->name - 1) len = sizeof x->name - 1;
            memcpy(x->name, l + 1, (size_t)len);
            x->name[len] = 0;
            unsigned long long ut = 0, st = 0;
            /* depois de ")": estado ppid pgrp sess tty tpgid flags minflt cminflt majflt cmajflt utime stime */
            sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
            x->t = ut + st;
            n++;
        }
        closedir(td);
    }
    closedir(pd);
    return n;
}

static int cmp_desc(const void *a, const void *b)
{
    const unsigned long long *x = a, *y = b;
    return x[0] < y[0] ? 1 : x[0] > y[0] ? -1 : 0;
}

int main(int argc, char **argv)
{
    int ims = 1000, count = -1, topn = 4, quiet = 0, o;
    while((o = getopt(argc, argv, "i:n:t:q")) != -1) {
        if(o == 'i') ims = atoi(optarg);
        else if(o == 'n') count = atoi(optarg);
        else if(o == 't') topn = atoi(optarg);
        else if(o == 'q') quiet = 1;
        else { fprintf(stderr, "uso: %s [-i ms] [-n amostras] [-t N] [-q]\n", argv[0]); return 2; }
    }
    if(ims < 100) ims = 100;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    long hz = sysconf(_SC_CLK_TCK);

    static th_t ta[MAXT], tb[MAXT];
    th_t *prev = ta, *now = tb;
    cpu_t c0, c1;
    unsigned long long x0 = 0, x1 = 0, i0 = 0, i1 = 0;
    disk_t d0, d1;
    static char vbuf[8192], mbuf[4096];
    read_cpu(&c0, &x0, &i0);
    read_disk(&d0);
    read_file("/proc/vmstat", vbuf + 1, sizeof vbuf - 1);
    vbuf[0] = '\n';
    unsigned long long pf0 = vm(vbuf, "pgfault"), mf0 = vm(vbuf, "pgmajfault"), si0 = vm(vbuf, "pswpin"), so0 = vm(vbuf, "pswpout");
    int np = topn ? read_threads(prev, MAXT) : 0;

    /* resumo */
    double s_busy = 0, s_usr = 0, s_sys = 0, s_io = 0, m_busy = 0;
    long m_avail = -1, s_avail_min = -1;
    unsigned long long t_rd = 0, t_wr = 0, t_mf = 0, t_so = 0;
    int samples = 0;

    if(!quiet)
        printf("#  t   busy  usr  sys   io  irq  ctx/s intr/s availMB anonMB cacheMB  pf/s mjf/s swi/s swo/s rdKB/s wrKB/s ioms  top\n");
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    while(!stop && (count < 0 || samples < count)) {
        usleep((useconds_t)ims * 1000);
        read_cpu(&c1, &x1, &i1);
        read_disk(&d1);
        read_file("/proc/vmstat", vbuf + 1, sizeof vbuf - 1);
        read_file("/proc/meminfo", mbuf, sizeof mbuf);
        unsigned long long pf1 = vm(vbuf, "pgfault"), mf1 = vm(vbuf, "pgmajfault"), si1 = vm(vbuf, "pswpin"), so1 = vm(vbuf, "pswpout");
        double d[8], tot = 0;
        for(int k = 0; k < 8; k++) { d[k] = (double)(c1.v[k] - c0.v[k]); tot += d[k]; }
        if(tot <= 0) tot = 1;
        double usr = 100 * (d[0] + d[1]) / tot, sys = 100 * d[2] / tot, io = 100 * d[4] / tot, irq = 100 * (d[5] + d[6]) / tot;
        double busy = 100 - 100 * (d[3] + d[4]) / tot;
        double sec = ims / 1000.0;
        long avail = mem_kb(mbuf, "MemAvailable:"), anon = mem_kb(mbuf, "AnonPages:"), cache = mem_kb(mbuf, "Cached:");
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double el = (double)(ts.tv_sec - ts0.tv_sec) + (ts.tv_nsec - ts0.tv_nsec) / 1e9;
        if(!quiet)
            printf("%5.0f %5.1f %4.0f %4.0f %4.0f %4.0f %6.0f %6.0f %7.1f %6.1f %7.1f %5.0f %5.0f %5.0f %5.0f %6.0f %6.0f %4llu ",
                   el, busy, usr, sys, io, irq, (x1 - x0) / sec, (i1 - i0) / sec, avail / 1024.0, anon / 1024.0, cache / 1024.0,
                   (pf1 - pf0) / sec, (mf1 - mf0) / sec, (si1 - si0) / sec, (so1 - so0) / sec, (d1.rd - d0.rd) / sec,
                   (d1.wr - d0.wr) / sec, d1.ioms - d0.ioms);
        if(topn) {
            int nn = read_threads(now, MAXT);
            static unsigned long long rank[MAXT][2];
            int nr = 0;
            for(int a = 0; a < nn; a++) {
                unsigned long long base = 0;
                for(int b = 0; b < np; b++) if(prev[b].tid == now[a].tid) { base = prev[b].t; break; }
                if(now[a].t > base) { rank[nr][0] = now[a].t - base; rank[nr][1] = (unsigned long long)a; nr++; }
            }
            qsort(rank, (size_t)nr, sizeof rank[0], cmp_desc);
            if(!quiet)
                for(int k = 0; k < nr && k < topn; k++)
                    printf(" %s/%d:%.0f%%", now[rank[k][1]].name, now[rank[k][1]].tid, 100.0 * rank[k][0] / (hz * sec));
            th_t *sw = prev; prev = now; now = sw; np = nn;
        }
        if(!quiet) { putchar('\n'); fflush(stdout); }
        samples++;
        s_busy += busy; s_usr += usr; s_sys += sys; s_io += io;
        if(busy > m_busy) m_busy = busy;
        if(s_avail_min < 0 || avail < s_avail_min) s_avail_min = avail;
        m_avail = avail;
        t_rd += d1.rd - d0.rd; t_wr += d1.wr - d0.wr; t_mf += mf1 - mf0; t_so += so1 - so0;
        c0 = c1; x0 = x1; i0 = i1; d0 = d1; pf0 = pf1; mf0 = mf1; si0 = si1; so0 = so1;
    }
    if(samples)
        printf("# resumo: %d amostras de %d ms | CPU média %.1f%% (usr %.1f sys %.1f io %.1f) máx %.1f%% | "
               "MemAvailable mín %.1f MB fim %.1f MB | SD lido %llu KB escrito %llu KB | faltas maiores %llu | swap out %llu\n",
               samples, ims, s_busy / samples, s_usr / samples, s_sys / samples, s_io / samples, m_busy,
               s_avail_min / 1024.0, m_avail / 1024.0, t_rd, t_wr, t_mf, t_so);
    return 0;
}
