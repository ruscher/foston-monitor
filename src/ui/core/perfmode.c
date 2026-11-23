/* perfmode.c — ver perfmode.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "perfmode.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* na ordem de parar; volta na inversa (o Bluetooth sobe antes do bluetoothd e do resto) */
static const struct { const char *script; const char *proc; unsigned group; } svc[] = {
    { "/etc/init.d/S60gps",        "bfgps",       PM_SVC_GPS },
    { "/etc/init.d/S50crond",      "crond",       PM_SVC_CRON },
    { "/etc/init.d/S43bfphone",    "bfphone",     PM_SVC_PHONE },
    { "/etc/init.d/S42bfbt-agent", "bfbt-agent",  PM_SVC_BT },
    { "/etc/init.d/S41bluealsa",   "bluealsa",    PM_SVC_BT },
    { "/etc/init.d/S40bluetoothd", "bluetoothd",  PM_SVC_BT },
    { "/etc/init.d/S35bluetooth",  "bfbt-attach", PM_SVC_BT },
};
#define N_SVC (int)(sizeof svc / sizeof svc[0])

/* A interface só diz o que quer (desired); uma thread por vez persegue esse alvo: se o
 * usuário mudar de ideia no meio de uma troca, ela emenda a troca contrária ao terminar.
 * mu protege state/desired/busy/groups; done/total são só para a barra. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pm_state_t state = PM_OFF;
static bool desired, busy, prio_set;
static unsigned groups;                 /* grupos pedidos na entrada */
static _Atomic int done, total;
static _Atomic unsigned paused;          /* grupos parados agora (perfmode_paused) */
static bool stopped[N_SVC];             /* o que este modo parou (e vai religar); só a thread */

/* pid do processo com esse nome (comm), 0 se não há */
static pid_t find_proc(const char *name)
{
    DIR *d = opendir("/proc");
    if(!d) return false;
    struct dirent *e;
    pid_t found = 0;
    while(!found && (e = readdir(d))) {
        if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[300], comm[32];
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if(!f) continue;
        if(fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            if(!strcmp(comm, name)) found = (pid_t)atoi(e->d_name);
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static bool running(const char *name) { return find_proc(name) != 0; }

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* roda o script com prazo: um serviço que não responde (o bluetoothd parado logo depois de
 * subir, com o chip ainda carregando, girava para sempre ignorando o SIGTERM e o "stop"
 * esperava junto, com a janela de progresso segurando os toques). Estourou: mata o script
 * (o grupo dele) e devolve false. */
static bool run(const char *script, const char *arg, unsigned limit_ms)
{
    pid_t p = fork();
    if(p == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        setpriority(PRIO_PROCESS, 0, 0);
        execl(script, script, arg, (char *)NULL);
        _exit(127);
    }
    if(p < 0) return false;
    setpgid(p, p);
    uint64_t end = now_ms() + limit_ms;
    for(;;) {
        pid_t r = waitpid(p, NULL, WNOHANG);
        if(r == p || (r < 0 && errno != EINTR)) return true;
        if(now_ms() >= end) break;
        usleep(50000);
    }
    kill(-p, SIGKILL);
    waitpid(p, NULL, 0);
    return false;
}

#define STOP_MS   8000u                 /* o mais lento medido: ~1 s */
#define START_MS 20000u                 /* o Bluetooth carrega o firmware do chip: ~4 s */
#define SETTLE_MS 4000u                 /* não para o que acabou de subir antes disso */
static uint64_t started_at;             /* quando a última volta terminou */

/* o que está parado fica anotado na RAM (/run some ao desligar): se o shell reiniciar no
 * meio do modo (travou, foi atualizado), o próximo religa tudo (perfmode_recover). Anota
 * ANTES de parar: na volta só sobe o que não está rodando, então anotar a mais não faz mal */
#define NOTE "/run/bigfoston-perfmode"

static void note_write(void)
{
    FILE *f = fopen(NOTE ".new", "w");
    if(!f) return;
    for(int i = 0; i < N_SVC; i++) if(stopped[i]) fprintf(f, "%s\n", svc[i].script);
    if(fclose(f) == 0) rename(NOTE ".new", NOTE);
}

static unsigned stopped_groups(void)
{
    unsigned g = 0;
    for(int i = 0; i < N_SVC; i++) if(stopped[i]) g |= svc[i].group;
    return g;
}

static bool still_wanted(void)
{
    pthread_mutex_lock(&mu);
    bool d = desired;
    pthread_mutex_unlock(&mu);
    return d;
}

static void do_enter(unsigned g)
{
    /* religou há pouco (o vídeo voltou logo depois de parar): deixa os serviços terminarem
     * de subir antes de parar de novo; desistiu no meio, nem para */
    while(now_ms() - started_at < SETTLE_MS) {
        if(!still_wanted()) return;
        usleep(100000);
    }
    bool pick[N_SVC];
    int n = 0;
    for(int i = 0; i < N_SVC; i++) {
        pick[i] = !stopped[i] && (g & svc[i].group) && access(svc[i].script, X_OK) == 0 && running(svc[i].proc);
        if(pick[i]) n++;
    }
    done = 0;
    total = n;
    for(int i = 0; i < N_SVC; i++) {
        if(!pick[i]) continue;
        stopped[i] = true;
        paused = stopped_groups();
        note_write();
        /* não parou no prazo (ou o script saiu e o processo ficou): à força */
        if(!run(svc[i].script, "stop", STOP_MS) || running(svc[i].proc)) {
            pid_t pid;
            for(int k = 0; k < 20 && (pid = find_proc(svc[i].proc)); k++) {
                kill(pid, k < 10 ? SIGTERM : SIGKILL);
                usleep(100000);
            }
        }
        done++;
    }
}

static void do_leave(void)
{
    int n = 0;
    for(int i = 0; i < N_SVC; i++) if(stopped[i]) n++;
    done = 0;
    total = n;
    for(int i = N_SVC - 1; i >= 0; i--) {
        if(!stopped[i]) continue;
        if(!running(svc[i].proc)) run(svc[i].script, "start", START_MS);
        stopped[i] = false;
        paused = stopped_groups();
        done++;
    }
    unlink(NOTE);
    started_at = now_ms();
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    for(;;) {
        pm_state_t s = state;                    /* PM_ENTERING ou PM_LEAVING */
        unsigned g = groups;
        pthread_mutex_unlock(&mu);
        if(s == PM_ENTERING) do_enter(g);
        else do_leave();
        pthread_mutex_lock(&mu);
        if(s == PM_ENTERING) state = desired ? PM_ON : PM_LEAVING;
        else state = desired ? PM_ENTERING : PM_OFF;
        if(state == PM_ON || state == PM_OFF) break;
    }
    busy = false;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* com mu: começa a troca se o estado parado não é o desejado */
static void kick(void)
{
    if(busy) return;                             /* a thread em curso emenda sozinha */
    pm_state_t from = state;
    if(desired && from == PM_OFF) state = PM_ENTERING;
    else if(!desired && from == PM_ON) state = PM_LEAVING;
    else return;
    busy = true;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t, &a, worker, NULL)) { state = from; busy = false; }
    pthread_attr_destroy(&a);
}

void perfmode_enter(unsigned g, bool prio)
{
    /* a thread que chama (a da interface): no Linux a prioridade é por thread */
    if(prio && !prio_set && setpriority(PRIO_PROCESS, 0, -5) == 0) prio_set = true;
    pthread_mutex_lock(&mu);
    desired = true;
    groups = g;
    kick();
    pthread_mutex_unlock(&mu);
}

void perfmode_leave(void)
{
    if(prio_set) { setpriority(PRIO_PROCESS, 0, 0); prio_set = false; }
    pthread_mutex_lock(&mu);
    desired = false;
    kick();
    pthread_mutex_unlock(&mu);
}

bool perfmode_recover(void)
{
    FILE *f = fopen(NOTE, "r");
    if(!f) return false;
    char line[128];
    int n = 0;
    pthread_mutex_lock(&mu);
    while(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        for(int i = 0; i < N_SVC; i++)
            if(!strcmp(line, svc[i].script) && !stopped[i]) { stopped[i] = true; n++; }
    }
    fclose(f);
    paused = stopped_groups();
    bool go = n && state == PM_OFF && !busy;
    if(go) { state = PM_ON; desired = false; kick(); go = state == PM_LEAVING; }
    pthread_mutex_unlock(&mu);
    if(!n) unlink(NOTE);
    return go;
}

pm_state_t perfmode_state(void)
{
    pthread_mutex_lock(&mu);
    pm_state_t s = state;
    pthread_mutex_unlock(&mu);
    return s;
}

unsigned perfmode_paused(void) { return paused; }

void perfmode_progress(int *d, int *t)
{
    *d = done;
    *t = total;
}
