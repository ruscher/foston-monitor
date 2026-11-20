/* clock.c — ver clock.h. */
#define _DEFAULT_SOURCE
#include "clock.h"
#include "settings.h"
#include "status.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define BOOT_MARK   "/run/bigfoston-clock"
#define PROFILE_TZ  "/etc/profile.d/tz.sh"
#define NTPD        "/usr/sbin/ntpd"
#define NTP_TIMEOUT 12          /* s por servidor */
#define SYNC_OK_S   3600        /* próxima sincronização após sucesso */
#define SYNC_FAIL_S 600         /* ... e após falha */
#define EPOCH_2024  1704067200L

static const clock_zone_t zones[] = {
    { "brasilia",  "Brasília, São Paulo, Rio, Salvador",   "<-03>3",  -180, false },
    { "manaus",    "Manaus, Cuiabá, Porto Velho, Boa Vista", "<-04>4", -240, false },
    { "rio_branco","Rio Branco (Acre)",                    "<-05>5",  -300, false },
    { "noronha",   "Fernando de Noronha",                  "<-02>2",  -120, false },
    { "buenos_aires","Buenos Aires, Montevidéu",           "<-03>3",  -180, false },
    { "santiago",  "Santiago (Chile)",       "<-04>4<-03>,M9.1.6/24,M4.1.6/24", -240, true },
    { "lima",      "Lima, Bogotá, Quito",                  "<-05>5",  -300, false },
    { "mexico",    "Cidade do México",                     "CST6",    -360, false },
    { "new_york",  "Nova York, Miami, Toronto",  "EST5EDT,M3.2.0,M11.1.0", -300, true },
    { "chicago",   "Chicago, Dallas",            "CST6CDT,M3.2.0,M11.1.0", -360, true },
    { "los_angeles","Los Angeles, Vancouver",    "PST8PDT,M3.2.0,M11.1.0", -480, true },
    { "utc",       "UTC (tempo universal)",                "UTC0",       0, false },
    { "lisboa",    "Lisboa, Londres",       "WET0WEST,M3.5.0/1,M10.5.0",   0, true },
    { "paris",     "Paris, Madri, Roma, Berlim", "CET-1CEST,M3.5.0,M10.5.0/3", 60, true },
    { "luanda",    "Luanda, Lagos",                        "WAT-1",      60, false },
    { "maputo",    "Maputo, Joanesburgo",                  "<+02>-2",   120, false },
    { "moscou",    "Moscou",                               "MSK-3",     180, false },
    { "india",     "Nova Délhi, Mumbai",                   "IST-5:30",  330, false },
    { "tokyo",     "Tóquio, Seul",                         "JST-9",     540, false },
    { "sydney",    "Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3", 600, true },
};
#define N_ZONES (int)(sizeof zones / sizeof zones[0])

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static clock_src_t src = CLK_UNSET;
static time_t src_when;
static char src_detail[64];
static bool busy;
static int sync_res;
static time_t sync_when;
static char sync_err[96];
static time_t next_sync;        /* relógio monotônico (s) da próxima tentativa automática */

int clock_zone_count(void) { return N_ZONES; }
const clock_zone_t *clock_zone(int i) { return &zones[i >= 0 && i < N_ZONES ? i : 0]; }

int clock_zone_index(const char *id)
{
    for(int i = 0; i < N_ZONES; i++)
        if(!strcmp(zones[i].id, id)) return i;
    return 0;
}

const clock_zone_t *clock_zone_current(void) { return clock_zone(clock_zone_index(settings()->tz)); }

void clock_utc_label(int m, char *out, size_t n)
{
    int a = m < 0 ? -m : m;
    snprintf(out, n, "UTC%c%02d:%02d", m < 0 ? '-' : '+', a / 60, a % 60);
}

static time_t mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* TZ POSIX em vigor: o do fuso, ou com +1 h fixo (horário de verão manual) */
static void posix_tz(char *out, size_t n)
{
    const clock_zone_t *z = clock_zone_current();
    if(!settings()->dst || z->rules) { snprintf(out, n, "%s", z->posix); return; }
    int m = z->utc_min + 60;              /* deslocamento em vigor */
    int a = m < 0 ? -m : m;
    char name[16];
    if(a % 60) snprintf(name, sizeof name, "<%c%02d%02d>", m < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(name, sizeof name, "<%c%02d>", m < 0 ? '-' : '+', a / 60);
    /* no POSIX o sinal é o inverso: UTC−2 é "2" */
    if(a % 60) snprintf(out, n, "%s%s%d:%02d", name, m > 0 ? "-" : "", a / 60, a % 60);
    else snprintf(out, n, "%s%s%d", name, m > 0 ? "-" : "", a / 60);
}

void clock_apply_tz(void)
{
    char tz[64];
    posix_tz(tz, sizeof tz);
    setenv("TZ", tz, 1);
    tzset();
#ifdef BIGFOSTON_TARGET
    /* sessões SSH novas usam o mesmo fuso */
    FILE *f = fopen(PROFILE_TZ ".tmp", "w");
    if(f) {
        fprintf(f, "# gerado pelo bigfoston-shell (Ajustes > Data)\nexport TZ='%s'\n", tz);
        if(fclose(f) == 0) rename(PROFILE_TZ ".tmp", PROFILE_TZ);
    }
#endif
}

bool clock_valid(void) { return time(NULL) >= EPOCH_2024; }

bool clock_can_set(void)
{
#ifdef BIGFOSTON_TARGET
    return !status_provider()->simulated && geteuid() == 0;
#else
    return false;               /* simulador: nunca mexe no relógio do PC */
#endif
}

static void set_source(clock_src_t s, const char *detail)
{
    pthread_mutex_lock(&lock);
    src = s;
    src_when = time(NULL);
    snprintf(src_detail, sizeof src_detail, "%s", detail ? detail : "");
    pthread_mutex_unlock(&lock);
}

void clock_init(void)
{
    clock_apply_tz();
    FILE *f = fopen(BOOT_MARK, "r");
    if(f) {
        long t = 0;
        if(fscanf(f, "%ld", &t) == 1 && clock_valid()) {
            src = CLK_BOOT;
            src_when = (time_t)t;
        }
        fclose(f);
    }
    next_sync = mono_s() + 5;    /* deixa a rede USB subir */
}

clock_src_t clock_source(time_t *when)
{
    pthread_mutex_lock(&lock);
    /* hora válida de origem desconhecida (SSH, simulador): "relógio do sistema" */
    clock_src_t s = !clock_valid() ? CLK_UNSET : src == CLK_UNSET ? CLK_SYSTEM : src;
    if(when) *when = src_when;
    pthread_mutex_unlock(&lock);
    return s;
}

const char *clock_source_detail(void) { return src_detail; }

bool clock_set_local(const struct tm *local, char *err, size_t n)
{
    struct tm t = *local;
    t.tm_isdst = -1;                       /* o fuso decide */
    time_t v = mktime(&t);
    if(v == (time_t)-1 || v < EPOCH_2024) { snprintf(err, n, "data inválida"); return false; }
    if(!clock_can_set()) { snprintf(err, n, "sem permissão"); return false; }
    struct timeval tv = { .tv_sec = v, .tv_usec = 0 };
    if(settimeofday(&tv, NULL)) { snprintf(err, n, "%s", strerror(errno)); return false; }
    set_source(CLK_MANUAL, NULL);
    fprintf(stderr, "relogio: acertado a mao para %ld\n", (long)v);
    return true;
}

/* ------------------------------------------------------------------ NTP */

/* roda "ntpd -n -q -p servidor" com limite de tempo; true se acertou */
static bool ntp_one(const char *server, char *err, size_t n)
{
    pid_t pid = fork();
    if(pid < 0) { snprintf(err, n, "fork: %s", strerror(errno)); return false; }
    if(pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if(nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl(NTPD, "ntpd", "-n", "-q", "-p", server, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    for(int i = 0; i < NTP_TIMEOUT * 10; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if(r == pid) {
            if(WIFEXITED(st) && WEXITSTATUS(st) == 0) return true;
            snprintf(err, n, WIFEXITED(st) && WEXITSTATUS(st) == 127 ? "ntpd ausente" : "%s: sem resposta", server);
            return false;
        }
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    snprintf(err, n, "%s: sem resposta", server);
    return false;
}

static char servers[sizeof settings()->ntp];   /* cópia feita pela thread do LVGL */

static void *sync_thread(void *arg)
{
    (void)arg;
    char err[96] = "nenhum servidor";
    bool ok = false;
    char *save = NULL;
    for(char *s = strtok_r(servers, " \t,", &save); s && !ok; s = strtok_r(NULL, " \t,", &save)) {
        ok = ntp_one(s, err, sizeof err);
        if(ok) set_source(CLK_NTP, s);
    }
    fprintf(stderr, "relogio: NTP %s%s\n", ok ? "ok com " : "falhou: ", ok ? src_detail : err);
    pthread_mutex_lock(&lock);
    sync_res = ok ? 1 : -1;
    sync_when = time(NULL);
    snprintf(sync_err, sizeof sync_err, "%s", ok ? "" : err);
    next_sync = mono_s() + (ok ? SYNC_OK_S : SYNC_FAIL_S);
    busy = false;
    pthread_mutex_unlock(&lock);
    return NULL;
}

void clock_sync_now(void)
{
    pthread_mutex_lock(&lock);
    if(busy) { pthread_mutex_unlock(&lock); return; }
    if(!clock_can_set()) {
        sync_res = -1;
        sync_when = time(NULL);
        snprintf(sync_err, sizeof sync_err, "simulador: o relógio do PC não é alterado");
        next_sync = mono_s() + SYNC_OK_S;
        pthread_mutex_unlock(&lock);
        return;
    }
    busy = true;
    snprintf(servers, sizeof servers, "%s", settings()->ntp);
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t, &at, sync_thread, NULL)) {
        pthread_mutex_lock(&lock);
        busy = false;
        pthread_mutex_unlock(&lock);
    }
    pthread_attr_destroy(&at);
}

bool clock_sync_busy(void)
{
    pthread_mutex_lock(&lock);
    bool b = busy;
    pthread_mutex_unlock(&lock);
    return b;
}

int clock_sync_result(time_t *when, char *err, size_t n)
{
    pthread_mutex_lock(&lock);
    int r = sync_res;
    if(when) *when = sync_when;
    if(err) snprintf(err, n, "%s", sync_err);
    pthread_mutex_unlock(&lock);
    return r;
}

void clock_tick(void)
{
    if(!settings()->auto_time || !clock_can_set()) return;
    pthread_mutex_lock(&lock);
    bool due = !busy && mono_s() >= next_sync;
    pthread_mutex_unlock(&lock);
    if(due) clock_sync_now();
}
