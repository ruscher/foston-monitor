/* pkg.c — ver pkg.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include "pkg.h"
#include "lodepng.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *pkg_err_name(pkg_err_t e)
{
    static const char *const n[] = { "ok", "read", "format", "unsafe", "too big", "no .desktop", "write", "not found" };
    return (unsigned)e < sizeof n / sizeof n[0] ? n[e] : "?";
}

bool pkg_valid_id(const char *id)
{
    size_t n = strlen(id);
    if(n < 1 || n > 40 || id[0] == '.' || id[0] == '-') return false;
    for(size_t i = 0; i < n; i++) {
        char c = id[i];
        if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

/* ------------------------------------------------------------ arquivo inteiro */

static unsigned char *slurp(const char *file, size_t *n)
{
    FILE *f = fopen(file, "rb");
    if(!f) return NULL;
    unsigned char *b = NULL;
    if(fseek(f, 0, SEEK_END) == 0) {
        long L = ftell(f);
        if(L > 0 && (unsigned long)L <= PKG_MAX_BYTES + 65536u && fseek(f, 0, SEEK_SET) == 0) {
            b = malloc((size_t)L);
            if(b && fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); b = NULL; }
            else *n = (size_t)L;
        }
    }
    fclose(f);
    return b;
}

/* gzip -> tar na memória (o inflate do lodepng, o mesmo dos EPUB) */
static unsigned char *gunzip(const unsigned char *g, size_t n, size_t *out_n)
{
    if(n < 18 || g[0] != 0x1f || g[1] != 0x8b || g[2] != 8) return NULL;
    uint8_t fl = g[3];
    size_t p = 10;
    if(fl & 4) { if(p + 2 > n) return NULL; p += 2 + (size_t)(g[p] | g[p + 1] << 8); }
    if(fl & 8) { while(p < n && g[p]) p++; p++; }
    if(fl & 16) { while(p < n && g[p]) p++; p++; }
    if(fl & 2) p += 2;
    if(p + 8 > n) return NULL;
    LodePNGDecompressSettings s = lodepng_default_decompress_settings;
    s.max_output_size = PKG_MAX_BYTES + 65536u;
    unsigned char *out = NULL;
    size_t on = 0;
    if(lodepng_inflate(&out, &on, g + p, n - p - 8, &s)) { free(out); return NULL; }
    *out_n = on;
    return out;
}

/* ------------------------------------------------------------ tar */

typedef struct {
    char path[256];         /* sem a pasta do app */
    char type;              /* '0' arquivo, '5' pasta */
    const unsigned char *data;
    size_t size;
} entry_t;

static size_t octal(const char *s, size_t n)
{
    size_t v = 0;
    for(size_t i = 0; i < n && s[i]; i++) {
        if(s[i] == ' ') continue;
        if(s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (size_t)(s[i] - '0');
    }
    return v;
}

static bool header_ok(const unsigned char *h)
{
    unsigned sum = 0;
    for(int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return sum == octal((const char *)h + 148, 8);
}

/* caminho seguro: relativo, sem "..", "." ou partes vazias e sem caracteres de controle */
static bool safe_path(const char *p)
{
    if(!*p || *p == '/') return false;
    for(const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t L = e ? (size_t)(e - s) : strlen(s);
        if(L == 0 && e && !e[1]) break;                /* barra no fim (pasta) */
        if(L == 0 || (L == 1 && s[0] == '.') || (L == 2 && s[0] == '.' && s[1] == '.')) return false;
        for(size_t i = 0; i < L; i++) if((unsigned char)s[i] < 0x20 || s[i] == '\\') return false;
        if(!e) break;
        s = e + 1;
    }
    return true;
}

typedef struct {
    unsigned char *buf;
    entry_t *ent;
    int n;
    char id[48];
    size_t bytes;
} pkgdata_t;

static void pkg_free(pkgdata_t *p) { free(p->buf); free(p->ent); memset(p, 0, sizeof *p); }

static pkg_err_t pkg_load(const char *file, pkgdata_t *p)
{
    memset(p, 0, sizeof *p);
    size_t n = 0;
    unsigned char *raw = slurp(file, &n);
    if(!raw) return PKG_E_READ;
    if(n >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        size_t tn = 0;
        unsigned char *t = gunzip(raw, n, &tn);
        free(raw);
        if(!t) return PKG_E_FORMAT;
        raw = t;
        n = tn;
    }
    p->buf = raw;
    p->ent = calloc(PKG_MAX_FILES, sizeof *p->ent);
    if(!p->ent) { pkg_free(p); return PKG_E_READ; }
    for(size_t off = 0; off + 512 <= n;) {
        const unsigned char *h = raw + off;
        bool zero = true;
        for(int i = 0; i < 512 && zero; i++) zero = !h[i];
        if(zero) break;                                    /* fim do tar */
        if(!header_ok(h)) { pkg_free(p); return PKG_E_FORMAT; }
        size_t size = octal((const char *)h + 124, 12);
        char type = (char)h[156];
        size_t body = (size + 511) / 512 * 512;
        if(off + 512 + body > n) { pkg_free(p); return PKG_E_FORMAT; }
        char name[260] = "";
        if(!memcmp(h + 257, "ustar", 5) && h[345])
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        else snprintf(name, sizeof name, "%.100s", (const char *)h);
        off += 512 + body;
        if(type == 'x' || type == 'g') continue;           /* cabeçalho pax: só metadados */
        if(type == '\0') type = '0';
        if(type != '0' && type != '5') { pkg_free(p); return PKG_E_UNSAFE; }   /* link, aparelho... */
        if(!strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name + 2) + 1);
        if(!safe_path(name)) { pkg_free(p); return PKG_E_UNSAFE; }
        /* a primeira parte é a pasta do app (o id); tudo fica dentro dela */
        char *slash = strchr(name, '/');
        size_t idl = slash ? (size_t)(slash - name) : strlen(name);
        if(idl >= sizeof p->id) { pkg_free(p); return PKG_E_UNSAFE; }
        if(!p->id[0]) { memcpy(p->id, name, idl); p->id[idl] = 0; }
        if(strncmp(name, p->id, idl) || strlen(p->id) != idl || !pkg_valid_id(p->id)) { pkg_free(p); return PKG_E_UNSAFE; }
        const char *rest = slash ? slash + 1 : "";
        if(!*rest) {
            if(type != '5') { pkg_free(p); return PKG_E_UNSAFE; }   /* arquivo com o nome da pasta */
            continue;
        }
        if(p->n >= PKG_MAX_FILES) { pkg_free(p); return PKG_E_BIG; }
        p->bytes += size;
        if(p->bytes > PKG_MAX_BYTES) { pkg_free(p); return PKG_E_BIG; }
        entry_t *e = &p->ent[p->n++];
        snprintf(e->path, sizeof e->path, "%s", rest);
        size_t L = strlen(e->path);
        if(L && e->path[L - 1] == '/') e->path[L - 1] = 0;
        e->type = type;
        e->data = h + 512;
        e->size = size;
    }
    if(!p->id[0]) { pkg_free(p); return PKG_E_FORMAT; }
    return PKG_OK;
}

/* o .desktop do pacote: um arquivo na raiz da pasta do app */
static const entry_t *find_desktop(const pkgdata_t *p)
{
    for(int i = 0; i < p->n; i++) {
        const entry_t *e = &p->ent[i];
        size_t L = strlen(e->path);
        if(e->type == '0' && !strchr(e->path, '/') && L > 8 && !strcmp(e->path + L - 8, ".desktop")) return e;
    }
    return NULL;
}

static void field(const entry_t *d, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    bool in = false;
    for(size_t i = 0; i < d->size;) {
        size_t j = i;
        while(j < d->size && d->data[j] != '\n') j++;
        const char *l = (const char *)d->data + i;
        size_t L = j - i;
        if(L && l[L - 1] == '\r') L--;
        if(L && l[0] == '[') in = L == 15 && !memcmp(l, "[Desktop Entry]", 15);
        else if(in && L > kl && !memcmp(l, key, kl) && l[kl] == '=') {
            size_t v = L - kl - 1;
            if(v >= n) v = n - 1;
            memcpy(out, l + kl + 1, v);
            out[v] = 0;
            return;
        }
        i = j + 1;
    }
}

/* Name[pt_BR], Name[pt] ou Name */
static void lfield(const entry_t *d, const char *key, const char *lang, char *out, size_t n)
{
    char k[48];
    out[0] = 0;
    if(lang && *lang) {
        snprintf(k, sizeof k, "%s[%s]", key, lang);
        field(d, k, out, n);
        if(!out[0]) { snprintf(k, sizeof k, "%s[%.2s]", key, lang); field(d, k, out, n); }
    }
    if(!out[0]) field(d, key, out, n);
}

static void fill_info(const pkgdata_t *p, const entry_t *d, const char *apps_dir, const char *lang, pkg_info_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", p->id);
    lfield(d, "Name", lang, out->name, sizeof out->name);
    lfield(d, "Comment", lang, out->comment, sizeof out->comment);
    field(d, "Categories", out->categories, sizeof out->categories);
    out->bytes = p->bytes;
    out->files = p->n;
    char dir[600];
    struct stat st;
    snprintf(dir, sizeof dir, "%s/%s", apps_dir, p->id);
    out->installed = !stat(dir, &st) && S_ISDIR(st.st_mode);
}

pkg_err_t pkg_info(const char *file, const char *apps_dir, const char *lang, pkg_info_t *out)
{
    pkgdata_t p;
    pkg_err_t e = pkg_load(file, &p);
    if(e) return e;
    const entry_t *d = find_desktop(&p);
    if(!d) { pkg_free(&p); return PKG_E_NODESKTOP; }
    fill_info(&p, d, apps_dir, lang, out);
    pkg_free(&p);
    return out->name[0] ? PKG_OK : PKG_E_NODESKTOP;
}

/* ------------------------------------------------------------ disco */

static int rm_tree(const char *path)
{
    struct stat st;
    if(lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if(!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if(!d) return -1;
    struct dirent *e;
    int rc = 0;
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1024];
        if(snprintf(c, sizeof c, "%s/%s", path, e->d_name) >= (int)sizeof c) { rc = -1; continue; }
        if(rm_tree(c)) rc = -1;
    }
    closedir(d);
    return rmdir(path) || rc ? -1 : 0;
}

static int mkdirs(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    for(char *s = p + 1; *s; s++)
        if(*s == '/') { *s = 0; if(mkdir(p, 0755) && errno != EEXIST) return -1; *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

static int write_file(const char *path, const unsigned char *d, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if(fd < 0) return -1;
    for(size_t w = 0; w < n;) {
        ssize_t r = write(fd, d + w, n - w);
        if(r <= 0) { close(fd); return -1; }
        w += (size_t)r;
    }
    return close(fd);
}

pkg_err_t pkg_install(const char *file, const char *apps_dir, const char *lang, pkg_info_t *out)
{
    pkgdata_t p;
    pkg_err_t e = pkg_load(file, &p);
    if(e) return e;
    const entry_t *d = find_desktop(&p);
    if(!d) { pkg_free(&p); return PKG_E_NODESKTOP; }
    pkg_info_t info;
    fill_info(&p, d, apps_dir, lang, &info);
    if(!info.name[0]) { pkg_free(&p); return PKG_E_NODESKTOP; }
    char tmp[600], dst[600], old[600];
    snprintf(tmp, sizeof tmp, "%s/.%s.novo", apps_dir, p.id);
    snprintf(dst, sizeof dst, "%s/%s", apps_dir, p.id);
    snprintf(old, sizeof old, "%s/.%s.velho", apps_dir, p.id);
    rm_tree(tmp);
    rm_tree(old);
    if(mkdirs(tmp)) { pkg_free(&p); return PKG_E_WRITE; }
    for(int i = 0; i < p.n; i++) {
        const entry_t *en = &p.ent[i];
        char path[900];
        snprintf(path, sizeof path, "%s/%s", tmp, en->path);
        int rc;
        if(en->type == '5') rc = mkdirs(path);
        else {
            char *sl = strrchr(path, '/');
            *sl = 0;
            rc = mkdirs(path);
            *sl = '/';
            if(!rc) rc = write_file(path, en->data, en->size);
        }
        if(rc) { rm_tree(tmp); pkg_free(&p); return PKG_E_WRITE; }
    }
    pkg_free(&p);
    /* troca: a versão anterior sai só depois de a nova estar inteira no cartão */
    struct stat st;
    bool had = !stat(dst, &st);
    if(had && rename(dst, old)) { rm_tree(tmp); return PKG_E_WRITE; }
    if(rename(tmp, dst)) {
        if(had) rename(old, dst);
        rm_tree(tmp);
        return PKG_E_WRITE;
    }
    if(had) rm_tree(old);
    sync();
    info.installed = had;
    if(out) *out = info;
    return PKG_OK;
}

pkg_err_t pkg_uninstall(const char *apps_dir, const char *id)
{
    if(!pkg_valid_id(id)) return PKG_E_UNSAFE;
    char dir[600];
    struct stat st;
    snprintf(dir, sizeof dir, "%s/%s", apps_dir, id);
    if(stat(dir, &st) || !S_ISDIR(st.st_mode)) return PKG_E_NOTFOUND;
    if(rm_tree(dir)) return PKG_E_WRITE;
    sync();
    return PKG_OK;
}
