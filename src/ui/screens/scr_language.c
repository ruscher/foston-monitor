/* scr_language.c — Língua: pt-BR (padrão), English, Español. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/app.h"
#include "../widgets/page.h"

static const char *const flags[LANG_COUNT] = { "PT", "EN", "ES" };

static void pick(lv_event_t *e)
{
    bf_lang_t l = (bf_lang_t)(intptr_t)lv_event_get_user_data(e);
    if(l == settings()->lang) return;
    settings()->lang = l;
    settings_save();
    app_rebuild();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(parent);
    page_header(pg, BF_SYMBOL_LANGUAGE, p->accent, tr(S_SET_LANGUAGE), tr(S_LANG_HINT));
    for(int i = 0; i < LANG_COUNT; i++) {
        bool cur = (bf_lang_t)i == settings()->lang;
        lv_obj_t *b = lv_button_create(pg);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, &bf_st_card, 0);
        lv_obj_set_size(b, LV_PCT(100), 72);
        lv_obj_set_style_pad_ver(b, 0, 0);
        lv_obj_set_style_bg_color(b, p->surface2, LV_STATE_PRESSED);
        if(cur) {
            lv_obj_set_style_border_color(b, p->primary, 0);
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        }
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, BF_SP_L, 0);
        lv_obj_t *code = bf_pill(b, flags[i], cur ? p->primary : p->text_dim);
        lv_obj_set_style_text_font(code, BF_FONT_M, 0);
        lv_obj_t *n = bf_label(b, i18n_lang_name((bf_lang_t)i), BF_FONT_M, p->text);
        lv_obj_set_flex_grow(n, 1);
        if(cur) bf_label(b, LV_SYMBOL_OK, BF_FONT_M, p->primary);
        lv_obj_add_event_cb(b, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

const bf_screen_def_t scr_language_def = { create, NULL, S_SET_LANGUAGE, NULL };
