/* scr_appgroup.c — um grupo de apps instalados (Apps > Jogos...), aberto pela pasta na grade.
 * Argumento: o grupo (bf_group_t). Toque abre o app, segurar oferece desinstalar. Vazio,
 * explica como instalar (.bfapp pelo Arquivos).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "screens.h"
#include "../core/theme.h"
#include "../core/apps.h"
#include "../core/fx.h"
#include "../core/webdata.h"
#include "../widgets/page.h"
#include "../widgets/tile.h"
#include <stdio.h>
#include <string.h>

#define MAX_IN 64

static int grp, idx[MAX_IN], n_in;

static void app_click(lv_event_t *e) { apps_launch(apps_get((int)(intptr_t)lv_event_get_user_data(e))); }
static void app_hold(lv_event_t *e) { apps_uninstall_ask(apps_get((int)(intptr_t)lv_event_get_user_data(e))); }
static void files_click(lv_event_t *e) { (void)e; files_open_dir("/mnt/sd"); }

/* o ícone do grupo grande, no degradê da cor dele */
static lv_obj_t *badge(lv_obj_t *parent, int size)
{
    lv_color_t c = lv_color_hex(desktop_group_color((bf_group_t)grp));
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
    lv_image_set_src(img, desktop_group_icon((bf_group_t)grp));
    lv_image_set_scale(img, (uint32_t)(256 * size * 7 / 10 / 56));
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    return sq;
}

static void create(lv_obj_t *parent, int arg)
{
    const bf_palette_t *p = pal();
    grp = arg >= 0 && arg < BF_GRP__N ? arg : BF_GRP_OTHER;
    n_in = apps_in_group((bf_group_t)grp, idx, MAX_IN);
    lv_obj_t *pg = page_create(parent);

    /* cabeçalho: ícone, nome do grupo, quantos apps */
    lv_obj_t *head = bf_plain(pg);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, BF_SP_M, 0);
    badge(head, 64);
    lv_obj_t *col = bf_plain(head);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    bf_label(col, desktop_group_name((bf_group_t)grp), BF_FONT_L, p->text);
    char sub[120];
    if(n_in == 1) snprintf(sub, sizeof sub, "%s  ·  %s", tr(S_GRP_COUNT1), tr(S_GRP_HINT));
    else if(n_in) {
        char cnt[40];
        snprintf(cnt, sizeof cnt, tr(S_GRP_COUNTN), n_in);
        snprintf(sub, sizeof sub, "%s  ·  %s", cnt, tr(S_GRP_HINT));
    } else snprintf(sub, sizeof sub, tr(S_GRP_EMPTY), desktop_group_name((bf_group_t)grp));
    bf_label(col, sub, BF_FONT_S, p->text_dim);

    if(n_in) {
        lv_obj_t *grid = bf_plain(pg);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, 16, 0);
        lv_obj_set_style_pad_column(grid, 16, 0);
        lv_obj_set_style_pad_top(grid, BF_SP_S, 0);
        for(int k = 0; k < n_in; k++) {
            const bf_app_entry_t *a = apps_get(idx[k]);
            const lv_image_dsc_t *ic = apps_icon(a);
            lv_obj_t *t;
            if(ic) t = tile_create(grid, ic, a->name, apps_color(a), NULL, NULL);
            else {
                char ini[8];
                web_initial(a->name, ini, sizeof ini);
                t = tile_create_text(grid, ini, a->name, apps_color(a), NULL, NULL);
            }
            /* SHORT_CLICKED: não abre depois de segurar o dedo */
            lv_obj_add_event_cb(t, app_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx[k]);
            lv_obj_add_event_cb(t, app_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx[k]);
            fx_slide_in(t, 0, 24, (uint32_t)k * 40, 220);
        }
    }

    /* como instalar (o conteúdo principal quando o grupo está vazio) */
    lv_obj_t *card = bf_card(pg);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, BF_SP_M, 0);
    page_round_icon(card, LV_SYMBOL_DOWNLOAD, lv_color_hex(desktop_group_color((bf_group_t)grp)), 44);
    lv_obj_t *tc = bf_plain(card);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    bf_label(tc, tr(S_GRP_MORE), BF_FONT_M, p->text);
    lv_obj_t *how = bf_label(tc, tr(S_GRP_HOWTO), BF_FONT_S, p->text_dim);
    lv_obj_set_width(how, LV_PCT(100));
    lv_label_set_long_mode(how, LV_LABEL_LONG_WRAP);
    lv_obj_t *b = bf_button(card, LV_SYMBOL_DIRECTORY, tr(S_GRP_OPEN_FILES), 1);
    lv_obj_set_height(b, 52);
    lv_obj_add_event_cb(b, files_click, LV_EVENT_CLICKED, NULL);
}

static const char *title_fn(int arg) { return desktop_group_name(arg >= 0 && arg < BF_GRP__N ? (bf_group_t)arg : BF_GRP_OTHER); }

const bf_screen_def_t scr_appgroup_def = { create, NULL, S_GRP_GAMES, title_fn };
