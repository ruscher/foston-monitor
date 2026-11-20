#define _DEFAULT_SOURCE
#include "runner.h"
#include "lvgl.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t job_pid;
static int job_fd = -1;
static lv_timer_t *job_timer;
static runner_out_cb job_out;
static runner_exit_cb job_done;
static void *job_ud;

bool runner_busy(void) { return job_pid > 0; }

/* Resolve o usuário sem privilégios quando o shell é root. */
int runner_resolve_user(uid_t *uid, gid_t *gid, char *home, size_t nh, char *name, size_t nn,
                        char *msg, size_t msgn)
{
    *uid = (uid_t)-1;
    if(geteuid() != 0) return 0;
    const char *want = getenv("BIGFOSTON_RUN_USER");
    if(!want || !*want) want = "bigfoston";
    struct passwd *pw = getpwnam(want);
    if(pw && pw->pw_uid != 0) {
        *uid = pw->pw_uid;
        *gid = pw->pw_gid;
        snprintf(home, nh, "%s", pw->pw_dir ? pw->pw_dir : "/");
        snprintf(name, nn, "%s", pw->pw_name);
        return 0;
    }
    const char *allow = getenv("BIGFOSTON_ALLOW_ROOT");
    if(allow && !strcmp(allow, "1")) return 0;
    snprintf(msg, msgn, "root: usuário '%s' inexistente; defina BIGFOSTON_RUN_USER "
             "ou BIGFOSTON_ALLOW_ROOT=1", want);
    return -EPERM;
}

void runner_child_setup(uid_t uid, gid_t gid, const char *home, const char *name)
{
    if(uid != (uid_t)-1) {
        if(setgroups(1, &gid) || setgid(gid) || setuid(uid) || setuid(0) == 0) {
            static const char m[] = "bigfoston: falha ao largar privilégios de root\n";
            if(write(2, m, sizeof m - 1) < 0) { /* nada a fazer */ }
            _exit(126);
        }
        setenv("HOME", home, 1);
        setenv("USER", name, 1);
        setenv("LOGNAME", name, 1);
        if(chdir(home) != 0 && chdir("/") != 0) { /* ignora */ }
    }
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static void job_finish(int status)
{
    int code = -1, sig = 0;
    if(WIFEXITED(status)) code = WEXITSTATUS(status);
    else if(WIFSIGNALED(status)) sig = WTERMSIG(status);
    if(job_fd >= 0) { close(job_fd); job_fd = -1; }
    if(job_timer) { lv_timer_delete(job_timer); job_timer = NULL; }
    job_pid = 0;
    runner_exit_cb done = job_done;
    void *ud = job_ud;
    job_out = NULL;
    job_done = NULL;
    if(done) done(code, sig, ud);
}

static void drain(void)
{
    char buf[1024];
    for(;;) {
        ssize_t r = read(job_fd, buf, sizeof buf);
        if(r > 0) { if(job_out) job_out(buf, (int)r, job_ud); continue; }
        break;
    }
}

static void job_poll(lv_timer_t *t)
{
    (void)t;
    if(job_pid <= 0) return;
    if(job_fd >= 0) drain();
    int st;
    pid_t r = waitpid(job_pid, &st, WNOHANG);
    if(r == job_pid) {
        if(job_fd >= 0) drain();
        job_finish(st);
    }
    else if(r < 0) job_finish(0);
}

int runner_exec(char *const argv[], bool capture, runner_out_cb out, runner_exit_cb done,
                void *ud, char *msg, size_t msgn)
{
    msg[0] = 0;
    if(!argv || !argv[0]) { snprintf(msg, msgn, "comando vazio"); return -EINVAL; }
    if(capture && runner_busy()) { snprintf(msg, msgn, "outro comando ainda está rodando"); return -EBUSY; }

    uid_t uid; gid_t gid = 0; char home[256] = "/", name[64] = "";
    int rc = runner_resolve_user(&uid, &gid, home, sizeof home, name, sizeof name, msg, msgn);
    if(rc) return rc;

    /* pipe "errpipe" com CLOEXEC: fecha sozinho se o exec der certo */
    int ep[2];
    if(pipe(ep)) { snprintf(msg, msgn, "pipe: %s", strerror(errno)); return -errno; }
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);

    int op[2] = { -1, -1 };
    if(capture && pipe(op)) {
        close(ep[0]); close(ep[1]);
        snprintf(msg, msgn, "pipe: %s", strerror(errno));
        return -errno;
    }

    pid_t pid = fork();
    if(pid < 0) {
        int e = errno;
        close(ep[0]); close(ep[1]);
        if(capture) { close(op[0]); close(op[1]); }
        snprintf(msg, msgn, "fork: %s", strerror(e));
        return -e;
    }
    if(pid == 0) {
        close(ep[0]);
        int devnull = open("/dev/null", O_RDWR);
        if(devnull >= 0) dup2(devnull, 0);
        if(capture) {
            close(op[0]);
            dup2(op[1], 1);
            dup2(op[1], 2);
            close(op[1]);
        }
        else {
            /* aplicativo independente: nova sessão e neto órfão (sem zumbi) */
            if(devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
            setsid();
            pid_t g = fork();
            if(g < 0) { int e = errno; if(write(ep[1], &e, sizeof e) < 0) {} _exit(127); }
            if(g > 0) _exit(0);
        }
        if(devnull > 2) close(devnull);
        runner_child_setup(uid, gid, home, name);
        execvp(argv[0], argv);
        int e = errno;
        if(write(ep[1], &e, sizeof e) < 0) { /* ignora */ }
        _exit(127);
    }

    close(ep[1]);
    int child_errno = 0;
    ssize_t n;
    do { n = read(ep[0], &child_errno, sizeof child_errno); } while(n < 0 && errno == EINTR);
    close(ep[0]);

    if(!capture) {
        int st;
        waitpid(pid, &st, 0); /* o filho intermediário sai na hora */
        if(n == sizeof child_errno) {
            snprintf(msg, msgn, "%s: %s", argv[0], strerror(child_errno));
            return -child_errno;
        }
        return pid;
    }

    close(op[1]);
    if(n == sizeof child_errno) {
        int st;
        close(op[0]);
        waitpid(pid, &st, 0);
        snprintf(msg, msgn, "%s: %s", argv[0], strerror(child_errno));
        return -child_errno;
    }
    fcntl(op[0], F_SETFL, fcntl(op[0], F_GETFL) | O_NONBLOCK);
    fcntl(op[0], F_SETFD, FD_CLOEXEC);
    job_pid = pid;
    job_fd = op[0];
    job_out = out;
    job_done = done;
    job_ud = ud;
    job_timer = lv_timer_create(job_poll, 100, NULL);
    return pid;
}

int runner_shell(const char *cmd, runner_out_cb out, runner_exit_cb done, void *ud,
                 char *msg, size_t msgn)
{
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
    return runner_exec(argv, true, out, done, ud, msg, msgn);
}

void runner_stop(void)
{
    if(job_pid > 0) kill(job_pid, SIGTERM);
}

void runner_forget(void)
{
    job_out = NULL;
    job_done = NULL;
    if(job_pid > 0) {
        kill(job_pid, SIGTERM);
        /* o timer continua até colher o processo, mas sem callbacks */
    }
}

int runner_split(char *buf, char **argv, int max)
{
    int argc = 0;
    char *r = buf, *w = buf;
    while(*r && argc < max - 1) {
        while(*r == ' ' || *r == '\t') r++;
        if(!*r) break;
        argv[argc++] = w;
        char q = 0;
        while(*r) {
            if(q) {
                if(*r == q) { q = 0; r++; continue; }
                if(q == '"' && *r == '\\' && r[1]) r++;
                *w++ = *r++;
            }
            else if(*r == '"' || *r == '\'') q = *r++;
            else if(*r == '\\' && r[1]) { r++; *w++ = *r++; }
            else if(*r == ' ' || *r == '\t') { r++; break; }
            else *w++ = *r++;
        }
        *w++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}
