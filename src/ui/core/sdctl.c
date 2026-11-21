/* sdctl.c — ver sdctl.h. */
#define _DEFAULT_SOURCE
#include "sdctl.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SD_TOOL "/usr/sbin/bigfoston-sd"
/* os mesmos caminhos do bigfoston-sd */
#define SD_MOUNT "/mnt/sd"
#define SD_DEV "/dev/mmcblk0p1"
#define SD_EJECTED_FLAG "/run/bigfoston-sd-ejected"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running, finished;
static int result;
static char output[256];
static bool do_eject;

bool sdctl_available(void) { return access(SD_TOOL, X_OK) == 0; }

/* executa o programa com um argumento fixo; devolve o código e a última linha */
static int run(const char *arg, char *out, size_t n)
{
    int fd[2];
    out[0] = 0;
    if(pipe(fd)) return -1;
    pid_t pid = fork();
    if(pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if(pid == 0) {
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        execl(SD_TOOL, "bigfoston-sd", arg, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    char buf[512], line[256] = "";
    size_t ll = 0;
    ssize_t r;
    while((r = read(fd[0], buf, sizeof buf)) > 0) {
        for(ssize_t i = 0; i < r; i++) {
            if(buf[i] == '\n') {
                if(ll) { line[ll] = 0; snprintf(out, n, "%s", line); }
                ll = 0;
            }
            else if(ll < sizeof line - 1) line[ll++] = buf[i];
        }
    }
    if(ll) { line[ll] = 0; snprintf(out, n, "%s", line); }
    close(fd[0]);
    int st = 0;
    while(waitpid(pid, &st, 0) < 0) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

sd_state_t sdctl_status(void)
{
    if(!sdctl_available()) return SD_UNKNOWN;
    FILE *f = fopen("/proc/mounts", "r");
    if(f) {
        char line[512], dev[128], dir[128], type[32], opts[256];
        while(fgets(line, sizeof line, f)) {
            if(sscanf(line, "%127s %127s %31s %255s", dev, dir, type, opts) == 4 && !strcmp(dir, SD_MOUNT)) {
                fclose(f);
                return !strncmp(opts, "rw", 2) && (opts[2] == ',' || !opts[2]) ? SD_RW : SD_RO;
            }
        }
        fclose(f);
    }
    if(access(SD_DEV, F_OK)) return SD_ABSENT;
    return access(SD_EJECTED_FLAG, F_OK) ? SD_MOUNTING : SD_EJECTED;
}

static void *worker(void *arg)
{
    (void)arg;
    char out[256];
    int rc = run(do_eject ? "eject" : "mount", out, sizeof out);
    pthread_mutex_lock(&lock);
    result = rc;
    snprintf(output, sizeof output, "%s", out);
    running = false;
    finished = true;
    pthread_mutex_unlock(&lock);
    return NULL;
}

bool sdctl_start(bool eject)
{
    pthread_mutex_lock(&lock);
    if(running || !sdctl_available()) { pthread_mutex_unlock(&lock); return false; }
    running = true;
    finished = false;
    do_eject = eject;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &at, worker, NULL) == 0;
    pthread_attr_destroy(&at);
    if(!ok) {
        pthread_mutex_lock(&lock);
        running = false;
        pthread_mutex_unlock(&lock);
    }
    return ok;
}

bool sdctl_done(int *rc, char *msg, size_t n)
{
    pthread_mutex_lock(&lock);
    bool f = finished;
    if(f) {
        finished = false;
        if(rc) *rc = result;
        if(msg) snprintf(msg, n, "%s", output);
    }
    pthread_mutex_unlock(&lock);
    return f;
}

bool sdctl_busy(void)
{
    pthread_mutex_lock(&lock);
    bool b = running;
    pthread_mutex_unlock(&lock);
    return b;
}
