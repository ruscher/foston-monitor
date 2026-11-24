/* dialog.c — modal centralizado sobre fundo escurecido. */
#include "dialog.h"
#include "../core/theme.h"

static const char dialog_magic = 0;

typedef struct { dialog_cb_t cb; void *ud; lv_obj_t *backdrop, *row; int primary, n; } dlg_t;

static void free_dlg(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

static void btn_click(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_current_target(e);
    dlg_t *d = lv_event_get_user_data(e);
    int idx = (int)lv_obj_get_index(b);
    dialog_cb_t cb = d->cb;
    void *ud = d->ud;
    lv_obj_delete_async(d->backdrop);
    if(cb) cb(idx, ud);
}

lv_obj_t *dialog_open(const char *symbol, lv_color_t sym_color, const char *title,
                      const char *body, const char *const *btns, int n_btns,
                      int primary_idx, int danger_idx, dialog_cb_t cb, void *ud)
{
    const bf_palette_t *p = pal();
    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_60, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);  /* bloqueia toques atrás */
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(bd, (void *)&dialog_magic);

    dlg_t *d = lv_malloc(sizeof *d);
    d->cb = cb; d->ud = ud; d->backdrop = bd;
    d->primary = primary_idx; d->n = n_btns;
    lv_obj_add_event_cb(bd, free_dlg, LV_EVENT_DELETE, d);

    lv_obj_t *card = bf_card(bd);
    lv_obj_set_width(card, 560);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_pad_all(card, BF_SP_XL, 0);
    lv_obj_set_style_radius(card, BF_RADIUS_L, 0);
    lv_obj_set_style_shadow_width(card, 40, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, BF_SP_L, 0);

    lv_obj_t *head = bf_plain(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, BF_SP_M, 0);
    if(symbol) bf_label(head, symbol, BF_FONT_L, sym_color);
    lv_obj_t *t = bf_label(head, title, BF_FONT_L, p->text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

    if(body) {
        lv_obj_t *b = bf_label(card, body, BF_FONT_M, p->text_dim);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *row = bf_plain(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    d->row = row;
    for(int i = 0; i < n_btns; i++) {
        int kind = i == danger_idx ? 2 : i == primary_idx ? 1 : 0;
        lv_obj_t *b = bf_button(row, NULL, btns[i], kind);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, btn_click, LV_EVENT_CLICKED, d);
    }
    return card;
}

void dialog_close_all(void)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_count(top);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&dialog_magic)
            lv_obj_delete_async(c);
    }
}

/* o diálogo de cima que ainda não foi respondido (NULL = nenhum) */
static dlg_t *top_dialog(void)
{
    lv_obj_t *top = lv_layer_top();
    for(int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if(lv_obj_get_user_data(c) != (void *)&dialog_magic || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        for(uint32_t k = 0; k < lv_obj_get_event_count(c); k++) {
            lv_event_dsc_t *e = lv_obj_get_event_dsc(c, k);
            if(lv_event_dsc_get_cb(e) == free_dlg) return lv_event_dsc_get_user_data(e);
        }
    }
    return NULL;
}

bool dialog_is_open(void) { return top_dialog() != NULL; }

bool dialog_key(bool enter)
{
    dlg_t *d = top_dialog();
    if(!d) return false;
    /* respondido: não acha de novo antes de sumir (delete_async) */
    lv_obj_set_user_data(d->backdrop, NULL);
    int idx = enter ? (d->primary >= 0 ? d->primary : d->n == 1 ? 0 : -2) : -1;
    if(idx == -2) { lv_obj_set_user_data(d->backdrop, (void *)&dialog_magic); return true; }
    if(idx >= 0) {
        lv_obj_t *b = lv_obj_get_child(d->row, idx);
        if(b) { lv_obj_send_event(b, LV_EVENT_CLICKED, NULL); return true; }
    }
    dialog_cb_t cb = d->cb;
    void *ud = d->ud;
    lv_obj_delete_async(d->backdrop);
    if(cb) cb(-1, ud);
    return true;
}
