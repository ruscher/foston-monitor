/* scr_advanced.c — Avançado: pasta de dados e informações de integração
 * (o tema fica em Ajustes > Aparência). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/status.h"
#include "../widgets/page.h"
#include <stdlib.h>

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    lv_obj_t *pg = page_create(parent);
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_M, 0);
    page_kv(c, tr(S_ADV_DATADIR), data_dir());
    page_kv(c, tr(S_ADV_STATUS), status_provider()->simulated ? tr(S_ADV_SIM) : tr(S_ADV_REAL));
    const char *apps = getenv("BIGFOSTON_APPS");
    lv_obj_t *v = page_kv(c, tr(S_ADV_APPS), apps && *apps ? apps : "$DATA/apps:/usr/share/bigfoston/apps");
    lv_obj_set_style_text_font(v, BF_FONT_S, 0);
}

const bf_screen_def_t scr_advanced_def = { create, NULL, S_SET_ADVANCED, NULL };
