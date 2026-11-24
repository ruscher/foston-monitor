/* perf.c — medidas de desempenho do shell (ver perf.h).
 *
 * Histogramas de 1 ms (0..PERF_MAXMS) por métrica, zerados a cada janela. O
 * custo é um clock_gettime e um incremento por evento. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* REG_RIP (ucontext) no x86-64: make target-hostcheck */
#endif
#include "perf.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h>
#endif

#define PERF_MAXMS 2000

typedef struct {
    uint32_t h[PERF_MAXMS + 1];
    uint32_t n;
    uint64_t sum_us;
    uint32_t max_us;
} hist_t;

static hist_t hs[PERF__N];
static uint64_t render_t0;
static uint64_t win_t0;
static const char *const names[PERF__N] = { "render", "loop", "map", "mapfetch", "mapdraw", "maplbl" };

uint64_t perf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void perf_add(perf_metric_t m, uint32_t us)
{
    if(m >= PERF__N) return;
    hist_t *h = &hs[m];
    uint32_t ms = us / 1000;
    h->h[ms > PERF_MAXMS ? PERF_MAXMS : ms]++;
    h->n++;
    h->sum_us += us;
    if(us > h->max_us) h->max_us = us;
}

static uint32_t pct(const hist_t *h, uint32_t p)
{
    if(!h->n) return 0;
    uint32_t want = (h->n * p + 99) / 100, acc = 0;
    for(uint32_t i = 0; i <= PERF_MAXMS; i++) {
        acc += h->h[i];
        if(acc >= want) return i;
    }
    return PERF_MAXMS;
}

static void ev_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if(c == LV_EVENT_RENDER_START) render_t0 = perf_now_us();
    else if(c == LV_EVENT_RENDER_READY && render_t0) {
        perf_add(PERF_RENDER, (uint32_t)(perf_now_us() - render_t0));
        render_t0 = 0;
    }
}

/* diagnóstico: as áreas invalidadas (quem faz a tela redesenhar parada) vão para o log */
static uint64_t inv_until;
void perf_trace_invalidate(uint32_t ms) { inv_until = perf_now_us() + (uint64_t)ms * 1000u; }

static void inv_cb(lv_event_t *e)
{
    if(!inv_until || perf_now_us() > inv_until) return;
    const lv_area_t *a = lv_event_get_param(e);
    if(a) fprintf(stderr, "invalidado %d,%d %dx%d\n", (int)a->x1, (int)a->y1, (int)lv_area_get_width(a),
                  (int)lv_area_get_height(a));
}

void perf_init(lv_display_t *d)
{
    lv_display_add_event_cb(d, inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(d, ev_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(d, ev_cb, LV_EVENT_RENDER_READY, NULL);
    perf_reset();
}

void perf_reset(void)
{
    memset(hs, 0, sizeof hs);
    win_t0 = perf_now_us();
}

int perf_format(char *buf, int n, const char *tag)
{
    double sec = (perf_now_us() - win_t0) / 1e6;
    if(sec <= 0) sec = 1e-3;
    int o = snprintf(buf, (size_t)n, "%s %.1fs fps %.1f", tag ? tag : "perf", sec, hs[PERF_RENDER].n / sec);
    for(int m = 0; m < PERF__N && o < n; m++) {
        const hist_t *h = &hs[m];
        if(!h->n) continue;
        o += snprintf(buf + o, (size_t)(n - o), " | %s n=%u avg=%.1f p50=%u p95=%u max=%.1f ms", names[m], h->n,
                      h->sum_us / 1000.0 / h->n, pct(h, 50), pct(h, 95), h->max_us / 1000.0);
    }
    return o;
}

/* perfilador por amostragem (BIGFOSTON_PROF=arquivo): a cada 1 ms de CPU do processo
 * guarda o PC e a thread; perf_prof_stop grava "tid pc" em hexadecimal, uma por linha,
 * para resolver no PC com o binário não reduzido (tools/bigfoston/prof-resolve.py) */
#define PROF_MAX 60000
static unsigned long prof_pc[PROF_MAX];
static unsigned short prof_tid[PROF_MAX];
static volatile int prof_n;
static const char *prof_path;

static void prof_sig(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    unsigned long pc = 0;
#if defined(__arm__)
    pc = ((ucontext_t *)uc)->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
    pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
#endif
    int n = prof_n;
    if(n < PROF_MAX) {
        prof_pc[n] = pc;
        prof_tid[n] = (unsigned short)syscall(SYS_gettid);
        prof_n = n + 1;
    }
}

void perf_prof_start(void)
{
    prof_path = getenv("BIGFOSTON_PROF");
    if(!prof_path || !*prof_path) { prof_path = NULL; return; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
}

void perf_prof_stop(void)
{
    if(!prof_path) return;
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_PROF, &it, NULL);
    FILE *f = fopen(prof_path, "w");
    if(f) {
        for(int i = 0; i < prof_n; i++) fprintf(f, "%u %lx\n", prof_tid[i], prof_pc[i]);
        fclose(f);
    }
    prof_path = NULL;
}
