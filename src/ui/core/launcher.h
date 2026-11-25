/* launcher.h — os itens da grade de Apps (apps do sistema, pacotes .bfapp, WebApps e pastas)
 * sobre a organização de core/appgroups.c: resolve o ID estável de cada um no nome, ícone,
 * cor e no que abrir. Usado por todos os temas da tela inicial (Grade, Apps do Foston Glass,
 * Apps do Painel) e pela tela de um grupo.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_LAUNCHER_H
#define BF_LAUNCHER_H
#include "appgroups.h"
#include "lvgl.h"
#include <stdbool.h>

typedef enum { LI_SYS, LI_PKG, LI_WEB, LI_GROUP } li_kind_t;

typedef struct {
    li_kind_t kind;
    int idx;            /* app do sistema (tabela de scr_home.c), apps_get(), web_app() ou id do grupo */
    const char *id;     /* ID estável (válido até a próxima mudança da organização) */
} li_item_t;

/* lê a organização (uma vez) e acrescenta o que for novo: apps do sistema, pacotes (no grupo
 * da categoria) e WebApps. Barato quando nada mudou */
void launcher_sync(void);
/* relê o arquivo na próxima sincronização (outra pasta de dados: testes) */
void launcher_reopen(void);
/* só lê o arquivo (uma vez), sem procurar apps novos: títulos, FIFO */
void launcher_load(void);
/* os itens visíveis de um pai (0 = principal, ou o id do grupo), em ordem; devolve quantos */
int launcher_items(int parent, li_item_t *out, int max);
/* modo quiosque: a grade de Apps sem o Ajustes e sem os menus de organização */
bool launcher_kiosk(void);
bool launcher_resolve(const char *id, li_item_t *out);

const char *launcher_name(const li_item_t *it, char *buf, size_t n);
const lv_image_dsc_t *launcher_icon(const li_item_t *it);   /* NULL: desenhar a inicial */
uint32_t launcher_color(const li_item_t *it);
void launcher_open(const li_item_t *it);
/* um tile da grade (152x146): ícone, inicial ou pasta com miniaturas; sem eventos */
lv_obj_t *launcher_tile(lv_obj_t *parent, const li_item_t *it);

/* grupos: nome exibido, cor, ícone (A8) e quantos apps visíveis */
const char *launcher_group_name(int gid);
uint32_t launcher_group_color(int gid);
const lv_image_dsc_t *launcher_group_icon(int gid);
int launcher_group_size(int gid);

/* ícones e cores que um grupo pode usar (Aparência do grupo) */
int launcher_icon_choices(void);
const char *launcher_icon_choice_key(int i);
const lv_image_dsc_t *launcher_icon_choice(int i);
int launcher_color_choices(void);
uint32_t launcher_color_choice(int i);

/* apagar de vez da organização (o app foi desinstalado / o WebApp removido) */
void launcher_forget_pkg(const char *pkg_id);
void launcher_forget_web(const char *url);
/* "sys:<chave>", "pkg:<id>", "web:<url>" */
void launcher_id_pkg(char *out, size_t n, const char *pkg_id);
#endif
