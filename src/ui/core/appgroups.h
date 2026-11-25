/* appgroups.h — a organização dos aplicativos em grupos (pastas) da tela inicial.
 *
 * Uma só organização para o sistema inteiro: Grade, Apps do Foston Glass, Apps do Painel e a
 * tela de um grupo leem daqui; cada tema só desenha. Nada aqui depende do LVGL (testável no
 * PC: tests/test_appgroups.c).
 *
 * Modelo: uma lista ordenada de entradas {id, pai}. O pai 0 é o nível principal; um número
 * > 0 é um grupo. A ordem dentro de um pai é a ordem relativa na lista. Os grupos aparecem no
 * nível principal como a entrada "grp:<n>", que marca a posição da pasta.
 *
 * IDs estáveis dos apps (nunca o nome exibido): "sys:<chave>" (apps do sistema, tabela de
 * scr_home.c), "pkg:<id>" (pacotes .bfapp, o nome da pasta) e "web:<url>" (WebApps).
 *
 * Persistência: <dados>/appgroups.conf, texto UTF-8 com campos separados por TAB, gravado só
 * quando algo muda, por troca atômica (.tmp + fsync + rename); a versão anterior fica em
 * .bak e é usada se a principal estiver ilegível (a ilegível vira .bad, para diagnóstico).
 * Uma operação que não consegue gravar é desfeita na memória (nada fica pela metade).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_APPGROUPS_H
#define BF_APPGROUPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AG_VERSION     1
#define AG_NAME_CHARS  24          /* caracteres (não bytes) no nome de um grupo */
#define AG_NAME_BYTES  (AG_NAME_CHARS * 4 + 1)
#define AG_KEY_MAX     16
#define AG_ICON_MAX    16
#define AG_GROUPS_MAX  48

typedef struct {
    int      id;                   /* > 0, estável (não é reaproveitado) */
    char     key[AG_KEY_MAX];      /* categoria de origem ("games"...) ou "" (criado pelo usuário) */
    char     name[AG_NAME_BYTES];  /* "" = o nome da categoria (traduzido, ver ag_set_namer) */
    uint32_t color;                /* 0 = a cor padrão */
    char     icon[AG_ICON_MAX];    /* "" = o ícone padrão; senão o nome de um ícone do sistema */
} ag_group_t;

typedef enum {
    AG_OK, AG_E_EMPTY, AG_E_LONG, AG_E_DUP, AG_E_FULL, AG_E_NOTFOUND, AG_E_SAME, AG_E_NESTED, AG_E_WRITE
} ag_err_t;

/* um app disponível agora; id NULL + key: "semente" de um grupo de categoria (criado só na
 * primeira vez, a migração: Jogos aparece mesmo vazio, como antes) */
typedef struct { const char *id; const char *key; } ag_avail_t;

/* abre o arquivo (lê; ou começa do zero). Pode ser chamado de novo (testes) */
void ag_open(const char *file);
void ag_close(void);
/* começou do zero (sem arquivo legível): a próxima sincronização planta as sementes */
bool ag_is_fresh(void);
/* sobe a cada mudança (a interface redesenha o que estiver aberto) */
unsigned ag_serial(void);
void ag_set_listener(void (*cb)(void));
/* nome exibido dos grupos de categoria sem nome próprio (a tradução de "games"...) */
void ag_set_namer(const char *(*fn)(const char *key));

/* acrescenta o que é novo: sem key no fim do nível principal; com key no grupo da categoria
 * (criado no fim se faltar). O que sumiu NÃO é apagado (cartão fora, por exemplo): só fica
 * escondido; ag_forget apaga de vez (desinstalar). true = mudou (e foi gravado). Não chama o
 * listener: quem sincroniza é quem vai desenhar */
bool ag_sync(const ag_avail_t *avail, int n);
void ag_forget(const char *id);

int ag_group_count(void);
const ag_group_t *ag_group_at(int i);          /* na ordem do nível principal */
const ag_group_t *ag_group_get(int gid);
const ag_group_t *ag_group_by_key(const char *key);
const char *ag_group_name(const ag_group_t *g); /* o nome próprio ou o da categoria */
int ag_marker_gid(const char *id);              /* "grp:<n>" -> n; senão 0 */

/* filhos de um pai, em ordem (ids apontam para a memória interna: válidos até a próxima
 * mudança). Devolve quantos existem (pode ser > max) */
int ag_children(int parent, const char **out, int max);
/* pai de um id: 0 principal, > 0 grupo, -1 não está na organização */
int ag_parent(const char *id);

/* nome de grupo: tira controles e espaços repetidos/nas pontas, confere o tamanho em
 * caracteres UTF-8 (AG_E_EMPTY / AG_E_LONG) */
ag_err_t ag_name_clean(const char *in, char *out, size_t n);
/* já existe um grupo com esse nome (sem diferença de maiúsculas, com acentos)? */
bool ag_name_taken(const char *name, int except_gid);

/* cria o grupo e põe o app nele, numa operação só (item pode ser NULL: grupo vazio). A pasta
 * nasce onde o app estava */
ag_err_t ag_group_create(const char *name, const char *item, int *out_gid);
ag_err_t ag_group_rename(int gid, const char *name);
ag_err_t ag_group_style(int gid, uint32_t color, const char *icon);
/* apaga só a pasta: os apps voltam ao nível principal, no lugar dela e na mesma ordem */
ag_err_t ag_group_delete(int gid);
/* para dentro do grupo (no fim) ou, com gid 0, para fora (logo depois da pasta) */
ag_err_t ag_move(const char *item, int gid);
/* reordena: item logo antes de before (que decide o pai), ou no fim de parent (before NULL).
 * Pastas só no nível principal (AG_E_NESTED) */
ag_err_t ag_place(const char *item, const char *before, int parent);
/* ordena os filhos de um pai (por nome, na interface) */
ag_err_t ag_sort(int parent, int (*cmp)(const char *a, const char *b, void *ud), void *ud);

/* diagnóstico: o próximo save falha (testes de erro de gravação) */
void ag_test_fail_next_save(bool on);
#endif
