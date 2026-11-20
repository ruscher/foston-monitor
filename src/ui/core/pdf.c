/* pdf.c — texto de PDF (ver pdf.h).
 *
 * Objetos diretos e em object streams (/ObjStm, PDF 1.5+), streams
 * FlateDecode (inflate do lodepng), árvore de páginas com recursos herdados,
 * fontes com ToUnicode (bfchar/bfrange, códigos de 1 ou 2 bytes) ou
 * codificação simples (WinAnsi/Standard + /Differences por nome de glifo).
 * Operadores Tj TJ ' " Td TD Tm T* Tf BT ET e formulários (Do). Linhas do
 * mesmo parágrafo são unidas; hífen no fim da linha é retirado. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pdf.h"
#include "lodepng.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX (32 * 1024 * 1024)
#define MAXOBJ   200000

/* ---------------- arquivo e objetos ---------------- */

typedef struct {
    const char *d;
    size_t n;
    size_t *off;            /* offset de "N 0 obj" no arquivo (0 = ausente) */
    int nobj;
    /* objetos vindos de object streams: texto do objeto já extraído */
    char **sobj;
} pdf_t;

typedef struct { char *p; size_t n, cap; } sb_t;

static void sb_add(sb_t *b, const char *s, size_t n)
{
    if(b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 64 * 1024;
        while(c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if(!q) return;
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_u(sb_t *b, uint32_t c)
{
    char t[4];
    if(c < 0x80) { t[0] = (char)c; sb_add(b, t, 1); }
    else if(c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 0x3f)); sb_add(b, t, 2); }
    else if(c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | ((c >> 6) & 0x3f)); t[2] = (char)(0x80 | (c & 0x3f)); sb_add(b, t, 3); }
}

static bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
static bool is_delim(char c) { return strchr("()<>[]{}/%", c) != NULL; }

/* índice "N G obj" por varredura (resiste a xref quebrada) */
static void index_objects(pdf_t *p)
{
    p->nobj = 0;
    for(size_t i = 0; i + 5 < p->n; i++) {
        if(p->d[i] != 'o' || strncmp(p->d + i, "obj", 3) || (i + 3 < p->n && !is_ws(p->d[i + 3]) && p->d[i + 3] != '<'))
            continue;
        /* volta: espaço, geração, espaço, número */
        size_t j = i;
        while(j > 0 && is_ws(p->d[j - 1])) j--;
        while(j > 0 && isdigit((unsigned char)p->d[j - 1])) j--;
        while(j > 0 && is_ws(p->d[j - 1])) j--;
        size_t k = j;
        while(k > 0 && isdigit((unsigned char)p->d[k - 1])) k--;
        if(k == j || (k > 0 && !is_ws(p->d[k - 1]) && p->d[k - 1] != '>')) continue;
        long num = strtol(p->d + k, NULL, 10);
        if(num <= 0 || num >= MAXOBJ) continue;
        if(num >= p->nobj) {
            int nn = (int)num + 1;
            size_t *o = realloc(p->off, sizeof *o * (size_t)nn);
            char **s = realloc(p->sobj, sizeof *s * (size_t)nn);
            if(!o || !s) { if(o) p->off = o; if(s) p->sobj = s; return; }
            p->off = o;
            p->sobj = s;
            for(int x = p->nobj; x < nn; x++) { p->off[x] = 0; p->sobj[x] = NULL; }
            p->nobj = nn;
        }
        p->off[num] = i + 3;           /* depois de "obj" (a última definição vale) */
    }
}

/* início do corpo do objeto n (texto), ou NULL */
static const char *obj_body(pdf_t *p, int n)
{
    if(n <= 0 || n >= p->nobj) return NULL;
    if(p->sobj[n]) return p->sobj[n];
    if(!p->off[n]) return NULL;
    return p->d + p->off[n];
}

static const char *skip_ws(const char *s)
{
    for(;;) {
        while(*s && is_ws(*s)) s++;
        if(*s == '%') { while(*s && *s != '\n' && *s != '\r') s++; continue; }
        return s;
    }
}

/* fim de um valor PDF começando em s (dicionário, array, string, nome, número, ref) */
static const char *value_end(const char *s)
{
    s = skip_ws(s);
    if(s[0] == '<' && s[1] == '<') {
        int depth = 0;
        while(*s) {
            if(s[0] == '<' && s[1] == '<') { depth++; s += 2; continue; }
            if(s[0] == '>' && s[1] == '>') { depth--; s += 2; if(!depth) return s; continue; }
            if(*s == '(') { int d = 0; while(*s) { if(*s == '\\' && s[1]) { s += 2; continue; } if(*s == '(') d++; else if(*s == ')' && !--d) { s++; break; } s++; } continue; }
            s++;
        }
        return s;
    }
    if(*s == '[') {
        int depth = 0;
        while(*s) {
            if(*s == '[') depth++;
            else if(*s == ']' && !--depth) return s + 1;
            else if(*s == '(') { int d = 0; while(*s) { if(*s == '\\' && s[1]) { s += 2; continue; } if(*s == '(') d++; else if(*s == ')' && !--d) break; s++; } }
            s++;
        }
        return s;
    }
    if(*s == '(') { int d = 0; while(*s) { if(*s == '\\' && s[1]) { s += 2; continue; } if(*s == '(') d++; else if(*s == ')' && !--d) return s + 1; s++; } return s; }
    if(*s == '<') { const char *e = strchr(s, '>'); return e ? e + 1 : s + strlen(s); }
    if(*s == '/') { s++; while(*s && !is_ws(*s) && !is_delim(*s)) s++; return s; }
    /* número, número G R, true, null */
    const char *e = s;
    while(*e && !is_ws(*e) && !is_delim(*e)) e++;
    const char *t = skip_ws(e);
    if(isdigit((unsigned char)*s) && isdigit((unsigned char)*t)) {
        const char *g = t;
        while(isdigit((unsigned char)*g)) g++;
        const char *r = skip_ws(g);
        if(*r == 'R' && (is_ws(r[1]) || is_delim(r[1]) || !r[1])) return r + 1;
    }
    return e;
}

/* valor de /Key no dicionário que começa em dict ("<<") — só no nível de cima */
static const char *dict_get(const char *dict, const char *key)
{
    if(!dict) return NULL;
    const char *s = skip_ws(dict);
    if(s[0] != '<' || s[1] != '<') return NULL;
    s += 2;
    size_t kl = strlen(key);
    for(;;) {
        s = skip_ws(s);
        if(!*s || (s[0] == '>' && s[1] == '>')) return NULL;
        if(*s != '/') { s = value_end(s); continue; }
        const char *name = s + 1, *ne = name;
        while(*ne && !is_ws(*ne) && !is_delim(*ne)) ne++;
        const char *v = skip_ws(ne);
        if((size_t)(ne - name) == kl && !strncmp(name, key, kl)) return v;
        s = value_end(v);
    }
}

/* resolve "N G R" para o corpo do objeto; outros valores voltam como estão */
static const char *resolve(pdf_t *p, const char *v)
{
    for(int hop = 0; v && hop < 8; hop++) {
        v = skip_ws(v);
        if(!isdigit((unsigned char)*v)) return v;
        char *e;
        long n = strtol(v, &e, 10);
        const char *t = skip_ws(e);
        if(!isdigit((unsigned char)*t)) return v;
        strtol(t, &e, 10);
        t = skip_ws(e);
        if(*t != 'R') return v;
        v = obj_body(p, (int)n);
    }
    return v;
}

static int ref_num(const char *v)
{
    v = skip_ws(v);
    return isdigit((unsigned char)*v) ? (int)strtol(v, NULL, 10) : 0;
}

static bool name_is(const char *v, const char *name)
{
    v = skip_ws(v);
    size_t l = strlen(name);
    return v && *v == '/' && !strncmp(v + 1, name, l) && (is_ws(v[1 + l]) || is_delim(v[1 + l]) || !v[1 + l]);
}

static long num_val(pdf_t *p, const char *v)
{
    v = resolve(p, v);
    return v ? strtol(skip_ws(v), NULL, 10) : 0;
}

/* dados de um stream (decodificados se FlateDecode); malloc */
static unsigned char *stream_data(pdf_t *p, const char *body, size_t *len)
{
    if(!body) return NULL;
    const char *dict = skip_ws(body);
    const char *de = value_end(dict);
    const char *s = skip_ws(de);
    if(strncmp(s, "stream", 6)) return NULL;
    s += 6;
    if(*s == '\r') s++;
    if(*s == '\n') s++;
    long declared = num_val(p, dict_get(dict, "Length"));
    size_t avail = (size_t)(p->d + p->n - s);
    if((const char *)s < p->d || (const char *)s > p->d + p->n) {
        /* objeto vindo de ObjStm não tem stream */
        return NULL;
    }
    size_t raw = declared > 0 && (size_t)declared <= avail ? (size_t)declared : 0;
    if(!raw) {
        const char *e = memmem(s, avail, "endstream", 9);
        raw = e ? (size_t)(e - s) : 0;
    }
    const char *filter = dict_get(dict, "Filter");
    if(filter && *skip_ws(filter) == '[') filter = skip_ws(filter) + 1;
    unsigned char *out = NULL;
    size_t on = 0;
    if(!filter) {
        out = malloc(raw + 1);
        if(!out) return NULL;
        memcpy(out, s, raw);
        on = raw;
    }
    else if(name_is(filter, "FlateDecode")) {
        if(lodepng_zlib_decompress(&out, &on, (const unsigned char *)s, raw, &lodepng_default_decompress_settings)) {
            /* zlib com checksum ruim: tenta só o deflate */
            free(out);
            out = NULL;
            on = 0;
            if(raw < 2 || lodepng_inflate(&out, &on, (const unsigned char *)s + 2, raw - 2, &lodepng_default_decompress_settings)) {
                free(out);
                return NULL;
            }
        }
        unsigned char *o2 = realloc(out, on + 1);
        if(!o2) { free(out); return NULL; }
        out = o2;
    }
    else return NULL;                         /* LZW, DCT (imagem), etc. */
    out[on] = 0;
    *len = on;
    return out;
}

/* objetos dentro de /Type /ObjStm */
static void load_objstreams(pdf_t *p)
{
    int n0 = p->nobj;
    for(int i = 1; i < n0; i++) {
        const char *b = obj_body(p, i);
        if(!b || p->sobj[i]) continue;
        const char *dict = skip_ws(b);
        if(dict[0] != '<' || !name_is(dict_get(dict, "Type") ? dict_get(dict, "Type") : "", "ObjStm")) continue;
        long count = num_val(p, dict_get(dict, "N")), first = num_val(p, dict_get(dict, "First"));
        size_t len;
        unsigned char *data = stream_data(p, b, &len);
        if(!data || count <= 0 || first <= 0 || (size_t)first > len) { free(data); continue; }
        const char *h = (const char *)data;
        long nums[4096], offs[4096];
        if(count > 4096) count = 4096;
        for(long k = 0; k < count; k++) {
            char *e;
            nums[k] = strtol(h, &e, 10);
            offs[k] = strtol(e, &e, 10);
            h = e;
        }
        for(long k = 0; k < count; k++) {
            long num = nums[k];
            size_t st = (size_t)first + (size_t)offs[k];
            size_t en = k + 1 < count ? (size_t)first + (size_t)offs[k + 1] : len;
            if(num <= 0 || num >= MAXOBJ || st >= len || en > len || en < st) continue;
            if(num >= p->nobj) {
                int nn = (int)num + 1;
                size_t *o = realloc(p->off, sizeof *o * (size_t)nn);
                char **sv = realloc(p->sobj, sizeof *sv * (size_t)nn);
                if(!o || !sv) { if(o) p->off = o; if(sv) p->sobj = sv; continue; }
                p->off = o;
                p->sobj = sv;
                for(int x = p->nobj; x < nn; x++) { p->off[x] = 0; p->sobj[x] = NULL; }
                p->nobj = nn;
            }
            if(p->off[num] || p->sobj[num]) continue;      /* objeto direto tem prioridade */
            p->sobj[num] = strndup((const char *)data + st, en - st);
        }
        free(data);
    }
}

/* ---------------- fontes ---------------- */

typedef struct {
    int ref;                /* objeto da fonte */
    int bytes;              /* 1 ou 2 bytes por código */
    uint32_t map[65536 > 256 ? 256 : 256];   /* simples: código -> Unicode */
    uint32_t *wide;         /* ToUnicode de 2 bytes: 65536 entradas */
    bool has_tu;
} font_t;

#define MAX_FONTS 64
static font_t fonts[MAX_FONTS];
static int n_fonts;

static const uint16_t winansi_hi[32] = {
    0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178,
};

/* nomes de glifos comuns (Adobe Glyph List, parte latina) */
static uint32_t glyph_unicode(const char *n, size_t l)
{
    static const struct { const char *n; uint32_t u; } g[] = {
        {"space",32},{"exclam",33},{"quotedbl",34},{"numbersign",35},{"dollar",36},{"percent",37},{"ampersand",38},
        {"quotesingle",39},{"quoteright",0x2019},{"parenleft",40},{"parenright",41},{"asterisk",42},{"plus",43},
        {"comma",44},{"hyphen",45},{"period",46},{"slash",47},{"zero",48},{"one",49},{"two",50},{"three",51},
        {"four",52},{"five",53},{"six",54},{"seven",55},{"eight",56},{"nine",57},{"colon",58},{"semicolon",59},
        {"less",60},{"equal",61},{"greater",62},{"question",63},{"at",64},{"bracketleft",91},{"backslash",92},
        {"bracketright",93},{"underscore",95},{"quoteleft",0x2018},{"braceleft",123},{"bar",124},{"braceright",125},
        {"asciitilde",126},{"endash",0x2013},{"emdash",0x2014},{"quotedblleft",0x201c},{"quotedblright",0x201d},
        {"bullet",0x2022},{"ellipsis",0x2026},{"fi",0xfb01},{"fl",0xfb02},{"ff",0xfb00},{"ffi",0xfb03},{"ffl",0xfb04},
        {"aacute",0xe1},{"agrave",0xe0},{"acircumflex",0xe2},{"atilde",0xe3},{"adieresis",0xe4},{"ccedilla",0xe7},
        {"eacute",0xe9},{"egrave",0xe8},{"ecircumflex",0xea},{"iacute",0xed},{"ocircumflex",0xf4},{"oacute",0xf3},
        {"otilde",0xf5},{"uacute",0xfa},{"udieresis",0xfc},{"ntilde",0xf1},{"Aacute",0xc1},{"Agrave",0xc0},
        {"Acircumflex",0xc2},{"Atilde",0xc3},{"Ccedilla",0xc7},{"Eacute",0xc9},{"Ecircumflex",0xca},{"Iacute",0xcd},
        {"Oacute",0xd3},{"Ocircumflex",0xd4},{"Otilde",0xd5},{"Uacute",0xda},{"Ntilde",0xd1},{"ordfeminine",0xaa},
        {"ordmasculine",0xba},{"degree",0xb0},{"guillemotleft",0xab},{"guillemotright",0xbb},{"minus",0x2212},
        {"dotlessi",0x131},{"germandbls",0xdf},{"copyright",0xa9},{"registered",0xae},{"section",0xa7},
    };
    if(l == 1) return (unsigned char)n[0];
    if(l == 7 && !strncmp(n, "uni", 3)) return (uint32_t)strtoul(n + 3, NULL, 16);
    for(size_t i = 0; i < sizeof g / sizeof g[0]; i++)
        if(strlen(g[i].n) == l && !strncmp(g[i].n, n, l)) return g[i].u;
    return 0;
}

static uint32_t hexval(const char *s, const char *e)
{
    uint32_t v = 0;
    for(; s < e; s++) {
        int c = *s;
        if(c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
        else if((c | 32) >= 'a' && (c | 32) <= 'f') v = v * 16 + (uint32_t)((c | 32) - 'a' + 10);
    }
    return v;
}

static void tu_set(font_t *f, uint32_t code, uint32_t u)
{
    if(f->bytes == 2) {
        if(!f->wide) f->wide = calloc(65536, sizeof *f->wide);
        if(f->wide && code < 65536) f->wide[code] = u;
    }
    else if(code < 256) f->map[code] = u;
}

/* primeiro código UTF-16BE de <....> (ignora pares substitutos) */
static uint32_t tu_value(const char *s, const char *e)
{
    return e - s >= 4 ? hexval(s, s + 4) : hexval(s, e);
}

static void parse_cmap(font_t *f, const char *c)
{
    const char *cs = strstr(c, "begincodespacerange");
    if(cs) {
        const char *lt = strchr(cs, '<'), *gt = lt ? strchr(lt, '>') : NULL;
        if(lt && gt) f->bytes = (gt - lt - 1) >= 4 ? 2 : 1;
    }
    for(const char *b = strstr(c, "beginbfchar"); b; b = strstr(b + 1, "beginbfchar")) {
        const char *end = strstr(b, "endbfchar");
        for(const char *s = b; end && s < end;) {
            const char *a1 = strchr(s, '<'); if(!a1 || a1 > end) break;
            const char *a2 = strchr(a1, '>'); if(!a2) break;
            const char *b1 = strchr(a2, '<'); if(!b1 || b1 > end) break;
            const char *b2 = strchr(b1, '>'); if(!b2) break;
            tu_set(f, hexval(a1 + 1, a2), tu_value(b1 + 1, b2));
            s = b2 + 1;
        }
    }
    for(const char *b = strstr(c, "beginbfrange"); b; b = strstr(b + 1, "beginbfrange")) {
        const char *end = strstr(b, "endbfrange");
        for(const char *s = b; end && s < end;) {
            const char *a1 = strchr(s, '<'); if(!a1 || a1 > end) break;
            const char *a2 = strchr(a1, '>'); if(!a2) break;
            const char *c1 = strchr(a2, '<'); if(!c1 || c1 > end) break;
            const char *c2 = strchr(c1, '>'); if(!c2) break;
            uint32_t lo = hexval(a1 + 1, a2), hi = hexval(c1 + 1, c2);
            const char *v = skip_ws(c2 + 1);
            if(*v == '[') {                                  /* lista de destinos */
                const char *ve = strchr(v, ']');
                uint32_t k = lo;
                for(const char *q = v; ve && q < ve && k <= hi;) {
                    const char *d1 = strchr(q, '<'); if(!d1 || d1 > ve) break;
                    const char *d2 = strchr(d1, '>'); if(!d2) break;
                    tu_set(f, k++, tu_value(d1 + 1, d2));
                    q = d2 + 1;
                }
                s = ve ? ve + 1 : end;
            }
            else if(*v == '<') {
                const char *d2 = strchr(v, '>');
                if(!d2) break;
                uint32_t u = tu_value(v + 1, d2);
                for(uint32_t k = lo; k <= hi && k - lo < 65536; k++) tu_set(f, k, u + (k - lo));
                s = d2 + 1;
            }
            else s = v + 1;
        }
    }
}

static font_t *get_font(pdf_t *p, int ref)
{
    for(int i = 0; i < n_fonts; i++) if(fonts[i].ref == ref) return &fonts[i];
    if(n_fonts == MAX_FONTS) return &fonts[MAX_FONTS - 1];
    font_t *f = &fonts[n_fonts++];
    memset(f, 0, sizeof *f);
    f->ref = ref;
    f->bytes = 1;
    const char *d = obj_body(p, ref);
    const char *sub = d ? dict_get(skip_ws(d), "Subtype") : NULL;
    if(sub && name_is(sub, "Type0")) f->bytes = 2;
    /* codificação simples: WinAnsi (e Standard, quase igual em ASCII) */
    for(int c = 32; c < 256; c++) f->map[c] = c >= 0x80 && c < 0xa0 ? winansi_hi[c - 0x80] : (uint32_t)c;
    const char *enc = d ? resolve(p, dict_get(skip_ws(d), "Encoding")) : NULL;
    const char *diff = enc && enc[0] == '<' ? dict_get(enc, "Differences") : NULL;
    if(diff && *(diff = skip_ws(diff)) == '[') {
        int code = 0;
        const char *e = value_end(diff);
        for(const char *s = diff + 1; s < e;) {
            s = skip_ws(s);
            if(isdigit((unsigned char)*s)) { code = (int)strtol(s, (char **)&s, 10); continue; }
            if(*s == '/') {
                const char *n = s + 1, *ne = n;
                while(*ne && !is_ws(*ne) && !is_delim(*ne)) ne++;
                uint32_t u = glyph_unicode(n, (size_t)(ne - n));
                if(code >= 0 && code < 256 && u) f->map[code] = u;
                code++;
                s = ne;
                continue;
            }
            s++;
        }
    }
    const char *tu = d ? dict_get(skip_ws(d), "ToUnicode") : NULL;
    if(tu) {
        size_t len;
        unsigned char *cm = stream_data(p, resolve(p, tu), &len);
        if(cm) { parse_cmap(f, (const char *)cm); f->has_tu = true; free(cm); }
    }
    return f;
}

/* ---------------- conteúdo das páginas ---------------- */

typedef struct {
    sb_t *out;
    font_t *font;
    float y_last, size;
    float tly, leading;      /* matriz de linha: y da linha atual, entrelinha (TL) */
    bool have_y, line_has_text;
    float line_gap;          /* distância típica entre linhas */
    int newlines_pending;    /* 1 = mesma frase/linha nova, 2 = parágrafo */
} txt_t;

static void emit_break(txt_t *t)
{
    if(!t->line_has_text) return;
    t->newlines_pending = t->newlines_pending ? t->newlines_pending : 1;
}

static void flush_break(txt_t *t)
{
    sb_t *o = t->out;
    if(!t->newlines_pending || !o->n) { t->newlines_pending = 0; return; }
    if(t->newlines_pending >= 2) {
        while(o->n && o->p[o->n - 1] == ' ') o->n--;
        sb_add(o, "\n\n", 2);
    }
    else {
        /* mesma frase: junta; tira hífen de quebra de palavra */
        if(o->n >= 2 && o->p[o->n - 1] == '-' && isalpha((unsigned char)o->p[o->n - 2])) o->n--;
        else if(o->p[o->n - 1] != ' ' && o->p[o->n - 1] != '\n') sb_add(o, " ", 1);
    }
    o->p[o->n] = 0;
    t->newlines_pending = 0;
}

static void move_y(txt_t *t, float y)
{
    if(t->have_y) {
        float dy = t->y_last - y;
        if(dy < 0) dy = -dy;
        if(dy > 0.5f) {
            float lh = t->size > 0 ? t->size * 1.2f : 12;
            if(t->line_gap <= 0) t->line_gap = t->size > 2 && dy > t->size * 1.6f ? t->size * 1.2f : dy;
            else if(dy < t->line_gap && dy > t->size * 0.8f) t->line_gap = dy;
            emit_break(t);
            if(dy > (t->line_gap > lh ? t->line_gap : lh) * 1.45f) t->newlines_pending = 2;
        }
    }
    t->y_last = y;
    t->have_y = true;
}

static void show_string(txt_t *t, const unsigned char *s, size_t n)
{
    if(!t->font || !n) return;
    flush_break(t);
    font_t *f = t->font;
    for(size_t i = 0; i < n;) {
        uint32_t code, u = 0;
        if(f->bytes == 2 && i + 1 < n) { code = (uint32_t)s[i] << 8 | s[i + 1]; i += 2; u = f->wide ? f->wide[code] : 0; }
        else { code = s[i++]; u = f->map[code & 0xff]; }
        if(!u) continue;
        if(u == ' ' && t->out->n && t->out->p[t->out->n - 1] == ' ') continue;   /* sem espaço duplo */
        if(u >= 0xfb00 && u <= 0xfb04) {                 /* ligaduras -> letras */
            static const char *lig[] = { "ff", "fi", "fl", "ffi", "ffl" };
            sb_add(t->out, lig[u - 0xfb00], strlen(lig[u - 0xfb00]));
        }
        else if(u == 0xa0) sb_add(t->out, " ", 1);
        else if(u >= 0x20) sb_u(t->out, u);
        t->line_has_text = true;
    }
}

/* string PDF literal (...) ou hex <...> -> bytes */
static size_t pdf_string(const char *s, const char *e, unsigned char *out, size_t max)
{
    size_t n = 0;
    if(*s == '<') {
        int hi = -1;
        for(s++; s < e && *s != '>' && n < max; s++) {
            int c = *s, v;
            if(c >= '0' && c <= '9') v = c - '0';
            else if((c | 32) >= 'a' && (c | 32) <= 'f') v = (c | 32) - 'a' + 10;
            else continue;
            if(hi < 0) hi = v; else { out[n++] = (unsigned char)(hi << 4 | v); hi = -1; }
        }
        if(hi >= 0 && n < max) out[n++] = (unsigned char)(hi << 4);
        return n;
    }
    int depth = 0;
    for(; s < e && n < max; s++) {
        char c = *s;
        if(c == '(') { if(depth++) out[n++] = '('; continue; }
        if(c == ')') { if(--depth) out[n++] = ')'; else break; continue; }
        if(c == '\\' && s + 1 < e) {
            c = *++s;
            if(c >= '0' && c <= '7') {
                int v = 0, k = 0;
                while(k < 3 && s < e && *s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; k++; }
                s--;
                out[n++] = (unsigned char)v;
            }
            else if(c == 'n') out[n++] = '\n';
            else if(c == 'r') out[n++] = '\r';
            else if(c == 't') out[n++] = '\t';
            else if(c == '\r' || c == '\n') { if(c == '\r' && s + 1 < e && s[1] == '\n') s++; }
            else out[n++] = (unsigned char)c;
            continue;
        }
        out[n++] = (unsigned char)c;
    }
    return n;
}

static void run_content(pdf_t *p, const char *res, const char *c, size_t len, txt_t *t, int depth);

static font_t *font_by_name(pdf_t *p, const char *res, const char *name, size_t nl)
{
    const char *fd = resolve(p, dict_get(res, "Font"));
    if(!fd) return NULL;
    char key[64];
    snprintf(key, sizeof key, "%.*s", (int)nl, name);
    const char *fr = dict_get(fd, key);
    if(!fr) return NULL;
    int ref = ref_num(fr);
    return ref ? get_font(p, ref) : NULL;
}

static void do_xobject(pdf_t *p, const char *res, const char *name, size_t nl, txt_t *t, int depth)
{
    if(depth > 4) return;
    const char *xd = resolve(p, dict_get(res, "XObject"));
    if(!xd) return;
    char key[64];
    snprintf(key, sizeof key, "%.*s", (int)nl, name);
    const char *xr = dict_get(xd, key);
    if(!xr) return;
    const char *body = resolve(p, xr);
    if(!body) return;
    const char *dict = skip_ws(body);
    const char *sub = dict_get(dict, "Subtype");
    if(!sub || !name_is(sub, "Form")) return;
    size_t len;
    unsigned char *data = stream_data(p, body, &len);
    if(!data) return;
    const char *r2 = resolve(p, dict_get(dict, "Resources"));
    run_content(p, r2 ? r2 : res, (const char *)data, len, t, depth + 1);
    free(data);
}

static void run_content(pdf_t *p, const char *res, const char *c, size_t len, txt_t *t, int depth)
{
    const char *s = c, *end = c + len;
    float stack[8];
    int ns = 0;
    const char *last_name = NULL;
    size_t last_name_len = 0;
    const char *str_s = NULL, *str_e = NULL;     /* último operando string/array */
    static unsigned char sbuf[8192];
    while(s < end) {
        while(s < end && is_ws(*s)) s++;
        if(s >= end) break;
        char ch = *s;
        if(ch == '%') { while(s < end && *s != '\n' && *s != '\r') s++; continue; }
        if(ch == '(' || ch == '<' || ch == '[') {
            if(ch == '<' && s + 1 < end && s[1] == '<') {    /* dicionário inline (BDC/BI) */
                const char *e = value_end(s);
                s = e > s ? e : s + 2;
                continue;
            }
            const char *e = value_end(s);
            if(e <= s || e > end) e = end;
            str_s = s;
            str_e = e;
            s = e;
            continue;
        }
        if(ch == '/') {
            const char *n = s + 1, *ne = n;
            while(ne < end && !is_ws(*ne) && !is_delim(*ne)) ne++;
            last_name = n;
            last_name_len = (size_t)(ne - n);
            s = ne;
            continue;
        }
        if(isdigit((unsigned char)ch) || ch == '-' || ch == '+' || ch == '.') {
            char *e;
            float v = strtof(s, &e);
            if(e == s) { s++; continue; }
            if(ns == 8) { memmove(stack, stack + 1, sizeof stack[0] * 7); ns--; }
            stack[ns++] = v;
            s = e;
            continue;
        }
        /* operador */
        const char *op = s;
        while(s < end && !is_ws(*s) && !is_delim(*s)) s++;
        size_t ol = (size_t)(s - op);
        if(ol == 2 && !strncmp(op, "BI", 2)) {               /* imagem inline: pula até EI */
            const char *ei = memmem(s, (size_t)(end - s), "EI", 2);
            s = ei ? ei + 2 : end;
        }
        else if(ol == 2 && !strncmp(op, "Tf", 2)) {
            if(last_name) t->font = font_by_name(p, res, last_name, last_name_len);
            if(ns) t->size = stack[ns - 1] < 0 ? -stack[ns - 1] : stack[ns - 1];
        }
        else if(ol == 2 && !strncmp(op, "TL", 2)) {
            if(ns) t->leading = stack[ns - 1];
        }
        else if(ol == 2 && (!strncmp(op, "Td", 2) || !strncmp(op, "TD", 2))) {
            /* relativo ao início da linha atual (BT zera a matriz) */
            if(ns >= 2 && op[1] == 'D') t->leading = -stack[ns - 1];
            if(ns >= 2 && stack[ns - 1] != 0) { t->tly += stack[ns - 1]; move_y(t, t->tly); }
            else if(ns >= 2 && stack[ns - 2] > 0 && t->line_has_text && t->size > 0 && stack[ns - 2] > t->size * 0.3f) {
                /* deslocamento horizontal grande na mesma linha: espaço */
                flush_break(t);
                if(t->out->n && t->out->p[t->out->n - 1] != ' ') sb_add(t->out, " ", 1);
            }
        }
        else if(ol == 2 && !strncmp(op, "Tm", 2)) {
            if(ns >= 6) { t->tly = stack[ns - 1]; move_y(t, t->tly); }
        }
        else if(ol == 2 && !strncmp(op, "T*", 2)) {
            t->tly -= t->leading > 0 ? t->leading : (t->line_gap > 0 ? t->line_gap : t->size * 1.2f);
            move_y(t, t->tly);
        }
        else if(ol == 2 && !strncmp(op, "BT", 2)) t->tly = 0;
        else if((ol == 2 && !strncmp(op, "Tj", 2)) || (ol == 1 && (*op == '\'' || *op == '"'))) {
            if(*op == '\'' || *op == '"') {
                t->tly -= t->leading > 0 ? t->leading : (t->line_gap > 0 ? t->line_gap : t->size * 1.2f);
                move_y(t, t->tly);
            }
            if(str_s) show_string(t, sbuf, pdf_string(str_s, str_e, sbuf, sizeof sbuf));
        }
        else if(ol == 2 && !strncmp(op, "TJ", 2) && str_s && *str_s == '[') {
            for(const char *q = str_s + 1; q < str_e;) {
                q = skip_ws(q);
                if(q >= str_e || *q == ']') break;
                if(*q == '(' || *q == '<') {
                    const char *qe = value_end(q);
                    show_string(t, sbuf, pdf_string(q, qe, sbuf, sizeof sbuf));
                    q = qe;
                }
                else {
                    char *e;
                    float v = strtof(q, &e);
                    if(e == q) { q++; continue; }
                    if(v < -200 && t->out->n && t->out->p[t->out->n - 1] != ' ') sb_add(t->out, " ", 1);
                    q = e;
                }
            }
        }
        else if(ol == 2 && !strncmp(op, "Do", 2) && last_name) do_xobject(p, res, last_name, last_name_len, t, depth);
        if(ol) { ns = 0; str_s = NULL; }
        if(!ol) s++;
    }
}

/* páginas em ordem (árvore /Pages), herdando /Resources */
static void walk_pages(pdf_t *p, const char *node, const char *res, txt_t *t, pdf_text_t *out, int *cap, int depth)
{
    if(!node || depth > 32) return;
    const char *dict = skip_ws(node);
    const char *r = resolve(p, dict_get(dict, "Resources"));
    if(r && *r == '<') res = r;
    const char *kids = resolve(p, dict_get(dict, "Kids"));
    if(kids && *skip_ws(kids) == '[') {
        const char *e = value_end(kids);
        for(const char *s = skip_ws(kids) + 1; s < e;) {
            s = skip_ws(s);
            if(*s == ']' || s >= e) break;
            const char *ve = value_end(s);
            walk_pages(p, resolve(p, s), res, t, out, cap, depth + 1);
            s = ve > s ? ve : s + 1;
        }
        return;
    }
    /* folha: página */
    if(out->pages == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        size_t *po = realloc(out->page_off, sizeof *po * (size_t)*cap);
        if(!po) return;
        out->page_off = po;
    }
    if(t->out->n) { while(t->out->n && t->out->p[t->out->n - 1] == ' ') t->out->n--; sb_add(t->out, "\n\n", 2); }
    out->page_off[out->pages++] = t->out->n;
    t->have_y = false;
    t->line_has_text = false;
    t->newlines_pending = 0;
    const char *cont = dict_get(dict, "Contents");
    if(!cont || !res) return;
    cont = skip_ws(cont);
    const char *list = resolve(p, cont);
    bool is_array = list && *skip_ws(list) == '[';
    /* conteúdo pode ser um stream ou um array de streams (concatenados) */
    sb_t all = { 0 };
    if(is_array) {
        const char *e = value_end(list);
        for(const char *s = skip_ws(list) + 1; s < e;) {
            s = skip_ws(s);
            if(*s == ']' || s >= e) break;
            const char *ve = value_end(s);
            size_t len;
            unsigned char *d = stream_data(p, resolve(p, s), &len);
            if(d) { sb_add(&all, (const char *)d, len); sb_add(&all, "\n", 1); free(d); }
            s = ve > s ? ve : s + 1;
        }
    }
    else {
        size_t len;
        unsigned char *d = stream_data(p, list, &len);
        if(d) { sb_add(&all, (const char *)d, len); free(d); }
    }
    if(all.p) run_content(p, res, all.p, all.n, t, 0);
    free(all.p);
}

static void info_string(pdf_t *p, const char *info, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *v = info ? resolve(p, dict_get(info, key)) : NULL;
    if(!v || (*v != '(' && *v != '<')) return;
    unsigned char b[512];
    size_t l = pdf_string(v, value_end(v), b, sizeof b);
    sb_t s = { 0 };
    if(l >= 2 && b[0] == 0xfe && b[1] == 0xff)            /* UTF-16BE */
        for(size_t i = 2; i + 1 < l; i += 2) sb_u(&s, (uint32_t)b[i] << 8 | b[i + 1]);
    else
        for(size_t i = 0; i < l; i++) sb_u(&s, b[i] >= 0x80 && b[i] < 0xa0 ? winansi_hi[b[i] - 0x80] : b[i]);
    snprintf(out, n, "%s", s.p ? s.p : "");
    free(s.p);
}

bool pdf_extract(const char *path, pdf_text_t *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if(!f) { snprintf(err, errn, "não foi possível abrir"); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n < 64 || n > FILE_MAX) { fclose(f); snprintf(err, errn, "tamanho não suportado (máx. 32 MB)"); return false; }
    char *d = malloc((size_t)n + 1);
    if(!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); snprintf(err, errn, "erro de leitura"); return false; }
    fclose(f);
    d[n] = 0;
    pdf_t p = { d, (size_t)n, NULL, 0, NULL };
    bool ok = false;
    if(strncmp(d, "%PDF", 4)) { snprintf(err, errn, "não é um PDF"); goto out; }
    if(memmem(d, (size_t)n, "/Encrypt", 8)) { snprintf(err, errn, "PDF protegido (criptografado)"); goto out; }
    index_objects(&p);
    load_objstreams(&p);
    n_fonts = 0;

    /* catálogo: /Root do trailer (ou do xref stream) */
    const char *root = NULL;
    for(const char *tr = d + n; tr > d && !root;) {
        const char *k = NULL;
        for(const char *q = tr - 1; q > d; q--) if(!strncmp(q, "/Root", 5)) { k = q; break; }
        if(!k) break;
        root = resolve(&p, k + 5);
        if(root && !dict_get(skip_ws(root), "Pages")) root = NULL;
        tr = k;
    }
    if(!root) {
        for(int i = 1; i < p.nobj && !root; i++) {
            const char *b = obj_body(&p, i);
            const char *ty = b && *skip_ws(b) == '<' ? dict_get(skip_ws(b), "Type") : NULL;
            if(ty && name_is(ty, "Catalog")) root = b;
        }
    }
    if(!root) { snprintf(err, errn, "estrutura do PDF não reconhecida"); goto out; }
    const char *pages = resolve(&p, dict_get(skip_ws(root), "Pages"));
    if(!pages) { snprintf(err, errn, "PDF sem páginas"); goto out; }

    for(const char *q = d + n - 1; q > d; q--)
        if(!strncmp(q, "/Info", 5)) {
            const char *info = resolve(&p, q + 5);
            if(info && *skip_ws(info) == '<') {
                info_string(&p, skip_ws(info), "Title", out->title, sizeof out->title);
                info_string(&p, skip_ws(info), "Author", out->author, sizeof out->author);
            }
            break;
        }

    sb_t text = { 0 };
    txt_t t = { .out = &text };
    int cap = 0;
    walk_pages(&p, pages, NULL, &t, out, &cap, 0);
    size_t letters = 0;
    for(size_t i = 0; i < text.n; i++) if(!is_ws(text.p[i])) letters++;
    if(!out->pages) { snprintf(err, errn, "PDF sem páginas"); free(text.p); goto out; }
    if(letters < (size_t)out->pages * 3) {
        snprintf(err, errn, "o PDF não tem texto extraível (provavelmente só imagens/escaneado)");
        free(text.p);
        goto out;
    }
    out->text = text.p;
    out->len = text.n;
    ok = true;
out:
    for(int i = 0; i < n_fonts; i++) free(fonts[i].wide);
    n_fonts = 0;
    for(int i = 0; i < p.nobj; i++) free(p.sobj[i]);
    free(p.sobj);
    free(p.off);
    free(d);
    if(!ok) { free(out->page_off); out->page_off = NULL; out->pages = 0; }
    return ok;
}

void pdf_free(pdf_text_t *p)
{
    free(p->text);
    free(p->page_off);
    memset(p, 0, sizeof *p);
}
