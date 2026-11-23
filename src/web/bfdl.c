/* bfdl.c — baixa um arquivo para o cartão (downloads do app Internet).
 *
 *   bfdl --url URL --out ARQUIVO --dir PASTA [--ca CERTS] [--referer URL]
 *   (cookie da sessão, se houver, em $BFDL_COOKIE: não aparece no ps)
 *
 * Grava em ARQUIVO.part e só renomeia para ARQUIVO quando termina inteiro (fsync antes).
 * Se o .part já existe, continua de onde parou (Range + If-Range com o ETag ou a data
 * guardados em ARQUIVO.part.info): se o arquivo mudou no servidor, recomeça do zero.
 * SIGTERM (pausar) para limpo e mantém o .part. Só http/https (inclusive nos
 * redirecionamentos), certificado sempre verificado, nada é executado.
 *
 * Saída, uma linha por evento: "P feitos total B/s" (2 por segundo), "D ARQUIVO" no fim,
 * "E motivo" em erro (fat32, espaco, somente-leitura, sem-cartao, rede, tls, http NNN,
 * incompleto, gravar, caminho). Código de saída 0 só quando terminou.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define FAT32_MAX 4294967295LL

static volatile sig_atomic_t stop;
static FILE *out;
static long long offset, got, total = -1, last_got;
static double last_t;
static char validator[256], new_validator[256];
static int http_code;
static const char *fail_reason;
static const char *dl_dir;
static bool space_checked;

static void on_term(int s) { (void)s; stop = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* cabeçalhos: tamanho total e validador para continuar depois */
static size_t on_header(char *b, size_t sz, size_t n, void *ud)
{
    (void)ud;
    size_t len = sz * n;
    char h[512];
    snprintf(h, sizeof h, "%.*s", (int)(len < sizeof h - 1 ? len : sizeof h - 1), b);
    h[strcspn(h, "\r\n")] = 0;
    if(!strncasecmp(h, "HTTP/", 5)) {
        new_validator[0] = 0;                  /* respostas de redirecionamento: recomeça */
        const char *sp = strchr(h, ' ');
        http_code = sp ? atoi(sp + 1) : 0;
    }
    else if(!strncasecmp(h, "ETag:", 5)) snprintf(new_validator, sizeof new_validator, "%s", h + 5 + strspn(h + 5, " "));
    else if(!strncasecmp(h, "Last-Modified:", 14) && !new_validator[0])
        snprintf(new_validator, sizeof new_validator, "%s", h + 14 + strspn(h + 14, " "));
    return len;
}

static size_t on_data(char *b, size_t sz, size_t n, void *ud)
{
    (void)ud;
    size_t len = sz * n;
    if(stop) return 0;
    if(http_code == 200 && offset) {           /* pedimos Range e veio tudo: recomeça do zero */
        if(fflush(out) || ftruncate(fileno(out), 0) || fseek(out, 0, SEEK_SET)) { fail_reason = "gravar"; return 0; }
        offset = 0;
    }
    if(offset + got + (long long)len > FAT32_MAX) { fail_reason = "fat32"; return 0; }
    if(fwrite(b, 1, len, out) != len) {
        fail_reason = errno == ENOSPC ? "espaco" : errno == EROFS ? "somente-leitura" : "gravar";
        return 0;
    }
    got += (long long)len;
    return len;
}

static int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ult, curl_off_t ulnow)
{
    (void)ud;
    (void)dlnow;
    (void)ult;
    (void)ulnow;
    if(stop) return 1;
    if(dltotal > 0) {
        long long t = offset + (long long)dltotal;
        if(t > FAT32_MAX) { fail_reason = "fat32"; return 1; }
        total = t;
        struct statvfs vs;                     /* cabe no cartão? (1 MB de folga) */
        if(!space_checked && !statvfs(dl_dir, &vs)) {
            space_checked = true;
            if((long long)vs.f_bavail * (long long)vs.f_frsize < (long long)dltotal + 1024 * 1024) {
                fail_reason = "espaco";
                return 1;
            }
        }
    }
    double t = now();
    if(t - last_t >= 0.5) {
        long long speed = (long long)((got - last_got) / (t - last_t));
        say("P %lld %lld %lld", offset + got, total, speed);
        last_t = t;
        last_got = got;
    }
    return 0;
}

static bool inside(const char *dir, const char *path)
{
    char rd[PATH_MAX], rp[PATH_MAX], parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *s = strrchr(parent, '/');
    if(!s) return false;
    *s = 0;
    if(!realpath(dir, rd) || !realpath(parent, rp)) return false;
    size_t n = strlen(rd);
    return !strcmp(rp, rd) || (!strncmp(rp, rd, n) && rp[n] == '/');
}

int main(int argc, char **argv)
{
    const char *url = NULL, *path = NULL, *dir = NULL, *ca = NULL, *referer = NULL;
    for(int i = 1; i + 1 < argc; i += 2) {
        if(!strcmp(argv[i], "--url")) url = argv[i + 1];
        else if(!strcmp(argv[i], "--out")) path = argv[i + 1];
        else if(!strcmp(argv[i], "--dir")) dir = argv[i + 1];
        else if(!strcmp(argv[i], "--ca")) ca = argv[i + 1];
        else if(!strcmp(argv[i], "--referer")) referer = argv[i + 1];
    }
    if(!url || !path || !dir || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))) {
        fprintf(stderr, "uso: bfdl --url URL --out ARQUIVO --dir PASTA [--ca CERTS] [--referer URL]\n");
        return 2;
    }
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    dl_dir = dir;
    mkdir(dir, 0755);
    if(access(dir, F_OK)) { say("E sem-cartao"); return 1; }
    if(!inside(dir, path)) { say("E caminho"); return 1; }   /* só grava na pasta de downloads */

    char part[PATH_MAX], info[PATH_MAX];
    snprintf(part, sizeof part, "%s.part", path);
    snprintf(info, sizeof info, "%s.part.info", path);
    struct stat st;
    if(!stat(part, &st) && st.st_size > 0) {
        offset = st.st_size;
        FILE *f = fopen(info, "r");
        if(f) {
            if(fgets(validator, sizeof validator, f)) validator[strcspn(validator, "\r\n")] = 0;
            fclose(f);
        }
        if(!validator[0]) offset = 0;          /* sem validador não dá para confiar: recomeça */
    }
    out = fopen(part, offset ? "r+b" : "wb");
    if(!out) { say("E %s", errno == EROFS ? "somente-leitura" : errno == ENOSPC ? "espaco" : "gravar"); return 1; }
    if(offset && fseeko(out, offset, SEEK_SET)) { say("E gravar"); return 1; }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *c = curl_easy_init();
    struct curl_slist *hdr = NULL;
    char range[64], ifr[300];
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if(ca) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* parado 60 s: falha de rede */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NetSurf/3.11 (BigFoston Web; Linux armv6l)");
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "identity");   /* o tamanho é o do arquivo */
    if(referer && *referer && !strncasecmp(referer, "http", 4)) curl_easy_setopt(c, CURLOPT_REFERER, referer);
    const char *cookie = getenv("BFDL_COOKIE");
    if(cookie && *cookie) curl_easy_setopt(c, CURLOPT_COOKIE, cookie);
    if(offset) {
        snprintf(range, sizeof range, "%lld-", offset);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        snprintf(ifr, sizeof ifr, "If-Range: %s", validator);
        hdr = curl_slist_append(hdr, ifr);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    }
    last_t = now();
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);

    /* validador do que está no .part (para continuar depois de uma pausa) */
    if(new_validator[0]) {
        FILE *f = fopen(info, "w");
        if(f) { fprintf(f, "%s\n", new_validator); fclose(f); }
    }
    bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
    ok = fclose(out) == 0 && ok;
    say("P %lld %lld 0", offset + got, total);
    if(stop) return 3;                                    /* pausado: .part fica */
    bool empty = offset + got == 0;
    if(empty && (fail_reason || (r != CURLE_OK && r != CURLE_HTTP_RETURNED_ERROR) || r == CURLE_HTTP_RETURNED_ERROR)) {
        unlink(part);                                     /* erro sem dados: nada de .part vazio */
        unlink(info);
    }
    if(!ok) { say("E gravar"); return 1; }
    if(fail_reason) { say("E %s", fail_reason); return 1; }
    bool complete_already = r == CURLE_HTTP_RETURNED_ERROR && code == 416 && offset > 0;
    if(complete_already) total = offset;                  /* o .part já tinha tudo */
    else if(r != CURLE_OK) {
        if(r == CURLE_HTTP_RETURNED_ERROR) say("E http %ld", code);
        else if(r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CONNECT_ERROR || r == CURLE_SSL_CACERT_BADFILE) say("E tls");
        else if(r == CURLE_WRITE_ERROR) say("E gravar");
        else say("E rede %s", curl_easy_strerror(r));
        return 1;
    }
    if(total > 0 && offset + got != total) { say("E incompleto"); return 1; }
    if(rename(part, path)) { say("E gravar"); return 1; }
    unlink(info);
    say("D %s", path);
    return 0;
}
