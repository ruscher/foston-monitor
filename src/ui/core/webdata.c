/* webdata.c — ver webdata.h. */
#define _DEFAULT_SOURCE
#include "webdata.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct { const char *name, *tmpl; } engines[] = {
    { "DuckDuckGo", "https://html.duckduckgo.com/html/?q=%s" },   /* sem JavaScript */
    { "Wikipédia",  "https://pt.m.wikipedia.org/w/index.php?search=%s" },
    { "Google",     "https://www.google.com/search?gbv=1&q=%s" }, /* versão básica */
    { "Bing",       "https://www.bing.com/search?q=%s" },
};
#define N_ENGINES (int)(sizeof engines / sizeof engines[0])

int web_search_count(void) { return N_ENGINES; }
const char *web_search_name(int i) { return i >= 0 && i < N_ENGINES ? engines[i].name : ""; }

/* ------------------------------------------------------------ URL */

static size_t scheme_len(const char *s)
{
    if(!isalpha((unsigned char)s[0])) return 0;
    size_t i = 1;
    while(isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    return s[i] == ':' ? i : 0;
}

bool web_scheme_allowed(const char *url)
{
    size_t n = scheme_len(url);
    static const char *const ok[] = { "http", "https", "file", "about", "data", "resource" };
    for(size_t i = 0; n && i < sizeof ok / sizeof ok[0]; i++)
        if(strlen(ok[i]) == n && !strncasecmp(url, ok[i], n)) return true;
    return false;
}

static bool is_ipv4_host(const char *s, size_t n)
{
    int dots = 0, digits = 0;
    for(size_t i = 0; i < n; i++) {
        if(s[i] == '.') { if(!digits) return false; dots++; digits = 0; }
        else if(isdigit((unsigned char)s[i])) { if(++digits > 3) return false; }
        else return false;
    }
    return dots == 3 && digits;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for(const unsigned char *p = (const unsigned char *)in; *p && k + 4 < n; p++) {
        if(isalnum(*p) || strchr("-_.~", *p)) out[k++] = (char)*p;
        else if(*p == ' ') out[k++] = '+';
        else { out[k++] = '%'; out[k++] = hex[*p >> 4]; out[k++] = hex[*p & 15]; }
    }
    out[k] = 0;
}

bool web_normalize(const char *text, int search_engine, char *out, size_t n)
{
    out[0] = 0;
    while(*text == ' ' || *text == '\t') text++;
    char t[WEB_URL_MAX];
    snprintf(t, sizeof t, "%s", text);
    for(size_t l = strlen(t); l && (t[l - 1] == ' ' || t[l - 1] == '\t' || t[l - 1] == '\n'); l--) t[l - 1] = 0;
    if(!t[0]) return false;
    for(char *c = t; *c; c++) if((unsigned char)*c < 0x20) return false;   /* sem controles no endereço */

    bool space = strchr(t, ' ') != NULL;
    size_t sl = scheme_len(t);
    /* "localhost:8080" e "exemplo.com:8080/x" parecem esquema mas não são */
    bool hostport = sl && isdigit((unsigned char)t[sl + 1]);
    if(sl && !hostport && !space) {
        if(!web_scheme_allowed(t)) return false;                       /* javascript:, mailto: ... */
        snprintf(out, n, "%s", t);
        return true;
    }
    if(!space) {
        size_t hl = strcspn(t, "/:?#");
        bool local = (hl == 9 && !strncasecmp(t, "localhost", 9)) || is_ipv4_host(t, hl);
        bool domain = false;
        if(!local) {
            /* tem um ponto com letras depois (exemplo.com, www.x.org.br) */
            const char *dot = memchr(t, '.', hl);
            const char *last = NULL;
            for(const char *p = t; p < t + hl; p++) if(*p == '.') last = p;
            domain = dot && dot != t && last && last + 2 < t + hl + 1 && isalpha((unsigned char)last[1]);
        }
        if(local || domain) {
            snprintf(out, n, "%s://%s", local ? "http" : "https", t);
            return true;
        }
    }
    if(search_engine < 0 || search_engine >= N_ENGINES) search_engine = 0;
    char q[WEB_URL_MAX];
    url_encode(t, q, sizeof q);
    const char *tm = engines[search_engine].tmpl, *ps = strstr(tm, "%s");
    snprintf(out, n, "%.*s%s%s", (int)(ps - tm), tm, q, ps + 2);
    return true;
}

void web_host(const char *url, char *out, size_t n)
{
    if(!strncasecmp(url, "file:", 5)) {                   /* arquivo local: só o nome */
        const char *b = strrchr(url, '/');
        snprintf(out, n, "%s", b && b[1] ? b + 1 : url);
        return;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t l = strcspn(p, "/:?#");
    if(l > 4 && !strncasecmp(p, "www.", 4)) { p += 4; l -= 4; }
    snprintf(out, n, "%.*s", (int)l, p);
    if(!out[0]) snprintf(out, n, "%s", url);
}

/* ------------------------------------------------------------ arquivos */

static web_item_t marks[WEB_MARKS_MAX], hist[WEB_HIST_MAX];
static web_app_t apps[WEB_APPS_MAX];
static int n_marks, n_hist, n_apps;
static bool hist_dirty, hist_on = true;
static char dir[512];

void web_data_set_dir(const char *d) { snprintf(dir, sizeof dir, "%s", d); }

static void path(char *out, size_t n, const char *name)
{
    if(!dir[0]) data_path(dir, sizeof dir, "web");
    snprintf(out, n, "%s/%s", dir, name);
}

/* copia cortando sem partir um caractere UTF-8 no meio */
static void copy_utf8(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if(l >= n) {
        l = n - 1;
        while(l && ((unsigned char)src[l] & 0xc0) == 0x80) l--;   /* recua até o início do caractere */
    }
    memcpy(dst, src, l);
    dst[l] = 0;
}

/* campo seguro para TSV: sem tab, sem quebra de linha, sem controles */
static void clean(char *s)
{
    for(; *s; s++) if((unsigned char)*s < 0x20) *s = ' ';
}

static bool valid_url(const char *u) { return u[0] && strlen(u) < WEB_URL_MAX && web_scheme_allowed(u); }

/* lê linhas "a\tb\tc" chamando fn(campos, n) */
static void read_tsv(const char *name, void (*fn)(char **f, int nf))
{
    char p[600], line[WEB_URL_MAX + WEB_TITLE_MAX + 64];
    path(p, sizeof p, name);
    FILE *f = fopen(p, "r");
    if(!f) return;
    while(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if(!line[0] || line[0] == '#') continue;
        char *fl[4];
        int nf = 0;
        for(char *s = line; nf < 4; ) {
            fl[nf++] = s;
            char *t = strchr(s, '\t');
            if(!t) break;
            *t = 0;
            s = t + 1;
        }
        fn(fl, nf);
    }
    fclose(f);
}

/* grava por inteiro em .new e troca (nunca deixa o arquivo pela metade) */
static bool write_file(const char *name, void (*fn)(FILE *f))
{
    char p[600], tmp[620];
    path(p, sizeof p, name);
    mkdir(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if(!f) return false;
    fn(f);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if(!ok || rename(tmp, p)) { unlink(tmp); return false; }
    return true;
}

static void load_mark(char **f, int nf)
{
    if(nf < 2 || n_marks >= WEB_MARKS_MAX || !valid_url(f[1])) return;
    web_item_t *m = &marks[n_marks++];
    copy_utf8(m->title, sizeof m->title, f[0]);
    snprintf(m->url, sizeof m->url, "%s", f[1]);
    m->when = 0;
}

static void load_hist(char **f, int nf)
{
    if(nf < 3 || !valid_url(f[2])) return;
    /* arquivo: mais antigo primeiro; memória: mais recente primeiro (cheio: sai o mais antigo) */
    if(n_hist == WEB_HIST_MAX) n_hist--;
    memmove(&hist[1], &hist[0], sizeof hist[0] * (size_t)n_hist);
    web_item_t *h = &hist[0];
    h->when = (time_t)strtoll(f[0], NULL, 10);
    copy_utf8(h->title, sizeof h->title, f[1]);
    snprintf(h->url, sizeof h->url, "%s", f[2]);
    n_hist++;
}

static void load_app(char **f, int nf)
{
    if(nf < 3 || n_apps >= WEB_APPS_MAX || !valid_url(f[1])) return;
    web_app_t *a = &apps[n_apps++];
    copy_utf8(a->name, sizeof a->name, f[0]);
    snprintf(a->url, sizeof a->url, "%s", f[1]);
    a->color = (unsigned)strtoul(f[2], NULL, 16) & 0xffffff;
}

void web_data_load(void)
{
    n_marks = n_hist = n_apps = 0;
    hist_dirty = false;
    read_tsv("favoritos.tsv", load_mark);
    read_tsv("historico.tsv", load_hist);
    read_tsv("webapps.tsv", load_app);
    char p[600];
    path(p, sizeof p, "historico.desligado");
    hist_on = access(p, F_OK) != 0;
}

static void put_marks(FILE *f)
{
    fputs("# BigFoston Web: favoritos (título\\tendereço)\n", f);
    for(int i = 0; i < n_marks; i++) fprintf(f, "%s\t%s\n", marks[i].title, marks[i].url);
}

static void put_hist(FILE *f)
{
    for(int i = n_hist - 1; i >= 0; i--) fprintf(f, "%lld\t%s\t%s\n", (long long)hist[i].when, hist[i].title, hist[i].url);
}

static void put_apps(FILE *f)
{
    fputs("# BigFoston Web: WebApps (nome\\tendereço\\tcor)\n", f);
    for(int i = 0; i < n_apps; i++) fprintf(f, "%s\t%s\t%06x\n", apps[i].name, apps[i].url, apps[i].color);
}

void web_data_flush(void)
{
    if(hist_dirty && write_file("historico.tsv", put_hist)) hist_dirty = false;
}

/* ------------------------------------------------------------ favoritos */

int web_marks_count(void) { return n_marks; }
const web_item_t *web_mark(int i) { return i >= 0 && i < n_marks ? &marks[i] : NULL; }

int web_mark_find(const char *url)
{
    for(int i = 0; i < n_marks; i++) if(!strcmp(marks[i].url, url)) return i;
    return -1;
}

bool web_mark_add(const char *title, const char *url)
{
    if(!valid_url(url) || web_mark_find(url) >= 0 || n_marks >= WEB_MARKS_MAX) return false;
    web_item_t *m = &marks[n_marks++];
    copy_utf8(m->title, sizeof m->title, title && *title ? title : url);
    snprintf(m->url, sizeof m->url, "%s", url);
    clean(m->title);
    return write_file("favoritos.tsv", put_marks);
}

void web_mark_remove(int i)
{
    if(i < 0 || i >= n_marks) return;
    memmove(&marks[i], &marks[i + 1], sizeof marks[0] * (size_t)(n_marks - i - 1));
    n_marks--;
    write_file("favoritos.tsv", put_marks);
}

bool web_mark_edit(int i, const char *title, const char *url)
{
    if(i < 0 || i >= n_marks || !valid_url(url)) return false;
    copy_utf8(marks[i].title, sizeof marks[i].title, title && *title ? title : url);
    snprintf(marks[i].url, sizeof marks[i].url, "%s", url);
    clean(marks[i].title);
    return write_file("favoritos.tsv", put_marks);
}

/* ------------------------------------------------------------ histórico */

bool web_history_enabled(void) { return hist_on; }

void web_history_set_enabled(bool on)
{
    char p[600];
    path(p, sizeof p, "historico.desligado");
    hist_on = on;
    if(on) unlink(p);
    else {
        mkdir(dir, 0755);
        FILE *f = fopen(p, "w");
        if(f) fclose(f);
    }
}

int web_history_count(void) { return n_hist; }
const web_item_t *web_history(int i) { return i >= 0 && i < n_hist ? &hist[i] : NULL; }

void web_history_add(const char *title, const char *url)
{
    if(!hist_on || !valid_url(url) || !strncasecmp(url, "about:", 6)) return;
    char keep[sizeof hist[0].title] = "";
    for(int i = 0; i < n_hist; i++)               /* a mesma página sobe para o topo */
        if(!strcmp(hist[i].url, url)) {
            /* sem título novo (recarga, página ainda sem <title>): fica o que já se sabia */
            if(!(title && *title)) snprintf(keep, sizeof keep, "%s", hist[i].title);
            memmove(&hist[i], &hist[i + 1], sizeof hist[0] * (size_t)(n_hist - i - 1));
            n_hist--;
            break;
        }
    if(n_hist == WEB_HIST_MAX) n_hist--;
    memmove(&hist[1], &hist[0], sizeof hist[0] * (size_t)n_hist);
    n_hist++;
    copy_utf8(hist[0].title, sizeof hist[0].title, title && *title ? title : keep[0] ? keep : url);
    snprintf(hist[0].url, sizeof hist[0].url, "%s", url);
    clean(hist[0].title);
    hist[0].when = time(NULL);
    hist_dirty = true;                            /* grava em lote (web_data_flush) */
}

void web_history_set_title(const char *url, const char *title)
{
    if(n_hist && !strcmp(hist[0].url, url) && title && *title) {
        copy_utf8(hist[0].title, sizeof hist[0].title, title);
        clean(hist[0].title);
        hist_dirty = true;
    }
}

void web_history_remove(int i)
{
    if(i < 0 || i >= n_hist) return;
    memmove(&hist[i], &hist[i + 1], sizeof hist[0] * (size_t)(n_hist - i - 1));
    n_hist--;
    hist_dirty = true;
    web_data_flush();
}

void web_history_clear(void)
{
    n_hist = 0;
    hist_dirty = true;
    web_data_flush();
}

/* ------------------------------------------------------------ WebApps */

int web_apps_count(void) { return n_apps; }
const web_app_t *web_app(int i) { return i >= 0 && i < n_apps ? &apps[i] : NULL; }

void web_apps_load(void)
{
    n_apps = 0;
    read_tsv("webapps.tsv", load_app);
}

int web_app_find(const char *url)
{
    for(int i = 0; i < n_apps; i++)
        if(!strcmp(apps[i].url, url)) return i;
    return -1;
}

const char *web_display_name(const char *name, const char *url, char *host, size_t n)
{
    if(name && *name && !strstr(name, "://")) return name;
    web_host(url, host, n);
    return host;
}

void web_initial(const char *text, char *out, size_t n)
{
    if(n < 5) { if(n) out[0] = 0; return; }
    unsigned char c0 = (unsigned char)text[0];
    if(!c0) { snprintf(out, n, "?"); return; }
    int k = c0 >= 0xf0 ? 4 : c0 >= 0xe0 ? 3 : c0 >= 0xc0 ? 2 : 1;
    int i = 0;
    while(i < k && text[i]) { out[i] = text[i]; i++; }
    out[i] = 0;
    if(k == 1) out[0] = (char)toupper(c0);
}

bool web_app_add(const char *name, const char *url)
{
    if(!valid_url(url) || n_apps >= WEB_APPS_MAX || !name || !*name || web_app_find(url) >= 0) return false;
    static const unsigned hues[] = { 0x3584e4, 0x9b6ad6, 0x2ec27e, 0xe5534b, 0xe0a40c, 0x1fb5cc, 0xd6409f, 0x26a269 };
    unsigned h = 5381;
    for(const char *c = url; *c; c++) h = h * 33 + (unsigned char)*c;
    web_app_t *a = &apps[n_apps++];
    copy_utf8(a->name, sizeof a->name, name);
    clean(a->name);
    snprintf(a->url, sizeof a->url, "%s", url);
    a->color = hues[h % 8];
    return write_file("webapps.tsv", put_apps);
}

void web_app_remove(int i)
{
    if(i < 0 || i >= n_apps) return;
    memmove(&apps[i], &apps[i + 1], sizeof apps[0] * (size_t)(n_apps - i - 1));
    n_apps--;
    write_file("webapps.tsv", put_apps);
}

bool web_app_rename(int i, const char *name)
{
    if(i < 0 || i >= n_apps || !name || !*name) return false;
    copy_utf8(apps[i].name, sizeof apps[i].name, name);
    clean(apps[i].name);
    return write_file("webapps.tsv", put_apps);
}
