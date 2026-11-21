/*
 * gpsce.c — carrega o gps.dll do próprio FOSTON (imagem BFX1 feita por
 * tools/bigfoston/gps-extract.py) num processo Linux e imita as funções do
 * Windows CE que ele importa. docs/gps.md.
 *
 *   bfgps [-b gps.bfx] [-r registro.txt] [-d pasta-de-dados] [-t segundos] [--hw] [-v]
 *
 * Sem --hw (marco M1/M2) nada toca o hardware: a serial é um poço (escritas
 * descartadas, leituras vazias) e GPIO/alimentação só são registrados.
 * A saída NMEA (VCOM_Driver do CE) vai para a saída padrão.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cefmt.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t (*hfn_t)(u32 *a);

static int verbose, hw, strace_;
static const char *data_dir = "/mnt/sd/BigFoston/gps";
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

#define R(x) ((uint64_t)(u32)(x))
#define TRUE_ 1
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_ITEMS 259
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE_ 0xFFFFFFFFu
#define INVALID_H 0xFFFFFFFFu

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
}
#define LOGV(...) do { if(verbose) logf_(__VA_ARGS__); } while(0)

static __thread u32 last_error;

/* -s: rastreio da serial (primeiros bytes de cada leitura/escrita) */
static void hexlog(const char *dir, const uint8_t *b, u32 n, u32 asked)
{
    if(!strace_) return;
    char line[3 * 40 + 8];
    u32 k = n < 40 ? n : 40, o = 0;
    for(u32 i = 0; i < k; i++) o += (u32)snprintf(line + o, sizeof line - o, "%02x ", b[i]);
    line[o] = 0;
    logf_("serial %s %u/%u: %s%s", dir, n, asked, line, n > k ? "..." : "");
}

/* ------------------------------------------------------------------ handles */

enum { H_FREE, H_FILE, H_SERIAL, H_EVENT, H_MUTEX, H_THREAD, H_MSGQ, H_FIND, H_GIO, H_BUS, H_LIB, H_DEV };

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int manual, state;
    unsigned pulses;      /* EVENT_PULSE: libera quem esperava naquele instante */
    char name[96];
    int refs;
} event_t;

typedef struct {
    pthread_t t;
    u32 start, param, exit_code;
    int done;
    pthread_mutex_t m;
    pthread_cond_t c;
} thread_t;

typedef struct msgq {
    char name[96];
    u32 max_msgs, max_size;
    pthread_mutex_t m;
    pthread_cond_t c;
    uint8_t *buf;         /* max_msgs * (4 + max_size) */
    u32 head, count;
    int refs;
    struct msgq *next;
} msgq_t;

typedef struct {
    int type;
    int fd;
    void *obj;
    char name[64];
} handle_t;

#define MAX_H 512
static handle_t handles[MAX_H];
static pthread_mutex_t h_lock = PTHREAD_MUTEX_INITIALIZER;

static u32 h_new(int type, int fd, void *obj, const char *name)
{
    pthread_mutex_lock(&h_lock);
    for(int i = 1; i < MAX_H; i++) {
        if(handles[i].type == H_FREE) {
            handles[i].type = type;
            handles[i].fd = fd;
            handles[i].obj = obj;
            snprintf(handles[i].name, sizeof handles[i].name, "%s", name ? name : "");
            pthread_mutex_unlock(&h_lock);
            return 0x100000u + (u32)i * 4;
        }
    }
    pthread_mutex_unlock(&h_lock);
    return 0;
}

static handle_t *h_get(u32 h)
{
    if(h < 0x100004u || h >= 0x100000u + MAX_H * 4 || (h & 3)) return NULL;
    handle_t *p = &handles[(h - 0x100000u) / 4];
    return p->type == H_FREE ? NULL : p;
}

/* ---------------------------------------------------------- string helpers */

static const u16 *W(u32 p) { return (const u16 *)(uintptr_t)p; }
static char *A(u32 p) { return (char *)(uintptr_t)p; }
static void *P(u32 p) { return (void *)(uintptr_t)p; }

static void wstr(char *out, size_t n, u32 p)
{
    if(!p) snprintf(out, n, "(null)");
    else w16_to_utf8(out, n, W(p));
}

/* caminho do CE -> pasta de dados ("\ResidentFlash\x" -> <dados>/ResidentFlash/x) */
static void ce_path(char *out, size_t n, const char *ce)
{
    char tmp[300];
    size_t o = 0;
    for(const char *s = ce; *s && o + 1 < sizeof tmp; s++)
        tmp[o++] = *s == '\\' ? '/' : *s;
    tmp[o] = 0;
    const char *t = tmp;
    while(*t == '/') t++;
    snprintf(out, n, "%s/root/%s", data_dir, t);
}

static void mkparents(const char *path)
{
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for(char *s = p + 1; *s; s++) {
        if(*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    }
}

/* ------------------------------------------------------------- dispatcher */

typedef struct { const char *name; hfn_t fn; } handler_t;
#define MAX_THUNKS 256
static struct { const char *name; hfn_t fn; } thunk_tab[MAX_THUNKS];
static int n_thunks;
static u32 *thunk_mem;

uint64_t bf_dispatch(u32 idx, u32 *a);
uint64_t bf_dispatch(u32 idx, u32 *a)
{
    if(idx >= (u32)n_thunks) { logf_("thunk inválido %u", idx); return 0; }
    if(!thunk_tab[idx].fn) {
        logf_("NÃO IMPLEMENTADO %s(%#x, %#x, %#x, %#x)", thunk_tab[idx].name, a[0], a[1], a[2], a[3]);
        last_error = 50;
        return 0;
    }
    return thunk_tab[idx].fn(a);
}

extern void bf_thunk_common(void);

/* endereço chamável pelo código do CE para um handler C */
static u32 make_thunk(const char *name, hfn_t fn)
{
    if(n_thunks >= MAX_THUNKS) { logf_("thunks esgotados"); exit(1); }
    int i = n_thunks++;
    thunk_tab[i].name = name;
    thunk_tab[i].fn = fn;
    u32 *t = thunk_mem + i * 4;
    t[0] = 0xE59FC000;    /* ldr r12, [pc]   -> t[2] */
    t[1] = 0xE59FF000;    /* ldr pc, [pc]    -> t[3] */
    t[2] = (u32)i;
    t[3] = (u32)(uintptr_t)bf_thunk_common;
    __builtin___clear_cache((char *)t, (char *)(t + 4));
    return (u32)(uintptr_t)t;
}

extern uint32_t ce_call6(u32 fn, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5);

/* ------------------------------------------------------------------ tempo */

static void systime_from(struct tm *tm, int ms, u16 *st)
{
    st[0] = (u16)(tm->tm_year + 1900);
    st[1] = (u16)(tm->tm_mon + 1);
    st[2] = (u16)tm->tm_wday;
    st[3] = (u16)tm->tm_mday;
    st[4] = (u16)tm->tm_hour;
    st[5] = (u16)tm->tm_min;
    st[6] = (u16)tm->tm_sec;
    st[7] = (u16)ms;
}

static uint64_t h_GetSystemTime(u32 *a)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    systime_from(&tm, (int)(tv.tv_usec / 1000), P(a[0]));
    return 0;
}

static uint64_t h_GetLocalTime(u32 *a)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    systime_from(&tm, (int)(tv.tv_usec / 1000), P(a[0]));
    return 0;
}

static uint64_t h_SetSystemTime(u32 *a)
{
    u16 *st = P(a[0]);
    logf_("SetSystemTime %04u-%02u-%02u %02u:%02u:%02u UTC (ignorado: a hora fica com o sistema)",
          st[0], st[1], st[3], st[4], st[5], st[6]);
    return R(TRUE_);
}

/* FILETIME: centenas de ns desde 1601 */
#define FT_EPOCH 116444736000000000ULL
static uint64_t h_SystemTimeToFileTime(u32 *a)
{
    u16 *st = P(a[0]);
    u32 *ft = P(a[1]);
    struct tm tm = { .tm_year = st[0] - 1900, .tm_mon = st[1] - 1, .tm_mday = st[3],
                     .tm_hour = st[4], .tm_min = st[5], .tm_sec = st[6] };
    time_t t = timegm(&tm);
    uint64_t v = FT_EPOCH + (uint64_t)t * 10000000ULL + (uint64_t)st[7] * 10000ULL;
    ft[0] = (u32)v;
    ft[1] = (u32)(v >> 32);
    return R(TRUE_);
}

static uint64_t h_FileTimeToSystemTime(u32 *a)
{
    u32 *ft = P(a[0]);
    uint64_t v = (uint64_t)ft[0] | (uint64_t)ft[1] << 32;
    if(v < FT_EPOCH) v = FT_EPOCH;
    time_t t = (time_t)((v - FT_EPOCH) / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    systime_from(&tm, (int)((v / 10000ULL) % 1000), P(a[1]));
    return R(TRUE_);
}

static uint64_t h_GetTickCount(u32 *a)
{
    (void)a;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return R((u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
}

static uint64_t h_Sleep(u32 *a)
{
    if(verbose > 1) logf_("SLEEP %u", a[0]);
    usleep(a[0] ? a[0] * 1000u : 0);
    if(!a[0]) sched_yield();
    return 0;
}

/* ------------------------------------------------------------- formatação */

static uint64_t h_sprintf(u32 *a)
{
    const u32 *args = a + 2;
    size_t n = ce_vformat(A(a[0]), 4096, A(a[1]), 0, &args);
    return R(n);
}

static uint64_t h_vsnprintf(u32 *a)
{
    const u32 *args = P(a[3]);      /* va_list do CE: ponteiro para as palavras */
    size_t n = ce_vformat(A(a[0]), a[1], A(a[2]), 0, &args);
    return R(n >= a[1] ? (u32)-1 : (u32)n);
}

static uint64_t h_wsprintfW(u32 *a)
{
    char fmt[512], out[1024];
    w16_to_utf8(fmt, sizeof fmt, W(a[1]));
    const u32 *args = a + 2;
    ce_vformat(out, sizeof out, fmt, 1, &args);
    return R(utf8_to_w16(P(a[0]), 1024, out));
}

static uint64_t h_NKDbgPrintfW(u32 *a)
{
    char fmt[512], out[1024];
    w16_to_utf8(fmt, sizeof fmt, W(a[0]));
    const u32 *args = a + 1;
    ce_vformat(out, sizeof out, fmt, 1, &args);
    size_t l = strlen(out);
    while(l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    logf_("dll: %s", out);
    return 0;
}

static uint64_t h_wcslen(u32 *a) { return R(w16_len(W(a[0]))); }
static uint64_t h_wcscpy(u32 *a)
{
    u16 *d = P(a[0]);
    const u16 *s = W(a[1]);
    size_t i = 0;
    do d[i] = s[i]; while(s[i++]);
    return R(a[0]);
}
static uint64_t h_wcsncpy(u32 *a)
{
    u16 *d = P(a[0]);
    const u16 *s = W(a[1]);
    u32 i = 0;
    for(; i < a[2] && s[i]; i++) d[i] = s[i];
    for(; i < a[2]; i++) d[i] = 0;
    return R(a[0]);
}
static uint64_t h_wcschr(u32 *a)
{
    const u16 *s = W(a[0]);
    for(;; s++) {
        if(*s == (u16)a[1]) return R((uintptr_t)s);
        if(!*s) return 0;
    }
}
static uint64_t h_itow(u32 *a)
{
    char b[40];
    int radix = (int)a[2];
    if(radix == 10) snprintf(b, sizeof b, "%d", (int)a[0]);
    else if(radix == 16) snprintf(b, sizeof b, "%x", a[0]);
    else if(radix == 8) snprintf(b, sizeof b, "%o", a[0]);
    else snprintf(b, sizeof b, "%u", a[0]);
    utf8_to_w16(P(a[1]), 40, b);
    return R(a[1]);
}

/* rand do CE: LCG de 15 bits (o do musl devolve 31 bits) */
static u32 rand_state = 1;
static uint64_t h_srand(u32 *a) { rand_state = a[0]; return 0; }
static uint64_t h_rand(u32 *a)
{
    (void)a;
    rand_state = rand_state * 214013u + 2531011u;
    return R((rand_state >> 16) & 0x7FFF);
}

/* ------------------------------------------------------------------ memória */

static uint64_t h_LocalAlloc(u32 *a)
{
    void *p = (a[0] & 0x40) ? calloc(1, a[1] ? a[1] : 1) : malloc(a[1] ? a[1] : 1);
    return R((uintptr_t)p);
}
static uint64_t h_LocalFree(u32 *a) { free(P(a[0])); return 0; }

/* -------------------------------------------------------------- erros */

static uint64_t h_GetLastError(u32 *a) { (void)a; return R(last_error); }

/* ---------------------------------------------------------------- eventos */

static event_t *named_event(const char *name, int manual, int initial, int create, int *existed)
{
    static pthread_mutex_t nl = PTHREAD_MUTEX_INITIALIZER;
    static event_t *list[64];
    pthread_mutex_lock(&nl);
    if(name && *name) {
        for(int i = 0; i < 64; i++) {
            if(list[i] && !strcmp(list[i]->name, name)) {
                list[i]->refs++;
                if(existed) *existed = 1;
                pthread_mutex_unlock(&nl);
                return list[i];
            }
        }
    }
    if(!create) { pthread_mutex_unlock(&nl); return NULL; }
    event_t *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual;
    e->state = initial;
    e->refs = 1;
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
    if(name && *name)
        for(int i = 0; i < 64; i++) if(!list[i]) { list[i] = e; break; }
    pthread_mutex_unlock(&nl);
    return e;
}

static uint64_t h_CreateEventW(u32 *a)
{
    char name[96] = "";
    if(a[3]) wstr(name, sizeof name, a[3]);
    int existed = 0;
    event_t *e = named_event(name, (int)a[1], (int)a[2], 1, &existed);
    u32 h = h_new(H_EVENT, -1, e, name);
    last_error = existed ? 183 : 0;          /* ERROR_ALREADY_EXISTS */
    LOGV("CreateEventW(%s, manual %u, inicial %u) = %#x", name, a[1], a[2], h);
    return R(h);
}

static uint64_t h_OpenEventW(u32 *a)
{
    char name[96] = "";
    wstr(name, sizeof name, a[2]);
    event_t *e = named_event(name, 0, 0, 0, NULL);
    if(!e) { last_error = ERROR_FILE_NOT_FOUND; return 0; }
    return R(h_new(H_EVENT, -1, e, name));
}

static uint64_t h_EventModify(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->type != H_EVENT) { last_error = ERROR_INVALID_HANDLE; return 0; }
    event_t *e = h->obj;
    pthread_mutex_lock(&e->m);
    switch(a[1]) {
    case 1:   /* EVENT_PULSE: acorda quem espera agora e deixa o evento desligado */
        e->pulses++;
        pthread_cond_broadcast(&e->c);
        e->state = 0;
        break;
    case 2: e->state = 0; break;                                   /* EVENT_RESET */
    case 3: e->state = 1; pthread_cond_broadcast(&e->c); break;    /* EVENT_SET */
    }
    pthread_mutex_unlock(&e->m);
    return R(TRUE_);
}

static void abstime(struct timespec *ts, u32 ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if(ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static uint64_t wait_impl(u32 *a);
static uint64_t h_WaitForSingleObject(u32 *a)
{
    uint64_t r = wait_impl(a);
    if(verbose > 1) {
        handle_t *h = h_get(a[0]);
        logf_("WAIT %s(%d) %d ms -> %u", h ? h->name : "?", h ? h->type : -1, (int)a[1], (u32)r);
    }
    return r;
}
static uint64_t wait_impl(u32 *a)
{
    handle_t *h = h_get(a[0]);
    u32 ms = a[1];
    struct timespec ts;
    if(!h) { last_error = ERROR_INVALID_HANDLE; return R(WAIT_FAILED); }
    if(ms != INFINITE_) abstime(&ts, ms);
    if(h->type == H_EVENT) {
        event_t *e = h->obj;
        int r = 0;
        pthread_mutex_lock(&e->m);
        unsigned p0 = e->pulses;
        while(!e->state && e->pulses == p0 && r != ETIMEDOUT)
            r = ms == INFINITE_ ? pthread_cond_wait(&e->c, &e->m) : pthread_cond_timedwait(&e->c, &e->m, &ts);
        int got = e->state || e->pulses != p0;
        if(e->state && !e->manual) e->state = 0;
        pthread_mutex_unlock(&e->m);
        return R(got ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
    }
    if(h->type == H_THREAD) {
        thread_t *t = h->obj;
        int r = 0;
        pthread_mutex_lock(&t->m);
        while(!t->done && r != ETIMEDOUT)
            r = ms == INFINITE_ ? pthread_cond_wait(&t->c, &t->m) : pthread_cond_timedwait(&t->c, &t->m, &ts);
        int done = t->done;
        pthread_mutex_unlock(&t->m);
        return R(done ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
    }
    if(h->type == H_MUTEX) {
        int r = ms == INFINITE_ ? pthread_mutex_lock(h->obj) : pthread_mutex_timedlock(h->obj, &ts);
        return R(r ? WAIT_TIMEOUT : WAIT_OBJECT_0);
    }
    logf_("WaitForSingleObject em handle tipo %d (%s): falha", h->type, h->name);
    return R(WAIT_FAILED);
}

static uint64_t h_CreateMutexW(u32 *a)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &at);
    if(a[1]) pthread_mutex_lock(m);
    return R(h_new(H_MUTEX, -1, m, "mutex"));
}

static uint64_t h_ReleaseMutex(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->type != H_MUTEX) return 0;
    return R(pthread_mutex_unlock(h->obj) == 0);
}

/* CRITICAL_SECTION do CE (20 bytes): a primeira palavra guarda o mutex */
static uint64_t h_InitializeCriticalSection(u32 *a)
{
    u32 *cs = P(a[0]);
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &at);
    memset(cs, 0, 20);
    cs[0] = (u32)(uintptr_t)m;
    return 0;
}
static uint64_t h_EnterCriticalSection(u32 *a)
{
    u32 *cs = P(a[0]);
    if(!cs[0]) h_InitializeCriticalSection(a);
    pthread_mutex_lock(P(cs[0]));
    return 0;
}
static uint64_t h_LeaveCriticalSection(u32 *a)
{
    u32 *cs = P(a[0]);
    if(cs[0]) pthread_mutex_unlock(P(cs[0]));
    return 0;
}

/* ----------------------------------------------------------------- threads */

static void *thread_main(void *arg)
{
    thread_t *t = arg;
    u32 rc = ce_call6(t->start, t->param, 0, 0, 0, 0, 0);
    pthread_mutex_lock(&t->m);
    t->exit_code = rc;
    t->done = 1;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
    return NULL;
}

static uint64_t h_CreateThread(u32 *a)
{
    thread_t *t = calloc(1, sizeof *t);
    t->start = a[2];
    t->param = a[3];
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t->t, &at, thread_main, t)) { free(t); return 0; }
    pthread_attr_destroy(&at);
    char nm[32];
    snprintf(nm, sizeof nm, "thread@%08x", t->start);
    u32 h = h_new(H_THREAD, -1, t, nm);
    if(a[5]) *(u32 *)P(a[5]) = h;
    LOGV("CreateThread(%#x, %#x) = %#x", t->start, t->param, h);
    return R(h);
}

static uint64_t h_CeSetThreadPriority(u32 *a) { LOGV("CeSetThreadPriority(%#x, %u)", a[0], a[1]); return R(TRUE_); }
static uint64_t h_TerminateThread(u32 *a) { logf_("TerminateThread(%#x) ignorado", a[0]); return R(TRUE_); }
static uint64_t h_GetExitCodeThread(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->type != H_THREAD) return 0;
    thread_t *t = h->obj;
    *(u32 *)P(a[1]) = t->done ? t->exit_code : 259;   /* STILL_ACTIVE */
    return R(TRUE_);
}

/* ----------------------------------------------------------- filas (msgq) */

static msgq_t *queues;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t h_CreateMsgQueue(u32 *a)
{
    char name[96] = "";
    if(a[0]) wstr(name, sizeof name, a[0]);
    u32 *opt = P(a[1]);   /* dwSize, dwFlags, dwMaxMessages, cbMaxMessage, bReadAccess */
    pthread_mutex_lock(&q_lock);
    msgq_t *q = NULL;
    if(name[0]) for(q = queues; q && strcmp(q->name, name); q = q->next) {}
    if(!q) {
        q = calloc(1, sizeof *q);
        snprintf(q->name, sizeof q->name, "%s", name);
        q->max_msgs = opt[2] ? opt[2] : 64;
        q->max_size = opt[3] ? opt[3] : 256;
        q->buf = malloc((size_t)q->max_msgs * (4 + q->max_size));
        pthread_mutex_init(&q->m, NULL);
        pthread_cond_init(&q->c, NULL);
        q->next = queues;
        queues = q;
    }
    q->refs++;
    pthread_mutex_unlock(&q_lock);
    u32 h = h_new(H_MSGQ, opt[4] ? 1 : 0, q, name);
    LOGV("CreateMsgQueue(%s, %u x %u, %s) = %#x", name, q->max_msgs, q->max_size, opt[4] ? "leitura" : "escrita", h);
    return R(h);
}

static uint64_t h_WriteMsgQueue(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->type != H_MSGQ) { last_error = ERROR_INVALID_HANDLE; return 0; }
    msgq_t *q = h->obj;
    u32 n = a[2] > q->max_size ? q->max_size : a[2];
    pthread_mutex_lock(&q->m);
    if(q->count >= q->max_msgs) { pthread_mutex_unlock(&q->m); last_error = 1460; return 0; }  /* cheia */
    uint8_t *slot = q->buf + ((q->head + q->count) % q->max_msgs) * (4 + q->max_size);
    memcpy(slot, &n, 4);
    memcpy(slot + 4, P(a[1]), n);
    q->count++;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return R(TRUE_);
}

static uint64_t h_ReadMsgQueue(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->type != H_MSGQ) { last_error = ERROR_INVALID_HANDLE; return 0; }
    msgq_t *q = h->obj;
    u32 ms = a[4];
    struct timespec ts;
    if(ms != INFINITE_) abstime(&ts, ms);
    int r = 0;
    pthread_mutex_lock(&q->m);
    while(!q->count && r != ETIMEDOUT)
        r = ms == INFINITE_ ? pthread_cond_wait(&q->c, &q->m) : pthread_cond_timedwait(&q->c, &q->m, &ts);
    if(!q->count) {
        pthread_mutex_unlock(&q->m);
        last_error = 1460;   /* ERROR_TIMEOUT */
        if(verbose > 1) logf_("MSGQ leitura %#x vazia (%d ms)", a[0], (int)ms);
        return 0;
    }
    uint8_t *slot = q->buf + q->head * (4 + q->max_size);
    u32 n;
    memcpy(&n, slot, 4);
    if(n > a[2]) n = a[2];
    memcpy(P(a[1]), slot + 4, n);
    q->head = (q->head + 1) % q->max_msgs;
    q->count--;
    pthread_mutex_unlock(&q->m);
    if(a[3]) *(u32 *)P(a[3]) = n;
    if(a[5]) *(u32 *)P(a[5]) = 0;
    return R(TRUE_);
}

static uint64_t h_CloseMsgQueue(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(h) h->type = H_FREE;
    return R(TRUE_);
}

/* --------------------------------------------------------------- registro */

typedef struct regval {
    char key[256];   /* em minúsculas */
    char name[64];   /* em minúsculas; "" = padrão */
    u32 type;        /* 1 REG_SZ, 4 REG_DWORD, 3 REG_BINARY */
    u32 len;
    uint8_t data[512];
    struct regval *next;
} regval_t;
static regval_t *regvals;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void lower(char *s) { for(; *s; s++) *s = (char)tolower((unsigned char)*s); }

static regval_t *reg_find(const char *key, const char *name, int create)
{
    char k[256], n[64];
    snprintf(k, sizeof k, "%.255s", key);
    snprintf(n, sizeof n, "%.63s", name);
    lower(k);
    lower(n);
    for(regval_t *v = regvals; v; v = v->next)
        if(!strcmp(v->key, k) && !strcmp(v->name, n)) return v;
    if(!create) return NULL;
    regval_t *v = calloc(1, sizeof *v);
    snprintf(v->key, sizeof v->key, "%s", k);
    snprintf(v->name, sizeof v->name, "%s", n);
    v->next = regvals;
    regvals = v;
    return v;
}

static int reg_key_exists(const char *key)
{
    char k[256];
    snprintf(k, sizeof k, "%.255s", key);
    lower(k);
    size_t l = strlen(k);
    for(regval_t *v = regvals; v; v = v->next)
        if(!strncmp(v->key, k, l) && (v->key[l] == 0 || v->key[l] == '\\')) return 1;
    return 0;
}

static void reg_load(const char *file)
{
    FILE *f = fopen(file, "r");
    if(!f) { logf_("registro %s: %s", file, strerror(errno)); return; }
    char line[512], key[256] = "";
    while(fgets(line, sizeof line, f)) {
        char *s = line;
        while(isspace((unsigned char)*s)) s++;
        if(*s == '#' || !*s) continue;
        s[strcspn(s, "\r\n")] = 0;
        if(*s == '[') {
            char *e = strchr(s, ']');
            if(e) { *e = 0; snprintf(key, sizeof key, "%.255s", s + 1); reg_find(key, "\x01", 1); }
            continue;
        }
        if(*s != '"') continue;
        char *e = strchr(s + 1, '"');
        if(!e || e[1] != '=') continue;
        *e = 0;
        regval_t *v = reg_find(key, s + 1, 1);
        char *val = e + 2;
        if(!strncmp(val, "dword:", 6)) {
            u32 d = (u32)strtoul(val + 6, NULL, 16);
            v->type = 4;
            v->len = 4;
            memcpy(v->data, &d, 4);
        }
        else if(*val == '"') {
            char *q = strrchr(val + 1, '"');
            if(q) *q = 0;
            v->type = 1;
            v->len = (u32)(utf8_to_w16((u16 *)v->data, sizeof v->data / 2, val + 1) + 1) * 2;
        }
    }
    fclose(f);
}

/* HKEY: valores pré-definidos do CE ou um handle nosso com o caminho */
static int hkey_path(u32 hk, char *out, size_t n)
{
    if(hk == 0x80000002u) { out[0] = 0; return 1; }          /* HKEY_LOCAL_MACHINE */
    if(hk == 0x80000001u) { snprintf(out, n, "HKCU"); return 1; }
    handle_t *h = h_get(hk);
    if(!h || h->type != H_DEV || !h->obj) return 0;
    snprintf(out, n, "%.255s", (char *)h->obj);
    return 1;
}

static u32 reg_open(u32 *a, int create)
{
    char base[256], sub[256] = "", full[514];
    if(!hkey_path(a[0], base, sizeof base)) return ERROR_INVALID_HANDLE;
    if(a[1]) wstr(sub, sizeof sub, a[1]);
    snprintf(full, sizeof full, "%s%s%s", base, base[0] && sub[0] ? "\\" : "", sub);
    pthread_mutex_lock(&reg_lock);
    int ok = reg_key_exists(full);
    if(!ok && create) { reg_find(full, "\x01", 1); ok = 1; }
    pthread_mutex_unlock(&reg_lock);
    LOGV("Reg%sKey(%s) = %s", create ? "Create" : "Open", full, ok ? "ok" : "não existe");
    if(!ok) return ERROR_FILE_NOT_FOUND;
    u32 h = h_new(H_DEV, -1, strdup(full), "regkey");
    *(u32 *)P(create ? a[7] : a[4]) = h;
    if(create && a[8]) *(u32 *)P(a[8]) = 2;   /* REG_OPENED_EXISTING_KEY */
    return 0;
}

static uint64_t h_RegOpenKeyExW(u32 *a) { return R(reg_open(a, 0)); }
static uint64_t h_RegCreateKeyExW(u32 *a) { return R(reg_open(a, 1)); }

static uint64_t h_RegQueryValueExW(u32 *a)
{
    char key[256], name[64] = "";
    if(!hkey_path(a[0], key, sizeof key)) return R(ERROR_INVALID_HANDLE);
    if(a[1]) wstr(name, sizeof name, a[1]);
    pthread_mutex_lock(&reg_lock);
    regval_t *v = reg_find(key, name, 0);
    u32 rc = 0;
    if(!v || !v->type) rc = ERROR_FILE_NOT_FOUND;
    else {
        if(a[3]) *(u32 *)P(a[3]) = v->type;
        u32 *cb = P(a[5]);
        if(a[4]) {
            if(!cb || *cb < v->len) rc = 234;    /* ERROR_MORE_DATA */
            else memcpy(P(a[4]), v->data, v->len);
        }
        if(cb) *cb = v->len;
    }
    pthread_mutex_unlock(&reg_lock);
    LOGV("RegQueryValueExW(%s, %s) = %u", key, name, rc);
    return R(rc);
}

static uint64_t h_RegSetValueExW(u32 *a)
{
    char key[256], name[64] = "";
    if(!hkey_path(a[0], key, sizeof key)) return R(ERROR_INVALID_HANDLE);
    if(a[1]) wstr(name, sizeof name, a[1]);
    pthread_mutex_lock(&reg_lock);
    regval_t *v = reg_find(key, name, 1);
    v->type = a[3];
    v->len = a[5] > sizeof v->data ? sizeof v->data : a[5];
    memcpy(v->data, P(a[4]), v->len);
    pthread_mutex_unlock(&reg_lock);
    LOGV("RegSetValueExW(%s, %s, tipo %u, %u bytes)", key, name, a[3], a[5]);
    return 0;
}

static uint64_t h_RegDeleteKeyW(u32 *a) { (void)a; return 0; }
static uint64_t h_RegCloseKey(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(h && h->type == H_DEV) { free(h->obj); h->type = H_FREE; }
    return 0;
}

/* --------------------------------------------------------------- arquivos */

static uint64_t h_CreateFileW(u32 *a)
{
    char name[300];
    wstr(name, sizeof name, a[0]);
    size_t l = strlen(name);
    if(l && name[l - 1] == ':') {            /* dispositivo do CE */
        if(!strncasecmp(name, "COM", 3)) {
            int fd = -1;
            if(hw) {
                fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
                if(fd < 0) { logf_("abrir /dev/ttyS0: %s", strerror(errno)); last_error = 2; return R(INVALID_H); }
            }
            u32 h = h_new(H_SERIAL, fd, NULL, name);
            logf_("CreateFileW(%s) = serial %#x (%s)", name, h, hw ? "/dev/ttyS0" : "sem hardware: poço");
            return R(h);
        }
        if(!strncasecmp(name, "GIO", 3)) {
            u32 h = h_new(H_GIO, -1, NULL, name);
            logf_("CreateFileW(%s) = GPIO %#x", name, h);
            return R(h);
        }
        logf_("CreateFileW(%s): dispositivo desconhecido", name);
        last_error = ERROR_FILE_NOT_FOUND;
        return R(INVALID_H);
    }
    char path[512];
    ce_path(path, sizeof path, name);
    int flags = 0;
    u32 acc = a[1], disp = a[4];
    if((acc & 0xC0000000u) == 0xC0000000u) flags = O_RDWR;
    else if(acc & 0x40000000u) flags = O_WRONLY;
    else flags = O_RDONLY;
    switch(disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;    /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;   /* CREATE_ALWAYS */
    case 4: flags |= O_CREAT; break;             /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;             /* TRUNCATE_EXISTING */
    }
    if(flags & O_CREAT) mkparents(path);
    int fd = open(path, flags, 0644);
    LOGV("CreateFileW(%s -> %s) = fd %d", name, path, fd);
    if(fd < 0) { last_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : 5; return R(INVALID_H); }
    return R(h_new(H_FILE, fd, NULL, name));
}

static int serial_read(int fd, uint8_t *buf, u32 n);

static uint64_t h_ReadFile(u32 *a)
{
    handle_t *h = h_get(a[0]);
    u32 *nread = P(a[3]);
    if(nread) *nread = 0;
    if(!h) { last_error = ERROR_INVALID_HANDLE; return 0; }
    if(h->type == H_SERIAL && h->fd < 0) { usleep(50000); return R(TRUE_); }
    if(h->type == H_SERIAL) {
        int g = serial_read(h->fd, P(a[1]), a[2]);
        if(nread) *nread = (u32)g;
        if(g || verbose > 1) hexlog("<-", P(a[1]), (u32)g, a[2]);
        return R(TRUE_);
    }
    ssize_t r = read(h->fd, P(a[1]), a[2]);
    if(r < 0) { last_error = 30; return 0; }
    if(nread) *nread = (u32)r;
    return R(TRUE_);
}

static uint64_t h_WriteFile(u32 *a)
{
    handle_t *h = h_get(a[0]);
    u32 *nw = P(a[3]);
    if(nw) *nw = 0;
    if(!h) { last_error = ERROR_INVALID_HANDLE; return 0; }
    if(h->type == H_SERIAL && h->fd < 0) {
        LOGV("serial: %u bytes descartados", a[2]);
        if(nw) *nw = a[2];
        return R(TRUE_);
    }
    ssize_t r = write(h->fd, P(a[1]), a[2]);
    if(r < 0) { last_error = 29; return 0; }
    if(nw) *nw = (u32)r;
    if(h->type == H_SERIAL) hexlog("->", P(a[1]), (u32)r, a[2]);
    return R(TRUE_);
}

static uint64_t h_SetFilePointer(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h || h->fd < 0) return R(0xFFFFFFFFu);
    off_t off = (off_t)(int32_t)a[1];
    if(a[2]) off = (off_t)((uint64_t)*(u32 *)P(a[2]) << 32 | a[1]);
    off_t r = lseek(h->fd, off, a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END);
    if(r < 0) return R(0xFFFFFFFFu);
    if(a[2]) *(u32 *)P(a[2]) = (u32)((uint64_t)r >> 32);
    return R((u32)r);
}

static uint64_t h_FlushFileBuffers(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(h && h->type == H_FILE) fsync(h->fd);
    return R(TRUE_);
}

static uint64_t h_CloseHandle(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(!h) { last_error = ERROR_INVALID_HANDLE; return 0; }
    if((h->type == H_FILE || h->type == H_SERIAL) && h->fd >= 0) close(h->fd);
    if(h->type == H_EVENT || h->type == H_THREAD || h->type == H_MUTEX) {
        /* o objeto continua vivo (outros handles e a thread podem usá-lo) */
    }
    h->type = H_FREE;
    return R(TRUE_);
}

static uint64_t h_DeleteFileW(u32 *a)
{
    char name[300], path[512];
    wstr(name, sizeof name, a[0]);
    ce_path(path, sizeof path, name);
    return R(unlink(path) == 0);
}

static uint64_t h_CopyFileW(u32 *a)
{
    char n1[300], n2[300], p1[512], p2[512];
    wstr(n1, sizeof n1, a[0]);
    wstr(n2, sizeof n2, a[1]);
    ce_path(p1, sizeof p1, n1);
    ce_path(p2, sizeof p2, n2);
    if(a[2] && access(p2, F_OK) == 0) return 0;
    int in = open(p1, O_RDONLY);
    if(in < 0) { last_error = ERROR_FILE_NOT_FOUND; return 0; }
    mkparents(p2);
    int out = open(p2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(out < 0) { close(in); return 0; }
    char buf[8192];
    ssize_t r;
    while((r = read(in, buf, sizeof buf)) > 0) if(write(out, buf, (size_t)r) != r) break;
    close(in);
    close(out);
    return R(r == 0);
}

static uint64_t h_CreateDirectoryW(u32 *a)
{
    char name[300], path[512];
    wstr(name, sizeof name, a[0]);
    ce_path(path, sizeof path, name);
    mkparents(path);
    return R(mkdir(path, 0755) == 0 || errno == EEXIST);
}

static uint64_t h_GetFileAttributesW(u32 *a)
{
    char name[300], path[512];
    struct stat st;
    wstr(name, sizeof name, a[0]);
    ce_path(path, sizeof path, name);
    if(stat(path, &st)) { last_error = ERROR_FILE_NOT_FOUND; return R(0xFFFFFFFFu); }
    return R(S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
}

static uint64_t h_SetFileAttributesW(u32 *a) { (void)a; return R(TRUE_); }

/* WIN32_FIND_DATAW do CE: atributos, 3 FILETIME, tamanho alto/baixo, OID, nome[260] */
static uint64_t h_FindFirstFileW(u32 *a)
{
    char name[300], path[512];
    wstr(name, sizeof name, a[0]);
    ce_path(path, sizeof path, name);
    char *slash = strrchr(path, '/');
    if(!slash) return R(INVALID_H);
    *slash = 0;
    const char *pat = slash + 1;
    DIR *d = opendir(path);
    if(!d) { last_error = ERROR_FILE_NOT_FOUND; return R(INVALID_H); }
    struct dirent *e;
    while((e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        if(fnmatch(pat, e->d_name, FNM_CASEFOLD)) continue;
        uint8_t *fd = P(a[1]);
        memset(fd, 0, 40 + 520);
        char full[800];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        u32 attr = 0x80, size = 0;
        if(!stat(full, &st)) { attr = S_ISDIR(st.st_mode) ? 0x10 : 0x80; size = (u32)st.st_size; }
        memcpy(fd, &attr, 4);
        memcpy(fd + 32, &size, 4);
        utf8_to_w16((u16 *)(fd + 40), 260, e->d_name);
        closedir(d);
        return R(h_new(H_FIND, -1, NULL, "find"));
    }
    closedir(d);
    last_error = ERROR_FILE_NOT_FOUND;
    return R(INVALID_H);
}

static uint64_t h_FindClose(u32 *a) { return h_CloseHandle(a); }

static uint64_t h_fopen(u32 *a)
{
    char path[512];
    ce_path(path, sizeof path, A(a[0]));
    if(strchr(A(a[1]), 'w') || strchr(A(a[1]), 'a')) mkparents(path);
    FILE *f = fopen(path, A(a[1]));
    /* logs do driver: sem buffer, para nada se perder se o processo terminar */
    if(f && (strchr(A(a[1]), 'w') || strchr(A(a[1]), 'a'))) setvbuf(f, NULL, _IONBF, 0);
    LOGV("fopen(%s -> %s, %s) = %p", A(a[0]), path, A(a[1]), (void *)f);
    return R((uintptr_t)f);
}

/* --------------------------------------------------------------- serial */

static u32 comm_to[5];    /* COMMTIMEOUTS: intervalo, mult. leitura, const. leitura, escrita... */
static u32 comm_baud = 38400;

static uint64_t h_comm_ok(u32 *a) { LOGV("serial: config (%#x)", a[0]); return R(TRUE_); }
static uint64_t h_SetCommTimeouts(u32 *a)
{
    memcpy(comm_to, P(a[1]), sizeof comm_to);
    if(strace_) logf_("serial: tempos %u/%u/%u (intervalo/mult/const)", comm_to[0], comm_to[1], comm_to[2]);
    return R(TRUE_);
}
static uint64_t h_GetCommTimeouts(u32 *a) { memcpy(P(a[1]), comm_to, sizeof comm_to); return R(TRUE_); }
static uint64_t h_PurgeComm(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(h && h->type == H_SERIAL && h->fd >= 0) tcflush(h->fd, (a[1] & 0x8) ? TCIOFLUSH : TCOFLUSH);
    return R(TRUE_);
}

static uint64_t h_GetCommState(u32 *a)
{
    u32 *dcb = P(a[1]);
    memset(dcb, 0, 28);
    dcb[0] = 28;          /* DCBlength */
    dcb[1] = comm_baud;   /* BaudRate */
    dcb[2] = 1;           /* fBinary */
    ((uint8_t *)dcb)[18] = 8;   /* ByteSize */
    return R(TRUE_);
}

static speed_t baud_code(u32 b)
{
    switch(b) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static uint64_t h_SetCommState(u32 *a)
{
    handle_t *h = h_get(a[0]);
    u32 *dcb = P(a[1]);
    if(strace_ || verbose) logf_("SetCommState: %u baud", dcb[1]);
    comm_baud = dcb[1];
    if(!h || h->type != H_SERIAL || h->fd < 0) return R(TRUE_);
    speed_t sp = baud_code(dcb[1]);
    if(!sp) { logf_("SetCommState: %u baud não suportado", dcb[1]); return 0; }
    struct termios t;
    if(tcgetattr(h->fd, &t)) return 0;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);   /* if_type 1: sem controle de fluxo */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    int ok = tcsetattr(h->fd, TCSADRAIN, &t) == 0;
    /* RTS/DTR como o DCB pede (o Linux liga os dois ao abrir): fDtrControl
     * bits 4-5, fRtsControl bits 12-13 (0 desligado, 1 ligado, 2/3 controle) */
    u32 fl = dcb[2];
    int dtr = (fl >> 4) & 3, rts = (fl >> 12) & 3, bits = 0;
    ioctl(h->fd, TIOCMGET, &bits);
    if(dtr == 1) bits |= TIOCM_DTR; else bits &= ~TIOCM_DTR;
    if(rts == 1) bits |= TIOCM_RTS; else bits &= ~TIOCM_RTS;
    ioctl(h->fd, TIOCMSET, &bits);
    if(strace_) logf_("serial: DCB flags %#x (DTR %d, RTS %d, bytesize %u, paridade %u, stop %u)", fl, dtr, rts,
                      ((uint8_t *)dcb)[18], ((uint8_t *)dcb)[19], ((uint8_t *)dcb)[20]);
    return R(ok);
}

/* ReadFile da serial: espera até o tempo total do CE e devolve o que houver */
static int serial_read(int fd, uint8_t *buf, u32 n)
{
    u32 total = comm_to[2] + comm_to[1] * n;
    if(comm_to[0] == 0xFFFFFFFFu && !comm_to[1] && !comm_to[2]) total = 0;   /* volta já */
    if(!total) total = 0;
    u32 got = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for(;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        long left = (long)total - el;
        if(poll(&pf, 1, got ? (int)(comm_to[0] && comm_to[0] != 0xFFFFFFFFu ? comm_to[0] : 0) : (left > 0 ? (int)left : 0)) <= 0)
            break;
        ssize_t r = read(fd, buf + got, n - got);
        if(r <= 0) break;
        got += (u32)r;
        if(got >= n) break;
    }
    return (int)got;
}

/* ------------------------------------------------ dispositivos e kernel */

static uint64_t h_KernelIoControl(u32 *a)
{
    u32 code = a[0];
    u32 *out = P(a[3]);
    if(code == 0x01012038u && out && a[4] >= 8) {    /* IOCTL_HAL_GET_GPS_HW_PIN */
        out[0] = 64;      /* reset */
        out[1] = 63;      /* standby */
        if(a[5]) *(u32 *)P(a[5]) = 8;
        return R(TRUE_);
    }
    logf_("KernelIoControl(%#x) não tratado", code);
    last_error = 50;
    return 0;
}

/*
 * GIO1: (buses.dll do CE). DeviceIoControl(0x220400) preenche uma tabela:
 * +0x00 contexto, +0x08 OUT, +0x10 DIR, +0x18 pull-up, +0x20 pull-down;
 * cada função é f(contexto, pino, valor) (gps.dll 0x1DFADF0). Sem --hw só
 * registra; no M3 escreve nos registradores de 0x80002000.
 */
static const char *gio_what[] = { "OUT", "DIR", "PU", "PD" };
static volatile u32 *gpio_regs;      /* 0x80002000, só com --hw */
static uint64_t gio_call(int what, u32 *a)
{
    u32 pin = a[1], val = a[2];
    if(pin != 63 && pin != 64) {           /* o gps.dll só usa reset e standby */
        logf_("GPIO %s pino %u = %u: pino fora do GPS, recusado", gio_what[what], pin, val);
        return 0;
    }
    if(!hw || !gpio_regs) {
        logf_("GPIO %s pino %u = %u (sem hardware)", gio_what[what], pin, val);
        return R(TRUE_);
    }
    /* como o driver de áudio (GPIO 5): leitura-modificação-escrita em
     * 0x80002000 + tipo*0x100 + (pino/32)*0x10 */
    volatile u32 *r = gpio_regs + (what == 0 ? 0x300 : what == 1 ? 0x000 : what == 2 ? 0x100 : 0x200) / 4 + (pin / 32) * 4;
    u32 bit = 1u << (pin % 32);
    *r = val ? (*r | bit) : (*r & ~bit);
    LOGV("GPIO %s pino %u = %u", gio_what[what], pin, val);
    return R(TRUE_);
}
static uint64_t g_out(u32 *a) { return gio_call(0, a); }
static uint64_t g_dir(u32 *a) { return gio_call(1, a); }
static uint64_t g_pu(u32 *a) { return gio_call(2, a); }
static uint64_t g_pd(u32 *a) { return gio_call(3, a); }
static uint64_t g_other(u32 *a) { logf_("GPIO: função desconhecida (%#x, %u, %u)", a[0], a[1], a[2]); return 0; }

static uint64_t h_DeviceIoControl(u32 *a)
{
    handle_t *h = h_get(a[0]);
    if(h && h->type == H_GIO && a[1] == 0x220400u && a[4] && a[5] >= 0x40) {
        static u32 tab[16];
        if(!tab[2]) {
            for(int i = 1; i < 16; i++) tab[i] = make_thunk("GIO?", g_other);
            tab[2] = make_thunk("GIO.OUT", g_out);
            tab[4] = make_thunk("GIO.DIR", g_dir);
            tab[6] = make_thunk("GIO.PU", g_pu);
            tab[8] = make_thunk("GIO.PD", g_pd);
            tab[0] = 0x6100;           /* contexto (não usado por nós) */
        }
        memcpy(P(a[4]), tab, 0x40);
        if(a[6]) *(u32 *)P(a[6]) = 0x40;
        return R(TRUE_);
    }
    logf_("DeviceIoControl(%s, %#x, in %u, out %u): não tratado", h ? h->name : "?", a[1], a[3], a[5]);
    last_error = 50;
    return 0;
}

static uint64_t h_DeactivateDevice(u32 *a) { logf_("DeactivateDevice(%#x)", a[0]); return R(TRUE_); }

static uint64_t h_CreateBusAccessHandle(u32 *a)
{
    char k[160];
    wstr(k, sizeof k, a[0]);
    logf_("CreateBusAccessHandle(%s)", k);
    return R(h_new(H_BUS, -1, NULL, "bus"));
}

/*
 * BusIoControl(0x220C08 liga / 0x220C0C desliga, recurso 3) = PMU 0x8006136C
 * bit 0 (bus.dll). O CE já deixa o GPS alimentado: por cautela nunca
 * desligamos; o reset pelo GPIO 64 reinicia o chip.
 */
static volatile u32 *pmu_regs;       /* 0x80061000 */
static uint64_t h_BusIoControl(u32 *a)
{
    u32 res = a[2] && a[3] >= 4 ? *(u32 *)P(a[2]) : 0xFFFFFFFFu;
    if(res == 3 && a[1] == 0x220C0Cu) {
        LOGV("BusIoControl: desligar o GPS (recurso 3) ignorado; fica alimentado");
        return R(TRUE_);
    }
    if(res == 3 && a[1] == 0x220C08u) {
        if(hw && pmu_regs) {
            u32 v = pmu_regs[0x36C / 4];
            if(!(v & 1)) { pmu_regs[0x36C / 4] = v | 1; logf_("PMU 0x8006136C: GPS ligado (%#x -> %#x)", v, v | 1); }
        }
        return R(TRUE_);
    }
    logf_("BusIoControl(%#x, recurso %u): não tratado", a[1], res);
    return R(TRUE_);
}

/* -------------------------------------------- VCOM_Driver (saída NMEA) */

static u32 vcom_lib[2];

/* VCOM_Driver do CE: o gps.dll escreve o NMEA (COM_Write) e os aplicativos
 * leem (COM_Read). Aqui a escrita vai para a saída padrão e também para um
 * buffer que o COM_Read devolve. */
static char vbuf[16384];
static u32 vlen;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t v_Init(u32 *a) { LOGV("VCOM COM_Init(%#x)", a[0]); return R(0x7c0001); }
static uint64_t v_Deinit(u32 *a) { (void)a; return R(TRUE_); }
static uint64_t v_Open(u32 *a) { LOGV("VCOM COM_Open(%#x, %#x)", a[0], a[1]); return R(0x7c0002); }
static uint64_t v_Close(u32 *a) { (void)a; return R(TRUE_); }
static uint64_t v_Write(u32 *a)
{
    LOGV("VCOM COM_Write(%#x, %u bytes)", a[0], a[2]);
    fwrite(P(a[1]), 1, a[2], stdout);
    fflush(stdout);
    pthread_mutex_lock(&vlock);
    u32 n = a[2] < sizeof vbuf - vlen ? a[2] : (u32)(sizeof vbuf - vlen);
    memcpy(vbuf + vlen, P(a[1]), n);
    vlen += n;
    pthread_mutex_unlock(&vlock);
    return R(a[2]);
}
static uint64_t v_Read(u32 *a)
{
    pthread_mutex_lock(&vlock);
    u32 n = a[2] < vlen ? a[2] : vlen;
    memcpy(P(a[1]), vbuf, n);
    memmove(vbuf, vbuf + n, vlen - n);
    vlen -= n;
    pthread_mutex_unlock(&vlock);
    if(!n) usleep(100000);
    return R(n);
}
static uint64_t v_Seek(u32 *a) { (void)a; return R(0xFFFFFFFFu); }
static uint64_t v_IOControl(u32 *a) { LOGV("VCOM IOControl(%#x)", a[1]); return R(TRUE_); }
static uint64_t v_Power(u32 *a) { (void)a; return 0; }

static uint64_t h_LoadLibraryW(u32 *a)
{
    char name[200];
    wstr(name, sizeof name, a[0]);
    logf_("LoadLibraryW(%s)", name);
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    if(!strcasecmp(b, "VCOM_Driver.dll")) return R(vcom_lib[0] = 0x7c000000);
    if(!strcasecmp(b, "VCOM_Driver2.dll")) return R(vcom_lib[1] = 0x7d000000);
    last_error = 126;
    return 0;
}

static uint64_t h_GetProcAddressW(u32 *a)
{
    char name[64];
    wstr(name, sizeof name, a[1]);
    static const struct { const char *n; hfn_t f; } v[] = {
        { "COM_Init", v_Init }, { "COM_Deinit", v_Deinit }, { "COM_Open", v_Open }, { "COM_Close", v_Close },
        { "COM_Write", v_Write }, { "COM_Read", v_Read }, { "COM_Seek", v_Seek }, { "COM_IOControl", v_IOControl },
        { "COM_PowerUp", v_Power }, { "COM_PowerDown", v_Power },
    };
    for(size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        if(!strcmp(name, v[i].n)) {
            u32 t = make_thunk(v[i].n, v[i].f);
            LOGV("GetProcAddressW(%#x, %s) = %#x", a[0], name, t);
            return R(t);
        }
    logf_("GetProcAddressW(%#x, %s): não existe", a[0], name);
    return 0;
}

static uint64_t h_FreeLibrary(u32 *a) { (void)a; return R(TRUE_); }

/* ------------------------------------------------- tabela de imports */

extern void ce_negd(void), ce_negs(void), ce_ned(void), ce_nes(void), ce_rt_sdiv(void), ce_rt_udiv(void);
extern void __aeabi_dadd(void), __aeabi_dsub(void), __aeabi_dmul(void), __aeabi_ddiv(void);
extern void __aeabi_fadd(void), __aeabi_fsub(void), __aeabi_fmul(void), __aeabi_fdiv(void);
extern void __aeabi_dcmpeq(void), __aeabi_dcmplt(void), __aeabi_dcmple(void), __aeabi_dcmpgt(void), __aeabi_dcmpge(void);
extern void __aeabi_fcmpeq(void), __aeabi_fcmplt(void), __aeabi_fcmple(void), __aeabi_fcmpgt(void), __aeabi_fcmpge(void);
extern void __aeabi_i2d(void), __aeabi_ui2d(void), __aeabi_l2d(void), __aeabi_d2iz(void), __aeabi_d2uiz(void), __aeabi_d2lz(void);
extern void __aeabi_f2d(void), __aeabi_d2f(void), __aeabi_i2f(void), __aeabi_ui2f(void), __aeabi_f2iz(void), __aeabi_f2uiz(void);
extern void __aeabi_ldivmod(void);

/* chamadas diretas: a convenção do CE coincide com a EABI soft-float */
static const struct { const char *name; void *fn; } direct[] = {
    { "__addd", __aeabi_dadd }, { "__subd", __aeabi_dsub }, { "__muld", __aeabi_dmul }, { "__divd", __aeabi_ddiv },
    { "__adds", __aeabi_fadd }, { "__subs", __aeabi_fsub }, { "__muls", __aeabi_fmul }, { "__divs", __aeabi_fdiv },
    { "__negd", ce_negd }, { "__negs", ce_negs }, { "__ned", ce_ned }, { "__nes", ce_nes },
    { "__eqd", __aeabi_dcmpeq }, { "__ltd", __aeabi_dcmplt }, { "__led", __aeabi_dcmple },
    { "__gtd", __aeabi_dcmpgt }, { "__ged", __aeabi_dcmpge },
    { "__eqs", __aeabi_fcmpeq }, { "__lts", __aeabi_fcmplt }, { "__les", __aeabi_fcmple },
    { "__gts", __aeabi_fcmpgt }, { "__ges", __aeabi_fcmpge },
    { "__itod", __aeabi_i2d }, { "__utod", __aeabi_ui2d }, { "__i64tod", __aeabi_l2d },
    { "__dtoi", __aeabi_d2iz }, { "__dtou", __aeabi_d2uiz }, { "__dtoi64", __aeabi_d2lz },
    { "__stod", __aeabi_f2d }, { "__dtos", __aeabi_d2f }, { "__itos", __aeabi_i2f }, { "__utos", __aeabi_ui2f },
    { "__stoi", __aeabi_f2iz }, { "__stou", __aeabi_f2uiz },
    { "__rt_sdiv", ce_rt_sdiv }, { "__rt_udiv", ce_rt_udiv }, { "__rt_sdiv64by64", __aeabi_ldivmod },
    { "sqrt", sqrt }, { "sin", sin }, { "cos", cos }, { "tan", tan }, { "asin", asin }, { "acos", acos },
    { "atan", atan }, { "atan2", atan2 }, { "exp", exp }, { "pow", pow }, { "log10", log10 },
    { "fabs", fabs }, { "floor", floor }, { "ceil", ceil }, { "fmod", fmod }, { "modf", modf }, { "atof", atof },
    { "memcpy", memcpy }, { "memset", memset }, { "memcmp", memcmp }, { "strlen", strlen },
    { "strcpy", strcpy }, { "strcat", strcat }, { "strncmp", strncmp }, { "qsort", qsort },
    { "malloc", malloc }, { "free", free },
    { "fclose", fclose }, { "fread", fread }, { "fwrite", fwrite }, { "fseek", fseek },
};

static const handler_t handlers[] = {
    { "GetSystemTime", h_GetSystemTime }, { "GetLocalTime", h_GetLocalTime }, { "SetSystemTime", h_SetSystemTime },
    { "SystemTimeToFileTime", h_SystemTimeToFileTime }, { "FileTimeToSystemTime", h_FileTimeToSystemTime },
    { "GetTickCount", h_GetTickCount }, { "Sleep", h_Sleep },
    { "sprintf", h_sprintf }, { "_vsnprintf", h_vsnprintf }, { "wsprintfW", h_wsprintfW },
    { "NKDbgPrintfW", h_NKDbgPrintfW }, { "wcslen", h_wcslen }, { "wcscpy", h_wcscpy }, { "wcsncpy", h_wcsncpy },
    { "wcschr", h_wcschr }, { "_itow", h_itow }, { "rand", h_rand }, { "srand", h_srand },
    { "LocalAlloc", h_LocalAlloc }, { "LocalFree", h_LocalFree }, { "GetLastError", h_GetLastError },
    { "CreateEventW", h_CreateEventW }, { "OpenEventW", h_OpenEventW }, { "EventModify", h_EventModify },
    { "WaitForSingleObject", h_WaitForSingleObject }, { "CreateMutexW", h_CreateMutexW },
    { "ReleaseMutex", h_ReleaseMutex }, { "InitializeCriticalSection", h_InitializeCriticalSection },
    { "EnterCriticalSection", h_EnterCriticalSection }, { "LeaveCriticalSection", h_LeaveCriticalSection },
    { "CreateThread", h_CreateThread }, { "CeSetThreadPriority", h_CeSetThreadPriority },
    { "TerminateThread", h_TerminateThread }, { "GetExitCodeThread", h_GetExitCodeThread },
    { "CreateMsgQueue", h_CreateMsgQueue }, { "ReadMsgQueue", h_ReadMsgQueue },
    { "WriteMsgQueue", h_WriteMsgQueue }, { "CloseMsgQueue", h_CloseMsgQueue },
    { "RegOpenKeyExW", h_RegOpenKeyExW }, { "RegCreateKeyExW", h_RegCreateKeyExW },
    { "RegQueryValueExW", h_RegQueryValueExW }, { "RegSetValueExW", h_RegSetValueExW },
    { "RegDeleteKeyW", h_RegDeleteKeyW }, { "RegCloseKey", h_RegCloseKey },
    { "CreateFileW", h_CreateFileW }, { "ReadFile", h_ReadFile }, { "WriteFile", h_WriteFile },
    { "SetFilePointer", h_SetFilePointer }, { "FlushFileBuffers", h_FlushFileBuffers },
    { "CloseHandle", h_CloseHandle }, { "DeleteFileW", h_DeleteFileW }, { "CopyFileW", h_CopyFileW },
    { "CreateDirectoryW", h_CreateDirectoryW }, { "GetFileAttributesW", h_GetFileAttributesW },
    { "SetFileAttributesW", h_SetFileAttributesW }, { "FindFirstFileW", h_FindFirstFileW },
    { "FindClose", h_FindClose }, { "fopen", h_fopen },
    { "GetCommState", h_GetCommState }, { "SetCommState", h_SetCommState }, { "SetupComm", h_comm_ok },
    { "GetCommTimeouts", h_GetCommTimeouts }, { "SetCommTimeouts", h_SetCommTimeouts }, { "PurgeComm", h_PurgeComm },
    { "KernelIoControl", h_KernelIoControl }, { "DeviceIoControl", h_DeviceIoControl },
    { "DeactivateDevice", h_DeactivateDevice },
    { "CreateBusAccessHandle", h_CreateBusAccessHandle }, { "BusIoControl", h_BusIoControl },
    { "LoadLibraryW", h_LoadLibraryW }, { "GetProcAddressW", h_GetProcAddressW }, { "FreeLibrary", h_FreeLibrary },
};

/* ------------------------------------------------------------- carregador */

typedef struct { u32 va, vsize, filesize, prot; } seg_t;
static u32 exp_va(const char *name, const uint8_t *exps, u32 nexp)
{
    for(u32 i = 0; i < nexp; i++)
        if(!strncmp((const char *)exps + i * 32 + 4, name, 28)) return *(const u32 *)(exps + i * 32);
    return 0;
}

/* --probe BAUD [HEX]: só a serial, sem o DLL nem reset. Ouve 3 s, envia HEX
 * (opcional) e ouve mais 3 s; mostra quantos bytes chegaram e os primeiros. */
static int probe(u32 baud, const char *hex)
{
    int fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) { logf_("/dev/ttyS0: %s", strerror(errno)); return 1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
    cfsetispeed(&t, baud_code(baud));
    cfsetospeed(&t, baud_code(baud));
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    for(int phase = 0; phase < 2; phase++) {
        if(phase == 1) {
            if(!hex) break;
            uint8_t b[256];
            u32 n = 0;
            for(const char *q = hex; q[0] && q[1] && n < sizeof b; q += 2) {
                if(*q == ' ') { q--; continue; }
                unsigned v;
                if(sscanf(q, "%2x", &v) != 1) break;
                b[n++] = (uint8_t)v;
            }
            strace_ = 1;
            hexlog("->", b, n, n);
            if(write(fd, b, n) != (ssize_t)n) logf_("escrita incompleta");
            tcdrain(fd);
        }
        uint8_t buf[4096];
        u32 got = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for(;;) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 3000) break;
            struct pollfd pf = { fd, POLLIN, 0 };
            if(poll(&pf, 1, 100) <= 0) continue;
            ssize_t r = read(fd, buf + got, sizeof buf - got);
            if(r > 0) got += (u32)r;
            if(got >= sizeof buf) break;
        }
        strace_ = 1;
        logf_("%s: %u bytes em 3 s a %u baud", phase ? "depois do envio" : "ouvindo", got, baud);
        if(got) hexlog("<-", buf, got, got);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc >= 3 && !strcmp(argv[1], "--probe")) return probe((u32)atoi(argv[2]), argc > 3 ? argv[3] : NULL);
    const char *bfx = NULL, *regfile = "/etc/bigfoston/gps-registry.txt";
    int seconds = 20;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-b") && i + 1 < argc) bfx = argv[++i];
        else if(!strcmp(argv[i], "-r") && i + 1 < argc) regfile = argv[++i];
        else if(!strcmp(argv[i], "-d") && i + 1 < argc) data_dir = argv[++i];
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--hw")) hw = 1;
        else if(!strcmp(argv[i], "-v")) verbose++;
        else if(!strcmp(argv[i], "-s")) strace_ = 1;
        else { fprintf(stderr, "uso: %s [-b gps.bfx] [-r registro] [-d dados] [-t s] [--hw] [-v] [-s]\n", argv[0]); return 2; }
    }
    char defbfx[512];
    if(!bfx) { snprintf(defbfx, sizeof defbfx, "%s/gps.bfx", data_dir); bfx = defbfx; }

    FILE *f = fopen(bfx, "rb");
    if(!f) { logf_("%s: %s", bfx, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)fsz);
    if(!img || fread(img, 1, (size_t)fsz, f) != (size_t)fsz) { logf_("leitura de %s falhou", bfx); return 1; }
    fclose(f);
    if(memcmp(img, "BFX1", 4)) { logf_("%s não é BFX1", bfx); return 1; }
    u32 entry, nseg, nimp, nexp;
    memcpy(&entry, img + 4, 4);
    memcpy(&nseg, img + 8, 4);
    memcpy(&nimp, img + 12, 4);
    memcpy(&nexp, img + 16, 4);

    /* região única do primeiro ao último segmento, depois mprotect por segmento */
    seg_t segs[8];
    const uint8_t *p = img + 20;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    if(nseg > 8) return 1;
    for(u32 i = 0; i < nseg; i++) {
        memcpy(&segs[i], p, 16);
        p += 16 + segs[i].filesize;
        if(segs[i].va < lo) lo = segs[i].va;
        if(segs[i].va + segs[i].vsize > hi) hi = segs[i].va + segs[i].vsize;
    }
    lo &= ~0xFFFu;
    hi = (hi + 0xFFFu) & ~0xFFFu;
    void *m = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if(m == MAP_FAILED || (uintptr_t)m != lo) { logf_("mmap em %#x: %s", lo, strerror(errno)); return 1; }
    p = img + 20;
    for(u32 i = 0; i < nseg; i++) {
        memcpy((void *)(uintptr_t)segs[i].va, p + 16, segs[i].filesize);
        p += 16 + segs[i].filesize;
    }
    const uint8_t *imps = p;
    const uint8_t *exps = imps + nimp * 68;

    thunk_mem = mmap(NULL, MAX_THUNKS * 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(thunk_mem == MAP_FAILED) { logf_("mmap dos thunks: %s", strerror(errno)); return 1; }

    int missing = 0;
    for(u32 i = 0; i < nimp; i++) {
        u32 iat;
        char name[47];
        memcpy(&iat, imps + i * 68, 4);
        memcpy(name, imps + i * 68 + 22, 46);
        name[46] = 0;
        u32 target = 0;
        for(size_t k = 0; k < sizeof direct / sizeof direct[0] && !target; k++)
            if(!strcmp(direct[k].name, name)) target = (u32)(uintptr_t)direct[k].fn;
        if(!target) {
            hfn_t fn = NULL;
            for(size_t k = 0; k < sizeof handlers / sizeof handlers[0]; k++)
                if(!strcmp(handlers[k].name, name)) fn = handlers[k].fn;
            if(!fn) { logf_("import sem implementação: %s", name); missing++; }
            target = make_thunk(strdup(name), fn);
        }
        *(u32 *)(uintptr_t)iat = target;
    }
    for(u32 i = 0; i < nseg; i++) {
        int pr = (segs[i].prot & 1 ? PROT_READ : 0) | (segs[i].prot & 2 ? PROT_WRITE : 0) |
                 (segs[i].prot & 4 ? PROT_EXEC : 0);
        u32 a0 = segs[i].va & ~0xFFFu, a1 = (segs[i].va + segs[i].vsize + 0xFFFu) & ~0xFFFu;
        mprotect((void *)(uintptr_t)a0, a1 - a0, pr ? pr : PROT_READ);
    }
    __builtin___clear_cache((char *)(uintptr_t)lo, (char *)(uintptr_t)hi);
    logf_("gps.dll carregado em %#x-%#x: %u imports (%d sem implementação), %s",
          lo, hi, nimp, missing, hw ? "COM HARDWARE" : "sem hardware");

    reg_load(regfile);
    if(hw) {
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if(mfd < 0) { logf_("/dev/mem: %s", strerror(errno)); return 1; }
        gpio_regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0x80002000);
        pmu_regs = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0x80061000);
        close(mfd);
        if(gpio_regs == MAP_FAILED || pmu_regs == MAP_FAILED) { logf_("mmap de /dev/mem falhou"); return 1; }
        logf_("GPIO 63/64 antes: dir %08x %08x out %08x %08x; PMU 0x8006136C %08x",
              gpio_regs[0x010 / 4], gpio_regs[0x020 / 4], gpio_regs[0x310 / 4], gpio_regs[0x320 / 4], pmu_regs[0x36C / 4]);
    }

    /* DllMain(hinst, DLL_PROCESS_ATTACH, 0) */
    u32 r = ce_call6(entry, lo, 1, 0, 0, 0, 0);
    logf_("DllEntry = %u", r);
    uint16_t ctx[32];
    utf8_to_w16(ctx, 32, "Drivers\\Active\\99");
    u32 init = ce_call6(exp_va("COM_Init", exps, nexp), (u32)(uintptr_t)ctx, 0, 0, 0, 0, 0);
    logf_("COM_Init = %#x", init);
    if(!init) return 1;
    u32 open_h = ce_call6(exp_va("COM_Open", exps, nexp), init, 0xC0000000u, 0, 0, 0, 0);
    logf_("COM_Open = %#x", open_h);
    /* como um aplicativo: lê COM1: (COM_Read do gps.dll) e mostra o NMEA */
    u32 rd = exp_va("COM_Read", exps, nexp);
    static char app[1024];
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    long total = 0;
    for(;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        if(ts1.tv_sec - ts0.tv_sec >= seconds) break;
        u32 n = ce_call6(rd, open_h, (u32)(uintptr_t)app, sizeof app, 0, 0, 0);
        if(n && n != 0xFFFFFFFFu && n <= sizeof app) {
            total += n;
            LOGV("COM_Read: %u bytes", n);
        }
        else usleep(50000);
    }
    logf_("COM_Read: %ld bytes de NMEA em %d s", total, seconds);
    logf_("fim (%d s)", seconds);
    return 0;
}
