/* bgsearch.c - see bgsearch.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _FILE_OFFSET_BITS 64
#include "bgsearch.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXW 8                 /* words used from the query */
#define MAXCAND 40000

struct bgs_index {
    int fd;
    off_t size;
    bgs_header_t h;
    bgs_tile_t *tiles;         /* version 3: in memory (~60 KB for Brazil) */
    char corrected[256];       /* the text bgs_query used after a typing correction */
};

static int rd(const bgs_index_t *ix, off_t off, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(ix->fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

bgs_index_t *bgs_open(const char *path, char *err, size_t errlen)
{
    bgs_index_t *ix = calloc(1, sizeof *ix);
    struct stat st;
    if (!ix) { snprintf(err, errlen, "sem memória"); return NULL; }
    ix->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ix->fd < 0 || fstat(ix->fd, &st) || rd(ix, 0, &ix->h, sizeof ix->h)) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        goto fail;
    }
    ix->size = st.st_size;
    const bgs_header_t *h = &ix->h;
    if (h->magic != BGS_MAGIC || (h->version != BGS_VERSION && h->version != 2)
        || (off_t)h->off_entries + (off_t)h->nentries * (off_t)sizeof(bgs_entry_t) > ix->size
        || (off_t)h->off_tokens + (off_t)h->ntokens * (off_t)sizeof(bgs_token_t) > ix->size
        || (off_t)h->off_postings + (off_t)h->npostings * 4 > ix->size
        || (off_t)h->off_strings + h->strings_len > ix->size
        || (off_t)h->off_catidx + (BGS_C_COUNT + 1) * 4 + (off_t)h->ncatpairs * (h->version >= 3 ? 12 : 8) > ix->size
        || (h->version >= 3 && (off_t)h->off_tiles + (off_t)h->ntiles * (off_t)sizeof(bgs_tile_t) > ix->size)) {
        snprintf(err, errlen, "%s: não é um índice de busca do Big GPS (versão %u)", path, BGS_VERSION);
        goto fail;
    }
    if (h->version >= 3 && h->ntiles) {
        ix->tiles = malloc((size_t)h->ntiles * sizeof *ix->tiles);
        if (!ix->tiles || rd(ix, h->off_tiles, ix->tiles, (size_t)h->ntiles * sizeof *ix->tiles)) {
            snprintf(err, errlen, "%s: tabela de regiões ilegível", path);
            goto fail;
        }
    }
    return ix;
fail:
    bgs_close(ix);
    return NULL;
}

void bgs_close(bgs_index_t *ix)
{
    if (!ix) return;
    if (ix->fd >= 0) close(ix->fd);
    free(ix->tiles);
    free(ix);
}

uint32_t bgs_count(const bgs_index_t *ix) { return ix->h.nentries; }
uint32_t bgs_version(const bgs_index_t *ix) { return ix->h.version; }

static void str_at(const bgs_index_t *ix, uint32_t off, char *buf, size_t n)
{
    buf[0] = 0;
    if (off == 0xFFFFFFFFu || off >= ix->h.strings_len || n < 2) return;
    size_t want = n - 1;
    if (off + want > ix->h.strings_len) want = ix->h.strings_len - off;
    if (rd(ix, (off_t)ix->h.off_strings + off, buf, want)) { buf[0] = 0; return; }
    buf[want] = 0;
    /* cut at the NUL; a cut inside a multibyte character is removed */
    size_t l = strlen(buf);
    if (l == want) while (l && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) buf[--l] = 0;
    if (l && ((unsigned char)buf[l - 1] & 0xC0) == 0xC0 && l == want) buf[--l] = 0;
}

static int token(const bgs_index_t *ix, uint32_t i, bgs_token_t *t, char *word, size_t n)
{
    if (rd(ix, (off_t)ix->h.off_tokens + (off_t)i * sizeof *t, t, sizeof *t)) return -1;
    str_at(ix, t->text, word, n);
    return 0;
}

/* first token >= w (or > every word with prefix w when upper) */
static uint32_t bound(const bgs_index_t *ix, const char *w, int upper)
{
    uint32_t lo = 0, hi = ix->h.ntokens;
    size_t wl = strlen(w);
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        bgs_token_t t;
        char tw[64];
        if (token(ix, m, &t, tw, sizeof tw)) return lo;
        int c = upper ? strncmp(tw, w, wl) : strcmp(tw, w);
        if (upper ? c <= 0 : c < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- abbreviations and synonyms --------------------------------------------------------
 * Names in OSM use full words ("Avenida", "Superquadra Norte") or Brazilian abbreviations
 * ("Av.", "SQN", "SHIS QI 5"), and people type either.  Both the query and each candidate
 * go through the same canonical form: a known full phrase becomes its abbreviation and a
 * highway code ("BR 020", "br20", "BR-020") becomes one word ("br020").  The candidate is
 * matched against its plain words, its canonical words and the full form of each of its
 * abbreviations, so a half-typed "superquadra no" still finds "SQN 312".  Street-type words
 * ("av", "rua", "rodovia") count when present but are not required: OSM often leaves them
 * out ("W3 Norte").  Phrases are written without stopwords (bgs_stopword). */
typedef struct { const char *abbr, *full; unsigned char type; } bgs_syn_t;
static const bgs_syn_t SYN[] = {
    /* street types (optional in the query) */
    { "av", "avenida", 1 }, { "r", "rua", 1 }, { "al", "alameda", 1 }, { "tv", "travessa", 1 },
    { "trav", "travessa", 1 }, { "pc", "praca", 1 }, { "pca", "praca", 1 }, { "rod", "rodovia", 1 },
    { "estr", "estrada", 1 }, { "lgo", "largo", 1 }, { "vd", "viaduto", 1 },
    { "est", "estrada", 1 },
    /* titles and common words */
    { "dr", "doutor", 0 }, { "dra", "doutora", 0 }, { "prof", "professor", 0 },
    { "profa", "professora", 0 }, { "pres", "presidente", 0 }, { "gov", "governador", 0 },
    { "sen", "senador", 0 }, { "dep", "deputado", 0 }, { "mal", "marechal", 0 },
    { "gal", "general", 0 }, { "gen", "general", 0 }, { "cel", "coronel", 0 },
    { "cap", "capitao", 0 }, { "ten", "tenente", 0 }, { "eng", "engenheiro", 0 },
    { "pe", "padre", 0 }, { "sta", "santa", 0 }, { "sto", "santo", 0 },
    { "nsa sra", "nossa senhora", 0 }, { "n sra", "nossa senhora", 0 }, { "sra", "senhora", 0 },
    { "jd", "jardim", 0 }, { "jard", "jardim", 0 }, { "vl", "vila", 0 }, { "pq", "parque", 0 },
    { "res", "residencial", 0 }, { "cond", "condominio", 0 }, { "conj", "conjunto", 0 },
    { "qd", "quadra", 0 }, { "lt", "lote", 0 }, { "bl", "bloco", 0 }, { "km", "quilometro", 0 },
    /* Brasília */
    { "sqn", "superquadra norte", 0 }, { "sqs", "superquadra sul", 0 },
    { "sqsw", "superquadra sudoeste", 0 }, { "sqnw", "superquadra noroeste", 0 },
    { "sqn", "super quadra norte", 0 }, { "sqs", "super quadra sul", 0 },
    { "cln", "comercio local norte", 0 }, { "cls", "comercio local sul", 0 },
    { "crn", "comercio residencial norte", 0 }, { "crs", "comercio residencial sul", 0 },
    { "shin", "setor habitacoes individuais norte", 0 },
    { "shis", "setor habitacoes individuais sul", 0 },
    { "shcgn", "setor habitacoes coletivas geminadas norte", 0 },
    { "scs", "setor comercial sul", 0 }, { "scn", "setor comercial norte", 0 },
    { "sbs", "setor bancario sul", 0 }, { "sbn", "setor bancario norte", 0 },
    { "sas", "setor autarquias sul", 0 }, { "san", "setor autarquias norte", 0 },
    { "sgan", "setor grandes areas norte", 0 }, { "sgas", "setor grandes areas sul", 0 },
    { "smhn", "setor medico hospitalar norte", 0 }, { "smhs", "setor medico hospitalar sul", 0 },
    { "qi", "quadra interna", 0 }, { "ql", "quadra lago", 0 },
    { "eixao", "eixo rodoviario", 0 },
};
#define NSYN (int)(sizeof SYN / sizeof SYN[0])

/* state and federal highway prefixes */
static int hw_prefix(const char *w)
{
    static const char *const p[] = { "br", "ac", "al", "ap", "am", "ba", "ce", "df", "es", "go",
        "ma", "mt", "ms", "mg", "pa", "pb", "pr", "pe", "pi", "rj", "rn", "rs", "ro", "rr", "sc",
        "sp", "se", "to" };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) if (!strcmp(w, p[i])) return 1;
    return 0;
}

static int all_digits(const char *w, size_t *len)
{
    size_t l = 0;
    while (w[l] >= '0' && w[l] <= '9') l++;
    *len = l;
    return l && !w[l];
}

#define WMAX 40
#define WLEN 32
typedef struct { int n; char w[WMAX][WLEN]; } wlist_t;

/* words of a normalized string, without stopwords */
static void split_words(const char *norm, wlist_t *o)
{
    o->n = 0;
    for (const char *p = norm; *p && o->n < WMAX;) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && l < WLEN && !bgs_stopword(p, l)) {
            memcpy(o->w[o->n], p, l);
            o->w[o->n][l] = 0;
            o->n++;
        }
        p += l;
        while (*p == ' ') p++;
    }
}

/* number of words of a phrase that match at in->w[i]; 0 = no match */
static int phrase_at(const wlist_t *in, int i, const char *phrase)
{
    int m = 0;
    for (const char *p = phrase; *p;) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (i + m >= in->n || strlen(in->w[i + m]) != l || strncmp(in->w[i + m], p, l)) return 0;
        m++;
        p += l;
        while (*p == ' ') p++;
    }
    return m;
}

/* canonical words: full phrases -> abbreviation (longest first), highway codes joined */
static void canon(const wlist_t *in, wlist_t *o)
{
    o->n = 0;
    for (int i = 0; i < in->n && o->n < WMAX;) {
        int best = 0, bs = -1;
        for (int k = 0; k < NSYN; k++) {
            int m = phrase_at(in, i, SYN[k].full);
            if (m > best) { best = m; bs = k; }
            int a = phrase_at(in, i, SYN[k].abbr);             /* "nsa sra" -> "nsa sra" */
            if (a > 1 && a > best) { best = a; bs = k; }
        }
        size_t dl;
        if (bs >= 0) {
            snprintf(o->w[o->n++], WLEN, "%s", SYN[bs].abbr);
            i += best;
        } else if (hw_prefix(in->w[i]) && i + 1 < in->n && all_digits(in->w[i + 1], &dl) && dl >= 2 && dl <= 3) {
            snprintf(o->w[o->n++], WLEN, "%s%03d", in->w[i], atoi(in->w[i + 1]));
            i += 2;
        } else {
            /* "br20", "br020" typed together */
            const char *w = in->w[i];
            size_t l = strlen(w);
            if (l >= 4 && l <= 5 && w[0] >= 'a' && w[1] >= 'a' && all_digits(w + 2, &dl)) {
                char pre[3] = { w[0], w[1], 0 };
                if (hw_prefix(pre)) { snprintf(o->w[o->n++], WLEN, "%s%03d", pre, atoi(w + 2)); i++; continue; }
            }
            snprintf(o->w[o->n++], WLEN, "%s", w);
            i++;
        }
    }
}

static const bgs_syn_t *syn_of(const char *abbr)
{
    for (int k = 0; k < NSYN; k++) if (!strcmp(SYN[k].abbr, abbr)) return &SYN[k];
    return NULL;
}

/* "plain canonical expansions" of a candidate's name and context */
static void match_text(const char *norm, char *out, size_t n)
{
    wlist_t pl, cn;
    split_words(norm, &pl);
    canon(&pl, &cn);
    size_t o = (size_t)snprintf(out, n, "%s", norm);
    for (int i = 0; i < cn.n && o + 1 < n; i++) {
        o += (size_t)snprintf(out + o, n - o, " %s", cn.w[i]);
        const bgs_syn_t *s = syn_of(cn.w[i]);
        if (s && o + 1 < n) o += (size_t)snprintf(out + o, n - o, " %s", s->full);
        size_t dl;                                        /* br020 -> also "020" */
        if (strlen(cn.w[i]) == 5 && all_digits(cn.w[i] + 2, &dl) && o + 1 < n)
            o += (size_t)snprintf(out + o, n - o, " %s", cn.w[i] + 2);
        if (o >= n) o = n - 1;
    }
}

/* word w is a prefix (exact: equal) of some word of text */
static int has_word(const char *text, const char *w, int *exact)
{
    size_t wl = strlen(w);
    int found = 0;
    *exact = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= wl && !strncmp(p, w, wl)) { found = 1; if (l == wl) { *exact = 1; return 1; } }
        p += l;
        while (*p == ' ') p++;
    }
    return found;
}

static uint32_t dist_m(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y0 / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat);
    double dx = (double)(int32_t)(x1 - x0), dy = (double)(int32_t)(y1 - y0);
    double d = sqrt(dx * dx + dy * dy) * mpu;
    return d > 4e9 ? 4000000000u : (uint32_t)d;
}

static void fill(const bgs_index_t *ix, const bgs_entry_t *e, uint32_t x, uint32_t y, bgs_result_t *r)
{
    str_at(ix, e->name, r->name, sizeof r->name);
    str_at(ix, e->context, r->context, sizeof r->context);
    r->x = e->x;
    r->y = e->y;
    r->type = e->type;
    r->cat = e->cat;
    r->dist_m = dist_m(x, y, e->x, e->y);
}

static void insert(bgs_result_t *out, double *score, int *n, int max, const bgs_result_t *r, double s)
{
    /* the same place mapped twice (a point and a building): keep the better one */
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i].name, r->name) && dist_m(out[i].x, out[i].y, r->x, r->y) < 300) {
            if (s <= score[i]) return;
            for (int k = i; k + 1 < *n; k++) { out[k] = out[k + 1]; score[k] = score[k + 1]; }
            (*n)--;
            break;
        }
    if (*n == max && s <= score[max - 1]) return;
    int k = *n < max ? (*n)++ : max - 1;
    while (k > 0 && score[k - 1] < s) { out[k] = out[k - 1]; score[k] = score[k - 1]; k--; }
    out[k] = *r;
    score[k] = s;
}

/* index words looked up for one query word (the word itself as a prefix, the first word of
 * the full form of an abbreviation, abbreviations whose full form starts with the word, the
 * number of a highway code) */
/* sum of the postings of tokens [lo, hi), reading the token records in blocks (no text);
 * stops past cap */
static uint64_t tok_sum(const bgs_index_t *ix, uint32_t lo, uint32_t hi, uint64_t cap)
{
    bgs_token_t b[256];
    uint64_t c = 0;
    for (uint32_t j = lo; j < hi && c <= cap; j += 256) {
        uint32_t k = hi - j < 256 ? hi - j : 256;
        if (rd(ix, (off_t)ix->h.off_tokens + (off_t)j * sizeof *b, b, k * sizeof *b)) break;
        for (uint32_t i = 0; i < k; i++) c += b[i].count;
    }
    return c;
}

/* postings of the words that start with w (capped) */
static uint64_t word_count(const bgs_index_t *ix, const char *w)
{
    return tok_sum(ix, bound(ix, w, 0), bound(ix, w, 1), 2u * MAXCAND);
}

/* entries read through a 4 KB window: the candidates come in id order */
typedef struct { off_t base; int len; unsigned char b[4096]; } ewin_t;
static int entry_at(const bgs_index_t *ix, ewin_t *w, uint32_t id, bgs_entry_t *e)
{
    off_t off = (off_t)ix->h.off_entries + (off_t)id * sizeof *e;
    if (w->len <= 0 || off < w->base || off + (off_t)sizeof *e > w->base + w->len) {
        w->base = off & ~(off_t)4095;
        ssize_t r = pread(ix->fd, w->b, sizeof w->b, w->base);
        w->len = r > 0 ? (int)r : 0;
        if (off + (off_t)sizeof *e > w->base + w->len) return rd(ix, off, e, sizeof *e);
    }
    memcpy(e, w->b + (off - w->base), sizeof *e);
    return 0;
}

typedef struct { uint32_t id; float ub; } cub_t;
static int cmp_ub(const void *a, const void *b)
{
    float x = ((const cub_t *)a)->ub, y = ((const cub_t *)b)->ub;
    return x < y ? 1 : x > y ? -1 : 0;
}

#define MAXT 8
typedef struct {
    char w[WLEN];
    int optional;              /* street type: counts if present, not required */
    int exact;                 /* abbreviation: whole word only ("doutor" -> "dr", not "drogaria") */
    int nt;
    char t[MAXT][WLEN];
    unsigned char texact[MAXT];  /* this index word only as a whole word */
    uint32_t lo[MAXT], hi[MAXT];
    uint64_t cnt;
} qword_t;

static void add_term(qword_t *q, const char *t, size_t len)
{
    if (q->nt >= MAXT || !len || len >= WLEN) return;
    char b[WLEN];
    memcpy(b, t, len);
    b[len] = 0;
    for (int i = 0; i < q->nt; i++)                                       /* covered */
        if (!strncmp(q->t[i], b, strlen(q->t[i])) && (!q->texact[i] || !strcmp(q->t[i], b))) return;
    for (int i = 0; i < q->nt; i++)                                       /* b covers t[i] */
        if (!q->texact[i] && !strncmp(b, q->t[i], len)) { snprintf(q->t[i], WLEN, "%s", b); return; }
    snprintf(q->t[q->nt++], WLEN, "%s", b);
}

/* version 3: sorted, unique entries of every index word of q (NULL when more than cap) */
#define MAXLIST 400000
static uint32_t *word_list(const bgs_index_t *ix, const qword_t *q, int *n)
{
    uint64_t tot = 0;
    for (int t = 0; t < q->nt && tot <= MAXLIST; t++) tot += tok_sum(ix, q->lo[t], q->hi[t], MAXLIST);
    if (tot > MAXLIST) return NULL;
    uint32_t *v = malloc((tot ? tot : 1) * sizeof *v);
    int k = 0;
    for (int t = 0; t < q->nt && v; t++)
        for (uint32_t j = q->lo[t]; j < q->hi[t]; j++) {
            bgs_token_t tk;
            if (rd(ix, (off_t)ix->h.off_tokens + (off_t)j * sizeof tk, &tk, sizeof tk)
                || (uint64_t)tk.first + tk.count > ix->h.npostings) continue;
            if ((uint64_t)k + tk.count > tot) break;
            if (rd(ix, (off_t)ix->h.off_postings + (off_t)tk.first * 4, v + k, tk.count * 4)) continue;
            k += (int)tk.count;
        }
    if (!v) return NULL;
    if (q->nt > 1 || q->hi[0] - q->lo[0] > 1) {   /* several lists: merge */
        qsort(v, (size_t)k, sizeof *v, cmp_u32);
        int u = 0;
        for (int i = 0; i < k; i++) if (!u || v[u - 1] != v[i]) v[u++] = v[i];
        k = u;
    }
    *n = k;
    return v;
}

/* check the candidates (sorted here) best-first by the score their entry allows */
static void verify(const bgs_index_t *ix, const qword_t *qw, int nw, int nopt, uint32_t *cand, int nc,
                   uint32_t x, uint32_t y, cub_t *cu, ewin_t *ew, bgs_result_t *out, double *score,
                   int *pnr, int max, const char *guide, int round)
{
    int nr = *pnr;
    /* the best score a candidate can reach comes from its entry alone (rank and distance;
     * the names are the slow random reads): check the names best-first and stop when no
     * candidate left can enter the results */
    qsort(cand, (size_t)nc, sizeof *cand, cmp_u32);
    int nu = 0;
    for (int i = 0; i < nc; i++) {
        if (i && cand[i] == cand[i - 1]) continue;
        if (cand[i] >= ix->h.nentries) continue;
        bgs_entry_t e;
        if (entry_at(ix, ew, cand[i], &e)) continue;
        cu[nu].id = cand[i];
        cu[nu].ub = (float)(e.rank + 85 + 1000.0 * nopt - 15.0 * log2(1.0 + dist_m(x, y, e.x, e.y) / 1000.0));
        nu++;
    }
    qsort(cu, (size_t)nu, sizeof *cu, cmp_ub);
    if (round >= 0 && getenv("BGS_DEBUG")) fprintf(stderr, "rodada %d: guia '%s' %d candidatos, %d únicos\n", round, guide, nc, nu);
    for (int i = 0; i < nu; i++) {
        if (nr == max && cu[i].ub <= score[max - 1]) break;
        bgs_entry_t e;
        if (entry_at(ix, ew, cu[i].id, &e)) continue;
        bgs_result_t r;
        fill(ix, &e, x, y, &r);
        char norm[256], full[200], mt[768];
        snprintf(full, sizeof full, "%s %s", r.name, r.context);
        bgs_norm(full, norm, sizeof norm);
        match_text(norm, mt, sizeof mt);
        int ok = 1, allex = 1, typebonus = 0;
        for (int k = 0; k < nw && ok; k++) {
            int ex;
            int f = has_word(mt, qw[k].w, &ex);
            if (qw[k].exact && !ex) f = 0;
            /* a street type typed and present ranks above every result without it */
            if (qw[k].optional) { if (f && ex) typebonus += 1000; continue; }
            if (!f) ok = 0;
            else if (!ex) allex = 0;
        }
        if (!ok) continue;
        /* all the words in the name itself: above a match through the neighbourhood
         * ("SQN 312" before "W3 Norte, SQN 312") */
        char nn[200], nmt[512];
        bgs_norm(r.name, nn, sizeof nn);
        match_text(nn, nmt, sizeof nmt);
        /* words in the name: 40 for all of them, plus 20 when the first word is in the name
         * (the name usually comes before the town: "av paulista sao paulo" is the Avenida
         * Paulista in São Paulo, not an "Avenida São Paulo" in Flórida Paulista) */
        int nreqw = 0, inn = 0, first_in = 0;
        for (int k = 0; k < nw; k++) {
            int ex;
            if (qw[k].optional) continue;
            int f = has_word(nmt, qw[k].w, &ex) && (!qw[k].exact || ex);
            if (!nreqw) first_in = f;
            nreqw++;
            inn += f;
        }
        double namebonus = nreqw ? 40.0 * inn / nreqw + (first_in ? 20 : 0) : 0;
        double sc = e.rank + (allex ? 25 : 0) + namebonus + typebonus
                    - 15.0 * log2(1.0 + r.dist_m / 1000.0);
        insert(out, score, &nr, max, &r, sc);
    }
    *pnr = nr;
}

/* version 3: the candidates (sorted ids) grouped by tile; a tile's best possible score
 * (its highest rank, the bonuses, its distance) orders the tiles, and the search stops at
 * the first tile that cannot enter the results: far candidates are never read */
typedef struct { int s, e; float ub; } seg_t;
static int cmp_seg(const void *a, const void *b)
{
    float x = ((const seg_t *)a)->ub, y = ((const seg_t *)b)->ub;
    return x < y ? 1 : x > y ? -1 : 0;
}

static double tile_dist_m(uint32_t key, uint32_t x, uint32_t y)
{
    uint32_t tx = key & 0xFFFF, ty = key >> 16;
    uint64_t x0 = (uint64_t)tx << 22, x1 = x0 + (1u << 22) - 1, y0 = (uint64_t)ty << 22, y1 = y0 + (1u << 22) - 1;
    uint32_t cx = x < x0 ? (uint32_t)x0 : x > x1 ? (uint32_t)x1 : x;
    uint32_t cy = y < y0 ? (uint32_t)y0 : y > y1 ? (uint32_t)y1 : y;
    return dist_m(x, y, cx, cy);
}

static void verify_tiles(const bgs_index_t *ix, const qword_t *qw, int nw, int nopt, const uint32_t *acc, int na,
                         uint32_t x, uint32_t y, uint32_t *cand, cub_t *cu, ewin_t *ew, bgs_result_t *out,
                         double *score, int *pnr, int max)
{
    if (!ix->tiles || !ix->h.ntiles) {                     /* no tile table: everything */
        for (int a = 0; a < na; a += MAXCAND) {
            int k = na - a < MAXCAND ? na - a : MAXCAND;
            memcpy(cand, acc + a, (size_t)k * sizeof *cand);
            verify(ix, qw, nw, nopt, cand, k, x, y, cu, ew, out, score, pnr, max, "v3", 0);
        }
        return;
    }
    seg_t *sg = malloc((size_t)(ix->h.ntiles < (uint32_t)na ? ix->h.ntiles : (uint32_t)na + 1) * sizeof *sg);
    if (!sg) return;
    int ns = 0;
    uint32_t t = 0;
    for (int i = 0; i < na;) {
        while (t + 1 < ix->h.ntiles && ix->tiles[t + 1].first <= acc[i]) t++;
        uint32_t end = t + 1 < ix->h.ntiles ? ix->tiles[t + 1].first : ix->h.nentries;
        int j = i;
        while (j < na && acc[j] < end) j++;
        sg[ns].s = i;
        sg[ns].e = j;
        sg[ns].ub = (float)(ix->tiles[t].maxrank + 85 + 1000.0 * nopt - 15.0 * log2(1.0 + tile_dist_m(ix->tiles[t].key, x, y) / 1000.0));
        ns++;
        i = j;
    }
    qsort(sg, (size_t)ns, sizeof *sg, cmp_seg);
    /* the tiles come nearest-best first: once the results are full, a common name all over
     * the country ("rua das flores") stops after BUDGET more candidates; what is left out
     * is far away (the bound of a tile is loose, so it rarely prunes such names by itself) */
    enum { BUDGET = 1500 };
    int read = 0, tiles = 0, full_at = -1;
    for (int k = 0; k < ns; k++) {
        if (*pnr == max && sg[k].ub <= score[max - 1]) break;
        if (*pnr == max && full_at < 0) full_at = read;
        if (full_at >= 0 && read - full_at > BUDGET) break;
        for (int a = sg[k].s; a < sg[k].e; a += MAXCAND) {
            int n = sg[k].e - a < MAXCAND ? sg[k].e - a : MAXCAND;
            memcpy(cand, acc + a, (size_t)n * sizeof *cand);
            verify(ix, qw, nw, nopt, cand, n, x, y, cu, ew, out, score, pnr, max, "v3", -1);
            read += n;
        }
        tiles++;
    }
    if (getenv("BGS_DEBUG")) fprintf(stderr, "v3: %d regiões de %d, %d candidatos lidos de %d\n", tiles, ns, read, na);
    free(sg);
}

/* Damerau-Levenshtein distance, giving up above k */
static int edits(const char *a, const char *b, int k)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (abs(la - lb) > k || la > 40 || lb > 40) return k + 1;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        int rowmin = k + 1;
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] != b[j - 1];
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < v) v = d[i - 1][j - 1] + c;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < v) v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
            if (v < rowmin) rowmin = v;
        }
        if (rowmin > k) return k + 1;
    }
    return d[la][lb];
}

/* the closest indexed word to w among those starting with pre (fewest edits, then the most
 * common); *best / *bestd: count and edits of the best word so far */
static void fix_prefix(const bgs_index_t *ix, const char *w, const char *pre, char *out, size_t n, uint32_t *best, int *bestd)
{
    uint32_t lo = bound(ix, pre, 0), hi = bound(ix, pre, 1);
    if (hi - lo > 40000) hi = lo + 40000;                 /* a bound on the work per prefix */
    int k = strlen(w) >= 8 ? 2 : 1;
    bgs_token_t buf[512];
    for (uint32_t i = lo; i < hi;) {
        uint32_t m = hi - i < 512 ? hi - i : 512;
        if (rd(ix, (off_t)ix->h.off_tokens + (off_t)i * sizeof buf[0], buf, m * sizeof buf[0])) return;
        for (uint32_t j = 0; j < m; j++) {
            if (buf[j].count <= *best && *bestd <= 1) continue;   /* cannot beat it */
            char tw[48];
            str_at(ix, buf[j].text, tw, sizeof tw);
            if (!strcmp(tw, w)) continue;
            int d = edits(w, tw, k);
            if (d > k || d > *bestd || (d == *bestd && buf[j].count <= *best)) continue;
            *best = buf[j].count;
            *bestd = d;
            snprintf(out, n, "%s", tw);
        }
        i += m;
    }
}

/* candidates: the same first two letters; and, for an error in the second
 * letter, the 3-letter prefixes it would have (missing, wrong, extra or swapped letter) —
 * small ranges instead of every word with the first letter */
static uint32_t fix_word(const bgs_index_t *ix, const char *w, char *out, size_t n)
{
    uint32_t best = 0;
    int bestd = 99;
    char pre[8];
    /* every stage, keeping the most common neighbour: a rare word one edit away in the
     * first range ("nrtl" for "nrte") must not hide the common one ("norte") */
    snprintf(pre, sizeof pre, "%.2s", w);                       /* covers the first 3 letters too */
    fix_prefix(ix, w, pre, out, n, &best, &bestd);
    if (strlen(w) >= 3) {
        for (char c = 'a'; c <= 'z'; c++) {
            snprintf(pre, sizeof pre, "%c%c%c", w[0], c, w[1]);     /* missing 2nd letter */
            fix_prefix(ix, w, pre, out, n, &best, &bestd);
            snprintf(pre, sizeof pre, "%c%c%c", w[0], c, w[2]);     /* wrong 2nd letter */
            fix_prefix(ix, w, pre, out, n, &best, &bestd);
        }
        snprintf(pre, sizeof pre, "%c%c", w[0], w[2]);             /* extra 2nd letter */
        fix_prefix(ix, w, pre, out, n, &best, &bestd);
        snprintf(pre, sizeof pre, "%c%c", w[1], w[0]);             /* first two swapped */
        fix_prefix(ix, w, pre, out, n, &best, &bestd);
    }
    return best;
}

int bgs_correct(bgs_index_t *ix, const char *text, char *out, size_t n)
{
    char qn[256];
    bgs_norm(text, qn, sizeof qn);
    size_t o = 0, tl = strlen(text);
    int changed = 0, typing = tl && text[tl - 1] != ' ';
    out[0] = 0;
    for (const char *p = qn; *p;) {
        size_t l = strcspn(p, " ");
        char w[WLEN], fix[64];
        snprintf(w, sizeof w, "%.*s", (int)(l < WLEN ? l : WLEN - 1), p);
        const char *use = w;
        size_t dl;
        int last = !p[l] || !p[l + strspn(p + l, " ")];
        /* a finished word must exist whole (a rarer word that merely starts with the typo, like
         * "avendano" for "avenda", does not count); the word being typed, as a prefix */
        uint32_t own = 0;                       /* how common the word itself is (0 = absent) */
        uint32_t lo = bound(ix, w, 0);
        if (last && typing) own = (uint32_t)tok_sum(ix, lo, bound(ix, w, 1), 1000);   /* being typed: a prefix */
        else {
            bgs_token_t tk;
            char tw[64];
            if (lo < ix->h.ntokens && !token(ix, lo, &tk, tw, sizeof tw) && !strcmp(tw, w)) own = tk.count;
        }
        /* absent, or rare enough that a word 50x more common one edit away is the likely intent
         * ("avenda" is a surname somewhere; "avenida" is everywhere) */
        if (strlen(w) >= 4 && !all_digits(w, &dl) && !syn_of(w) && own < 20) {
            uint32_t c = fix_word(ix, w, fix, sizeof fix);
            if (c && (own == 0 || c >= 50 * own)) { use = fix; changed = 1; }
        }
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? " " : "", use);
        p += l;
        while (*p == ' ') p++;
    }
    return changed && o < n;
}

const char *bgs_corrected(const bgs_index_t *ix) { return ix->corrected; }

static int query(bgs_index_t *ix, const char *text, uint32_t x, uint32_t y, bgs_result_t *out, int max);

int bgs_query(bgs_index_t *ix, const char *text, uint32_t x, uint32_t y, bgs_result_t *out, int max)
{
    ix->corrected[0] = 0;
    int n = query(ix, text, x, y, out, max);
    char fixed[256];
    size_t tl = strlen(text);
    /* nothing found and the text is complete (or long enough): try it corrected */
    if (n == 0 && tl >= 4 && bgs_correct(ix, text, fixed, sizeof fixed)) {
        if (text[tl - 1] != ' ') strncat(fixed, " ", sizeof fixed - strlen(fixed) - 1);   /* whole words now */
        n = query(ix, fixed, x, y, out, max);
        if (n) snprintf(ix->corrected, sizeof ix->corrected, "%s", fixed);
    }
    return n;
}

static int query(bgs_index_t *ix, const char *text, uint32_t x, uint32_t y, bgs_result_t *out, int max)
{
    char qn[256];
    wlist_t pl, cn;
    bgs_norm(text, qn, sizeof qn);
    split_words(qn, &pl);
    canon(&pl, &cn);
    if (!cn.n || max < 1) return 0;
    size_t tl = strlen(text);
    int typing = tl && text[tl - 1] != ' ';       /* the last word may still be incomplete */
    qword_t qw[MAXW];
    int nw = 0, nreq = 0;
    for (int i = 0; i < cn.n && nw < MAXW; i++) {
        qword_t *q = &qw[nw];
        memset(q, 0, sizeof *q);
        snprintf(q->w, WLEN, "%s", cn.w[i]);
        const bgs_syn_t *s = syn_of(q->w);
        q->optional = s && s->type == 1;
        size_t dl0;
        int hw = strlen(q->w) == 5 && all_digits(q->w + 2, &dl0);
        /* converted from a full phrase or a highway code: whole word; an abbreviation typed
         * as such is a prefix only while it is the last word being typed */
        int conv = i >= pl.n || strcmp(cn.w[i], pl.w[i]) || cn.n != pl.n;
        q->exact = (s || hw) && (conv || !(typing && i == cn.n - 1));
        /* one or two letters ("se" in "praca da se", "5" in "qi 5"): whole word, unless it
         * is the whole query; as a prefix it would match half the index */
        if (strlen(q->w) <= 2 && cn.n > 1) q->exact = 1;
        add_term(q, q->w, strlen(q->w));
        if (q->exact) q->texact[0] = 1;
        if (s) {                                  /* the rarest word of the full form */
            const char *best = NULL;
            size_t bl = 0;
            uint64_t bc = ~0ull;
            for (const char *p = s->full; *p;) {
                size_t l = strcspn(p, " ");
                char w[WLEN];
                if (l < WLEN) {
                    memcpy(w, p, l);
                    w[l] = 0;
                    uint64_t c = word_count(ix, w);
                    if (c < bc) { bc = c; best = p; bl = l; }
                }
                p += l;
                while (*p == ' ') p++;
            }
            if (best) add_term(q, best, bl);
        }
        size_t wl = strlen(q->w), dl;
        if (wl == 5 && all_digits(q->w + 2, &dl)) add_term(q, q->w + 2, 3);
        if (wl >= 3)                              /* "superq" -> sqn, sqs, sqsw */
            for (int k = 0; k < NSYN; k++)
                if (!strncmp(SYN[k].full, q->w, wl) && strcspn(SYN[k].full, " ") >= wl) add_term(q, SYN[k].abbr, strlen(SYN[k].abbr));
        for (int t = 0; t < q->nt; t++) {
            q->lo[t] = bound(ix, q->t[t], 0);
            q->hi[t] = bound(ix, q->t[t], 1);
            if (q->texact[t]) {                   /* whole word: only the token itself */
                bgs_token_t tk;
                char tw[64];
                q->hi[t] = q->lo[t] < q->hi[t] && !token(ix, q->lo[t], &tk, tw, sizeof tw) && !strcmp(tw, q->t[t]) ? q->lo[t] + 1 : q->lo[t];
            }
            q->cnt += tok_sum(ix, q->lo[t], q->hi[t], 2u * MAXCAND);
        }
        if (!q->optional) nreq++;
        nw++;
    }
    if (!nreq) for (int k = 0; k < nw; k++) { qw[k].optional = 0; nreq++; }   /* only "avenida" */
    /* required words in order of fewest entries: the rarest drives the search; a word found
     * only in the context ("... curitiba") may give nothing, so the next one drives a new round */
    int order[MAXW], no = 0;
    for (int k = 0; k < nw; k++) {
        if (qw[k].optional) continue;
        order[no] = k;
        for (int j = no++; j > 0 && qw[order[j - 1]].cnt > qw[order[j]].cnt; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    }
    int nopt = 0;
    for (int k = 0; k < nw; k++) nopt += qw[k].optional;
    uint32_t *cand = malloc(MAXCAND * sizeof *cand);
    cub_t *cu = malloc(MAXCAND * sizeof *cu);
    ewin_t *ew = calloc(1, sizeof *ew);
    double *score = malloc((size_t)max * sizeof *score);
    int nr = 0;
    if (ix->h.version >= 3 && cand && cu && ew && score) {
        /* every word is in the lists (name and context): intersect the lists of the
         * required words, smallest first; a word with a huge list only filters */
        uint32_t *acc = NULL;
        int na = 0, used = 0;
        for (int r = 0; r < no; r++) {
            int nl;
            uint32_t *l = word_list(ix, &qw[order[r]], &nl);
            if (!l) continue;
            if (!acc) { acc = l; na = nl; used = 1; continue; }
            int a = 0, b = 0, o = 0;
            while (a < na && b < nl) {
                if (acc[a] < l[b]) a++;
                else if (acc[a] > l[b]) b++;
                else { acc[o++] = acc[a]; a++; b++; }
            }
            na = o;
            used++;
            free(l);
        }
        if (acc) {
            verify_tiles(ix, qw, nw, nopt, acc, na, x, y, cand, cu, ew, out, score, &nr, max);
            if (getenv("BGS_DEBUG")) fprintf(stderr, "v3: %d listas, %d candidatos\n", used, na);
            free(acc);
            no = 0;                                       /* no rounds */
        }
    }
    for (int round = 0; round < no && nr < max && cand && cu && ew && score; round++) {
        qword_t *q = &qw[order[round]];
        if (!q->cnt) continue;
        int nc = 0;
        for (int t = 0; t < q->nt; t++)
            for (uint32_t i = q->lo[t]; i < q->hi[t] && nc < MAXCAND; i++) {
                bgs_token_t tk;
                char tw[64];
                if (token(ix, i, &tk, tw, sizeof tw) || (uint64_t)tk.first + tk.count > ix->h.npostings) continue;
                uint32_t take = tk.count;
                if (take > (uint32_t)(MAXCAND - nc)) take = (uint32_t)(MAXCAND - nc);
                if (rd(ix, (off_t)ix->h.off_postings + (off_t)tk.first * 4, cand + nc, take * 4)) continue;
                nc += (int)take;
            }
        verify(ix, qw, nw, nopt, cand, nc, x, y, cu, ew, out, score, &nr, max, q->w, round);
    }
    free(score);
    free(ew);
    free(cu);
    free(cand);
    return nr;
}

int bgs_nearby(bgs_index_t *ix, int cat, uint32_t x, uint32_t y, uint32_t max_m, bgs_result_t *out, int max)
{
    /* the category's points are grouped by zoom-10 tile: read the tiles around (x, y) */
    if (cat <= 0 || cat >= BGS_C_COUNT || max < 1) return 0;
    uint32_t cf[2];
    if (rd(ix, (off_t)ix->h.off_catidx + (off_t)cat * 4, cf, sizeof cf) || cf[0] > cf[1] || cf[1] > ix->h.ncatpairs) return 0;
    off_t pairs = (off_t)ix->h.off_catidx + (BGS_C_COUNT + 1) * 4;
    const int v3 = ix->h.version >= 3;                   /* (key, first, end) triples */
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y / 4294967296.0)));
    double tile_m = 40075016.686 * cos(lat) / 1024.0;           /* zoom-10 tile width here */
    int reach = (int)(max_m / tile_m) + 1;
    if (reach > 6) reach = 6;
    int tx = (int)(x >> 22), ty = (int)(y >> 22);
    double *score = malloc((size_t)max * sizeof *score);
    int nr = 0;
    enum { BLK = 256 };
    bgs_entry_t blk[BLK];
    for (int dy = -reach; dy <= reach && score; dy++)
        for (int dx = -reach; dx <= reach; dx++) {
            int cx = tx + dx, cy = ty + dy;
            if (cx < 0 || cy < 0 || cx > 1023 || cy > 1023) continue;
            uint32_t key = (uint32_t)cy << 16 | (uint32_t)cx, lo = cf[0], hi = v3 ? cf[1] : cf[1] - 1;   /* v2: last = sentinel */
            if (lo >= hi) continue;
            const off_t step = v3 ? 12 : 8;
            while (lo < hi) {                                   /* first pair with key >= wanted */
                uint32_t m = lo + (hi - lo) / 2, pk[2];
                if (rd(ix, pairs + (off_t)m * step, pk, 8)) { lo = hi; break; }
                if (pk[0] < key) lo = m + 1;
                else hi = m;
            }
            uint32_t pk[4], e0, e1;
            if (v3) {
                if (lo >= cf[1] || rd(ix, pairs + (off_t)lo * 12, pk, 12) || pk[0] != key) continue;
                e0 = pk[1]; e1 = pk[2];
            } else {
                if (lo >= cf[1] - 1 || rd(ix, pairs + (off_t)lo * 8, pk, 16) || pk[0] != key) continue;
                e0 = pk[1]; e1 = pk[3];                         /* entries of this tile */
            }
            if (e1 > ix->h.nentries || e0 > e1) continue;
            for (uint32_t i = e0; i < e1; i += BLK) {
                uint32_t k = e1 - i < BLK ? e1 - i : BLK;
                if (rd(ix, (off_t)ix->h.off_entries + (off_t)i * sizeof(bgs_entry_t), blk, k * sizeof(bgs_entry_t))) break;
                for (uint32_t j = 0; j < k; j++) {
                    if (blk[j].type != BGS_T_POI || blk[j].cat != cat) continue;
                    uint32_t d = dist_m(x, y, blk[j].x, blk[j].y);
                    if (d > max_m || (nr == max && -(double)d <= score[max - 1])) continue;
                    bgs_result_t r;
                    fill(ix, &blk[j], x, y, &r);
                    insert(out, score, &nr, max, &r, -(double)d);
                }
            }
        }
    free(score);
    return nr;
}
