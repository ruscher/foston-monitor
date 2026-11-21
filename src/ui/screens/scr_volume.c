/* scr_volume.c — Volume: slider real (ALSA/amixer) ou aviso honesto. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../core/settings.h"
#include "../widgets/page.h"

static void changed(lv_event_t *e)
{
    /* só aplica ao soltar: amixer é um processo, não chamar a cada pixel */
    if(lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    status_provider()->volume_set(v);
    settings()->volume = v;
    settings_save();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    lv_obj_t *pg = page_create(parent);
    page_header(pg, LV_SYMBOL_VOLUME_MAX, pal()->primary, tr(S_SET_VOLUME), tr(S_SUB_VOLUME));
    int cur = status_provider()->volume_get();
    if(cur < 0) { page_unavailable(pg, tr(S_MISS_VOLUME)); return; }
    page_slider_card(pg, tr(S_VOLUME_LEVEL), LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MAX, 0, cur, changed, NULL);
    page_sim_note(pg);
}

const bf_screen_def_t scr_volume_def = { create, NULL, S_SET_VOLUME, NULL };
