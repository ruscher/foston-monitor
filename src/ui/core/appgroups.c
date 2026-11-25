/* appgroups.c — ver appgroups.h.
 *
 * Formato do arquivo (uma linha por registro, campos separados por TAB):
 *   bigfoston-apps  1
 *   next   <próximo id de grupo>
 *   group  <id>  <key>  <cor hex ou 0>  <ícone>  <nome>
 *   item   <pai>  <id>
 *   end
 * Os grupos vêm antes das entradas; "end" no fim prova que o arquivo está inteiro.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _DEFAULT_SOURCE
#include "appgroups.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { char *id; int parent; } ent_t;

typedef struct {
    ag_group_t g[AG_GROUPS_MAX];
    int ng;
    ent_t *e;
    int ne, cap;
    int next;
} state_t;

static state_t st, snap;
static char path[512];
static bool fresh = true, fail_next;
static unsigned serial;
static void (*listener)(void);
static const char *(*namer)(const char *key);

/* ------------------------------------------------------------ memória */

static void st_free(state_t *s)
{
    for(int i = 0; i < s->ne; i++) free(s->e[i].id);
    free(s->e);
    memset(s, 0, sizeof *s);
}

static bool st_copy(state_t *dst, const state_t *src)
{
    memset(dst, 0, sizeof *dst);
    memcpy(dst->g, src->g, sizeof src->g);
    dst->ng = src->ng;
    dst->next = src->next;
    if(!src->ne) return true;
    dst->e = malloc(sizeof *dst->e * (size_t)src->ne);
    if(!dst->e) return false;
    dst->cap = src->ne;
    for(int i = 0; i < src->ne; i++) {
        dst->e[i].parent = src->e[i].parent;
        dst->e[i].id = strdup(src->e[i].id);
        if(!dst->e[i].id) { dst->ne = i; st_free(dst); return false; }
        dst->ne = i + 1;
    }
    return true;
}

static int find(const char *id)
{
    for(int i = 0; i < st.ne; i++) if(!strcmp(st.e[i].id, id)) return i;
    return -1;
}

static ag_group_t *grp(int gid)
{
    for(int i = 0; i < st.ng; i++) if(st.g[i].id == gid) return &st.g[i];
    return NULL;
}

/* insere na posição at (0..ne); o id é copiado */
static bool ins(int at, const char *id, int parent)
{
    if(st.ne == st.cap) {
        int nc = st.cap ? st.cap * 2 : 32;
        ent_t *ne = realloc(st.e, sizeof *ne * (size_t)nc);
        if(!ne) return false;
        st.e = ne;
        st.cap = nc;
    }
    char *d = strdup(id);
    if(!d) return false;
    if(at < 0 || at > st.ne) at = st.ne;
    memmove(&st.e[at + 1], &st.e[at], sizeof *st.e * (size_t)(st.ne - at));
    st.e[at] = (ent_t){ d, parent };
    st.ne++;
    return true;
}

static void del(int i)
{
    free(st.e[i].id);
    memmove(&st.e[i], &st.e[i + 1], sizeof *st.e * (size_t)(st.ne - i - 1));
    st.ne--;
}

/* tira a entrada i e a recoloca em at (índice contado antes da retirada) */
static void relocate(int i, int at, int parent)
{
    ent_t t = st.e[i];
    t.parent = parent;
    memmove(&st.e[i], &st.e[i + 1], sizeof *st.e * (size_t)(st.ne - i - 1));
    if(at > i) at--;
    if(at < 0 || at > st.ne - 1) at = st.ne - 1;
    memmove(&st.e[at + 1], &st.e[at], sizeof *st.e * (size_t)(st.ne - 1 - at));
    st.e[at] = t;
}

int ag_marker_gid(const char *id)
{
    if(!id || strncmp(id, "grp:", 4)) return 0;
    char *end;
    long v = strtol(id + 4, &end, 10);
    return *end || v <= 0 || v > 1000000 ? 0 : (int)v;
}

static void marker(char *out, size_t n, int gid) { snprintf(out, n, "grp:%d", gid); }

/* ------------------------------------------------------------ nomes */

/* comprimento em caracteres; -1 se o UTF-8 for inválido */
static int u8len(const char *s)
{
    int n = 0;
    for(const unsigned char *p = (const unsigned char *)s; *p;) {
        int k = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if(!k) return -1;
        for(int j = 1; j < k; j++) if((p[j] & 0xc0) != 0x80) return -1;
        p += k;
        n++;
    }
    return n;
}

ag_err_t ag_name_clean(const char *in, char *out, size_t n)
{
    if(!n) return AG_E_LONG;
    out[0] = 0;
    if(!in) return AG_E_EMPTY;
    size_t o = 0;
    bool sp = true;                                    /* tira os espaços do começo */
    for(const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p < 0x20 || *p == 0x7f ? ' ' : *p;
        if(c == ' ') { if(sp) continue; sp = true; }
        else sp = false;
        if(o + 1 >= n) return AG_E_LONG;
        out[o++] = (char)c;
    }
    while(o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if(!o) return AG_E_EMPTY;
    int L = u8len(out);
    if(L < 0) {                                        /* bytes soltos: troca por '?' */
        for(char *p = out; *p; p++) if((unsigned char)*p >= 0x80) *p = '?';
        L = (int)strlen(out);
    }
    return L > AG_NAME_CHARS ? AG_E_LONG : AG_OK;
}

/* compara sem maiúsculas: ASCII e as letras acentuadas do Latin-1 (À..Þ -> à..þ) */
static int fold_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while(*x && *y) {
        unsigned cx = *x, cy = *y;
        if(cx == 0xc3 && x[1] && y[0] == 0xc3 && y[1]) {
            unsigned lx = x[1], ly = y[1];
            if(lx >= 0x80 && lx <= 0x9e && lx != 0x97) lx += 0x20;
            if(ly >= 0x80 && ly <= 0x9e && ly != 0x97) ly += 0x20;
            if(lx != ly) return (int)lx - (int)ly;
            x += 2; y += 2;
            continue;
        }
        if(cx >= 'A' && cx <= 'Z') cx += 32;
        if(cy >= 'A' && cy <= 'Z') cy += 32;
        if(cx != cy) return (int)cx - (int)cy;
        x++; y++;
    }
    return (int)*x - (int)*y;
}

const char *ag_group_name(const ag_group_t *g)
{
    if(!g) return "";
    if(g->name[0]) return g->name;
    const char *n = namer && g->key[0] ? namer(g->key) : NULL;
    return n && *n ? n : g->key;
}

bool ag_name_taken(const char *name, int except_gid)
{
    for(int i = 0; i < st.ng; i++)
        if(st.g[i].id != except_gid && !fold_cmp(ag_group_name(&st.g[i]), name)) return true;
    return false;
}

/* ------------------------------------------------------------ arquivo */

static bool save(void)
{
    if(!path[0]) return true;                          /* sem arquivo (testes de memória) */
    if(fail_next) { fail_next = false; return false; }
    char tmp[530], bak[530];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(bak, sizeof bak, "%s.bak", path);
    FILE *f = fopen(tmp, "w");
    if(!f) {
        fprintf(stderr, "bigfoston: grupos: não foi possível gravar %s: %s\n", tmp, strerror(errno));
        return false;
    }
    fprintf(f, "bigfoston-apps\t%d\nnext\t%d\n", AG_VERSION, st.next);
    for(int i = 0; i < st.ng; i++) {
        const ag_group_t *g = &st.g[i];
        fprintf(f, "group\t%d\t%s\t%06x\t%s\t%s\n", g->id, g->key[0] ? g->key : "-", (unsigned)g->color,
                g->icon[0] ? g->icon : "-", g->name);
    }
    for(int i = 0; i < st.ne; i++) fprintf(f, "item\t%d\t%s\n", st.e[i].parent, st.e[i].id);
    fputs("end\n", f);
    /* no cartão SD (FAT): o conteúdo no cartão antes da troca de nome */
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if(fclose(f)) ok = false;
    if(!ok) { unlink(tmp); return false; }
    if(rename(path, bak) && errno != ENOENT) { /* sem cópia anterior: segue */ }
    if(rename(tmp, path)) {
        fprintf(stderr, "bigfoston: grupos: rename %s: %s\n", path, strerror(errno));
        rename(bak, path);                             /* devolve a anterior */
        unlink(tmp);
        return false;
    }
    return true;
}

static bool hexcolor(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if(*end || v > 0xffffff) return false;
    *out = (uint32_t)v;
    return true;
}

static bool safe_word(const char *s, size_t max)
{
    size_t L = strlen(s);
    if(!L || L >= max) return false;
    for(; *s; s++) if(!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}

/* lê um arquivo inteiro e válido para st; false = ilegível (st fica vazio) */
static bool load_file(const char *file)
{
    FILE *f = fopen(file, "r");
    if(!f) return false;
    st_free(&st);
    st.next = 1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t L;
    bool head = false, end = false, ok = true;
    int maxid = 0;
    while(ok && (L = getline(&line, &cap, f)) >= 0) {
        while(L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if(end) { if(L) ok = false; continue; }        /* nada depois do fim */
        char *fld[6];
        int nf = 0;
        for(char *p = line; nf < 6;) {
            fld[nf++] = p;
            char *t = nf < 6 ? strchr(p, '\t') : NULL;    /* o nome (último campo) pode ter tudo */
            if(!t) break;
            *t = 0;
            p = t + 1;
        }
        if(!head) {
            head = nf == 2 && !strcmp(fld[0], "bigfoston-apps") && atoi(fld[1]) >= 1 && atoi(fld[1]) <= AG_VERSION;
            if(!head) ok = false;
        }
        else if(!strcmp(fld[0], "end") && nf == 1) end = true;
        else if(!strcmp(fld[0], "next") && nf == 2) { int v = atoi(fld[1]); if(v > st.next) st.next = v; }
        else if(!strcmp(fld[0], "group") && nf == 6) {
            int id = atoi(fld[1]);
            if(id <= 0 || id > 1000000 || grp(id) || st.ng >= AG_GROUPS_MAX) continue;   /* repetido: o 1º vale */
            ag_group_t *g = &st.g[st.ng];
            memset(g, 0, sizeof *g);
            g->id = id;
            if(strcmp(fld[2], "-") && safe_word(fld[2], sizeof g->key)) snprintf(g->key, sizeof g->key, "%s", fld[2]);
            if(!hexcolor(fld[3], &g->color)) g->color = 0;
            if(strcmp(fld[4], "-") && safe_word(fld[4], sizeof g->icon)) snprintf(g->icon, sizeof g->icon, "%s", fld[4]);
            char nm[AG_NAME_BYTES];
            ag_err_t e = ag_name_clean(fld[5], nm, sizeof nm);
            if(e == AG_OK) snprintf(g->name, sizeof g->name, "%s", nm);
            else if(!g->key[0]) snprintf(g->name, sizeof g->name, "%d", id);   /* sem nome nenhum: o número */
            st.ng++;
            if(id > maxid) maxid = id;
        }
        else if(!strcmp(fld[0], "item") && nf == 3) {
            int parent = atoi(fld[1]);
            const char *id = fld[2];
            if(!*id || strlen(id) > 1100 || find(id) >= 0) continue;
            int mg = ag_marker_gid(id);
            if(!strncmp(id, "grp:", 4) && (!mg || !grp(mg))) continue;   /* pasta de grupo que não existe */
            if(mg || (parent && !grp(parent))) parent = 0;   /* pasta só no principal; pai sumido: principal */
            if(!ins(st.ne, id, parent)) ok = false;
        }
        /* linhas desconhecidas: ignoradas (versões futuras compatíveis) */
    }
    free(line);
    fclose(f);
    if(!ok || !head || !end) { st_free(&st); return false; }
    char m[24];
    for(int i = 0; i < st.ng; i++) {                   /* grupo sem pasta: no fim */
        marker(m, sizeof m, st.g[i].id);
        if(find(m) < 0 && !ins(st.ne, m, 0)) { st_free(&st); return false; }
    }
    if(st.next <= maxid) st.next = maxid + 1;
    return true;
}

void ag_close(void)
{
    st_free(&st);
    st_free(&snap);
    path[0] = 0;
    fresh = true;
}

void ag_open(const char *file)
{
    ag_close();
    st.next = 1;
    snprintf(path, sizeof path, "%s", file ? file : "");
    if(!path[0]) return;
    char alt[530];
    if(load_file(path)) { fresh = false; serial++; return; }
    if(access(path, F_OK) == 0) {                      /* existe mas não dá para ler: guarda */
        snprintf(alt, sizeof alt, "%s.bad", path);
        fprintf(stderr, "bigfoston: grupos: %s ilegível, guardado em .bad\n", path);
        rename(path, alt);
    }
    snprintf(alt, sizeof alt, "%s.bak", path);
    if(load_file(alt)) {
        fprintf(stderr, "bigfoston: grupos: usando a cópia anterior (%s)\n", alt);
        fresh = false;
    } else st.next = 1;
    serial++;
}

bool ag_is_fresh(void) { return fresh; }
unsigned ag_serial(void) { return serial; }
void ag_set_listener(void (*cb)(void)) { listener = cb; }
void ag_set_namer(const char *(*fn)(const char *key)) { namer = fn; }
void ag_test_fail_next_save(bool on) { fail_next = on; }

/* ------------------------------------------------------------ transação */

static bool begin(void)
{
    st_free(&snap);
    return st_copy(&snap, &st);
}

/* grava; se não der, volta ao estado de antes de begin() */
static ag_err_t commit(void)
{
    if(!save()) {
        st_free(&st);
        st = snap;
        memset(&snap, 0, sizeof snap);
        return AG_E_WRITE;
    }
    st_free(&snap);
    serial++;
    if(listener) listener();
    return AG_OK;
}

static ag_err_t rollback(ag_err_t e)
{
    st_free(&st);
    st = snap;
    memset(&snap, 0, sizeof snap);
    return e;
}

/* ------------------------------------------------------------ consultas */

int ag_group_count(void) { return st.ng; }

const ag_group_t *ag_group_at(int i)
{
    int k = 0;
    for(int j = 0; j < st.ne; j++) {
        int g = st.e[j].parent == 0 ? ag_marker_gid(st.e[j].id) : 0;
        if(g && k++ == i) return grp(g);
    }
    return NULL;
}

const ag_group_t *ag_group_get(int gid) { return grp(gid); }

const ag_group_t *ag_group_by_key(const char *key)
{
    if(!key || !*key) return NULL;
    for(int i = 0; i < st.ng; i++) if(!strcmp(st.g[i].key, key)) return &st.g[i];
    return NULL;
}

int ag_children(int parent, const char **out, int max)
{
    int n = 0;
    for(int i = 0; i < st.ne; i++)
        if(st.e[i].parent == parent) { if(n < max) out[n] = st.e[i].id; n++; }
    return n;
}

int ag_parent(const char *id)
{
    int i = id ? find(id) : -1;
    return i < 0 ? -1 : st.e[i].parent;
}

/* ------------------------------------------------------------ sincronização */

static ag_group_t *new_group(const char *key, const char *name)
{
    if(st.ng >= AG_GROUPS_MAX) return NULL;
    char m[24];
    marker(m, sizeof m, st.next);
    ag_group_t *g = &st.g[st.ng];
    memset(g, 0, sizeof *g);
    g->id = st.next;
    if(key) snprintf(g->key, sizeof g->key, "%s", key);
    if(name) snprintf(g->name, sizeof g->name, "%s", name);
    st.ng++;
    st.next++;
    return g;
}

bool ag_sync(const ag_avail_t *a, int n)
{
    bool changed = false;
    if(!begin()) return false;
    char m[24];
    for(int i = 0; i < n; i++) {
        if(!a[i].id) {                                 /* semente: só na primeira vez */
            if(!fresh || !a[i].key || ag_group_by_key(a[i].key)) continue;
            ag_group_t *g = new_group(a[i].key, NULL);
            if(!g) continue;
            marker(m, sizeof m, g->id);
            if(!ins(st.ne, m, 0)) { st.ng--; continue; }
            changed = true;
            continue;
        }
        if(!*a[i].id || strpbrk(a[i].id, "\t\r\n") || strlen(a[i].id) > 1100) continue;   /* não cabe no arquivo */
        if(find(a[i].id) >= 0 || !strncmp(a[i].id, "grp:", 4)) continue;
        int parent = 0;
        if(a[i].key && *a[i].key) {
            ag_group_t *g = (ag_group_t *)ag_group_by_key(a[i].key);
            if(!g && (g = new_group(a[i].key, NULL))) {
                marker(m, sizeof m, g->id);
                if(!ins(st.ne, m, 0)) { st.ng--; g = NULL; }
            }
            if(g) parent = g->id;
        }
        if(ins(st.ne, a[i].id, parent)) changed = true;
    }
    fresh = false;
    if(!changed) { st_free(&snap); return false; }
    /* sem gravar (cartão só de leitura): fica na memória, tenta de novo na próxima mudança */
    if(!save()) fprintf(stderr, "bigfoston: grupos: organização nova não gravada\n");
    st_free(&snap);
    serial++;                                          /* sem listener: quem sincroniza já desenha */
    return true;
}

void ag_forget(const char *id)
{
    int i = id ? find(id) : -1;
    if(i < 0 || ag_marker_gid(id)) return;
    if(!begin()) return;
    del(i);
    commit();
}

/* ------------------------------------------------------------ mudanças */

ag_err_t ag_group_create(const char *name, const char *item, int *out_gid)
{
    char nm[AG_NAME_BYTES];
    ag_err_t e = ag_name_clean(name, nm, sizeof nm);
    if(e) return e;
    if(ag_name_taken(nm, 0)) return AG_E_DUP;
    if(st.ng >= AG_GROUPS_MAX) return AG_E_FULL;
    int it = item ? find(item) : -1;
    if(item && it < 0) return AG_E_NOTFOUND;
    if(item && ag_marker_gid(item)) return AG_E_NESTED;
    if(!begin()) return AG_E_FULL;
    ag_group_t *g = new_group(NULL, nm);
    char m[24];
    marker(m, sizeof m, g->id);
    /* a pasta nasce no lugar do app; se ele estava num grupo, logo depois da pasta dele */
    int at = st.ne;
    if(it >= 0) {
        int p = st.e[it].parent;
        if(!p) at = it;
        else {
            char pm[24];
            marker(pm, sizeof pm, p);
            int pi = find(pm);
            at = pi >= 0 ? pi + 1 : st.ne;
        }
    }
    if(!ins(at, m, 0)) return rollback(AG_E_FULL);
    if(item) {
        it = find(item);
        st.e[it].parent = g->id;
    }
    int gid = g->id;
    e = commit();
    if(!e && out_gid) *out_gid = gid;
    return e;
}

ag_err_t ag_group_rename(int gid, const char *name)
{
    ag_group_t *g = grp(gid);
    if(!g) return AG_E_NOTFOUND;
    char nm[AG_NAME_BYTES];
    ag_err_t e = ag_name_clean(name, nm, sizeof nm);
    if(e) return e;
    if(!strcmp(ag_group_name(g), nm)) return AG_E_SAME;
    if(ag_name_taken(nm, gid)) return AG_E_DUP;
    if(!begin()) return AG_E_FULL;
    snprintf(grp(gid)->name, sizeof g->name, "%s", nm);
    return commit();
}

ag_err_t ag_group_style(int gid, uint32_t color, const char *icon)
{
    ag_group_t *g = grp(gid);
    if(!g) return AG_E_NOTFOUND;
    char ic[AG_ICON_MAX] = "";
    if(icon && *icon && safe_word(icon, sizeof ic)) snprintf(ic, sizeof ic, "%s", icon);
    color &= 0xffffff;
    if(g->color == color && !strcmp(g->icon, ic)) return AG_E_SAME;
    if(!begin()) return AG_E_FULL;
    g = grp(gid);
    g->color = color;
    snprintf(g->icon, sizeof g->icon, "%s", ic);
    return commit();
}

ag_err_t ag_group_delete(int gid)
{
    if(!grp(gid)) return AG_E_NOTFOUND;
    char m[24];
    marker(m, sizeof m, gid);
    ent_t *v = malloc(sizeof *v * (size_t)(st.ne ? st.ne : 1));
    if(!v) return AG_E_FULL;
    if(!begin()) { free(v); return AG_E_FULL; }
    /* os apps tomam o lugar da pasta, na ordem em que estavam */
    int k = 0;
    char *mk = NULL;
    for(int i = 0; i < st.ne; i++) {
        if(st.e[i].parent == gid) continue;
        if(!strcmp(st.e[i].id, m)) {
            mk = st.e[i].id;
            for(int j = 0; j < st.ne; j++)
                if(st.e[j].parent == gid) v[k++] = (ent_t){ st.e[j].id, 0 };
            continue;
        }
        v[k++] = st.e[i];
    }
    if(!mk)                                            /* sem pasta (não deveria): no fim */
        for(int j = 0; j < st.ne; j++)
            if(st.e[j].parent == gid) v[k++] = (ent_t){ st.e[j].id, 0 };
    free(mk);
    memcpy(st.e, v, sizeof *v * (size_t)k);
    st.ne = k;
    free(v);
    for(int i = 0; i < st.ng; i++)
        if(st.g[i].id == gid) { memmove(&st.g[i], &st.g[i + 1], sizeof st.g[0] * (size_t)(st.ng - i - 1)); st.ng--; break; }
    return commit();
}

ag_err_t ag_move(const char *item, int gid)
{
    int i = item ? find(item) : -1;
    if(i < 0) return AG_E_NOTFOUND;
    if(ag_marker_gid(item)) return AG_E_NESTED;
    if(gid && !grp(gid)) return AG_E_NOTFOUND;
    int from = st.e[i].parent;
    if(from == gid) return AG_E_SAME;
    if(!begin()) return AG_E_FULL;
    if(gid) relocate(i, st.ne, gid);                   /* no fim do grupo */
    else {                                             /* para fora: logo depois da pasta */
        char m[24];
        marker(m, sizeof m, from);
        int mi = find(m);
        relocate(i, mi >= 0 ? mi + 1 : st.ne, 0);
    }
    return commit();
}

ag_err_t ag_place(const char *item, const char *before, int parent)
{
    int i = item ? find(item) : -1;
    if(i < 0) return AG_E_NOTFOUND;
    int b = before ? find(before) : -1;
    if(before && b < 0) return AG_E_NOTFOUND;
    if(b == i) return AG_E_SAME;
    int np = b >= 0 ? st.e[b].parent : parent;
    if(np && !grp(np)) return AG_E_NOTFOUND;
    if(np && ag_marker_gid(item)) return AG_E_NESTED;
    /* já está ali? (logo antes de before, ou o último do pai) */
    if(b >= 0 && st.e[i].parent == np) {
        int k = i + 1;
        while(k < st.ne && st.e[k].parent != np) k++;
        if(k == b) return AG_E_SAME;
    }
    if(b < 0 && st.e[i].parent == np) {
        int k = i + 1;
        while(k < st.ne && st.e[k].parent != np) k++;
        if(k == st.ne) return AG_E_SAME;
    }
    if(!begin()) return AG_E_FULL;
    relocate(i, b >= 0 ? b : st.ne, np);
    return commit();
}

typedef struct { int (*cmp)(const char *, const char *, void *); void *ud; } sortctx_t;
static sortctx_t sctx;

static int sort_cmp(const void *x, const void *y)
{
    return sctx.cmp(((const ent_t *)x)->id, ((const ent_t *)y)->id, sctx.ud);
}

ag_err_t ag_sort(int parent, int (*cmp)(const char *, const char *, void *), void *ud)
{
    if(parent && !grp(parent)) return AG_E_NOTFOUND;
    int n = 0;
    for(int i = 0; i < st.ne; i++) if(st.e[i].parent == parent) n++;
    if(n < 2) return AG_E_SAME;
    ent_t *v = malloc(sizeof *v * (size_t)n);
    int *pos = malloc(sizeof *pos * (size_t)n);
    if(!v || !pos) { free(v); free(pos); return AG_E_FULL; }
    int k = 0;
    for(int i = 0; i < st.ne; i++) if(st.e[i].parent == parent) { pos[k] = i; v[k++] = st.e[i]; }
    sctx = (sortctx_t){ cmp, ud };
    /* estável: qsort não é; ordenação por inserção (poucos itens) */
    for(int i = 1; i < n; i++) {
        ent_t t = v[i];
        int j = i - 1;
        while(j >= 0 && sort_cmp(&v[j], &t) > 0) { v[j + 1] = v[j]; j--; }
        v[j + 1] = t;
    }
    bool same = true;
    for(int i = 0; i < n; i++) if(st.e[pos[i]].id != v[i].id) same = false;
    if(same) { free(v); free(pos); return AG_E_SAME; }
    if(!begin()) { free(v); free(pos); return AG_E_FULL; }
    /* begin() copiou as strings para snap; st continua com as mesmas: só a ordem muda */
    for(int i = 0; i < n; i++) st.e[pos[i]] = v[i];
    free(v);
    free(pos);
    return commit();
}
