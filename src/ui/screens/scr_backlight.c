/* scr_backlight.c — Luz Posterior: slider real (/sys/class/backlight) ou aviso. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/status.h"
#include "../core/settings.h"
#include "../widgets/page.h"

static void changed(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    status_provider()->backlight_set(v);
    settings()->backlight = v;
    if(lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_status_provider_t *sp = status_provider();
    lv_obj_t *pg = page_create(parent);
    page_header(pg, BF_SYMBOL_SUN, lv_color_hex(0xf5c211), tr(S_SET_BACKLIGHT), tr(S_SUB_BACKLIGHT));
    int cur = sp->backlight_get();
    if(cur < 0) { page_unavailable(pg, tr(S_MISS_BACKLIGHT)); return; }
    page_slider_card(pg, tr(S_BRIGHTNESS), BF_SYMBOL_MOON, BF_SYMBOL_SUN, 5, cur, changed, NULL);
    const char *dev = sp->backlight_device();
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(c, BF_SP_M, 0);
    page_kv(c, tr(S_DEVICE), dev ? dev : "—");
    page_sim_note(pg);
}

const bf_screen_def_t scr_backlight_def = { create, NULL, S_SET_BACKLIGHT, NULL };
