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
static unsigned want;                   /* grupos pedidos nesta entrada */
#define N_SVC (int)(sizeof svc / sizeof svc[0])

static _Atomic int state = PM_OFF, done, total;
static atomic_bool leave_after;
static bool stopped[N_SVC];             /* o que este modo parou (e vai religar) */

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

static void *leave_worker(void *arg);

/* o que está parado fica anotado na RAM (/run some ao desligar): se o shell reiniciar no
 * meio do modo (travou, foi atualizado), o próximo religa tudo (perfmode_recover) */
#define NOTE "/run/bigfoston-perfmode"

static void note_write(void)
{
    FILE *f = fopen(NOTE ".new", "w");
    if(!f) return;
    for(int i = 0; i < N_SVC; i++) if(stopped[i]) fprintf(f, "%s\n", svc[i].script);
    if(fclose(f) == 0) rename(NOTE ".new", NOTE);
}

static void *enter_worker(void *arg)
{
    (void)arg;
    int n = 0;
    for(int i = 0; i < N_SVC; i++) {
        stopped[i] = (want & svc[i].group) && access(svc[i].script, X_OK) == 0 && running(svc[i].proc);
        if(stopped[i]) n++;
    }
    total = n;
    done = 0;
    for(int i = 0; i < N_SVC; i++) {
        if(!stopped[i]) continue;
        run(svc[i].script, "stop");
        note_write();
        done++;
    }
    state = PM_ON;
    if(leave_after) { leave_after = false; state = PM_LEAVING; leave_worker(NULL); }
    return NULL;
}

static void *leave_worker(void *arg)
{
    (void)arg;
    int n = 0;
    for(int i = 0; i < N_SVC; i++) if(stopped[i]) n++;
    total = n;
    done = 0;
    for(int i = N_SVC - 1; i >= 0; i--) {
        if(!stopped[i]) continue;
        run(svc[i].script, "start");
        stopped[i] = false;
        done++;
    }
    unlink(NOTE);
    state = PM_OFF;
    return NULL;
}

static void spawn(void *(*fn)(void *))
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t, &a, fn, NULL)) state = fn == enter_worker ? PM_OFF : PM_ON;
    pthread_attr_destroy(&a);
}

void perfmode_enter(unsigned groups, bool prio)
{
    if(state != PM_OFF) { leave_after = false; return; }
    state = PM_ENTERING;
    total = done = 0;
    want = groups;
    if(prio) setpriority(PRIO_PROCESS, 0, -5);   /* a thread da interface (a principal) primeiro */
    spawn(enter_worker);
}

void perfmode_leave(void)
{
    /* chamada pela thread da interface: no Linux a prioridade é por thread */
    if(state != PM_OFF) setpriority(PRIO_PROCESS, 0, 0);
    if(state == PM_ENTERING) { leave_after = true; return; }
    if(state != PM_ON) return;
    state = PM_LEAVING;
    total = done = 0;
    spawn(leave_worker);
}

bool perfmode_recover(void)
{
    FILE *f = fopen(NOTE, "r");
    if(!f) return false;
    char line[128];
    int n = 0;
    while(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        for(int i = 0; i < N_SVC; i++)
            if(!strcmp(line, svc[i].script)) { stopped[i] = true; n++; }
    }
    fclose(f);
    if(!n || state != PM_OFF) { if(!n) unlink(NOTE); return false; }
    state = PM_LEAVING;
    total = n;
    done = 0;
    spawn(leave_worker);
    return true;
}

pm_state_t perfmode_state(void) { return (pm_state_t)state; }

void perfmode_progress(int *d, int *t)
{
    *d = done;
    *t = total;
}
