/* scr_appearance.c — Ajustes > Aparência: modo (escuro/claro/automático),
 * tema de cores, papel de parede, animações, sons e escurecer a tela.
 * Modo, cores e papel de parede reconstroem a interface (app_rebuild); a
 * posição da lista é mantida. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/status.h"
#include "../core/app.h"
#include "../core/fx.h"
#include "../widgets/page.h"
#include "../assets/logo/bf_logo.h"
#include <time.h>

static lv_obj_t *pg;
static int last_scroll;
static bool keep_scroll;
static const int dim_choices[] = BF_DIM_CHOICES;
#define N_DIM (int)(sizeof dim_choices / sizeof dim_choices[0])

/* ------------------------------------------------------------ blocos */

static lv_obj_t *section(const char *sym, const char *title)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_M, 0);
    lv_obj_t *h = bf_plain(c);
    lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, BF_SP_S, 0);
    bf_label(h, sym, BF_FONT_M, p->accent);
    bf_label(h, title, BF_FONT_M, p->text);
    return c;
}

static lv_obj_t *seg_row(lv_obj_t *parent)
{
    lv_obj_t *r = bf_plain(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, BF_SP_S, 0);
    return r;
}

static lv_obj_t *seg_button(lv_obj_t *row, const char *sym, const char *txt, bool on,
                            lv_event_cb_t cb, intptr_t v)
{
    lv_obj_t *b = bf_button(row, sym, txt, 0);
    lv_obj_set_flex_grow(b, 1);
    if(on) lv_obj_add_state(b, LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)v);
    return b;
}

static lv_obj_t *hint(lv_obj_t *parent, const char *txt, lv_color_t c)
{
    lv_obj_t *l = bf_label(parent, txt, BF_FONT_S, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* ------------------------------------------------------------ ações */

static void apply_rebuild(void)
{
    settings_save();
    keep_scroll = true;
    app_rebuild();
}

static void mode_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if(settings()->mode == v) return;
    settings()->mode = v;
    apply_rebuild();
}

static void palette_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if(settings()->palette == v) return;
    settings()->palette = v;
    apply_rebuild();
}

static void wall_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if(settings()->wallpaper == v) return;
    settings()->wallpaper = v;
    apply_rebuild();
}

static void home_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if(settings()->home_mode == v) return;
    settings()->home_mode = v;
    apply_rebuild();                 /* a barra inferior muda no quiosque */
}

static void app_toggle(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    if(lv_obj_has_state(b, LV_STATE_CHECKED)) settings()->home_apps |= 1u << i;
    else settings()->home_apps &= ~(1u << i);
    settings_save();
}

static void dim_click(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    settings()->dim_min = v;
    settings_save();
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target(e));
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *b = lv_obj_get_child(row, (int32_t)i);
        if(b == lv_event_get_target(e)) lv_obj_add_state(b, LV_STATE_CHECKED);
        else lv_obj_remove_state(b, LV_STATE_CHECKED);
    }
}

static void sw_anim(lv_event_t *e)
{
    settings()->anim = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

static void sw_sounds(lv_event_t *e)
{
    settings()->sounds = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    fx_sound(FX_TOGGLE);           /* prévia (só toca se acabou de ligar) */
}

static void sw_notify(lv_event_t *e)
{
    settings()->notify = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

static void boot_preview(lv_timer_t *t) { (void)t; fx_sound(FX_STARTUP); }

static void sw_boot(lv_event_t *e)
{
    settings()->boot_sound = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    /* prévia depois do som do próprio interruptor (um som por vez) */
    if(settings()->boot_sound) lv_timer_set_repeat_count(lv_timer_create(boot_preview, 200, NULL), 1);
}

/* ------------------------------------------------------------ tela */

static void build_mode(void)
{
    const bf_palette_t *p = pal();
    const bf_settings_t *s = settings();
    lv_obj_t *c = section(BF_SYMBOL_ADJUST, tr(S_AP_MODE));
    lv_obj_t *r = seg_row(c);
    seg_button(r, BF_SYMBOL_MOON, tr(S_AP_DARK), s->mode == BF_MODE_DARK, mode_click, BF_MODE_DARK);
    seg_button(r, BF_SYMBOL_SUN, tr(S_AP_LIGHT), s->mode == BF_MODE_LIGHT, mode_click, BF_MODE_LIGHT);
    seg_button(r, BF_SYMBOL_CLOCK, tr(S_AP_AUTO), s->mode == BF_MODE_AUTO, mode_click, BF_MODE_AUTO);
    if(s->mode == BF_MODE_AUTO) {
        time_t n = time(NULL);
        struct tm tmv;
        bool clock_ok = localtime_r(&n, &tmv) && tmv.tm_year + 1900 >= 2024;
        hint(c, tr(clock_ok ? S_AP_AUTO_HINT : S_AP_AUTO_NOCLOCK), clock_ok ? p->text_dim : p->warning);
    }
}

static void build_colors(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = section(BF_SYMBOL_PALETTE, tr(S_AP_COLORS));
    lv_obj_t *r = bf_plain(c);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(r, BF_SP_S, 0);
    for(int i = 0; i < theme_preset_count(); i++) {
        const bf_preset_t *pr = theme_preset(i);
        bool on = settings()->palette == i;
        lv_obj_t *b = lv_button_create(r);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 112, 104);
        lv_obj_set_style_radius(b, BF_RADIUS_M, 0);
        lv_obj_set_style_bg_color(b, on ? p->surface2 : p->surface, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, on ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, on ? p->accent : p->border, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 6, 0);
        lv_obj_add_event_cb(b, palette_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* amostra: círculo com a cor principal e a de destaque */
        lv_obj_t *sw = lv_obj_create(b);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 48, 48);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(pr->primary), 0);
        lv_obj_set_style_bg_grad_color(sw, lv_color_hex(pr->accent), 0);
        lv_obj_set_style_bg_grad_dir(sw, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_main_stop(sw, 110, 0);
        lv_obj_set_style_bg_grad_stop(sw, 146, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sw, 3, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(pr->tint), 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        if(on) {
            lv_obj_t *ok = bf_label(sw, LV_SYMBOL_OK, BF_FONT_M, lv_color_white());
            lv_obj_center(ok);
        }
        lv_obj_t *l = bf_label(b, tr(pr->name), BF_FONT_S, on ? p->text : p->text_dim);
        bf_label_ellipsis(l, 104);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void build_wallpaper(void)
{
    const bf_settings_t *s = settings();
    lv_obj_t *c = section(BF_SYMBOL_IMAGE, tr(S_AP_WALL));
    lv_obj_t *r = seg_row(c);
    seg_button(r, NULL, tr(S_AP_WALL_PLAIN), s->wallpaper == BF_WALL_PLAIN, wall_click, BF_WALL_PLAIN);
    seg_button(r, NULL, tr(S_AP_WALL_GRAD), s->wallpaper == BF_WALL_GRADIENT, wall_click, BF_WALL_GRADIENT);
    lv_obj_t *b = seg_button(r, NULL, NULL, s->wallpaper == BF_WALL_LOGO, wall_click, BF_WALL_LOGO);
    lv_obj_t *img = lv_image_create(b);           /* o símbolo no próprio botão */
    lv_image_set_src(img, &bf_logo_mark);
    bf_label(b, tr(S_AP_WALL_LOGO), NULL, pal()->text);
}

static void build_effects(void)
{
    const bf_palette_t *p = pal();
    const bf_settings_t *s = settings();
    lv_obj_t *c = section(BF_SYMBOL_MAGIC, tr(S_AP_EFFECTS));
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    struct { bf_str_t t; bool on; lv_event_cb_t cb; } rows[] = {
        { S_AP_ANIM, s->anim, sw_anim },
        { S_AP_SOUNDS, s->sounds, sw_sounds },
        { S_AP_BOOT_SOUND, s->boot_sound, sw_boot },
        { S_AP_NOTIFY, s->notify, sw_notify },
    };
    for(unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        lv_obj_t *sw = page_switch_row(c, tr(rows[i].t), rows[i].on, rows[i].cb, NULL);
        lv_obj_t *row = lv_obj_get_parent(sw);    /* cartão dentro de cartão: sem moldura */
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(row, 0, 0);
        lv_obj_set_height(row, 64);
    }
    if(!fx_audio_present()) hint(c, tr(S_AP_NO_AUDIO), p->warning);
}

static void build_home(void)
{
    const bf_palette_t *p = pal();
    const bf_settings_t *s = settings();
    lv_obj_t *c = section(LV_SYMBOL_HOME, tr(S_AP_HOME));
    lv_obj_t *r = seg_row(c);
    seg_button(r, LV_SYMBOL_LIST, tr(S_AP_HOME_GRID), s->home_mode == BF_HOME_GRID, home_click, BF_HOME_GRID);
    seg_button(r, BF_SYMBOL_CLOCK, tr(S_AP_HOME_PANEL), s->home_mode == BF_HOME_PANEL, home_click, BF_HOME_PANEL);
    seg_button(r, BF_SYMBOL_KEY, tr(S_AP_HOME_KIOSK), s->home_mode == BF_HOME_KIOSK, home_click, BF_HOME_KIOSK);
    hint(c, tr(s->home_mode == BF_HOME_PANEL ? S_AP_HOME_PANEL_HINT :
               s->home_mode == BF_HOME_KIOSK ? S_AP_HOME_KIOSK_HINT : S_AP_HOME_GRID_HINT), p->text_dim);
    bf_label(c, tr(S_AP_HOME_APPS), BF_FONT_S, p->text);
    lv_obj_t *chips = bf_plain(c);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(chips, BF_SP_S, 0);
    lv_obj_set_style_pad_column(chips, BF_SP_S, 0);
    for(int i = 0; i < home_app_count(); i++) {
        /* no quiosque Ajustes nunca aparece na grade (abre segurando o botão) */
        if(s->home_mode == BF_HOME_KIOSK && home_app_is_settings(i)) continue;
        bool on = s->home_apps >> i & 1u;
        lv_obj_t *b = bf_button(chips, NULL, home_app_name(i), 0);   /* marcado = cor principal */
        lv_obj_set_height(b, 56);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
        if(on) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, app_toggle, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
}

static void build_dim(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *c = section(BF_SYMBOL_SUN, tr(S_AP_DIM));
    if(status_provider()->backlight_get() < 0) {
        hint(c, tr(S_UNAVAILABLE_SHORT), p->warning);
        return;
    }
    lv_obj_t *r = seg_row(c);
    for(int i = 0; i < N_DIM; i++) {
        char t[16];
        if(dim_choices[i]) lv_snprintf(t, sizeof t, "%d min", dim_choices[i]);
        else lv_snprintf(t, sizeof t, "%s", tr(S_AP_NEVER));
        seg_button(r, NULL, t, settings()->dim_min == dim_choices[i], dim_click, dim_choices[i]);
    }
    hint(c, tr(S_AP_DIM_HINT), p->text_dim);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    pg = page_create(parent);
    if(data_dir_volatile()) {
        lv_obj_t *n = bf_pill(pg, tr(S_AP_VOLATILE), pal()->warning);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_radius(n, BF_RADIUS_S, 0);
        lv_obj_set_width(n, LV_PCT(100));
        lv_obj_set_style_pad_ver(n, 6, 0);
    }
    build_mode();
    build_home();
    build_colors();
    build_wallpaper();
    build_effects();
    build_dim();
    if(keep_scroll || nav_is_return()) {
        lv_obj_update_layout(pg);
        lv_obj_scroll_to_y(pg, last_scroll, LV_ANIM_OFF);
    }
    keep_scroll = false;
}

static void destroy(void)
{
    if(pg) last_scroll = lv_obj_get_scroll_y(pg);
    pg = NULL;
}

const bf_screen_def_t scr_appearance_def = { create, destroy, S_SET_APPEARANCE, NULL };
