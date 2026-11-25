/* webproc.c — ver webproc.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "webproc.h"
#include "runner.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_ 8192
#define MEM_LIMIT (88L * 1024 * 1024)   /* espaço de endereços do motor (bibliotecas incluídas) */

static pid_t pid;
static int to_fd = -1, from_fd = -1, shm_fd = -1;
static void *shm;
static size_t shm_size;
static lv_image_dsc_t dsc;
static lv_timer_t *poll_t;
static webproc_event_cb cb;
static void *cb_ud;
static char inbuf[LINE_MAX_];
static size_t inlen;

const char *webproc_dir(void)
{
    const char *e = getenv("BIGFOSTON_WEB_DIR");
    return e && *e ? e : "/mnt/sd/BigFoston/web";
}

bool webproc_available(void)
{
    char p[600];
    snprintf(p, sizeof p, "%s/bfweb", webproc_dir());
    return access(p, X_OK) == 0;
}

bool webproc_running(void) { return pid > 0; }
int webproc_pid(void) { return pid; }
const lv_image_dsc_t *webproc_image(void) { return shm ? &dsc : NULL; }

long webproc_rss_kb(void)
{
    if(pid <= 0) return 0;
    char p[64], line[128];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    FILE *f = fopen(p, "r");
    long kb = 0;
    while(f && fgets(line, sizeof line, f)) if(!strncmp(line, "VmRSS:", 6)) kb = strtol(line + 6, NULL, 10);
    if(f) fclose(f);
    return kb;
}

static void cleanup(void)
{
    if(poll_t) { lv_timer_delete(poll_t); poll_t = NULL; }
    if(to_fd >= 0) close(to_fd);
    if(from_fd >= 0) close(from_fd);
    to_fd = from_fd = -1;
    if(shm) {
        lv_image_cache_drop(&dsc);
        munmap(shm, shm_size);
        shm = NULL;
    }
    if(shm_fd >= 0) close(shm_fd);
    shm_fd = -1;
    inlen = 0;
}

static void reaped(int status)
{
    pid = 0;
    cleanup();
    char m[64];
    if(WIFSIGNALED(status)) snprintf(m, sizeof m, "EXIT sinal %d", WTERMSIG(status));
    else snprintf(m, sizeof m, "EXIT %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if(cb) cb(m, cb_ud);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    /* lê tudo o que chegou; cada linha vai ao callback */
    for(int rounds = 0; from_fd >= 0 && rounds < 16; rounds++) {
        ssize_t n = read(from_fd, inbuf + inlen, sizeof inbuf - 1 - inlen);
        if(n <= 0) {
            if(n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            break;                                    /* EOF: o processo saiu (o waitpid confirma) */
        }
        inlen += (size_t)n;
        char *s = inbuf, *nl;
        while((nl = memchr(s, '\n', inbuf + inlen - s))) {
            *nl = 0;
            if(cb) cb(s, cb_ud);
            if(from_fd < 0) return;                   /* o callback parou o motor */
            s = nl + 1;
        }
        inlen -= (size_t)(s - inbuf);
        memmove(inbuf, s, inlen);
        if(inlen == sizeof inbuf - 1) inlen = 0;     /* linha absurda: descarta */
    }
    if(pid > 0) {
        int st;
        if(waitpid(pid, &st, WNOHANG) == pid) reaped(st);
    }
}

bool webproc_start(const webproc_opts_t *o, webproc_event_cb ecb, void *ud, char *err, size_t errn)
{
    if(pid > 0) webproc_stop();
    char bin[600], res[600];
    snprintf(bin, sizeof bin, "%s/bfweb", webproc_dir());
    snprintf(res, sizeof res, "%s/res", webproc_dir());
    if(access(bin, X_OK)) { snprintf(err, errn, "%s: %s", bin, strerror(errno)); return false; }

    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    char home[256] = "", name[64] = "";
    if(runner_resolve_user(&uid, &gid, home, sizeof home, name, sizeof name, err, errn) < 0) return false;

    shm_size = (size_t)o->width * o->height * 2;
    shm_fd = memfd_create("bfweb-page", 0);          /* herdado pelo filho como fd 3 */
    if(shm_fd < 0 || ftruncate(shm_fd, (off_t)shm_size)) { snprintf(err, errn, "memfd: %s", strerror(errno)); cleanup(); return false; }
    shm = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(shm == MAP_FAILED) { shm = NULL; snprintf(err, errn, "mmap: %s", strerror(errno)); cleanup(); return false; }
    memset(shm, 0xff, shm_size);                      /* branco até a primeira página */
    memset(&dsc, 0, sizeof dsc);
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = (uint32_t)o->width;
    dsc.header.h = (uint32_t)o->height;
    dsc.header.stride = (uint32_t)o->width * 2;
    dsc.data = shm;
    dsc.data_size = (uint32_t)shm_size;

    int in[2], out[2];
    if(pipe2(in, O_CLOEXEC)) { snprintf(err, errn, "pipe: %s", strerror(errno)); cleanup(); return false; }
    if(pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); snprintf(err, errn, "pipe: %s", strerror(errno)); cleanup(); return false; }

    char w[16], h[16], data[64];
    snprintf(w, sizeof w, "%d", o->width);
    snprintf(h, sizeof h, "%d", o->height);
    /* dados do motor (cookies da sessão) na RAM: o usuário sem privilégios não grava no
     * cartão, e nada da navegação fica no SD sem o shell decidir */
    snprintf(data, sizeof data, "/tmp/bfweb-%d", (int)(uid == (uid_t)-1 ? getuid() : uid));
    mkdir(data, 0700);
    if(uid != (uid_t)-1 && chown(data, uid, gid)) { /* sem chown: o motor usa o que der */ }

    pid_t p = fork();
    if(p < 0) {
        snprintf(err, errn, "fork: %s", strerror(errno));
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        cleanup();
        return false;
    }
    if(p == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int log = open("/tmp/bfweb.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if(log >= 0) {
            if(uid != (uid_t)-1 && fchown(log, uid, gid)) { /* o log é só ajuda */ }
            dup2(log, 2);
        }
        dup2(shm_fd, 3);                              /* sem O_CLOEXEC: chega ao bfweb */
        for(int fd = 4; fd < 256; fd++) close(fd);
        setpriority(PRIO_PROCESS, 0, 10);             /* GPS e shell primeiro */
        struct rlimit rl = { MEM_LIMIT, MEM_LIMIT };
        setrlimit(RLIMIT_AS, &rl);
        struct rlimit core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &core);
        if(uid != (uid_t)-1) runner_child_setup(uid, gid, home, name);
        char *av[20];
        int n = 0;
        av[n++] = bin;
        av[n++] = "--fd"; av[n++] = "3";
        av[n++] = "-w"; av[n++] = w;
        av[n++] = "-h"; av[n++] = h;
        av[n++] = "--data"; av[n++] = data;
        av[n++] = "--res"; av[n++] = res;
        if(o->javascript) av[n++] = "--js";
        if(!o->images) av[n++] = "--no-images";
        av[n++] = (char *)(o->url && *o->url ? o->url : "about:blank");
        av[n] = NULL;
        execv(bin, av);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    to_fd = in[1];
    from_fd = out[0];
    fcntl(from_fd, F_SETFL, fcntl(from_fd, F_GETFL) | O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);                         /* motor caiu: write devolve EPIPE */
    pid = p;
    cb = ecb;
    cb_ud = ud;
    inlen = 0;
    poll_t = lv_timer_create(poll_cb, 15, NULL);
    return true;
}

void webproc_send(const char *fmt, ...)
{
    if(to_fd < 0) return;
    char line[LINE_MAX_];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof line - 2) n = sizeof line - 2;
    for(int i = 0; i < n; i++) if(line[i] == '\n' || line[i] == '\r') line[i] = ' ';   /* um comando por linha */
    line[n++] = '\n';
    for(int off = 0; off < n; ) {
        ssize_t w = write(to_fd, line + off, (size_t)(n - off));
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return;
        off += (int)w;
    }
}

void webproc_stop(void)
{
    if(pid <= 0) { cleanup(); return; }
    webproc_send("QUIT");
    /* até ~1,5 s para salvar e sair; depois à força */
    int st;
    for(int i = 0; i < 30; i++) {
        if(waitpid(pid, &st, WNOHANG) == pid) { pid = 0; break; }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if(pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        pid = 0;
    }
    cb = NULL;
    cleanup();
}
