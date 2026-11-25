/* launcher_ui.c — ver launcher_ui.h.
 *
 * Os IDs guardados aqui são cópias: a organização pode mudar (e liberar as strings dela)
 * enquanto um menu ou uma janela está aberta.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "launcher_ui.h"
#include "screens.h"
#include "../core/theme.h"
#include "../core/apps.h"
#include "../core/fx.h"
#include "../core/settings.h"
#include "../core/webdata.h"
#include "../widgets/hold.h"
#include "../widgets/ctxmenu.h"
#include "../widgets/namebox.h"
#include "../widgets/dialog.h"
#include "../widgets/glass.h"
#include "../assets/icons/bf_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_MAX 1100

/* o app/pasta do menu aberto */
static char sel_id[ID_MAX], sel_name[96];
static int sel_parent;
/* modo mover */
static bool mv_on;
static char mv_id[ID_MAX], mv_name[96];
static int mv_parent;

/* ------------------------------------------------------------ redesenhar */

static bool refresh_pending;

static void refresh_async(void *u)
{
    (void)u;
    refresh_pending = false;
    bf_nav_entry_t c = nav_current();
    if(c.scr == SCR_APPGROUP && !ag_group_get(c.arg)) { nav_back(); return; }   /* o grupo foi excluído */
    if(c.scr == SCR_APPGROUP || (c.scr == SCR_HOME && (c.arg == HOME_ARG_APPS || settings()->home_mode == BF_HOME_GRID)))
        nav_show_now();
}

static void refresh(void)
{
    if(refresh_pending) return;
    refresh_pending = true;
    lv_async_call(refresh_async, NULL);
}

void lui_init(void) { ag_set_listener(refresh); }

/* ------------------------------------------------------------ mensagens */

static const char *err_text(ag_err_t e)
{
    static char b[160];
    switch(e) {
    case AG_E_EMPTY: return tr(S_LG_E_EMPTY);
    case AG_E_LONG: return tr(S_LG_E_LONG);
    case AG_E_DUP: return tr(S_LG_E_DUP);
    case AG_E_FULL: snprintf(b, sizeof b, tr(S_LG_E_FULL), AG_GROUPS_MAX); return b;
    case AG_E_NOTFOUND: return tr(S_LG_E_GONE);
    case AG_E_WRITE: return tr(S_LG_E_WRITE);
    default: return NULL;
    }
}

static void err_dialog(ag_err_t e)
{
    const char *m = err_text(e);
    if(!m) return;
    const char *ok[1] = { tr(S_PKG_OK) };
    dialog_open(LV_SYMBOL_WARNING, pal()->danger, tr(S_LG_E_TITLE), m, ok, 1, 0, -1, NULL, NULL);
}

/* confirmação discreta: uma pílula no rodapé que some sozinha e não pega toques */
static lv_obj_t *toast_obj;

static void toast_gone(lv_anim_t *a) { lv_obj_delete(a->var); }

/* apagado por qualquer caminho (fim da animação, interface reconstruída) */
static void toast_deleted(lv_event_t *e) { if(lv_event_get_target(e) == toast_obj) toast_obj = NULL; }

static void toast_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

static void toast(const lv_image_dsc_t *icon, uint32_t color, const char *text, const char *sub)
{
    const bf_palette_t *p = pal();
    bool glass = theme_glass();
    if(toast_obj) lv_obj_delete(toast_obj);       /* (as animações dele vão junto) */
    lv_obj_t *t = toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(t, toast_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_max_width(t, 720, 0);
    lv_obj_set_style_radius(t, 24, 0);
    lv_obj_set_style_bg_color(t, glass ? lv_color_hex(GL_DEEP) : p->surface, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(t, 18, 0);
    lv_obj_set_style_shadow_opa(t, LV_OPA_40, 0);
    lv_obj_set_style_pad_left(t, 8, 0);
    lv_obj_set_style_pad_right(t, 18, 0);
    lv_obj_set_style_pad_column(t, 10, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* o toque passa */
    lv_obj_t *c = lv_obj_create(t);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 34, 34);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if(icon) {
        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, icon);
        lv_image_set_scale(img, 256 * 22 / 56);
        lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
    }
    char b[240];
    if(sub && *sub) snprintf(b, sizeof b, "%s  \xC2\xB7  %s", text, sub);
    else snprintf(b, sizeof b, "%s", text);
    lv_obj_t *l = bf_label(t, b, BF_FONT_S, glass ? lv_color_hex(GL_WHITE) : p->text);
    /* largura do próprio texto (até 640): reticências só se não couber */
    int32_t tw = lv_text_get_width(b, (uint32_t)strlen(b), BF_FONT_S, 0);
    lv_obj_set_width(l, tw < 640 ? tw + 2 : 640);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -(BF_BOTBAR_H + 14));
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, toast_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 1800);
    lv_anim_set_duration(&a, fx_anim() == LV_ANIM_ON ? 300 : 0);
    lv_anim_set_completed_cb(&a, toast_gone);
    lv_anim_start(&a);
}

/* aviso com o ícone e a cor do grupo */
static void notice_group(bf_str_t fmt, int gid, const char *sub)
{
    char t[160];
    snprintf(t, sizeof t, tr(fmt), launcher_group_name(gid));
    toast(launcher_group_icon(gid), launcher_group_color(gid), t, sub);
}

static void count_text(int gid, char *b, size_t n)
{
    int k = launcher_group_size(gid);
    if(!k) snprintf(b, n, "%s", tr(S_LG_EMPTY));
    else if(k == 1) snprintf(b, n, "%s", tr(S_LG_COUNT1));
    else snprintf(b, n, tr(S_LG_COUNTN), k);
}

/* ------------------------------------------------------------ criar e renomear */

static char new_item[ID_MAX];
static bool new_with_item;

static const char *create_cb(const char *text, void *ud)
{
    (void)ud;
    if(!text) return NULL;
    int gid = 0;
    ag_err_t e = ag_group_create(text, new_with_item ? new_item : NULL, &gid);
    if(e) return err_text(e);
    notice_group(S_LG_CREATED, gid, new_with_item ? sel_name : NULL);
    return NULL;
}

static void new_group(const char *item)
{
    new_with_item = item != NULL;
    snprintf(new_item, sizeof new_item, "%s", item ? item : "");
    namebox_open(LV_SYMBOL_PLUS, tr(S_LG_NEW_TITLE), tr(S_LG_NAME_LABEL), "", tr(S_LG_CREATE), AG_NAME_CHARS,
                 create_cb, NULL);
}

static int ren_gid;

static const char *rename_cb(const char *text, void *ud)
{
    (void)ud;
    if(!text) return NULL;
    ag_err_t e = ag_group_rename(ren_gid, text);
    return e == AG_E_SAME ? NULL : err_text(e);
}

static void rename_group(int gid)
{
    if(!ag_group_get(gid)) { err_dialog(AG_E_NOTFOUND); return; }
    ren_gid = gid;
    namebox_open(LV_SYMBOL_EDIT, tr(S_LG_RENAME), tr(S_LG_NAME_LABEL), launcher_group_name(gid), tr(S_LG_SAVE),
                 AG_NAME_CHARS, rename_cb, NULL);
}

/* ------------------------------------------------------------ enviar para o grupo */

enum { P_MAIN = LUI_P_MAIN, P_NEW = LUI_P_NEW, P_CANCEL = LUI_P_CANCEL };

static void pick_cb(int id, void *ud)
{
    (void)ud;
    if(id < 0 || id == P_CANCEL) return;
    if(id == P_NEW) { new_group(sel_id); return; }
    int from = ag_parent(sel_id);
    ag_err_t e = ag_move(sel_id, id);
    if(e == AG_E_SAME) { if(id) notice_group(S_LG_ALREADY, id, sel_name); return; }
    if(e) { err_dialog(e); return; }
    if(id) notice_group(S_LG_SENT, id, sel_name);
    else if(from > 0) {
        char t[160];
        snprintf(t, sizeof t, tr(S_LG_REMOVED), ag_group_name(ag_group_get(from)));
        toast(&bf_icon_files, launcher_group_color(from), t, sel_name);
    }
}

static void send_to_group(void)
{
    int ng = ag_group_count(), n = 0;
    ctx_item_t *it = calloc((size_t)ng + 3, sizeof *it);
    char (*notes)[24] = calloc((size_t)ng + 1, sizeof *notes);
    if(!it || !notes) { free(it); free(notes); return; }
    int parent = ag_parent(sel_id);
    if(parent > 0)
        it[n++] = (ctx_item_t){ LV_SYMBOL_HOME, NULL, 0, tr(S_LG_TO_MAIN), NULL, P_MAIN, false, false };
    for(int i = 0; i < ng; i++) {
        const ag_group_t *g = ag_group_at(i);
        if(!g) continue;
        snprintf(notes[i], sizeof notes[i], "%d", launcher_group_size(g->id));
        it[n++] = (ctx_item_t){ NULL, launcher_group_icon(g->id), launcher_group_color(g->id), ag_group_name(g),
                                notes[i], g->id, false, g->id == parent };
    }
    it[n++] = (ctx_item_t){ LV_SYMBOL_PLUS, NULL, 0, tr(S_LG_NEW_GROUP), NULL, P_NEW, false, false };
    it[n++] = (ctx_item_t){ LV_SYMBOL_CLOSE, NULL, 0, tr(S_CANCEL), NULL, P_CANCEL, false, false };
    ctxmenu_open(NULL, tr(S_LG_SEND), ng ? sel_name : tr(S_LG_NO_GROUPS), it, n, pick_cb, NULL);
    free(it);
    free(notes);
}

/* ------------------------------------------------------------ modo mover */

bool lui_moving(void) { return mv_on; }

void lui_move_cancel(void)
{
    if(!mv_on) return;
    mv_on = false;
    refresh();
}

static void move_start(const char *id, int parent, const char *name)
{
    mv_on = true;
    snprintf(mv_id, sizeof mv_id, "%s", id);
    snprintf(mv_name, sizeof mv_name, "%s", name);
    mv_parent = parent;
    refresh();
}

/* toque num ícone com o modo ligado: o app vai para lá (numa pasta: para dentro dela) */
static void move_to(const char *target)
{
    mv_on = false;
    int tg = ag_marker_gid(target);
    ag_err_t e;
    if(tg && !ag_marker_gid(mv_id) && strcmp(target, mv_id)) {
        e = ag_move(mv_id, tg);
        if(!e) notice_group(S_LG_SENT, tg, mv_name);
    } else if(!strcmp(target, mv_id)) e = AG_E_SAME;
    else {
        e = ag_place(mv_id, target, mv_parent);
        if(!e) {
            char t[160];
            snprintf(t, sizeof t, tr(S_LG_MOVED), mv_name);
            toast(&bf_icon_files, 0x3584e4, t, NULL);
        }
    }
    if(e && e != AG_E_SAME) err_dialog(e);
    refresh();                                        /* tira a faixa mesmo sem mudança */
}

static void move_end_click(lv_event_t *e)
{
    (void)e;
    mv_on = false;
    ag_err_t r = ag_place(mv_id, NULL, mv_parent);
    if(r && r != AG_E_SAME) err_dialog(r);
    refresh();
}

static void move_cancel_click(lv_event_t *e) { (void)e; lui_move_cancel(); }

void lui_move_bar(lv_obj_t *container, int parent)
{
    if(!mv_on) return;
    if(parent != mv_parent || ag_parent(mv_id) < 0) { mv_on = false; return; }   /* outro lugar: desiste */
    const bf_palette_t *p = pal();
    bool glass = theme_glass();
    lv_obj_t *bar = lv_obj_create(container);
    lv_obj_remove_style_all(bar);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(bar, 776, 50);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(bar, 25, 0);
    lv_obj_set_style_bg_color(bar, glass ? lv_color_hex(GL_DEEP) : p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, glass ? lv_color_hex(GL_SKY) : p->accent, 0);
    lv_obj_set_style_shadow_width(bar, 20, 0);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    bf_label(bar, LV_SYMBOL_SHUFFLE, BF_FONT_M, glass ? lv_color_hex(GL_SKY) : p->accent);
    char t[200];
    snprintf(t, sizeof t, tr(parent ? S_LG_MOVE_HINT_G : S_LG_MOVE_HINT), mv_name);
    lv_obj_t *l = bf_label(bar, t, BF_FONT_S, glass ? lv_color_hex(GL_WHITE) : p->text);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    for(int i = 0; i < 2; i++) {
        lv_obj_t *b = bf_button(bar, i ? LV_SYMBOL_CLOSE : LV_SYMBOL_DOWN, tr(i ? S_CANCEL : S_LG_MOVE_END), 0);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 40);
        if(glass) {                                    /* vidro: botões translúcidos */
            lv_obj_set_style_bg_color(b, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_10, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_set_style_text_color(b, lv_color_hex(GL_WHITE), 0);
            for(uint32_t c = 0; c < lv_obj_get_child_count(b); c++)
                lv_obj_set_style_text_color(lv_obj_get_child(b, (int32_t)c), lv_color_hex(GL_WHITE), 0);
        }
        lv_obj_add_event_cb(b, i ? move_cancel_click : move_end_click, LV_EVENT_CLICKED, NULL);
    }
}

/* ------------------------------------------------------------ cor e ícone */

static int st_gid;
static uint32_t st_color;
static int st_icon;
static lv_obj_t *st_colors, *st_icons;

static void style_mark(void)
{
    const bf_palette_t *p = pal();
    for(uint32_t i = 0; st_colors && i < lv_obj_get_child_count(st_colors); i++) {
        lv_obj_t *c = lv_obj_get_child(st_colors, (int32_t)i);
        bool on = launcher_color_choice((int)i) == st_color;
        lv_obj_set_style_border_width(c, on ? 4 : 0, 0);
        lv_obj_t *l = lv_obj_get_child(c, 0);
        if(l) lv_label_set_text(l, on ? LV_SYMBOL_OK : "");
    }
    for(uint32_t i = 0; st_icons && i < lv_obj_get_child_count(st_icons); i++) {
        lv_obj_t *c = lv_obj_get_child(st_icons, (int32_t)i);
        bool on = (int)i == st_icon;
        lv_obj_set_style_bg_color(c, on ? lv_color_hex(st_color) : p->surface2, 0);
        lv_obj_set_style_border_width(c, on ? 2 : 0, 0);
    }
}

static void color_click(lv_event_t *e)
{
    st_color = launcher_color_choice((int)(intptr_t)lv_event_get_user_data(e));
    style_mark();
}

static void icon_click(lv_event_t *e)
{
    st_icon = (int)(intptr_t)lv_event_get_user_data(e);
    style_mark();
}

static void style_answer(int b, void *ud)
{
    (void)ud;
    st_colors = st_icons = NULL;
    if(b != 1) return;
    ag_err_t e = ag_group_style(st_gid, st_color, launcher_icon_choice_key(st_icon));
    if(e && e != AG_E_SAME) err_dialog(e);
}

static void style_group(int gid)
{
    if(!ag_group_get(gid)) { err_dialog(AG_E_NOTFOUND); return; }
    const bf_palette_t *p = pal();
    st_gid = gid;
    st_color = launcher_group_color(gid);
    st_icon = 0;
    const lv_image_dsc_t *cur = launcher_group_icon(gid);
    for(int i = 0; i < launcher_icon_choices(); i++) if(launcher_icon_choice(i) == cur) { st_icon = i; break; }
    const char *btn[2] = { tr(S_CANCEL), tr(S_LG_SAVE) };
    lv_obj_t *card = dialog_open(BF_SYMBOL_PALETTE, p->accent, tr(S_LG_STYLE), launcher_group_name(gid), btn, 2, 1,
                                 -1, style_answer, NULL);
    lv_obj_set_width(card, 600);
    lv_obj_set_style_pad_all(card, BF_SP_L, 0);
    lv_obj_set_style_pad_row(card, BF_SP_S, 0);
    int at = 2;                                         /* depois do título e do nome */
    lv_obj_t *l = bf_label(card, tr(S_LG_COLOR), BF_FONT_S, p->text_dim);
    lv_obj_move_to_index(l, at++);
    st_colors = bf_plain(card);
    lv_obj_set_size(st_colors, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_colors, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(st_colors, 14, 0);
    lv_obj_move_to_index(st_colors, at++);
    for(int i = 0; i < launcher_color_choices(); i++) {
        lv_obj_t *c = lv_obj_create(st_colors);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, 52, 52);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(launcher_color_choice(i)), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_white(), 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *k = bf_label(c, "", BF_FONT_M, lv_color_white());
        lv_obj_center(k);
        lv_obj_add_event_cb(c, color_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    l = bf_label(card, tr(S_LG_ICON), BF_FONT_S, p->text_dim);
    lv_obj_move_to_index(l, at++);
    st_icons = bf_plain(card);
    lv_obj_set_size(st_icons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_icons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(st_icons, 10, 0);
    lv_obj_set_style_pad_row(st_icons, 10, 0);
    lv_obj_move_to_index(st_icons, at++);
    for(int i = 0; i < launcher_icon_choices(); i++) {
        lv_obj_t *c = lv_obj_create(st_icons);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, 56, 56);
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_white(), 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, launcher_icon_choice(i));
        lv_image_set_scale(img, 256 * 36 / 56);
        lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
        lv_obj_add_event_cb(c, icon_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    style_mark();
}

/* ------------------------------------------------------------ excluir e ordenar */

static int del_gid;

static void delete_answer(int b, void *ud)
{
    (void)ud;
    if(b != 1) return;
    char name[AG_NAME_BYTES], t[160];
    snprintf(name, sizeof name, "%s", launcher_group_name(del_gid));
    bool had = launcher_group_size(del_gid) > 0;
    ag_err_t e = ag_group_delete(del_gid);
    if(e) { err_dialog(e); return; }
    snprintf(t, sizeof t, tr(S_LG_DELETED), name);
    toast(&bf_icon_info, 0x8a93a5, t, had ? tr(S_LG_APPS_BACK) : NULL);
}

static void delete_group(int gid)
{
    if(!ag_group_get(gid)) { err_dialog(AG_E_NOTFOUND); return; }
    del_gid = gid;
    char t[160];
    snprintf(t, sizeof t, tr(S_LG_DEL_Q), launcher_group_name(gid));
    const char *btn[2] = { tr(S_CANCEL), tr(S_LG_DEL_BTN) };
    dialog_open(LV_SYMBOL_TRASH, pal()->danger, t, tr(S_LG_DEL_BODY), btn, 2, -1, 1, delete_answer, NULL);
}

/* nome sem acento e sem maiúsculas, para ordenar (á, ç, õ... ao lado de a, c, o) */
static void sort_key(const char *s, char *out, size_t n)
{
    static const char base[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    size_t o = 0;
    for(const unsigned char *p = (const unsigned char *)s; *p && o + 1 < n; p++) {
        unsigned char c = *p;
        if(c == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf) { c = (unsigned char)base[p[1] - 0x80]; p++; }
        if(c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        out[o++] = (char)c;
    }
    out[o] = 0;
}

static int by_name(const char *a, const char *b, void *ud)
{
    (void)ud;
    li_item_t x, y;
    char na[96], nb[96], ka[96], kb[96];
    bool ox = launcher_resolve(a, &x), oy = launcher_resolve(b, &y);
    if(!ox || !oy) return ox - oy;                    /* os escondidos vão para o fim */
    sort_key(launcher_name(&x, na, sizeof na), ka, sizeof ka);
    sort_key(launcher_name(&y, nb, sizeof nb), kb, sizeof kb);
    return strcmp(ka, kb);
}

static void sort_group(int gid)
{
    ag_err_t e = ag_sort(gid, by_name, NULL);
    if(e == AG_OK || e == AG_E_SAME) notice_group(S_LG_SORTED, gid, NULL);
    else err_dialog(e);
}

/* ------------------------------------------------------------ menus */


static char web_url[WEB_URL_MAX];

static void webremove_answer(int b, void *ud)
{
    (void)ud;
    if(b != 0) return;
    int j = web_app_find(web_url);
    if(j >= 0) web_app_remove(j);
    launcher_forget_web(web_url);
    refresh();
}

static void menu_cb(int id, void *ud)
{
    (void)ud;
    hold_unselect_last();
    li_item_t it;
    bool ok = launcher_resolve(sel_id, &it);
    int gid = ag_marker_gid(sel_id);
    if(id < 0) return;
    if(!ok && id != M_RENAME) { err_dialog(AG_E_NOTFOUND); return; }
    switch(id) {
    case M_OPEN: case G_OPEN: launcher_open(&it); break;
    case M_NEW: new_group(sel_id); break;
    case M_SEND: send_to_group(); break;
    case M_MOVE: case G_MOVE: move_start(sel_id, sel_parent, sel_name); break;
    case M_REMOVE: pick_cb(P_MAIN, NULL); break;
    case M_RENAME: rename_group(sel_parent); break;
    case M_UNINSTALL: apps_uninstall_ask(apps_get(it.idx)); break;
    case M_WEBREMOVE: {
        snprintf(web_url, sizeof web_url, "%s", web_app(it.idx)->url);
        char body[200];
        snprintf(body, sizeof body, tr(S_WEB_APP_REMOVE_Q), sel_name);
        const char *b[2] = { tr(S_WEB_REMOVE), tr(S_WEB_CANCEL) };
        dialog_open(LV_SYMBOL_TRASH, pal()->danger, tr(S_WEB_APP_REMOVE), body, b, 2, -1, 0, webremove_answer, NULL);
        break;
    }
    case G_RENAME: rename_group(gid); break;
    case G_STYLE: style_group(gid); break;
    case G_SORT: sort_group(gid); break;
    case G_DELETE: delete_group(gid); break;
    }
}

static void group_menu_at(lv_obj_t *anchor, int gid, bool from_grid)
{
    if(!ag_group_get(gid)) return;
    snprintf(sel_id, sizeof sel_id, "grp:%d", gid);
    snprintf(sel_name, sizeof sel_name, "%s", launcher_group_name(gid));
    sel_parent = 0;
    char sub[48];
    count_text(gid, sub, sizeof sub);
    ctx_item_t it[6];
    int n = 0;
    if(from_grid) it[n++] = (ctx_item_t){ LV_SYMBOL_DIRECTORY, NULL, 0, tr(S_LG_OPEN_GROUP), NULL, G_OPEN, false, false };
    it[n++] = (ctx_item_t){ LV_SYMBOL_EDIT, NULL, 0, tr(S_LG_RENAME), NULL, G_RENAME, false, false };
    it[n++] = (ctx_item_t){ BF_SYMBOL_PALETTE, NULL, 0, tr(S_LG_STYLE), NULL, G_STYLE, false, false };
    if(from_grid) it[n++] = (ctx_item_t){ LV_SYMBOL_SHUFFLE, NULL, 0, tr(S_LG_MOVE_GROUP), NULL, G_MOVE, false, false };
    it[n++] = (ctx_item_t){ LV_SYMBOL_LIST, NULL, 0, tr(S_LG_SORT), NULL, G_SORT, false, false };
    it[n++] = (ctx_item_t){ LV_SYMBOL_TRASH, NULL, 0, tr(S_LG_DELETE), NULL, G_DELETE, true, false };
    ctxmenu_open(anchor, sel_name, sub, it, n, menu_cb, NULL);
}

void lui_group_menu(lv_obj_t *anchor, int gid) { group_menu_at(anchor, gid, false); }

void lui_item_menu(lv_obj_t *anchor, const char *id, int parent)
{
    li_item_t it;
    if(!launcher_resolve(id, &it)) return;
    if(it.kind == LI_GROUP) { group_menu_at(anchor, it.idx, true); return; }
    snprintf(sel_id, sizeof sel_id, "%s", id);
    launcher_name(&it, sel_name, sizeof sel_name);
    sel_parent = parent;
    char sub[160];
    if(parent) snprintf(sub, sizeof sub, tr(S_LG_IN_GROUP), launcher_group_name(parent));
    else snprintf(sub, sizeof sub, "%s", tr(S_LG_IN_APPS));
    ctx_item_t m[8];
    int n = 0;
    m[n++] = (ctx_item_t){ LV_SYMBOL_PLAY, NULL, 0, tr(S_LG_OPEN), NULL, M_OPEN, false, false };
    m[n++] = (ctx_item_t){ LV_SYMBOL_PLUS, NULL, 0, tr(S_LG_NEW_GROUP), NULL, M_NEW, false, false };
    m[n++] = (ctx_item_t){ LV_SYMBOL_RIGHT, NULL, 0, tr(S_LG_SEND), NULL, M_SEND, false, false };
    m[n++] = (ctx_item_t){ LV_SYMBOL_SHUFFLE, NULL, 0, tr(S_LG_MOVE), NULL, M_MOVE, false, false };
    if(parent) {
        m[n++] = (ctx_item_t){ LV_SYMBOL_UPLOAD, NULL, 0, tr(S_LG_REMOVE), NULL, M_REMOVE, false, false };
        m[n++] = (ctx_item_t){ LV_SYMBOL_EDIT, NULL, 0, tr(S_LG_RENAME), NULL, M_RENAME, false, false };
    }
    if(it.kind == LI_PKG)
        m[n++] = (ctx_item_t){ LV_SYMBOL_TRASH, NULL, 0, tr(S_PKG_UNINSTALL), NULL, M_UNINSTALL, true, false };
    else if(it.kind == LI_WEB)
        m[n++] = (ctx_item_t){ LV_SYMBOL_TRASH, NULL, 0, tr(S_WEB_APP_REMOVE), NULL, M_WEBREMOVE, true, false };
    ctxmenu_open(anchor, sel_name, sub, m, n, menu_cb, NULL);
}

void lui_demo_pick(int id) { ctxmenu_pick(id); }

/* ------------------------------------------------------------ busca */

#define FIND_MAX 40
static char found[FIND_MAX][ID_MAX];
static int n_found;

static void found_cb(int id, void *ud)
{
    (void)ud;
    li_item_t it;
    if(id >= 0 && id < n_found && launcher_resolve(found[id], &it)) launcher_open(&it);
}

/* um pai: guarda os que têm q no nome (sem acento e sem maiúsculas) */
static void find_in(int parent, const char *q)
{
    li_item_t v[160];
    int n = launcher_items(parent, v, 160);
    char nm[96], k[96];
    for(int i = 0; i < n && n_found < FIND_MAX; i++) {
        if(v[i].kind == LI_GROUP) { find_in(v[i].idx, q); continue; }
        sort_key(launcher_name(&v[i], nm, sizeof nm), k, sizeof k);
        if(strstr(k, q)) snprintf(found[n_found++], ID_MAX, "%s", v[i].id);
    }
}

static const char *search_cb(const char *text, void *ud)
{
    (void)ud;
    static char msg[160];
    if(!text) return NULL;
    char q[96];
    sort_key(text, q, sizeof q);
    char *a = q;
    while(*a == ' ') a++;
    for(char *e = a + strlen(a); e > a && e[-1] == ' ';) *--e = 0;
    if(!*a) return tr(S_LG_SEARCH_HINT);
    n_found = 0;
    find_in(0, a);
    if(!n_found) { snprintf(msg, sizeof msg, tr(S_LG_SEARCH_NONE), text); return msg; }
    static ctx_item_t it[FIND_MAX];
    static char names[FIND_MAX][96], where[FIND_MAX][AG_NAME_BYTES];
    for(int i = 0; i < n_found; i++) {
        li_item_t li;
        launcher_resolve(found[i], &li);
        launcher_name(&li, names[i], sizeof names[i]);
        int g = ag_parent(found[i]);
        snprintf(where[i], sizeof where[i], "%s", g > 0 ? launcher_group_name(g) : "");
        const lv_image_dsc_t *ic = launcher_icon(&li);
        it[i] = (ctx_item_t){ ic ? NULL : BF_SYMBOL_GLOBE, ic, launcher_color(&li), names[i], g > 0 ? where[i] : NULL,
                              i, false, false };
    }
    ctxmenu_open(NULL, tr(S_LG_SEARCH), text, it, n_found, found_cb, NULL);
    return NULL;
}

void lui_search(void)
{
    namebox_open(BF_SYMBOL_SEARCH, tr(S_LG_SEARCH), tr(S_LG_SEARCH_HINT), "", tr(S_LG_SEARCH_GO), 40, search_cb, NULL);
}

static void search_click(lv_event_t *e) { (void)e; lui_search(); }

void lui_search_button(lv_obj_t *container)
{
    const bf_palette_t *p = pal();
    bool glass = theme_glass();
    lv_obj_t *b = lv_button_create(container);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 52, 52);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, glass ? lv_color_hex(GL_DEEP) : p->surface, 0);
    lv_obj_set_style_bg_opa(b, p->dark || glass ? LV_OPA_70 : LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, glass ? lv_color_hex(GL_BLUE) : p->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, glass ? lv_color_hex(GL_SKY) : p->dark ? lv_color_white() : p->border, 0);
    lv_obj_set_style_border_opa(b, glass ? LV_OPA_50 : p->dark ? LV_OPA_10 : LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = bf_label(b, BF_SYMBOL_SEARCH, BF_FONT_M, glass ? lv_color_hex(GL_WHITE) : p->text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, search_click, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------ ícones */

typedef struct { char *id; int parent; } bind_t;

static void tile_tap(lv_obj_t *obj, void *ud)
{
    (void)obj;
    bind_t *b = ud;
    if(mv_on) { move_to(b->id); return; }
    li_item_t it;
    if(launcher_resolve(b->id, &it)) launcher_open(&it);
}

static void tile_hold(lv_obj_t *obj, void *ud)
{
    bind_t *b = ud;
    if(mv_on || ctxmenu_is_open() || namebox_is_open() || dialog_is_open()) { hold_select(obj, false); return; }
    lui_item_menu(obj, b->id, b->parent);
}

static void bind_free(lv_event_t *e)
{
    bind_t *b = lv_event_get_user_data(e);
    free(b->id);
    free(b);
}

static void locked_tap(lv_event_t *e) { tile_tap(NULL, lv_event_get_user_data(e)); }

void lui_bind_tile(lv_obj_t *tile, const li_item_t *it, int parent)
{
    bind_t *b = malloc(sizeof *b);
    if(!b) return;
    b->id = strdup(it->id);
    b->parent = parent;
    if(!b->id) { free(b); return; }
    lv_obj_add_event_cb(tile, bind_free, LV_EVENT_DELETE, b);
    if(launcher_kiosk()) {                         /* quiosque: só abrir, sem organizar */
        lv_obj_add_event_cb(tile, locked_tap, LV_EVENT_CLICKED, b);
        return;
    }
    hold_attach(tile, tile_tap, tile_hold, b);
    if(mv_on && !strcmp(mv_id, it->id)) hold_select(tile, true);   /* o que está sendo movido */
}
