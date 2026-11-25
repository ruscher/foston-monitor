/* test_appgroups.c — organização dos apps em grupos (core/appgroups.c): migração (Jogos como
 * semente, pacotes no grupo da categoria), criar grupo levando o app, enviar, mover entre
 * grupos, tirar do grupo, renomear, excluir sem perder apps, nomes (vazio, acentos, repetido,
 * comprido), reordenar, persistência (reabrir), arquivo corrompido (.bak), erro de gravação
 * (nada pela metade), app que some e volta, e muitos grupos/apps. */
#define _DEFAULT_SOURCE
#include "core/appgroups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails, total;

static void ok(const char *what, int cond)
{
    total++;
    if(!cond) { fails++; printf("FALHOU %s\n", what); }
}

static char file[600];

static const char *namer(const char *key) { return !strcmp(key, "games") ? "Jogos" : key; }

/* filhos de um pai numa string "a,b,c" */
static const char *kids(int parent)
{
    static char b[4096];
    const char *v[256];
    int n = ag_children(parent, v, 256);
    b[0] = 0;
    for(int i = 0; i < n && i < 256; i++) { if(i) strcat(b, ","); strcat(b, v[i]); }
    return b;
}

static int gid_of(const char *name)
{
    for(int i = 0; i < ag_group_count(); i++)
        if(!strcmp(ag_group_name(ag_group_at(i)), name)) return ag_group_at(i)->id;
    return 0;
}

static const ag_avail_t base[] = {
    { "sys:music", NULL }, { "sys:tv", NULL }, { "sys:settings", NULL },
    { NULL, "games" },
    { "pkg:sc-controller", "games" }, { "pkg:editor", "office" },
    { "web:https://exemplo.com.br/", NULL },
};
#define NBASE (int)(sizeof base / sizeof base[0])

static int by_id(const char *a, const char *b, void *ud) { (void)ud; return strcmp(a, b); }
static int by_id_rev(const char *a, const char *b, void *ud) { (void)ud; return strcmp(b, a); }

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "uso: %s PASTA\n", argv[0]); return 2; }
    snprintf(file, sizeof file, "%s/appgroups.conf", argv[1]);
    char bak[620], bad[620];
    snprintf(bak, sizeof bak, "%s.bak", file);
    snprintf(bad, sizeof bad, "%s.bad", file);
    unlink(file); unlink(bak); unlink(bad);
    ag_set_namer(namer);

    /* ---- migração */
    ag_open(file);
    ok("sem arquivo: começa do zero", ag_is_fresh());
    ok("sincroniza", ag_sync(base, NBASE));
    ok("ordem: sistema, Jogos, categoria nova, web",
       !strcmp(kids(0), "sys:music,sys:tv,sys:settings,grp:1,grp:2,web:https://exemplo.com.br/"));
    ok("Jogos com o SC Controller", !strcmp(kids(1), "pkg:sc-controller"));
    ok("nome de categoria traduzido", !strcmp(ag_group_name(ag_group_get(1)), "Jogos"));
    ok("Escritório pela key", ag_group_by_key("office") && !strcmp(kids(ag_group_by_key("office")->id), "pkg:editor"));
    ok("nada novo: não muda", !ag_sync(base, NBASE));

    /* ---- criar grupo levando o app (atômico) */
    int g = 0;
    ok("cria Entretenimento com a TV", ag_group_create("  Entretenimento ", "sys:tv", &g) == AG_OK && g == 3);
    ok("a TV saiu do principal e a pasta ficou no lugar dela",
       !strcmp(kids(0), "sys:music,grp:3,sys:settings,grp:1,grp:2,web:https://exemplo.com.br/"));
    ok("a TV está no grupo", !strcmp(kids(g), "sys:tv") && ag_parent("sys:tv") == g);
    ok("nome sem os espaços", !strcmp(ag_group_get(g)->name, "Entretenimento"));

    /* ---- nomes */
    char nm[AG_NAME_BYTES];
    ok("vazio recusado", ag_group_create("   ", "sys:music", NULL) == AG_E_EMPTY);
    ok("repetido (maiúsculas) recusado", ag_group_create("ENTRETENIMENTO", "sys:music", NULL) == AG_E_DUP);
    ok("repetido com categoria recusado", ag_group_create("jogos", NULL, NULL) == AG_E_DUP);
    ok("acentos: Educação", ag_group_create("Educação", NULL, &g) == AG_OK && !strcmp(ag_group_get(g)->name, "Educação"));
    ok("acentos sem maiúsculas: EDUCAÇÃO repetido", ag_name_taken("EDUCAÇÃO", 0));
    ok("24 caracteres acentuados cabem", ag_name_clean("ááááááááááááááááááááááá1", nm, sizeof nm) == AG_OK);
    ok("25 caracteres não", ag_name_clean("áááááááááááááááááááááááá1", nm, sizeof nm) == AG_E_LONG);
    ok("tab e quebra de linha viram espaço", ag_name_clean("a\tb\nc", nm, sizeof nm) == AG_OK && !strcmp(nm, "a b c"));
    ok("grupo vazio existe", ag_children(g, NULL, 0) == 0 && ag_group_get(g));
    int edu = g;

    /* ---- enviar / mover entre grupos / tirar */
    int ent = gid_of("Entretenimento");
    ok("envia Multimídia para Entretenimento", ag_move("sys:music", ent) == AG_OK);
    ok("no fim do grupo", !strcmp(kids(ent), "sys:tv,sys:music"));
    ok("enviar de novo: já está", ag_move("sys:music", ent) == AG_E_SAME);
    ok("move a TV para Educação", ag_move("sys:tv", edu) == AG_OK && !strcmp(kids(ent), "sys:music") &&
       !strcmp(kids(edu), "sys:tv"));
    ok("sem duplicata", ag_children(0, NULL, 0) + ag_children(1, NULL, 0) + ag_children(2, NULL, 0) +
       ag_children(ent, NULL, 0) + ag_children(edu, NULL, 0) == 10);
    ok("tira Multimídia: logo depois da pasta", ag_move("sys:music", 0) == AG_OK &&
       !strcmp(kids(0), "grp:3,sys:music,sys:settings,grp:1,grp:2,web:https://exemplo.com.br/,grp:4"));
    ok("grupo vazio continua", ag_group_get(ent) && ag_children(ent, NULL, 0) == 0);
    ok("pasta não entra em pasta", ag_move("grp:1", ent) == AG_E_NESTED && ag_place("grp:1", "sys:tv", 0) == AG_E_NESTED);
    ok("app que não existe", ag_move("sys:nada", ent) == AG_E_NOTFOUND);

    /* ---- reordenar */
    ok("Ajustes antes de Multimídia", ag_place("sys:settings", "sys:music", 0) == AG_OK &&
       !strcmp(kids(0), "grp:3,sys:settings,sys:music,grp:1,grp:2,web:https://exemplo.com.br/,grp:4"));
    ok("lugar igual: nada", ag_place("sys:settings", "sys:music", 0) == AG_E_SAME);
    ok("pasta para o fim", ag_place("grp:3", NULL, 0) == AG_OK &&
       !strcmp(kids(0), "sys:settings,sys:music,grp:1,grp:2,web:https://exemplo.com.br/,grp:4,grp:3"));
    ok("arrastar para dentro (place com before no grupo)", ag_place("sys:music", "sys:tv", 0) == AG_OK &&
       !strcmp(kids(edu), "sys:music,sys:tv"));
    ok("ordena ao contrário", ag_sort(edu, by_id_rev, NULL) == AG_OK && !strcmp(kids(edu), "sys:tv,sys:music"));
    ok("ordena o grupo", ag_sort(edu, by_id, NULL) == AG_OK && !strcmp(kids(edu), "sys:music,sys:tv"));
    ok("ordenado: nada", ag_sort(edu, by_id, NULL) == AG_E_SAME);

    /* ---- renomear */
    ok("renomeia", ag_group_rename(ent, "Diversão") == AG_OK && !strcmp(ag_group_name(ag_group_get(ent)), "Diversão"));
    ok("renomear para o de outro: recusado", ag_group_rename(ent, "educação") == AG_E_DUP);
    ok("renomear igual", ag_group_rename(ent, "Diversão") == AG_E_SAME);
    ok("renomeia Jogos (categoria)", ag_group_rename(1, "Meus jogos") == AG_OK && !strcmp(ag_group_get(1)->key, "games"));
    ok("estilo", ag_group_style(ent, 0x112233, "tv") == AG_OK && ag_group_get(ent)->color == 0x112233);

    /* ---- persistência */
    unsigned s0 = ag_serial();
    ag_open(file);
    ok("reaberto não é novo", !ag_is_fresh() && ag_serial() != s0);
    ok("mesma ordem depois de reabrir",
       !strcmp(kids(0), "sys:settings,grp:1,grp:2,web:https://exemplo.com.br/,grp:4,grp:3"));
    ok("mesmos grupos", !strcmp(kids(edu), "sys:music,sys:tv") && !strcmp(ag_group_get(1)->name, "Meus jogos") &&
       !strcmp(ag_group_get(ent)->icon, "tv") && ag_group_get(ent)->color == 0x112233);
    ok("semente não volta", !ag_sync(base, NBASE));

    /* ---- excluir grupo: os apps voltam no lugar da pasta */
    ok("exclui Educação", ag_group_delete(edu) == AG_OK && !ag_group_get(edu));
    ok("apps no lugar dela, na ordem",
       !strcmp(kids(0), "sys:settings,grp:1,grp:2,web:https://exemplo.com.br/,sys:music,sys:tv,grp:3"));
    ok("excluir de novo: não existe", ag_group_delete(edu) == AG_E_NOTFOUND);
    int nn = 0;
    ok("cria outro: id novo (não reaproveita)", ag_group_create("Outro", NULL, &nn) == AG_OK && nn == 5);
    ag_group_delete(nn);

    /* ---- app que some (cartão fora) e volta; desinstalar */
    static const ag_avail_t less[] = { { "sys:music", NULL }, { "sys:tv", NULL }, { "sys:settings", NULL } };
    ok("o que some não é apagado", !ag_sync(less, 3) && ag_parent("pkg:sc-controller") == 1);
    ag_forget("pkg:sc-controller");
    ok("desinstalar apaga", ag_parent("pkg:sc-controller") == -1 && ag_group_get(1));
    ok("reinstalar volta ao grupo da categoria", ag_sync(base, NBASE) && ag_parent("pkg:sc-controller") == 1);

    /* ---- erro de gravação: nada pela metade */
    char before[4096];
    snprintf(before, sizeof before, "%s", kids(0));
    int ng = ag_group_count();
    ag_test_fail_next_save(true);
    ok("criar com erro de gravação falha", ag_group_create("Falha", "sys:music", NULL) == AG_E_WRITE);
    ok("nem grupo nem app movido", ag_group_count() == ng && !strcmp(kids(0), before) && ag_parent("sys:music") == 0);
    ag_test_fail_next_save(true);
    ok("excluir com erro: grupo continua", ag_group_delete(1) == AG_E_WRITE && ag_group_get(1) &&
       ag_parent("pkg:sc-controller") == 1);

    /* ---- arquivo corrompido: usa o .bak; truncado também */
    ag_close();
    FILE *f = fopen(file, "w");
    if(f) { fputs("bigfoston-apps\t1\nnext\t9\ngroup\t1\t-\t0\t-\tMeio", f); fclose(f); }   /* sem "end" */
    ag_open(file);
    ok("truncado: usa a cópia anterior", !ag_is_fresh() && ag_group_get(1) != NULL);
    ok("truncado: guardado em .bad", access(bad, F_OK) == 0);
    unlink(bad);
    ag_close();
    f = fopen(file, "w");
    if(f) { fputs("bigfoston-apps\t99\nend\n", f); fclose(f); }
    unlink(bak);
    ag_open(file);
    ok("versão futura sem cópia: do zero, sem apagar (.bad)", ag_is_fresh() && access(bad, F_OK) == 0);
    unlink(bad);
    ag_close();
    f = fopen(file, "w");                              /* lixo e repetições */
    if(f) {
        fputs("bigfoston-apps\t1\nnext\t2\ngroup\t7\tgames\tzz\t../x\t\ngroup\t7\t-\t0\t-\tB\n"
              "item\t0\tsys:a\nitem\t0\tsys:a\nitem\t7\tsys:b\nitem\t3\tsys:c\nitem\t5\tgrp:7\nitem\t0\tgrp:8\n"
              "linha nova\tde versão futura\nend\n", f);
        fclose(f);
    }
    ag_open(file);
    ok("dados inválidos: limpos", !ag_is_fresh() && ag_group_count() == 1 && ag_group_get(7)->icon[0] == 0 &&
       ag_group_get(7)->color == 0);
    ok("duplicata e pai sumido corrigidos", !strcmp(kids(0), "sys:a,sys:c,grp:7") && !strcmp(kids(7), "sys:b"));
    ok("próximo id depois do maior", ag_group_create("Novo", NULL, &nn) == AG_OK && nn == 8);

    /* ---- muitos */
    ag_close();
    unlink(file); unlink(bak);
    ag_open(file);
    static ag_avail_t many[200];
    static char ids[200][24];
    for(int i = 0; i < 200; i++) { snprintf(ids[i], sizeof ids[i], "pkg:app%03d", i); many[i] = (ag_avail_t){ ids[i], NULL }; }
    ag_sync(many, 200);
    clock_t t0 = clock();
    int made = 0;
    char gn[32];
    for(int i = 0; i < AG_GROUPS_MAX + 2; i++) {
        snprintf(gn, sizeof gn, "Grupo %d", i);
        if(ag_group_create(gn, ids[i], NULL) == AG_OK) made++;
    }
    ok("até o limite de grupos", made == AG_GROUPS_MAX && ag_group_create("x", NULL, NULL) == AG_E_FULL);
    for(int i = 100; i < 200; i++) ag_move(ids[i], ag_group_at(i % AG_GROUPS_MAX)->id);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    int tot = ag_children(0, NULL, 0);
    for(int i = 0; i < ag_group_count(); i++) tot += ag_children(ag_group_at(i)->id, NULL, 0);
    ok("200 apps + 48 pastas, nenhum perdido", tot == 200 + AG_GROUPS_MAX);
    ag_open(file);
    tot = ag_children(0, NULL, 0);
    for(int i = 0; i < ag_group_count(); i++) tot += ag_children(ag_group_at(i)->id, NULL, 0);
    ok("200 apps relidos", tot == 200 + AG_GROUPS_MAX);
    printf("appgroups: %d grupos e 150 envios com gravação em %.0f ms (PC)\n", made, ms);
    ag_close();

    printf("appgroups: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
