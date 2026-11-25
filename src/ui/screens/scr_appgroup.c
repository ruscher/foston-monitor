/* scr_appgroup.c — um grupo de apps (pasta da tela Apps), igual em todos os temas.
 * Argumento: o id do grupo (core/appgroups.h). Toque abre o app; segurar 2 s abre o menu
 * (enviar para outro grupo, tirar do grupo, mover, desinstalar...); o botão Opções tem o menu
 * do grupo (renomear, cor e ícone, ordenar, excluir). Grupo de categoria (Jogos...) vazio
 * explica como instalar (.bfapp pelo Arquivos); grupo do usuário vazio explica como enviar.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "screens.h"
#include "launcher_ui.h"
#include "../core/theme.h"
#include "../core/fx.h"
#include "../widgets/page.h"
#include <stdio.h>
#include <string.h>

#define MAX_IN 128

static int gid;
static li_item_t in[MAX_IN];

static void files_click(lv_event_t *e) { (void)e; files_open_dir("/mnt/sd"); }
static void options_click(lv_event_t *e) { lui_group_menu(lv_event_get_current_target(e), gid); }

/* o ícone do grupo grande, no degradê da cor dele */
static lv_obj_t *badge(lv_obj_t *parent, int size)
{
    lv_color_t c = lv_color_hex(launcher_group_color(gid));
    lv_obj_t *sq = lv_obj_create(parent);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, size, size);
    lv_obj_set_style_radius(sq, size * 22 / 80, 0);
    lv_obj_set_style_bg_color(sq, lv_color_lighten(c, 30), 0);
    lv_obj_set_style_bg_grad_color(sq, lv_color_darken(c, 40), 0);
    lv_obj_set_style_bg_grad_dir(sq, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(sq, 18, 0);
    lv_obj_set_style_shadow_color(sq, c, 0);
    lv_obj_set_style_shadow_opa(sq, LV_OPA_40, 0);
    lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = lv_image_create(sq);
    lv_image_set_src(img, launcher_group_icon(gid));
    lv_image_set_scale(img, (uint32_t)(256 * size * 7 / 10 / 56));
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    return sq;
}

static void create(lv_obj_t *parent, int arg)
{
    const bf_palette_t *p = pal();
    launcher_sync();
    gid = arg;
    const ag_group_t *g = ag_group_get(gid);
    lv_obj_t *pg = page_create(parent);
    if(!g) {                                   /* excluído (pilha antiga, FIFO): nada a mostrar */
        bf_label(pg, tr(S_LG_E_GONE), BF_FONT_M, p->text_dim);
        return;
    }
    bool category = g->key[0] != 0;
    int n_in = launcher_items(gid, in, MAX_IN);

    /* cabeçalho: ícone, nome do grupo, quantos apps, Opções */
    lv_obj_t *head = bf_plain(pg);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, BF_SP_M, 0);
    badge(head, 64);
    lv_obj_t *col = bf_plain(head);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *nm = bf_label(col, launcher_group_name(gid), BF_FONT_L, p->text);
    bf_label_ellipsis(nm, LV_PCT(100));
    char sub[160], cnt[40];
    if(n_in == 1) snprintf(cnt, sizeof cnt, "%s", tr(S_LG_COUNT1));
    else snprintf(cnt, sizeof cnt, tr(S_LG_COUNTN), n_in);
    if(n_in && launcher_kiosk()) snprintf(sub, sizeof sub, "%s", cnt);
    else if(n_in) snprintf(sub, sizeof sub, "%s  ·  %s", cnt, tr(S_LG_GRP_HINT));
    else snprintf(sub, sizeof sub, "%s", tr(S_LG_EMPTY));
    bf_label(col, sub, BF_FONT_S, p->text_dim);
    if(!launcher_kiosk()) {                        /* quiosque: o grupo só se abre, não se organiza */
        lv_obj_t *opt = bf_button(head, LV_SYMBOL_SETTINGS, tr(S_LG_OPTIONS), 0);
        lv_obj_set_height(opt, 52);
        lv_obj_add_event_cb(opt, options_click, LV_EVENT_CLICKED, NULL);
    }

    if(n_in) {
        lv_obj_t *grid = bf_plain(pg);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, 16, 0);
        lv_obj_set_style_pad_column(grid, 16, 0);
        lv_obj_set_style_pad_top(grid, BF_SP_S, 0);
        for(int k = 0; k < n_in; k++) {
            lv_obj_t *t = launcher_tile(grid, &in[k]);
            lui_bind_tile(t, &in[k], gid);
            fx_slide_in(t, 0, 24, (uint32_t)(k < 8 ? k : 8) * 40, 220);
        }
        if(lui_moving()) {                         /* espaço para a faixa não cobrir a última linha */
            lv_obj_t *sp = bf_plain(pg);
            lv_obj_set_size(sp, 1, 40);
        }
    }

    if(category || (!n_in && !launcher_kiosk())) {
        /* grupo de categoria: como instalar mais; grupo do usuário vazio: como enviar apps */
        lv_obj_t *card = bf_card(pg);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card, BF_SP_M, 0);
        page_round_icon(card, category ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_RIGHT,
                        lv_color_hex(launcher_group_color(gid)), 44);
        lv_obj_t *tc = bf_plain(card);
        lv_obj_set_height(tc, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(tc, 1);
        lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
        bf_label(tc, tr(category ? S_GRP_MORE : S_LG_SEND), BF_FONT_M, p->text);
        lv_obj_t *how = bf_label(tc, tr(category ? S_GRP_HOWTO : S_LG_GRP_EMPTY), BF_FONT_S, p->text_dim);
        lv_obj_set_width(how, LV_PCT(100));
        lv_label_set_long_mode(how, LV_LABEL_LONG_WRAP);
        if(category) {
            lv_obj_t *b = bf_button(card, LV_SYMBOL_DIRECTORY, tr(S_GRP_OPEN_FILES), 1);
            lv_obj_set_height(b, 52);
            lv_obj_add_event_cb(b, files_click, LV_EVENT_CLICKED, NULL);
        }
    }
    lui_move_bar(parent, gid);
}

static const char *title_fn(int arg)
{
    launcher_load();
    return ag_group_get(arg) ? launcher_group_name(arg) : tr(S_LG_E_TITLE);
}

const bf_screen_def_t scr_appgroup_def = { create, NULL, S_GRP_GAMES, title_fn };
