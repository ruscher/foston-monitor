/* perfmode.c — ver perfmode.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "perfmode.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
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
static bool stopped[N_SVC];             /* o que este modo parou (e vai religar); só a thread */

static bool running(const char *name)
{
    DIR *d = opendir("/proc");
    if(!d) return false;
    struct dirent *e;
    bool found = false;
    while(!found && (e = readdir(d))) {
        if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[300], comm[32];
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if(!f) continue;
        if(fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            found = !strcmp(comm, name);
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static void run(const char *script, const char *arg)
{
    pid_t p = fork();
    if(p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        setpriority(PRIO_PROCESS, 0, 0);
        execl(script, script, arg, (char *)NULL);
        _exit(127);
    }
    if(p > 0) waitpid(p, NULL, 0);
}

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

static void do_enter(unsigned g)
{
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
        note_write();
        run(svc[i].script, "stop");
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
        if(!running(svc[i].proc)) run(svc[i].script, "start");
        stopped[i] = false;
        done++;
    }
    unlink(NOTE);
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

void perfmode_progress(int *d, int *t)
{
    *d = done;
    *t = total;
}
