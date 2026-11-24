/* powerplan.c — ver powerplan.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "powerplan.h"
#include "settings.h"
#include "status.h"
#include "perfui.h"

void fx_level_apply(int level)
{
    bf_settings_t *s = settings();
    if(level < 0 || level >= BF_FX__N) return;
    s->fx_level = level;
    s->anim = level == BF_FX_FULL;              /* o papel de parede dos mínimos: perfui (tick) */
    settings_save();
    perfui_settings_changed();
}

void powerplan_apply(int plan)
{
    bf_settings_t *s = settings();
    if(plan < 0 || plan >= BF_PLAN__N) return;
    s->power_plan = plan;
    switch(plan) {
    case BF_PLAN_ECO:
        if(s->backlight > 50) {
            s->backlight = 50;
            if(status_provider()->backlight_set) status_provider()->backlight_set(50);
        }
        s->dim_min = 1;
        s->sounds = false;
        s->fx_level = BF_FX_MIN;
        s->perf_always = true;
        s->map_detail = BF_DETAIL_ECONOMIC;
        break;
    case BF_PLAN_PERF:
        s->dim_min = 0;
        s->sounds = true;
        s->fx_level = BF_FX_MIN;
        s->perf_always = true;
        s->map_detail = BF_DETAIL_ECONOMIC;
        break;
    default:                                         /* equilibrado: o de sempre */
        s->dim_min = 0;
        s->sounds = true;
        s->fx_level = BF_FX_FULL;
        s->perf_always = false;
        s->map_detail = BF_DETAIL_AUTO;
        break;
    }
    s->anim = s->fx_level == BF_FX_FULL;
    s->perf_auto = s->perf_pad = s->perf_tv = true;   /* os motivos automáticos valem em todos */
    settings_save();
    perfui_settings_changed();
}
