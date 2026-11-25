/* apps.h — apps instalados (pacotes .bfapp em <dados>/apps/<id>/) na interface: a lista
 * por grupo (Categories= do .desktop), os ícones, abrir, instalar e desinstalar.
 *
 * Instalar: Arquivos -> tocar no .bfapp, o FIFO de controle ("install CAMINHO") ou
 * tools/bigfoston/bfapp.py install. O app novo entra no grupo da categoria (core/launcher.c).
 * Desinstalar: segurar o ícone 2 s > Desinstalar (o app sai também dos grupos).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_APPS_H
#define BF_APPS_H
#include "desktop.h"
#include "lvgl.h"
#include <stdbool.h>

/* os apps instalados (relido depois de instalar/desinstalar) */
int apps_count(void);
const bf_app_entry_t *apps_get(int i);
const bf_app_entry_t *apps_find(const char *id);
bf_group_t apps_group_of(const bf_app_entry_t *a);
/* índices dos apps do grupo, em ordem de nome; devolve quantos */
int apps_in_group(bf_group_t g, int *out, int max);

/* ícone 56x56 (A8, pintado de branco como os do sistema) ou NULL (sem ícone: a inicial) */
const lv_image_dsc_t *apps_icon(const bf_app_entry_t *a);
uint32_t apps_color(const bf_app_entry_t *a);

/* abre: a tela do shell (X-BigFoston-Screen), o Terminal (Terminal=true) ou o Exec */
void apps_launch(const bf_app_entry_t *a);

/* pergunta "Instalar?" com o nome, o grupo e o tamanho e instala (com o aviso no fim) */
void apps_install_ask(const char *file);
/* instala sem perguntar (FIFO de controle, testes); true = ok */
bool apps_install_now(const char *file);
void apps_uninstall_ask(const bf_app_entry_t *a);
bool apps_uninstall_now(const char *id);

/* pasta dos apps instalados: <dados>/apps */
void apps_dir(char *out, size_t n);
#endif
