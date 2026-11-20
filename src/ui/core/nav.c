#include "nav.h"
#include "../screens/screens.h"
#include "../widgets/topbar.h"
#include "../widgets/bottombar.h"
#include <string.h>

#define STACK_MAX   16
#define RECENTS_MAX 6

static const bf_screen_def_t *const defs[SCR__COUNT] = {
    [SCR_HOME] = &scr_home_def,           [SCR_SETTINGS] = &scr_settings_def,
    [SCR_SYSINFO] = &scr_sysinfo_def,     [SCR_RUN] = &scr_run_def,
    [SCR_MONITOR] = &scr_monitor_def,     [SCR_CALIBRATE] = &scr_calibrate_def,
    [SCR_BACKLIGHT] = &scr_backlight_def, [SCR_VOLUME] = &scr_volume_def,
    [SCR_LANGUAGE] = &scr_language_def,   [SCR_DATE] = &scr_date_def,
    [SCR_POWER] = &scr_power_def,         [SCR_RESET] = &scr_reset_def,
    [SCR_SSH] = &scr_ssh_def,             [SCR_ADVANCED] = &scr_advanced_def,
    [SCR_FEATURE] = &scr_feature_def,
};

static lv_obj_t *content;
static bf_nav_entry_t stack[STACK_MAX];
static int depth;
static const bf_screen_def_t *shown;
static bf_nav_entry_t recents[RECENTS_MAX];
static int n_recents;
static bool returning;

bool nav_is_return(void) { return returning; }

void nav_init(lv_obj_t *c)
{
    content = c;
    depth = 1;
    stack[0] = (bf_nav_entry_t){ SCR_HOME, 0 };
}

void nav_set_content(lv_obj_t *c) { content = c; shown = NULL; }

const char *nav_title(bf_screen_t scr, int arg)
{
    const bf_screen_def_t *d = defs[scr];
    return d->title_fn ? d->title_fn(arg) : tr(d->title);
}

static void recents_push(bf_nav_entry_t e)
{
    if(e.scr == SCR_HOME || e.scr == SCR_CALIBRATE) return;
    int i;
    for(i = 0; i < n_recents; i++)
        if(recents[i].scr == e.scr && recents[i].arg == e.arg) break;
    if(i == n_recents && n_recents < RECENTS_MAX) n_recents++;
    if(i >= RECENTS_MAX) i = RECENTS_MAX - 1;
    memmove(&recents[1], &recents[0], (size_t)i * sizeof recents[0]);
    recents[0] = e;
}

int nav_recents(bf_nav_entry_t *out, int max)
{
    int n = n_recents < max ? n_recents : max;
    memcpy(out, recents, (size_t)n * sizeof *out);
    return n;
}

bf_nav_entry_t nav_current(void) { return stack[depth - 1]; }

void nav_show_now(void)
{
    if(!content) return;
    if(shown && shown->destroy) shown->destroy();
    shown = NULL;
    lv_obj_clean(content);
    lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
    bf_nav_entry_t e = stack[depth - 1];
    shown = defs[e.scr];
    shown->create(content, e.arg);
    topbar_set_title(e.scr == SCR_HOME ? NULL : nav_title(e.scr, e.arg));
    topbar_update();
    bottombar_set_active(e.scr);
    recents_push(e);
}

void nav_teardown(void)
{
    if(shown && shown->destroy) shown->destroy();
    shown = NULL;
}

static void show_async(void *ud) { (void)ud; nav_show_now(); }

void nav_go(bf_screen_t scr, int arg)
{
    bf_nav_entry_t top = stack[depth - 1];
    if(top.scr == scr && top.arg == arg) return;
    if(scr == SCR_HOME) { nav_home(); return; }
    returning = false;
    /* se a tela já está na pilha, volta até ela (evita pilhas enormes) */
    for(int i = 0; i < depth; i++) {
        if(stack[i].scr == scr && stack[i].arg == arg) {
            depth = i + 1;
            lv_async_call(show_async, NULL);
            return;
        }
    }
    if(depth == STACK_MAX) {
        memmove(&stack[1], &stack[2], (STACK_MAX - 2) * sizeof stack[0]);
        depth--;
    }
    stack[depth++] = (bf_nav_entry_t){ scr, arg };
    lv_async_call(show_async, NULL);
}

void nav_back(void)
{
    if(depth <= 1) return;
    depth--;
    returning = true;
    lv_async_call(show_async, NULL);
}

void nav_home(void)
{
    if(depth == 1 && stack[0].scr == SCR_HOME && shown) return;
    depth = 1;
    returning = false;
    stack[0] = (bf_nav_entry_t){ SCR_HOME, 0 };
    lv_async_call(show_async, NULL);
}
