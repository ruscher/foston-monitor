/* fileops.c — ver fileops.h. */
#define _DEFAULT_SOURCE
#include "fileops.h"
#include "settings.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define SD_ROOT_DEFAULT "/mnt/sd"
#define BUF (64 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static fo_status_t st;
static atomic_int cancel_req;
static char src_path[1024], dst_dir[1024];

/* --------------------------------------------------------- regras */

static bool under(const char *path, const char *root)
{
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (path[n] == 0 || path[n] == '/');
}

bool fo_writable(const char *path)
{
    struct statvfs v;
    char real[PATH_MAX], data[PATH_MAX];
    if(!path || !*path || !realpath(path, real)) return false;
    const char *sd = getenv("BIGFOSTON_SD");          /* testes: outra raiz de cartão */
    bool in_sd = under(real, sd && *sd ? sd : SD_ROOT_DEFAULT);
    bool in_data = realpath(data_dir(), data) && under(real, data);
    if(!in_sd && !in_data) return false;              /* o sistema (/) nunca */
    if(statvfs(real, &v) || (v.f_flag & ST_RDONLY)) return false;
    return true;
}

bool fo_valid_name(const char *name)
{
    size_t n = strlen(name);
    if(!n || n > 200) return false;
    bool only_dots = true;
    for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if(*p < 0x20 || strchr("/\\:*?\"<>|", *p)) return false;
        if(*p != '.') only_dots = false;
    }
    if(only_dots) return false;
    return name[n - 1] != ' ' && name[n - 1] != '.';   /* o FAT apaga em silêncio */
}

static void set_err(char *err, size_t n, const char *what, int e)
{
    snprintf(err, n, "%.100s: %s", what, strerror(e));
}

bool fo_mkdir(const char *dir, const char *name, char *err, size_t n)
{
    char p[1024];
    if(!fo_valid_name(name)) { snprintf(err, n, "nome inválido"); return false; }
    if(!fo_writable(dir)) { snprintf(err, n, "local só de leitura"); return false; }
    if(snprintf(p, sizeof p, "%s/%s", dir, name) >= (int)sizeof p) { snprintf(err, n, "caminho longo demais"); return false; }
    if(mkdir(p, 0755)) { set_err(err, n, name, errno); return false; }
    sync();
    return true;
}

bool fo_rename(const char *path, const char *new_name, char *err, size_t n)
{
    char dir[1024], p[1024];
    if(!fo_valid_name(new_name)) { snprintf(err, n, "nome inválido"); return false; }
    snprintf(dir, sizeof dir, "%s", path);
    char *s = strrchr(dir, '/');
    if(!s) { snprintf(err, n, "caminho inválido"); return false; }
    *s = 0;
    if(!fo_writable(dir[0] ? dir : "/")) { snprintf(err, n, "local só de leitura"); return false; }
    if(snprintf(p, sizeof p, "%s/%s", dir, new_name) >= (int)sizeof p) { snprintf(err, n, "caminho longo demais"); return false; }
    if(!access(p, F_OK)) { snprintf(err, n, "já existe \"%s\"", new_name); return false; }
    if(rename(path, p)) { set_err(err, n, new_name, errno); return false; }
    sync();
    return true;
}

/* --------------------------------------------------------- thread */

static bool cancelled(void) { return atomic_load(&cancel_req) != 0; }

static void progress(long long add, const char *name)
{
    pthread_mutex_lock(&lock);
    st.done += add;
    if(name) snprintf(st.current, sizeof st.current, "%s", name);
    pthread_mutex_unlock(&lock);
}

static void fail(const char *what, int e)
{
    pthread_mutex_lock(&lock);
    if(st.err[0] == 0) set_err(st.err, sizeof st.err, what, e);
    pthread_mutex_unlock(&lock);
}

/* tamanho total (bytes) ou número de itens */
static long long measure(const char *path, bool items)
{
    struct stat s;
    if(lstat(path, &s)) return 0;
    if(!S_ISDIR(s.st_mode)) return items ? 1 : (long long)s.st_size;
    long long sum = items ? 1 : 0;
    DIR *d = opendir(path);
    if(!d) return sum;
    struct dirent *e;
    char p[1024];
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if(snprintf(p, sizeof p, "%s/%s", path, e->d_name) < (int)sizeof p) sum += measure(p, items);
    }
    closedir(d);
    return sum;
}

static bool copy_file(const char *src, const char *dst, const char *name)
{
    int in = open(src, O_RDONLY | O_CLOEXEC), out;
    if(in < 0) { fail(name, errno); return false; }
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if(out < 0) { fail(name, errno); close(in); return false; }
    static char buf[BUF];
    bool ok = true;
    progress(0, name);
    for(;;) {
        if(cancelled()) { ok = false; break; }
        ssize_t r = read(in, buf, sizeof buf);
        if(r < 0) { if(errno == EINTR) continue; fail(name, errno); ok = false; break; }
        if(!r) break;
        for(ssize_t o = 0; o < r;) {
            ssize_t w = write(out, buf + o, (size_t)(r - o));
            if(w < 0) { if(errno == EINTR) continue; fail(name, errno); ok = false; break; }
            o += w;
        }
        if(!ok) break;
        progress(r, NULL);
    }
    if(ok && fsync(out)) { fail(name, errno); ok = false; }
    if(close(out) && ok) { fail(name, errno); ok = false; }
    close(in);
    if(!ok) unlink(dst);                                /* nada pela metade */
    return ok;
}

static bool copy_tree(const char *src, const char *dst)
{
    struct stat s;
    const char *name = strrchr(src, '/');
    name = name ? name + 1 : src;
    if(lstat(src, &s)) { fail(name, errno); return false; }
    if(S_ISLNK(s.st_mode)) return true;                 /* o FAT não tem links: pula */
    if(!S_ISDIR(s.st_mode)) return copy_file(src, dst, name);
    if(mkdir(dst, 0755)) { fail(name, errno); return false; }
    DIR *d = opendir(src);
    if(!d) { fail(name, errno); return false; }
    struct dirent *e;
    bool ok = true;
    char a[1024], b[1024];
    while(ok && (e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if(snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a ||
           snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b) {
            fail(e->d_name, ENAMETOOLONG);
            ok = false;
            break;
        }
        ok = copy_tree(a, b);
    }
    closedir(d);
    return ok;
}

static bool delete_tree(const char *path)
{
    struct stat s;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if(cancelled()) return false;
    if(lstat(path, &s)) { fail(name, errno); return false; }
    if(S_ISDIR(s.st_mode)) {
        DIR *d = opendir(path);
        if(!d) { fail(name, errno); return false; }
        struct dirent *e;
        bool ok = true;
        char p[1024];
        while(ok && (e = readdir(d))) {
            if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if(snprintf(p, sizeof p, "%s/%s", path, e->d_name) >= (int)sizeof p) {
                fail(e->d_name, ENAMETOOLONG);
                ok = false;
                break;
            }
            ok = delete_tree(p);
        }
        closedir(d);
        if(!ok) return false;
        if(rmdir(path)) { fail(name, errno); return false; }
    }
    else if(unlink(path)) { fail(name, errno); return false; }
    progress(1, name);
    return true;
}

/* destino livre: "nome", "nome (2)", "nome (3)"... (a extensão fica no fim) */
static bool free_target(const char *dir, const char *name, char *out, size_t n)
{
    if(snprintf(out, n, "%s/%s", dir, name) >= (int)n) return false;
    if(access(out, F_OK)) return true;
    const char *dot = strrchr(name, '.');
    int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for(int i = 2; i < 1000; i++) {
        if(snprintf(out, n, "%s/%.*s (%d)%s", dir, base, name, i, dot && dot != name ? dot : "") >= (int)n)
            return false;
        if(access(out, F_OK)) return true;
    }
    return false;
}

static void *worker(void *arg)
{
    (void)arg;
    fo_op_t op;
    char dst[1024];
    pthread_mutex_lock(&lock);
    op = st.op;
    pthread_mutex_unlock(&lock);
    const char *name = strrchr(src_path, '/');
    name = name ? name + 1 : src_path;
    bool ok, was_cancelled = false;
    long long total;
    pthread_mutex_lock(&lock);
    total = st.total;
    pthread_mutex_unlock(&lock);

    if(op == FO_DELETE) {
        ok = delete_tree(src_path);
    }
    else {
        ok = false;
        if(!free_target(dst_dir, name, dst, sizeof dst)) fail(name, ENAMETOOLONG);
        else if(op == FO_MOVE && !rename(src_path, dst)) {  /* mesmo sistema de arquivos: instantâneo */
            ok = true;
            progress(total, name);
        }
        else if(op == FO_COPY || errno == EXDEV) {
            ok = copy_tree(src_path, dst);
            if(!ok) {
                struct stat s;
                was_cancelled = cancelled();
                if(!lstat(dst, &s) && S_ISDIR(s.st_mode)) {   /* pasta pela metade: desfaz */
                    atomic_store(&cancel_req, 0);
                    delete_tree(dst);
                }
            }
            if(ok && op == FO_MOVE) ok = delete_tree(src_path);
        }
        else fail(name, errno);
        pthread_mutex_lock(&lock);
        if(ok) snprintf(st.result, sizeof st.result, "%s", dst);
        pthread_mutex_unlock(&lock);
    }
    sync();                                            /* tudo no cartão antes de dizer "pronto" */
    pthread_mutex_lock(&lock);
    st.state = ok ? FO_DONE : (was_cancelled || cancelled()) ? FO_CANCELLED : FO_FAILED;
    pthread_mutex_unlock(&lock);
    return NULL;
}

bool fo_start(fo_op_t op, const char *src, const char *dst, char *err, size_t n)
{
    char rs[PATH_MAX], rd[PATH_MAX];
    pthread_mutex_lock(&lock);
    bool busy = st.state == FO_RUNNING;
    pthread_mutex_unlock(&lock);
    if(busy) { snprintf(err, n, "outra operação em curso"); return false; }
    if(!realpath(src, rs)) { set_err(err, n, src, errno); return false; }
    if(op == FO_DELETE || op == FO_MOVE) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", rs);
        char *s = strrchr(parent, '/');
        if(s) *s = 0;
        if(!fo_writable(parent[0] ? parent : "/")) { snprintf(err, n, "local de origem só de leitura"); return false; }
    }
    if(op != FO_DELETE) {
        if(!realpath(dst, rd) || !fo_writable(rd)) { snprintf(err, n, "destino só de leitura"); return false; }
        if(under(rd, rs)) { snprintf(err, n, "não dá para copiar uma pasta para dentro dela mesma"); return false; }
        struct statvfs v;
        long long need = measure(rs, false);
        if(op == FO_COPY && !statvfs(rd, &v) && (long long)v.f_bavail * (long long)v.f_frsize < need) {
            snprintf(err, n, "espaço insuficiente no destino");
            return false;
        }
    }
    if(snprintf(src_path, sizeof src_path, "%s", rs) >= (int)sizeof src_path ||
       snprintf(dst_dir, sizeof dst_dir, "%s", op == FO_DELETE ? "" : rd) >= (int)sizeof dst_dir) {
        snprintf(err, n, "caminho longo demais");
        return false;
    }
    pthread_mutex_lock(&lock);
    memset(&st, 0, sizeof st);
    st.op = op;
    st.state = FO_RUNNING;
    st.total = measure(rs, op == FO_DELETE);
    pthread_mutex_unlock(&lock);
    atomic_store(&cancel_req, 0);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t, &at, worker, NULL)) {
        pthread_attr_destroy(&at);
        pthread_mutex_lock(&lock);
        st.state = FO_IDLE;
        pthread_mutex_unlock(&lock);
        snprintf(err, n, "não foi possível iniciar");
        return false;
    }
    pthread_attr_destroy(&at);
    return true;
}

void fo_cancel(void) { atomic_store(&cancel_req, 1); }

void fo_get(fo_status_t *out)
{
    pthread_mutex_lock(&lock);
    *out = st;
    pthread_mutex_unlock(&lock);
}

void fo_reset(void)
{
    pthread_mutex_lock(&lock);
    if(st.state != FO_RUNNING) st.state = FO_IDLE;
    pthread_mutex_unlock(&lock);
}
