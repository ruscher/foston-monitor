/* topbar.c — barra superior: logotipo, título da tela, ícones de status, relógio. */
#include "topbar.h"
#include "battery.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../core/settings.h"
#include "../core/i18n.h"
#include "../assets/logo/bf_logo.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static lv_obj_t *bar, *spacer, *title_lbl, *sep_lbl, *clock_lbl, *date_lbl, *sim_badge;
static lv_obj_t *ic_gps, *ic_net, *ic_bt, *ic_usb, *ic_vol, *ic_bat, *bat_pct;

static void set_text_if(lv_obj_t *l, const char *t)
{
    const char *old = lv_label_get_text(l);
    if(!old || strcmp(old, t)) lv_label_set_text(l, t);
}

static lv_obj_t *icon(lv_obj_t *parent, const char *sym)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, BF_FONT_M, 0);
    return l;
}

/* Estado -> cor: ligado = texto, desligado = apagado, desconhecido = quase invisível */
static void icon_state(lv_obj_t *l, bf_tri_t st, lv_color_t on_color)
{
    lv_color_t c = st == BF_YES ? on_color : pal()->text_dim;
    lv_opa_t o = st == BF_YES ? LV_OPA_COVER : (st == BF_NO ? LV_OPA_70 : LV_OPA_30);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_opa(l, o, 0);
}

lv_obj_t *topbar_create(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BF_SCREEN_W, BF_TOPBAR_H);
    lv_obj_set_style_bg_color(bar, p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    /* filete de 2 px na cor de destaque do tema, na base */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, p->accent, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(bar, BF_SP_L, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 0, 0);

    /* símbolo FOSTONOS + nome */
    lv_obj_t *mark = lv_image_create(bar);
    lv_image_set_src(mark, &bf_logo_mark);
    lv_obj_set_style_margin_right(mark, BF_SP_S, 0);
    bf_label(bar, "Big", BF_FONT_M, p->text);
    bf_label(bar, "Foston", BF_FONT_M, p->dark ? lv_color_lighten(p->primary, LV_OPA_30) : p->primary);

    sep_lbl = bf_label(bar, LV_SYMBOL_RIGHT, BF_FONT_S, p->text_dim);
    lv_obj_set_style_margin_hor(sep_lbl, BF_SP_M, 0);
    title_lbl = bf_label(bar, "", BF_FONT_M, p->text);
    bf_label_ellipsis(title_lbl, 1);
    lv_obj_set_flex_grow(title_lbl, 1);   /* ocupa o espaço livre; corta com "…" */

    spacer = bf_plain(bar);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *icons = bf_plain(bar);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icons, 12, 0);

    sim_badge = bf_pill(icons, tr(S_SIM_BADGE), p->warning);
    ic_gps = icon(icons, LV_SYMBOL_GPS);
    ic_net = icon(icons, BF_SYMBOL_NET);
    ic_bt  = icon(icons, LV_SYMBOL_BLUETOOTH);
    ic_usb = icon(icons, LV_SYMBOL_USB);
    ic_vol = icon(icons, LV_SYMBOL_VOLUME_MAX);
    lv_obj_t *batbox = bf_plain(icons);
    lv_obj_set_size(batbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(batbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batbox, 4, 0);
    ic_bat = battery_create(batbox, BF_FONT_M);
    bat_pct = bf_label(batbox, "—", BF_FONT_S, p->text);

    lv_obj_t *clockbox = bf_plain(icons);
    lv_obj_set_size(clockbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clockbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clockbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clockbox, 8, 0);
    lv_obj_set_style_margin_left(clockbox, 4, 0);
    date_lbl = bf_label(clockbox, "", BF_FONT_S, p->text_dim);
    clock_lbl = bf_label(clockbox, "--:--", BF_FONT_M, p->text);

    topbar_set_title(NULL);
    topbar_update();
    return bar;
}

void topbar_set_title(const char *title)
{
    if(!bar) return;
    if(title) {
        set_text_if(title_lbl, title);
        lv_obj_remove_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sep_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(spacer, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sep_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *const wd[3][7] = {
    { "dom", "seg", "ter", "qua", "qui", "sex", "sáb" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "dom", "lun", "mar", "mié", "jue", "vie", "sáb" },
};

void topbar_update(void)
{
    if(!bar) return;
    const bf_palette_t *p = pal();
    const bf_settings_t *cfg = settings();
    char buf[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    bool clock_ok = tmv.tm_year + 1900 >= 2024;   /* sem hora no boot: não mostra 1970 */
    if(!clock_ok) snprintf(buf, sizeof buf, "--:--");
    else if(cfg->clock_24h) snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else snprintf(buf, sizeof buf, "%d:%02d %s", (tmv.tm_hour + 11) % 12 + 1, tmv.tm_min,
                      tmv.tm_hour < 12 ? "AM" : "PM");
    set_text_if(clock_lbl, buf);
    if(cfg->show_date && clock_ok) {
        /* com título de página, só dd/mm (economiza espaço); na tela inicial, com dia da semana */
        bf_lang_t l = i18n_get_lang();
        bool wide = lv_obj_has_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
        int a = l == LANG_EN ? tmv.tm_mon + 1 : tmv.tm_mday, b = l == LANG_EN ? tmv.tm_mday : tmv.tm_mon + 1;
        if(wide) snprintf(buf, sizeof buf, "%s %02d/%02d", wd[l][tmv.tm_wday], a, b);
        else snprintf(buf, sizeof buf, "%02d/%02d", a, b);
        set_text_if(date_lbl, buf);
        lv_obj_remove_flag(date_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(date_lbl, LV_OBJ_FLAG_HIDDEN);

    const bf_status_t *s = status_get();
    if(s->simulated) lv_obj_remove_flag(sim_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sim_badge, LV_OBJ_FLAG_HIDDEN);

    icon_state(ic_gps, s->gps_fix, p->accent);
    icon_state(ic_net, s->network, p->text);
    icon_state(ic_bt, s->bluetooth, p->primary);
    icon_state(ic_usb, s->usb, p->text);

    bf_tri_t vol_st = s->volume_pct < 0 ? BF_UNKNOWN : BF_YES;
    set_text_if(ic_vol, s->muted == BF_YES ? LV_SYMBOL_MUTE :
                (s->volume_pct >= 0 && s->volume_pct < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX));
    icon_state(ic_vol, vol_st, p->text);

    /* Bateria: nunca inventar porcentagem; o raio e o verde ficam no ícone. */
    battery_set(ic_bat, s->battery_pct, s->battery_state);
    if(s->battery_pct >= 0) {
        snprintf(buf, sizeof buf, "%d%%", s->battery_pct);
        set_text_if(bat_pct, buf);
    }
    else set_text_if(bat_pct, "—");
    lv_obj_set_style_text_color(bat_pct, s->battery_pct >= 0 ? p->text : p->text_dim, 0);
}
