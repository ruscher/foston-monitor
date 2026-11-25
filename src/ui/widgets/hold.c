/* hold.c — ver hold.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hold.h"
#include "../core/theme.h"
#include "../core/fx.h"
#include <stdlib.h>

typedef struct { hold_cb_t tap, hold; void *ud; } hold_t;

/* um só ponto de toque: um estado global */
static lv_obj_t *cur, *ring, *selected;
static uint32_t t0;
static lv_point_t p0;
static bool fired, cancelled;

static void ring_drop(void)
{
    if(ring) { lv_obj_delete(ring); ring = NULL; }
}

static void reset(void)
{
    ring_drop();
    cur = NULL;
    cancelled = false;
}

static void ring_make(lv_obj_t *obj)
{
    const bf_palette_t *p = pal();
    lv_obj_t *sq = lv_obj_get_child(obj, 0);               /* o quadrado do ícone */
    ring = lv_arc_create(obj);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 98, 98);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_range(ring, 0, 100);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, p->accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
    if(sq) lv_obj_align_to(ring, sq, LV_ALIGN_CENTER, 0, 0);
    else lv_obj_center(ring);
}

static void on_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    hold_t *h = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();

    switch(c) {
    case LV_EVENT_PRESSED:
        reset();
        cur = obj;
        fired = false;
        t0 = lv_tick_get();
        if(in) lv_indev_get_point(in, &p0);
        break;
    case LV_EVENT_PRESSING: {
        if(obj != cur || fired || cancelled) break;
        lv_point_t pt = p0;
        if(in) lv_indev_get_point(in, &pt);
        /* rolagem (página ao lado) ou dedo andando: não é toque longo */
        if((in && lv_indev_get_scroll_obj(in)) || LV_ABS(pt.x - p0.x) > HOLD_SLOP || LV_ABS(pt.y - p0.y) > HOLD_SLOP) {
            cancelled = true;
            ring_drop();
            break;
        }
        uint32_t el = lv_tick_elaps(t0);
        if(el >= HOLD_MS) {
            ring_drop();
            fired = true;
            if(in) lv_indev_wait_release(in);            /* soltar não vira toque */
            fx_sound(FX_TOGGLE);
            hold_select(obj, true);
            if(h->hold) h->hold(obj, h->ud);
            break;
        }
        if(el >= HOLD_RING_MS) {
            if(!ring) ring_make(obj);
            int32_t v = (int32_t)((el - HOLD_RING_MS) * 100 / (HOLD_MS - HOLD_RING_MS));
            if(lv_arc_get_value(ring) != v) lv_arc_set_value(ring, v);
        }
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if(obj == cur) { ring_drop(); cur = NULL; }
        break;
    case LV_EVENT_CLICKED:
        if(fired) { fired = false; break; }             /* (wait_release já evita; garantia) */
        if(cancelled || lv_tick_elaps(t0) > HOLD_TAP_MAX) break;   /* segurou e desistiu */
        if(h->tap) h->tap(obj, h->ud);
        break;
    case LV_EVENT_DELETE:
        if(obj == cur) { ring = NULL; cur = NULL; }       /* o anel é filho: já foi junto */
        if(obj == selected) selected = NULL;
        free(h);
        break;
    default:
        break;
    }
}

void hold_attach(lv_obj_t *obj, hold_cb_t tap, hold_cb_t hold, void *ud)
{
    hold_t *h = malloc(sizeof *h);
    if(!h) return;
    *h = (hold_t){ tap, hold, ud };
    lv_obj_add_event_cb(obj, on_event, LV_EVENT_ALL, h);
    /* selecionado: borda grossa na cor de destaque */
    lv_obj_set_style_border_color(obj, pal()->accent, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_CHECKED);
}

void hold_select(lv_obj_t *obj, bool on)
{
    if(!obj) return;
    if(on) {
        if(selected && selected != obj) lv_obj_remove_state(selected, LV_STATE_CHECKED);
        lv_obj_add_state(obj, LV_STATE_CHECKED);
        selected = obj;
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
        if(selected == obj) selected = NULL;
    }
}

void hold_unselect_last(void)
{
    if(selected) lv_obj_remove_state(selected, LV_STATE_CHECKED);
    selected = NULL;
}
