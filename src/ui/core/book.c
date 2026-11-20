/* book.c — TXT e EPUB para texto puro (ver book.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE        /* strcasestr (musl) */
#endif
#include "book.h"
#include "lodepng.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAX (16 * 1024 * 1024)
#define TEXT_MAX (12 * 1024 * 1024)

/* ---------------- buffer de saída ---------------- */

typedef struct { char *p; size_t n, cap; } sbuf_t;

static bool sb_put(sbuf_t *b, const char *s, size_t n)
{
    if(b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 64 * 1024;
        while(c < b->n + n + 1) c *= 2;
        if(c > TEXT_MAX + 1) return false;
        char *q = realloc(b->p, c);
        if(!q) return false;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return true;
}

static void sb_utf8(sbuf_t *b, uint32_t c)
{
    char t[4];
    if(c < 0x80) { t[0] = (char)c; sb_put(b, t, 1); }
    else if(c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 0x3f)); sb_put(b, t, 2); }
    else if(c < 0x10000) {
        t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | ((c >> 6) & 0x3f)); t[2] = (char)(0x80 | (c & 0x3f));
        sb_put(b, t, 3);
    }
    else { t[0] = '?'; sb_put(b, t, 1); }   /* fora do plano básico: a fonte não tem */
}

/* no máximo uma linha em branco seguida */
static void sb_newlines(sbuf_t *b, int want)
{
    int have = 0;
    for(size_t i = b->n; i > 0 && b->p[i - 1] == '\n' && have < 2; i--) have++;
    if(!b->n) return;
    while(have < want) { sb_put(b, "\n", 1); have++; }
}

static void *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || n > FILE_MAX) { fclose(f); return NULL; }
    char *p = malloc((size_t)n + 1);
    if(p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if(p) { p[n] = 0; *len = (size_t)n; }
    return p;
}

static bool valid_utf8(const unsigned char *s, size_t n)
{
    for(size_t i = 0; i < n;) {
        unsigned char c = s[i];
        int l = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if(!l || i + l > n) return false;
        for(int k = 1; k < l; k++) if((s[i + k] & 0xc0) != 0x80) return false;
        i += l;
    }
    return true;
}

/* Windows-1252 0x80..0x9F (o resto é Latin-1 = Unicode) */
static const uint16_t cp1252[32] = {
    0x20ac, '?', 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, '?', 0x17d, '?',
    '?', 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, '?', 0x17e, 0x178,
};

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void title_from_name(book_t *b, const char *path)
{
    snprintf(b->title, sizeof b->title, "%s", base_name(path));
    char *dot = strrchr(b->title, '.');
    if(dot) *dot = 0;
}

/* ---------------- TXT ---------------- */

static bool load_txt(const char *path, book_t *bk, char *err, size_t errn)
{
    size_t n;
    unsigned char *raw = read_all(path, &n);
    if(!raw) { snprintf(err, errn, "não foi possível ler (máx. 16 MB)"); return false; }
    sbuf_t b = { 0 };
    size_t i = 0;
    if(n >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) i = 3;   /* BOM */
    bool utf8 = valid_utf8(raw + i, n - i);
    for(; i < n; i++) {
        unsigned char c = raw[i];
        if(c == '\r') { if(i + 1 < n && raw[i + 1] == '\n') continue; c = '\n'; }
        if(c == '\t') { sb_put(&b, "    ", 4); continue; }
        if(c < 0x20 && c != '\n') continue;
        if(utf8 || c < 0x80) sb_put(&b, (const char *)&c, 1);
        else sb_utf8(&b, c < 0xa0 ? cp1252[c - 0x80] : c);
    }
    free(raw);
    if(!b.p) { snprintf(err, errn, "arquivo vazio"); return false; }
    bk->text = b.p;
    bk->len = b.n;
    title_from_name(bk, path);
    return true;
}

/* ---------------- ZIP ---------------- */

typedef struct { const unsigned char *d; size_t n; } zip_t;

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* extrai um arquivo pelo nome (comparação exata); retorna malloc + NUL */
static char *zip_get(const zip_t *z, const char *name, size_t *outlen)
{
    if(z->n < 22) return NULL;
    size_t e = z->n - 22;
    size_t stop = z->n > 65557 ? z->n - 65557 : 0;
    while(e > stop && rd32(z->d + e) != 0x06054b50) e--;
    if(rd32(z->d + e) != 0x06054b50) return NULL;
    uint16_t count = rd16(z->d + e + 10);
    uint32_t cd = rd32(z->d + e + 16);
    size_t nl = strlen(name);
    for(uint16_t i = 0; i < count && cd + 46 <= z->n; i++) {
        const unsigned char *c = z->d + cd;
        if(rd32(c) != 0x02014b50) return NULL;
        uint16_t method = rd16(c + 10), fnl = rd16(c + 28), exl = rd16(c + 30), cml = rd16(c + 32);
        uint32_t csize = rd32(c + 20), usize = rd32(c + 24), lho = rd32(c + 42);
        if(fnl == nl && cd + 46 + fnl <= z->n && !memcmp(c + 46, name, nl)) {
            if(lho + 30 > z->n) return NULL;
            const unsigned char *l = z->d + lho;
            size_t data = lho + 30 + rd16(l + 26) + rd16(l + 28);
            if(data + csize > z->n || usize > TEXT_MAX) return NULL;
            unsigned char *out = NULL;
            size_t on = 0;
            if(method == 0) {
                out = malloc((size_t)csize + 1);
                if(!out) return NULL;
                memcpy(out, z->d + data, csize);
                on = csize;
            }
            else if(method == 8) {
                if(lodepng_inflate(&out, &on, z->d + data, csize, &lodepng_default_decompress_settings)) {
                    free(out);
                    return NULL;
                }
                unsigned char *o2 = realloc(out, on + 1);
                if(!o2) { free(out); return NULL; }
                out = o2;
            }
            else return NULL;
            out[on] = 0;
            if(outlen) *outlen = on;
            return (char *)out;
        }
        cd += 46u + fnl + exl + cml;
    }
    return NULL;
}

/* ---------------- XML mínimo ---------------- */

/* valor do atributo `att` dentro da tag que começa em `tag` (até '>') */
static bool attr(const char *tag, const char *att, char *out, size_t n)
{
    const char *end = strchr(tag, '>');
    size_t al = strlen(att);
    for(const char *p = tag; p && end && p < end; p++) {
        if(!strncmp(p, att, al) && (p == tag || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t' || p[-1] == ':')) {
            const char *q = p + al;
            while(*q == ' ') q++;
            if(*q != '=') continue;
            q++;
            while(*q == ' ') q++;
            char quote = *q;
            if(quote != '"' && quote != '\'') continue;
            const char *v = ++q;
            const char *ve = strchr(v, quote);
            if(!ve || ve > end) return false;
            size_t l = (size_t)(ve - v) < n - 1 ? (size_t)(ve - v) : n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return true;
        }
    }
    return false;
}

/* decodifica %XX de href (nomes com espaço etc.) */
static void url_decode(char *s)
{
    char *o = s;
    for(; *s; s++) {
        if(*s == '%' && s[1] && s[2]) {
            int hi = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
            int lo = s[2] <= '9' ? s[2] - '0' : (s[2] | 32) - 'a' + 10;
            *o++ = (char)(hi << 4 | lo);
            s += 2;
        }
        else *o++ = *s;
    }
    *o = 0;
}

static uint32_t entity(const char *s, int *used)
{
    static const struct { const char *n; uint32_t c; } ent[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "laquo", 0xab }, { "raquo", 0xbb },
        { "ldquo", 0x201c }, { "rdquo", 0x201d }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "copy", 0xa9 },
        { "ordm", 0xba }, { "ordf", 0xaa }, { "deg", 0xb0 }, { "middot", 0xb7 }, { "bull", 0x2022 },
    };
    const char *semi = strchr(s, ';');
    if(!semi || semi - s > 10) return 0;
    *used = (int)(semi - s) + 1;
    if(s[1] == '#') {
        unsigned long v = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
        return v == 0xa0 ? ' ' : (uint32_t)v;
    }
    for(size_t i = 0; i < sizeof ent / sizeof ent[0]; i++)
        if(strlen(ent[i].n) == (size_t)(semi - s - 1) && !strncmp(s + 1, ent[i].n, (size_t)(semi - s - 1)))
            return ent[i].c;
    return 0;
}

static bool tag_is(const char *t, const char *name)
{
    size_t l = strlen(name);
    if(*t == '/') t++;
    return !strncasecmp(t, name, l) && (t[l] == '>' || t[l] == ' ' || t[l] == '/' || t[l] == '\n' || t[l] == '\t');
}

/* XHTML -> texto: blocos viram parágrafos, <br> quebra a linha */
static void xhtml_text(sbuf_t *b, const char *x)
{
    bool space = true;                       /* último caractere foi espaço/início */
    const char *body = strcasestr(x, "<body");
    if(body) x = body;
    for(const char *p = x; *p;) {
        if(*p == '<') {
            const char *t = p + 1;
            if(!strncmp(t, "!--", 3)) { const char *e = strstr(t, "-->"); p = e ? e + 3 : t + strlen(t); continue; }
            if(tag_is(t, "script") || tag_is(t, "style") || tag_is(t, "head")) {
                if(*t != '/') {
                    char close[16];
                    snprintf(close, sizeof close, "</%.*s", (int)strcspn(t, " >/\n\t"), t);
                    const char *e = strcasestr(t, close);
                    p = e ? e : t + strlen(t);
                    const char *g = strchr(p, '>');
                    p = g ? g + 1 : p + strlen(p);
                    continue;
                }
            }
            if(tag_is(t, "br")) { sb_put(b, "\n", 1); space = true; }
            else if(tag_is(t, "p") || tag_is(t, "div") || tag_is(t, "li") || tag_is(t, "tr") ||
                    tag_is(t, "blockquote") || tag_is(t, "h1") || tag_is(t, "h2") || tag_is(t, "h3") ||
                    tag_is(t, "h4") || tag_is(t, "h5") || tag_is(t, "h6") || tag_is(t, "section")) {
                sb_newlines(b, 2);
                space = true;
            }
            const char *g = strchr(p, '>');
            p = g ? g + 1 : p + strlen(p);
            continue;
        }
        if(*p == '&') {
            int used = 0;
            uint32_t c = entity(p, &used);
            if(c) {
                if(c == ' ') { if(!space) { sb_put(b, " ", 1); space = true; } }
                else { sb_utf8(b, c); space = false; }
                p += used;
                continue;
            }
        }
        if(*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            if(!space && b->n && b->p[b->n - 1] != '\n') sb_put(b, " ", 1);
            space = true;
            p++;
            continue;
        }
        sb_put(b, p, 1);
        space = false;
        p++;
    }
    sb_newlines(b, 2);
}

/* tira tags e espaços de um trecho curto (título, autor) */
static void plain(char *dst, size_t n, const char *src, size_t len)
{
    sbuf_t b = { 0 };
    char *tmp = strndup(src, len);
    if(!tmp) { dst[0] = 0; return; }
    xhtml_text(&b, tmp);
    free(tmp);
    size_t s = 0, e = b.n;
    while(s < e && (b.p[s] == '\n' || b.p[s] == ' ')) s++;
    while(e > s && (b.p[e - 1] == '\n' || b.p[e - 1] == ' ')) e--;
    snprintf(dst, n, "%.*s", (int)(e - s), b.p ? b.p + s : "");
    free(b.p);
}

static bool load_epub(const char *path, book_t *bk, char *err, size_t errn)
{
    size_t n;
    unsigned char *raw = read_all(path, &n);
    if(!raw) { snprintf(err, errn, "não foi possível ler (máx. 16 MB)"); return false; }
    zip_t z = { raw, n };
    bool ok = false;
    char *cont = zip_get(&z, "META-INF/container.xml", NULL), *opf = NULL;
    char opf_path[512] = "", dir[512] = "";
    sbuf_t b = { 0 };
    if(!cont) { snprintf(err, errn, "EPUB sem META-INF/container.xml"); goto out; }
    const char *rf = cont;
    while((rf = strstr(rf, "<rootfile")) && rf[9] != ' ' && rf[9] != '\n' && rf[9] != '\t') rf++;  /* não <rootfiles> */
    if(!rf || !attr(rf, "full-path", opf_path, sizeof opf_path)) { snprintf(err, errn, "container.xml sem rootfile"); goto out; }
    opf = zip_get(&z, opf_path, NULL);
    if(!opf) { snprintf(err, errn, "OPF não encontrado: %s", opf_path); goto out; }
    snprintf(dir, sizeof dir, "%s", opf_path);
    char *sl = strrchr(dir, '/');
    if(sl) sl[1] = 0; else dir[0] = 0;

    const char *t = strstr(opf, "dc:title");
    if(t && (t = strchr(t, '>'))) { const char *e = strstr(t, "</"); if(e) plain(bk->title, sizeof bk->title, t + 1, (size_t)(e - t - 1)); }
    const char *a = strstr(opf, "dc:creator");
    if(a && (a = strchr(a, '>'))) { const char *e = strstr(a, "</"); if(e) plain(bk->author, sizeof bk->author, a + 1, (size_t)(e - a - 1)); }

    /* spine: idref -> manifest href, na ordem */
    const char *sp = strstr(opf, "<spine");
    int chapters = 0;
    for(const char *ir = sp ? strstr(sp, "<itemref") : NULL; ir; ir = strstr(ir + 1, "<itemref")) {
        char id[200], href[512], full[1024];
        if(!attr(ir, "idref", id, sizeof id)) continue;
        bool found = false;
        for(const char *it = strstr(opf, "<item"); it; it = strstr(it + 1, "<item")) {
            if(it[5] != ' ' && it[5] != '\n' && it[5] != '\t') continue;      /* não confunde com <itemref */
            char iid[200];
            if(attr(it, "id", iid, sizeof iid) && !strcmp(iid, id) && attr(it, "href", href, sizeof href)) { found = true; break; }
        }
        if(!found) continue;
        url_decode(href);
        char *hash = strchr(href, '#');
        if(hash) *hash = 0;
        snprintf(full, sizeof full, "%s%s", dir, href);
        char *x = zip_get(&z, full, NULL);
        if(!x) continue;
        xhtml_text(&b, x);
        free(x);
        chapters++;
    }
    if(!chapters || !b.p) { snprintf(err, errn, "nenhum capítulo de texto encontrado"); goto out; }
    bk->text = b.p;
    bk->len = b.n;
    b.p = NULL;
    if(!bk->title[0]) title_from_name(bk, path);
    ok = true;
out:
    free(b.p);
    free(cont);
    free(opf);
    free(raw);
    return ok;
}

bool book_is_supported(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && (!strcasecmp(d, ".txt") || !strcasecmp(d, ".epub"));
}

bool book_load(const char *path, book_t *b, char *err, size_t errn)
{
    memset(b, 0, sizeof *b);
    const char *d = strrchr(path, '.');
    if(d && !strcasecmp(d, ".epub")) return load_epub(path, b, err, errn);
    return load_txt(path, b, err, errn);
}

void book_free(book_t *b)
{
    free(b->text);
    memset(b, 0, sizeof *b);
}
