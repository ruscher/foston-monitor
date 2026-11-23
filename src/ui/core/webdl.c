/* webdl.c — ver webdl.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "webdl.h"
#include "webproc.h"
#include "settings.h"
#include "osd.h"
#include "imgload.h"
#include "book.h"
#include "i18n.h"
#include "../screens/screens.h"
#include "../widgets/imgview.h"
#include "../assets/icons/bf_icons.h"
#include "lvgl.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_DL     40
#define MAX_RUN    2

typedef struct {
    webdl_t d;
    pid_t pid;
    int fd;
    char buf[512];
    size_t len;
    bool pausing, cancelling;
} job_t;

static job_t jobs[MAX_DL];
static int n_jobs, next_id = 1;
static bool inited;
static lv_timer_t *tick;
static webdl_listener listener;
static void *listener_ud;

const char *webdl_dir(void)
{
    static char d[512];
    const char *e = getenv("BIGFOSTON_DL_DIR");
    if(e && *e) snprintf(d, sizeof d, "%s", e);
    else if(access("/mnt/sd/BigFoston", W_OK) == 0) snprintf(d, sizeof d, "/mnt/sd/BigFoston/Downloads");
    else data_path(d, sizeof d, "Downloads");
    return d;
}

static void notify(void) { if(listener) listener(listener_ud); }

void webdl_set_listener(webdl_listener cb, void *ud) { listener = cb; listener_ud = ud; }

/* ------------------------------------------------------------ lista no disco */

static void list_path(char *out, size_t n)
{
    char d[512];
    data_path(d, sizeof d, "web");
    mkdir(d, 0755);
    snprintf(out, n, "%s/downloads.tsv", d);
}

static const char *state_name(webdl_state_t s)
{
    static const char *const n[] = { "fila", "pausado", "pausado", "pronto", "falhou" };
    return n[s];       /* baixando grava como pausado: ao reiniciar, continua pelo .part */
}

static void save_list(void)
{
    char p[600], tmp[620];
    list_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    /* cookie não vai para o cartão */
    for(int i = 0; i < n_jobs; i++) {
        const webdl_t *d = &jobs[i].d;
        fprintf(f, "%s\t%lld\t%s\t%s\t%s\t%s\n", state_name(d->state), d->total, d->name, d->path, d->url, d->referer);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if(fclose(f) || !ok || rename(tmp, p)) unlink(tmp);
}

static void load_list(void)
{
    char p[600], line[3200];
    list_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if(!f) return;
    while(n_jobs < MAX_DL && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fl[6];
        int nf = 0;
        for(char *s = line; nf < 6; ) {
            fl[nf++] = s;
            char *t = strchr(s, '\t');
            if(!t) break;
            *t = 0;
            s = t + 1;
        }
        if(nf < 6 || strncasecmp(fl[4], "http", 4)) continue;
        job_t *j = &jobs[n_jobs++];
        memset(j, 0, sizeof *j);
        j->fd = -1;
        j->d.id = next_id++;
        j->d.total = strtoll(fl[1], NULL, 10);
        snprintf(j->d.name, sizeof j->d.name, "%s", fl[2]);
        snprintf(j->d.path, sizeof j->d.path, "%s", fl[3]);
        snprintf(j->d.url, sizeof j->d.url, "%s", fl[4]);
        snprintf(j->d.referer, sizeof j->d.referer, "%s", fl[5]);
        j->d.state = !strcmp(fl[0], "pronto") ? WEBDL_DONE : !strcmp(fl[0], "falhou") ? WEBDL_FAILED : WEBDL_PAUSED;
        struct stat st;
        char part[600];
        snprintf(part, sizeof part, "%s.part", j->d.path);
        if(j->d.state == WEBDL_DONE && !stat(j->d.path, &st)) j->d.done = st.st_size;
        else if(!stat(part, &st)) j->d.done = st.st_size;
    }
    fclose(f);
}

void webdl_init(void)
{
    if(inited) return;
    inited = true;
    load_list();
}

/* ------------------------------------------------------------ nomes */

void webdl_safe_name(const char *in, char *out, size_t n)
{
    const char *b = strrchr(in, '/');
    b = b ? b + 1 : in;
    size_t k = 0;
    for(const unsigned char *p = (const unsigned char *)b; *p && k + 1 < n && k < 180; p++) {
        unsigned char c = *p;
        if(c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c)) c = '_';
        out[k++] = (char)c;
    }
    out[k] = 0;
    while(k && ((unsigned char)out[k - 1] & 0xc0) == 0x80) out[--k] = 0;   /* sem UTF-8 cortado */
    if(k && ((unsigned char)out[k - 1] & 0xc0) == 0xc0) out[--k] = 0;
    char *s = out;
    while(*s == '.' || *s == ' ') s++;                   /* nada de ".." nem oculto */
    memmove(out, s, strlen(s) + 1);
    for(k = strlen(out); k && (out[k - 1] == '.' || out[k - 1] == ' '); k--) out[k - 1] = 0;
    if(!out[0]) snprintf(out, n, "download");
    if(!strcasecmp(out, ".part") || (strlen(out) > 5 && !strcasecmp(out + strlen(out) - 5, ".part")))
        strncat(out, "_", n - strlen(out) - 1);          /* não confunde com um pela metade */
}

static bool name_taken(const char *path)
{
    char part[600];
    snprintf(part, sizeof part, "%s.part", path);
    if(!access(path, F_OK) || !access(part, F_OK)) return true;
    for(int i = 0; i < n_jobs; i++) if(!strcmp(jobs[i].d.path, path) && jobs[i].d.state != WEBDL_DONE) return true;
    return false;
}

static bool unique_path(const char *dir, const char *name, char *out, size_t n)
{
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for(int i = 0; i < 1000; i++) {
        int k = i ? snprintf(out, n, "%s/%.*s (%d)%s", dir, stem, name, i, dot && dot != name ? dot : "")
                  : snprintf(out, n, "%s/%s", dir, name);
        if(k < 0 || (size_t)k >= n) return false;
        if(!name_taken(out)) return true;
    }
    return false;
}

/* ------------------------------------------------------------ processos bfdl */

static void start_job(job_t *j);

static void poll_tick(lv_timer_t *t);

static void ensure_tick(void)
{
    if(!tick) tick = lv_timer_create(poll_tick, 250, NULL);
}

static void schedule_queue(void)
{
    int running = 0;
    for(int i = 0; i < n_jobs; i++) if(jobs[i].pid > 0) running++;
    for(int i = 0; i < n_jobs && running < MAX_RUN; i++)
        if(jobs[i].d.state == WEBDL_QUEUED && jobs[i].pid <= 0) { start_job(&jobs[i]); running++; }
}

static void fail(job_t *j, const char *why)
{
    j->d.state = WEBDL_FAILED;
    snprintf(j->d.error, sizeof j->d.error, "%s", why);
}

static void start_job(job_t *j)
{
    char bin[600], ca[600], lim[32];
    snprintf(bin, sizeof bin, "%s/bfdl", webproc_dir());
    snprintf(ca, sizeof ca, "%s/res/ca-bundle.crt", webproc_dir());
    if(access(bin, X_OK)) { fail(j, "bfdl ausente"); return; }
    mkdir(webdl_dir(), 0755);
    int out[2];
    if(pipe2(out, O_CLOEXEC)) { fail(j, strerror(errno)); return; }
    snprintf(lim, sizeof lim, "%d", 0);
    pid_t p = fork();
    if(p < 0) { close(out[0]); close(out[1]); fail(j, strerror(errno)); return; }
    if(p == 0) {
        dup2(out[1], 1);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        setpriority(PRIO_PROCESS, 0, 10);
        /* o cookie vai pelo ambiente (não aparece no ps) */
        if(j->d.cookie[0]) setenv("BFDL_COOKIE", j->d.cookie, 1);
        char *av[] = { bin, "--url", j->d.url, "--out", j->d.path, "--dir", (char *)webdl_dir(),
                       "--ca", ca, "--referer", j->d.referer, NULL };
        execv(bin, av);
        _exit(127);
    }
    close(out[1]);
    j->fd = out[0];
    fcntl(j->fd, F_SETFL, fcntl(j->fd, F_GETFL) | O_NONBLOCK);
    j->pid = p;
    j->len = 0;
    j->pausing = j->cancelling = false;
    j->d.state = WEBDL_RUNNING;
    j->d.error[0] = 0;
    j->d.speed = 0;
    ensure_tick();
}

static const char *reason_text(const char *r)
{
    static const struct { const char *k, *pt; } t[] = {
        { "fat32", "maior que 4 GB (limite do FAT32)" }, { "espaco", "cartão sem espaço" },
        { "somente-leitura", "cartão só de leitura" }, { "sem-cartao", "cartão ausente" },
        { "rede", "falha de rede" }, { "tls", "certificado recusado" }, { "http", "o servidor recusou" },
        { "incompleto", "incompleto" }, { "mudou", "o arquivo mudou no servidor: recomeçado" },
        { "gravar", "erro ao gravar" }, { "caminho", "caminho inválido" },
    };
    for(size_t i = 0; i < sizeof t / sizeof t[0]; i++) if(!strncmp(r, t[i].k, strlen(t[i].k))) return t[i].pt;
    return r;
}

static void line_from(job_t *j, const char *l)
{
    long long a, b, c;
    if(l[0] == 'P' && sscanf(l + 1, "%lld %lld %lld", &a, &b, &c) == 3) {
        j->d.done = a;
        if(b > 0) j->d.total = b;
        j->d.speed = c;
    }
    else if(l[0] == 'D') {
        j->d.state = WEBDL_DONE;
        j->d.speed = 0;
        osd_show(&bf_icon_web, 0x2ec27e, tr(S_WEB_DL_DONE), j->d.name);
    }
    else if(l[0] == 'E') {
        fail(j, reason_text(l + 2));
        osd_show(&bf_icon_web, 0xe5484d, tr(S_WEB_DL_FAIL), j->d.error);
    }
}

static void reap(job_t *j)
{
    int st;
    if(j->pid <= 0 || waitpid(j->pid, &st, WNOHANG) != j->pid) return;
    j->pid = 0;
    if(j->fd >= 0) { close(j->fd); j->fd = -1; }
    char part[600];
    snprintf(part, sizeof part, "%s.part", j->d.path);
    if(j->cancelling) { unlink(part); j->d.state = WEBDL_FAILED; snprintf(j->d.error, sizeof j->d.error, "%s", tr(S_WEB_CANCEL)); }
    else if(j->pausing) j->d.state = WEBDL_PAUSED;
    else if(j->d.state == WEBDL_RUNNING) fail(j, WIFSIGNALED(st) ? "interrompido" : "erro");
    j->d.speed = 0;
    save_list();
    schedule_queue();
}

static void poll_tick(lv_timer_t *t)
{
    (void)t;
    bool any = false, changed = false;
    for(int i = 0; i < n_jobs; i++) {
        job_t *j = &jobs[i];
        if(j->pid <= 0) continue;
        any = true;
        for(;;) {
            ssize_t n = read(j->fd, j->buf + j->len, sizeof j->buf - 1 - j->len);
            if(n <= 0) break;
            j->len += (size_t)n;
            char *s = j->buf, *nl;
            while((nl = memchr(s, '\n', j->buf + j->len - s))) { *nl = 0; line_from(j, s); s = nl + 1; changed = true; }
            j->len -= (size_t)(s - j->buf);
            memmove(j->buf, s, j->len);
            if(j->len == sizeof j->buf - 1) j->len = 0;
        }
        webdl_state_t before = j->d.state;
        reap(j);
        if(j->d.state != before || j->pid <= 0) changed = true;
    }
    if(changed) notify();
    if(!any) { lv_timer_delete(tick); tick = NULL; }
}

/* ------------------------------------------------------------ API */

bool webdl_request(const char *req, char *name_out, size_t n)
{
    webdl_init();
    char buf[4200];
    snprintf(buf, sizeof buf, "%s", req);
    char *f[6] = { 0 };
    int nf = 0;
    for(char *s = buf; nf < 6; ) {
        f[nf++] = s;
        char *t = strchr(s, '\t');
        if(!t) break;
        *t = 0;
        s = t + 1;
    }
    if(name_out && n) name_out[0] = 0;
    if(nf < 2 || (strncasecmp(f[0], "http://", 7) && strncasecmp(f[0], "https://", 8))) return false;
    if(n_jobs == MAX_DL) {                               /* lista cheia: some o mais antigo terminado */
        int k = -1;
        for(int i = 0; i < n_jobs && k < 0; i++) if(jobs[i].pid <= 0 && (jobs[i].d.state == WEBDL_DONE || jobs[i].d.state == WEBDL_FAILED)) k = i;
        if(k < 0) return false;
        memmove(&jobs[k], &jobs[k + 1], sizeof jobs[0] * (size_t)(n_jobs - k - 1));
        n_jobs--;
    }
    char name[200];
    webdl_safe_name(f[1], name, sizeof name);
    if(name_out) snprintf(name_out, n, "%s", name);
    job_t *j = &jobs[n_jobs];
    memset(j, 0, sizeof *j);
    j->fd = -1;
    if(!unique_path(webdl_dir(), name, j->d.path, sizeof j->d.path)) return false;
    const char *slash = strrchr(j->d.path, '/');
    snprintf(j->d.name, sizeof j->d.name, "%s", slash ? slash + 1 : j->d.path);
    snprintf(j->d.url, sizeof j->d.url, "%s", f[0]);
    if(nf > 2) j->d.total = strtoll(f[2], NULL, 10);
    if(nf > 4) snprintf(j->d.referer, sizeof j->d.referer, "%s", f[4]);
    if(nf > 5) snprintf(j->d.cookie, sizeof j->d.cookie, "%s", f[5]);
    j->d.id = next_id++;
    j->d.state = WEBDL_QUEUED;
    n_jobs++;
    if(name_out) snprintf(name_out, n, "%s", j->d.name);
    schedule_queue();
    save_list();
    notify();
    return true;
}

int webdl_count(void) { webdl_init(); return n_jobs; }
const webdl_t *webdl_at(int i) { return i >= 0 && i < n_jobs ? &jobs[i].d : NULL; }

static job_t *job(int id)
{
    for(int i = 0; i < n_jobs; i++) if(jobs[i].d.id == id) return &jobs[i];
    return NULL;
}

const webdl_t *webdl_find(int id) { job_t *j = job(id); return j ? &j->d : NULL; }

int webdl_active(void)
{
    int k = 0;
    for(int i = 0; i < n_jobs; i++) if(jobs[i].d.state == WEBDL_RUNNING || jobs[i].d.state == WEBDL_QUEUED) k++;
    return k;
}

void webdl_pause(int id)
{
    job_t *j = job(id);
    if(!j) return;
    if(j->pid > 0) { j->pausing = true; kill(j->pid, SIGTERM); }
    else if(j->d.state == WEBDL_QUEUED) { j->d.state = WEBDL_PAUSED; save_list(); }
    notify();
}

void webdl_resume(int id)
{
    job_t *j = job(id);
    if(!j || j->pid > 0 || j->d.state == WEBDL_DONE) return;
    j->d.state = WEBDL_QUEUED;
    j->d.error[0] = 0;
    schedule_queue();
    save_list();
    notify();
}

void webdl_cancel(int id)
{
    job_t *j = job(id);
    if(!j) return;
    if(j->pid > 0) { j->cancelling = true; kill(j->pid, SIGTERM); return; }
    char part[600];
    snprintf(part, sizeof part, "%s.part", j->d.path);
    unlink(part);
    int k = (int)(j - jobs);
    memmove(&jobs[k], &jobs[k + 1], sizeof jobs[0] * (size_t)(n_jobs - k - 1));
    n_jobs--;
    save_list();
    notify();
}

void webdl_open_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *ext = dot ? dot + 1 : "";
    static const char *const audio[] = { "mp3", "ogg", "wav", "flac", "m4a", "aac" };
    if(img_is_image(path)) { if(imgview_open(path)) return; }
    else if(book_is_supported(path)) { book_open_file(path); return; }
    else for(size_t i = 0; i < sizeof audio / sizeof audio[0]; i++)
        if(!strcasecmp(ext, audio[i])) { music_play_file(path); return; }
    files_open_dir(webdl_dir());       /* tipo sem app: mostra a pasta, nunca executa */
}
