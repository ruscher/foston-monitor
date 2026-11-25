/* launcher_ui.h — a organização dos apps na interface, igual em todos os temas: o toque longo
 * (2 s) num app ou numa pasta abre o menu; criar grupo, enviar para o grupo, tirar do grupo,
 * renomear, cor e ícone, ordenar, excluir; e o modo "mover" (tocar no novo lugar), que faz o
 * papel do arrastar no toque resistivo de um ponto.
 *
 * As telas (Apps/Grade em scr_home.c e o grupo em scr_appgroup.c) só chamam lui_bind_tile()
 * em cada ícone e lui_move_bar() para a faixa do modo mover.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_LAUNCHER_UI_H
#define BF_LAUNCHER_UI_H
#include "../core/launcher.h"

void lui_init(void);
/* toque abre (ou, no modo mover, coloca ali); toque longo abre o menu. parent: onde o ícone
 * está (0 = tela de apps, ou o id do grupo) */
void lui_bind_tile(lv_obj_t *tile, const li_item_t *it, int parent);
/* faixa "toque onde colocar..." no rodapé de container, se o modo mover for deste lugar */
void lui_move_bar(lv_obj_t *container, int parent);
bool lui_moving(void);
/* busca por nome (todos os apps, dentro dos grupos também): o botão de lupa e a janela */
void lui_search_button(lv_obj_t *container);
void lui_search(void);
void lui_move_cancel(void);
/* menu de opções de um grupo (botão na tela do grupo) */
void lui_group_menu(lv_obj_t *anchor, int gid);
/* menus por código (testes e FIFO de controle) */
void lui_item_menu(lv_obj_t *anchor, const char *id, int parent);
void lui_demo_pick(int id);                        /* escolhe a opção do menu aberto */

/* opções dos menus (lui_demo_pick); na lista "Enviar para o grupo" a opção é o id do grupo */
enum { M_OPEN = 1, M_NEW, M_SEND, M_MOVE, M_REMOVE, M_RENAME, M_UNINSTALL, M_WEBREMOVE,
       G_OPEN, G_RENAME, G_STYLE, G_MOVE, G_SORT, G_DELETE };
enum { LUI_P_MAIN = 0, LUI_P_NEW = 1000001, LUI_P_CANCEL = 1000002 };
#endif
