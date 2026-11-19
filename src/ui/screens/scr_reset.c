/* scr_reset.c — Reset Settings: volta os ajustes ao padrão (com confirmação). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/app.h"
#include "../widgets/page.h"
#include "../widgets/dialog.h"
#include <stdio.h>

static bool just_reset;

static void confirmed(int b, void *ud)
{
    (void)ud;
    if(b != 1) return;
    settings_defaults(settings());
    settings_save();
    app_apply_hw_settings();
    just_reset = true;
    app_rebuild();
}

static void ask(lv_event_t *e)
{
    (void)e;
    static const char *btns[2];
    btns[0] = tr(S_CANCEL);
    btns[1] = tr(S_RESET_BTN);
    dialog_open(BF_SYMBOL_UNDO, pal()->danger, tr(S_RESET_Q), NULL, btns, 2, -1, 1, confirmed, NULL);
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(parent);
    page_header(pg, BF_SYMBOL_UNDO, p->danger, tr(S_SET_RESET), tr(S_SUB_RESET));
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *t = bf_label(c, tr(S_RESET_BODY), BF_FONT_M, p->text);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    if(just_reset) {
        char msg[96];
        snprintf(msg, sizeof msg, LV_SYMBOL_OK "  %s", tr(S_RESET_DONE));
        bf_pill(pg, msg, p->success);
        just_reset = false;
    }
    lv_obj_t *b = bf_button(pg, BF_SYMBOL_UNDO, tr(S_RESET_BTN), 2);
    lv_obj_set_width(b, 320);
    lv_obj_add_event_cb(b, ask, LV_EVENT_CLICKED, NULL);
}

const bf_screen_def_t scr_reset_def = { create, NULL, S_SET_RESET, NULL };
