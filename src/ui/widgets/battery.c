/* battery.c — ver battery.h. */
#include "battery.h"
#include "../core/theme.h"
#include "../core/i18n.h"

/* filhos do contêiner, na ordem de criação */
enum { CH_BODY, CH_CAP, CH_BOLT };

typedef struct {
    int pct;
    bf_bat_state_t st;
    bool dark;              /* modo em que foi pintado (o Automático troca sem reconstruir) */
} bat_state_t;

static void bat_delete(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *battery_create(lv_obj_t *parent, int w, int h, const lv_font_t *font)
{
    int cap_w = w / 10 < 2 ? 2 : w / 10;
    int border = h >= 24 ? 3 : 2;
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w + cap_w, h);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_obj_create(c);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, w, h);
    lv_obj_set_style_border_width(body, border, 0);
    lv_obj_set_style_radius(body, h / 5, 0);
    lv_obj_set_style_pad_all(body, border + 1, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(body);           /* nível: filho do corpo */
    lv_obj_remove_style_all(fill);
    lv_obj_set_height(fill, LV_PCT(100));
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, h / 8, 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_obj_create(c);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, cap_w, h / 2 < 4 ? 4 : h / 2);
    lv_obj_align(cap, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cap, cap_w / 2, 0);

    lv_obj_t *bolt = lv_label_create(c);
    lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(bolt, font, 0);
    lv_obj_align(bolt, LV_ALIGN_LEFT_MID, (w - lv_font_get_glyph_width(font, 0xF0E7, 0)) / 2, 0);
    lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);

    bat_state_t *bs = lv_malloc(sizeof(*bs));
    if(bs) {
        bs->pct = -2;               /* força a primeira pintura */
        bs->st = BF_BAT_UNKNOWN;
        bs->dark = false;
        lv_obj_set_user_data(c, bs);
        lv_obj_add_event_cb(c, bat_delete, LV_EVENT_DELETE, bs);
    }
    battery_set(c, -1, BF_BAT_UNKNOWN);
    return c;
}

void battery_set(lv_obj_t *c, int pct, bf_bat_state_t st)
{
    if(!c) return;
    const bf_palette_t *p = pal();
    bat_state_t *bs = lv_obj_get_user_data(c);
    bool dark = theme_dark_now();
    if(pct > 100) pct = 100;
    if(pct < -1) pct = -1;
    /* nada mudou: não invalida a tela (a barra superior atualiza a cada segundo) */
    if(bs && bs->pct == pct && bs->st == st && bs->dark == dark)
        return;
    if(bs) { bs->pct = pct; bs->st = st; bs->dark = dark; }

    lv_obj_t *body = lv_obj_get_child(c, CH_BODY);
    lv_obj_t *cap = lv_obj_get_child(c, CH_CAP);
    lv_obj_t *bolt = lv_obj_get_child(c, CH_BOLT);
    lv_obj_t *fill = lv_obj_get_child(body, 0);

    lv_color_t outline = p->text, level = p->text, bolt_c = p->text;
    bool show_bolt = false;
    switch(st) {
    case BF_BAT_CHARGING:
        level = p->warning;
        show_bolt = true;
        break;
    case BF_BAT_FULL:
        outline = level = p->success;
        pct = 100;                      /* o algoritmo terminou a carga */
        show_bolt = true;
        break;
    case BF_BAT_NOT_CHARGING:            /* carregador ligado, carga parada: nível cinza */
        level = p->text_dim;
        show_bolt = true;
        break;
    default:
        if(pct >= 0 && pct <= 15) outline = level = p->danger;
        break;
    }
    if(pct < 0) {
        outline = p->text_dim;
        show_bolt = show_bolt && st != BF_BAT_UNKNOWN;
    }

    lv_obj_set_style_border_color(body, outline, 0);
    lv_obj_set_style_bg_color(cap, outline, 0);
    lv_obj_set_style_bg_color(fill, level, 0);
    /* nível: pelo menos um traço visível acima de 0% */
    lv_obj_update_layout(body);
    int inner = lv_obj_get_content_width(body);
    int fw = pct <= 0 ? 0 : inner * pct / 100;
    if(pct > 0 && fw < 2) fw = 2;
    lv_obj_set_width(fill, fw);
    if(fw) lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);

    /* o raio fica por cima do nível, na cor do texto (claro no modo escuro,
     * escuro no claro): contrasta com o âmbar/verde e com o fundo */
    lv_obj_set_style_text_color(bolt, bolt_c, 0);
    if(show_bolt) lv_obj_remove_flag(bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);
}

const char *battery_state_text(bf_bat_state_t st)
{
    switch(st) {
    case BF_BAT_CHARGING:     return tr(S_CHARGING);
    case BF_BAT_FULL:         return tr(S_BAT_FULL);
    case BF_BAT_NOT_CHARGING: return tr(S_BAT_NOT_CHARGING);
    default:                  return "";
    }
}
