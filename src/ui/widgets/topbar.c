/* topbar.c — barra superior: logotipo, título da tela, ícones de status, relógio. */
#include "topbar.h"
#include "battery.h"
#include "volpop.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../core/settings.h"
#include "../core/i18n.h"
#include "../assets/logo/bf_logo.h"
#include "../core/nav.h"
#include "bottombar.h"
#include "glass.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static lv_obj_t *bar, *spacer, *title_lbl, *sep_lbl, *clock_lbl, *date_lbl, *sim_badge;
static lv_obj_t *ic_gps, *ic_net, *ic_bt, *ic_usb, *ic_vol, *ic_bat, *bat_pct;
static bool glass;                       /* Foston Glass: navegação aqui, vidro azul */

/* Glass: Voltar, Início, Recentes, Ajustes (toque de 44 px, desenho delicado) */
enum { N_BACK, N_HOME, N_RECENTS, N_SETTINGS };
static void nav_click(lv_event_t *e)
{
    switch((int)(intptr_t)lv_event_get_user_data(e)) {
    case N_BACK: nav_back(); break;
    case N_HOME: nav_home(); break;
    case N_RECENTS: bottombar_open_recents(); break;
    case N_SETTINGS: nav_go(SCR_SETTINGS, 0); break;
    }
}

static void glass_nav(lv_obj_t *bar_)
{
    static const char *const sym[] = { LV_SYMBOL_LEFT, LV_SYMBOL_HOME, LV_SYMBOL_COPY, LV_SYMBOL_SETTINGS };
    for(int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(bar_);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 50, 44);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_t *l = glass_label(b, sym[i], BF_FONT_L, GL_WHITE);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, nav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void vol_click(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    volpop_open((a.x1 + a.x2) / 2, a.y2 + 6);
}

/* atalhos dos ícones de status: GPS -> GpsInfo, rede -> SSH, Bluetooth, bateria -> Potência */
static void jump_click(lv_event_t *e)
{
    volpop_close();
    nav_go((bf_screen_t)(intptr_t)lv_event_get_user_data(e), 0);
}

/* caixa de toque de 44 px de altura em volta de um ícone (o desenho continua pequeno) */
static lv_obj_t *hit_box(lv_obj_t *parent, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, BF_TOPBAR_H - 4);
    lv_obj_set_style_min_width(b, 40, 0);
    lv_obj_set_style_pad_hor(b, 5, 0);
    lv_obj_set_style_radius(b, BF_RADIUS_S, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 4, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

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
    if(glass) on_color = lv_color_hex(GL_WHITE);          /* vidro: tudo branco, o estado na opacidade */
    lv_color_t c = st == BF_YES ? on_color : (glass ? lv_color_hex(GL_TEXT2) : pal()->text_dim);
    lv_opa_t o = st == BF_YES ? LV_OPA_COVER : (st == BF_NO ? LV_OPA_70 : LV_OPA_30);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_opa(l, o, 0);
}

lv_obj_t *topbar_create(lv_obj_t *parent)
{
    const bf_palette_t *p = pal();
    glass = theme_glass();
    bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BF_SCREEN_W, BF_TOPBAR_H);
    if(glass) {                          /* vidro azul-profundo sobre o papel de parede */
        lv_obj_set_style_bg_color(bar, lv_color_hex(GL_DEEP), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_color(bar, lv_color_white(), 0);
        lv_obj_set_style_border_opa(bar, LV_OPA_20, 0);
        lv_obj_set_style_pad_left(bar, 6, 0);
        lv_obj_set_style_pad_right(bar, BF_SP_L, 0);
    } else {
        lv_obj_set_style_bg_color(bar, p->surface, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        /* filete de 2 px na cor de destaque do tema, na base */
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(bar, 2, 0);
        lv_obj_set_style_border_color(bar, p->accent, 0);
        lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(bar, BF_SP_L, 0);
    }
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 0, 0);

    if(glass) {
        glass_nav(bar);
        /* filete vertical entre a navegação e o título */
        sep_lbl = lv_obj_create(bar);
        lv_obj_remove_style_all(sep_lbl);
        lv_obj_set_size(sep_lbl, 1, 24);
        lv_obj_set_style_bg_color(sep_lbl, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(sep_lbl, LV_OPA_40, 0);
        lv_obj_set_style_margin_hor(sep_lbl, BF_SP_M, 0);
        title_lbl = glass_label(bar, "", BF_FONT_M, GL_WHITE);
    } else {
        /* símbolo FOSTONOS + nome */
        lv_obj_t *mark = lv_image_create(bar);
        lv_image_set_src(mark, &bf_logo_mark);
        lv_obj_set_style_margin_right(mark, BF_SP_S, 0);
        bf_label(bar, "Big", BF_FONT_M, p->text);
        bf_label(bar, "Foston", BF_FONT_M, p->dark ? lv_color_lighten(p->primary, LV_OPA_30) : p->primary);

        sep_lbl = bf_label(bar, LV_SYMBOL_RIGHT, BF_FONT_S, p->text_dim);
        lv_obj_set_style_margin_hor(sep_lbl, BF_SP_M, 0);
        title_lbl = bf_label(bar, "", BF_FONT_M, p->text);
    }
    bf_label_ellipsis(title_lbl, 1);
    lv_obj_set_flex_grow(title_lbl, 1);   /* ocupa o espaço livre; corta com "…" */

    spacer = bf_plain(bar);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *icons = bf_plain(bar);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icons, glass ? 2 : 0, 0);

    sim_badge = bf_pill(icons, tr(S_SIM_BADGE), p->warning);
    /* cada ícone numa caixa de toque grande; um toque leva à tela dele */
    ic_gps = icon(hit_box(icons, jump_click, (void *)(intptr_t)SCR_GPS), LV_SYMBOL_GPS);
    ic_net = icon(hit_box(icons, jump_click, (void *)(intptr_t)SCR_SSH), BF_SYMBOL_NET);
    ic_bt  = icon(hit_box(icons, jump_click, (void *)(intptr_t)SCR_BLUETOOTH), LV_SYMBOL_BLUETOOTH);
    ic_usb = icon(icons, LV_SYMBOL_USB);
    lv_obj_set_style_margin_hor(ic_usb, 5, 0);
    /* som: um toque abre o controle de volume, em qualquer tela */
    ic_vol = icon(hit_box(icons, vol_click, NULL), LV_SYMBOL_VOLUME_MAX);
    lv_obj_t *batbox = hit_box(icons, jump_click, (void *)(intptr_t)SCR_POWER);
    ic_bat = battery_create(batbox, BF_FONT_M);
    bat_pct = bf_label(batbox, "—", BF_FONT_S, p->text);

    lv_obj_t *clockbox = bf_plain(icons);
    lv_obj_set_size(clockbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clockbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clockbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clockbox, 8, 0);
    lv_obj_set_style_margin_left(clockbox, 4, 0);
    if(glass) {                          /* | dom 22/11 10:39 */
        lv_obj_t *vs = lv_obj_create(clockbox);
        lv_obj_remove_style_all(vs);
        lv_obj_set_size(vs, 1, 24);
        lv_obj_set_style_bg_color(vs, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(vs, LV_OPA_40, 0);
        lv_obj_set_style_margin_right(vs, 6, 0);
        date_lbl = glass_label(clockbox, "", BF_FONT_M, GL_WHITE);
        clock_lbl = glass_label(clockbox, "--:--", BF_FONT_M, GL_WHITE);
    } else {
        date_lbl = bf_label(clockbox, "", BF_FONT_S, p->text_dim);
        clock_lbl = bf_label(clockbox, "--:--", BF_FONT_M, p->text);
    }

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
    set_text_if(ic_vol, s->volume_pct >= 0 ? volpop_symbol(s->volume_pct) : LV_SYMBOL_VOLUME_MAX);
    icon_state(ic_vol, vol_st, p->text);

    /* Bateria: nunca inventar porcentagem; o raio e o verde ficam no ícone. */
    battery_set(ic_bat, s->battery_pct, s->battery_state);
    if(s->battery_pct >= 0) {
        snprintf(buf, sizeof buf, "%d%%", s->battery_pct);
        set_text_if(bat_pct, buf);
    }
    else set_text_if(bat_pct, "—");
    lv_obj_set_style_text_color(bat_pct, glass ? lv_color_hex(GL_WHITE) : s->battery_pct >= 0 ? p->text : p->text_dim, 0);
}
