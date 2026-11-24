/* perfui.c — ver perfui.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "perfui.h"
#include "perfmode.h"
#include "fx.h"
#include "nav.h"
#include "settings.h"
#include "theme.h"
#include "i18n.h"
#include "hwkbd.h"
#include "../widgets/dialog.h"
#include <stdio.h>
#include <string.h>

#define C_PERF 0xe66100

static bool manual, video, suppressed;   /* suppressed: desligado pelo usuário com o vídeo tocando */
static bool on;                          /* o que foi pedido ao perfmode */
static lv_timer_t *delay_t, *tick_t;
static lv_obj_t *tag, *tag_lbl;
static bool tag_hidden;
static int tag_y;

/* ---------------------------------------------------------------- janela de progresso */
static lv_obj_t *pm_box, *pm_bar, *pm_title, *pm_sub;
static pm_state_t pm_shown = PM_OFF;
static uint32_t pm_since;

/* apagada por fora (app_rebuild_now limpa a camada de cima ao trocar tema ou idioma) */
static void pm_deleted(lv_event_t *e)
{
    if(lv_event_get_target(e) != pm_box) return;
    pm_box = pm_bar = pm_title = pm_sub = NULL;
    pm_shown = PM_OFF;
}

static void pm_close(void)
{
    if(pm_box) lv_obj_delete(pm_box);
    pm_box = pm_bar = pm_title = pm_sub = NULL;
    pm_shown = PM_OFF;
}

static void pm_update(void)
{
    if(!pm_box) return;
    pm_state_t st = perfmode_state();
    int d, n;
    perfmode_progress(&d, &n);
    if(st == PM_ENTERING || st == PM_LEAVING) {
        if(st != pm_shown) {                     /* entrar virou sair (desligado no meio) */
            pm_shown = st;
            lv_label_set_text(pm_title, tr(st == PM_ENTERING ? S_PERF_ENTER : S_PERF_LEAVE));
            lv_label_set_text(pm_sub, tr(st == PM_ENTERING ? S_PERF_ENTER_SUB : S_PERF_LEAVE_SUB));
            pm_since = lv_tick_get();
        }
        lv_bar_set_value(pm_bar, n ? d * 100 / n : 0, LV_ANIM_ON);
        return;
    }
    lv_bar_set_value(pm_bar, 100, LV_ANIM_ON);
    if(lv_tick_elaps(pm_since) > 700) pm_close();   /* dá tempo de ler */
}

/* segura os toques durante a troca; se demorar (um serviço lento), um toque fecha a janela e
 * a troca segue por trás (a etiqueta e Ajustes mostram o estado) */
static uint32_t pm_opened;

static void pm_box_click(lv_event_t *e)
{
    (void)e;
    if(lv_tick_elaps(pm_opened) > 10000) pm_close();
}

static void pm_open(void)
{
    if(pm_box || !settings()->perf_modal) return;
    const bf_palette_t *p = pal();
    pm_box = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(pm_box, pm_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(pm_box);
    lv_obj_set_size(pm_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(pm_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pm_box, LV_OPA_50, 0);
    lv_obj_add_flag(pm_box, LV_OBJ_FLAG_CLICKABLE);       /* segura os toques enquanto troca */
    lv_obj_add_event_cb(pm_box, pm_box_click, LV_EVENT_CLICKED, NULL);
    pm_opened = lv_tick_get();
    lv_obj_t *card = lv_obj_create(pm_box);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, p->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, BF_RADIUS_L, 0);
    lv_obj_set_style_pad_all(card, BF_SP_XL, 0);
    lv_obj_set_style_pad_row(card, BF_SP_M, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    pm_title = bf_label(card, "", BF_FONT_L, p->text);
    pm_sub = bf_label(card, "", BF_FONT_M, p->text_dim);
    lv_obj_set_width(pm_sub, LV_PCT(100));
    lv_label_set_long_mode(pm_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(pm_sub, LV_TEXT_ALIGN_CENTER, 0);
    pm_bar = lv_bar_create(card);
    lv_obj_set_size(pm_bar, LV_PCT(100), 12);
    lv_obj_set_style_bg_color(pm_bar, p->text_dim, 0);   /* trilho visível sobre o cartão */
    lv_obj_set_style_bg_opa(pm_bar, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(pm_bar, lv_color_hex(C_PERF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(pm_bar, 6, 0);
    lv_obj_set_style_radius(pm_bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(pm_bar, 0, 100);
    pm_shown = PM_OFF;
    pm_since = lv_tick_get();
    pm_update();
}

/* ---------------------------------------------------------------- ligar e desligar */

static bool wanted(void) { return manual || (video && !suppressed && settings()->perf_auto); }

static void apply(void)
{
    bool w = wanted();
    if(w == on) return;
    on = w;
    if(w) {
        if(settings()->perf_quiet) fx_quiet(true);
        unsigned svc = settings()->perf_svc;
        if(hwkbd_bt_connected()) svc &= ~PM_SVC_BT;   /* o teclado Bluetooth continua digitando */
        perfmode_enter(svc, settings()->perf_prio);
    }
    else {
        fx_quiet(false);
        perfmode_leave();
    }
    pm_open();
}

static void delay_done(lv_timer_t *t)
{
    (void)t;
    delay_t = NULL;
    apply();
}

/* troca agora ou depois de ms (o vídeo pode voltar/parar no meio: a espera recomeça) */
static void schedule(uint32_t ms)
{
    if(delay_t) { lv_timer_delete(delay_t); delay_t = NULL; }
    if(!ms) { apply(); return; }
    delay_t = lv_timer_create(delay_done, ms, NULL);
    lv_timer_set_repeat_count(delay_t, 1);
}

void perfui_video(bool playing)
{
    if(playing == video) return;
    video = playing;
    fx_keep_awake(playing);                      /* vídeo tocando: a tela não escurece */
    if(!playing) suppressed = false;             /* o próximo vídeo pode ligar de novo */
    if(manual) return;                           /* ligado à mão: o vídeo não muda nada */
    schedule((uint32_t)(playing ? settings()->perf_enter_s : settings()->perf_exit_s) * 1000u);
}

void perfui_video_gone(void)
{
    video = suppressed = false;
    fx_keep_awake(false);
    if(!manual) schedule(0);
}

void perfui_manual(bool m)
{
    manual = m;
    if(!m && video) suppressed = true;           /* desligou à mão: não volta neste vídeo */
    schedule(0);
}

void perfui_disable(void)
{
    manual = false;
    if(video) suppressed = true;
    schedule(0);
}

void perfui_settings_changed(void)
{
    if(on) fx_quiet(settings()->perf_quiet);     /* sons e animações valem na hora */
    /* automático desligado no meio do vídeo: sai já; religado com o vídeo tocando: entra
     * depois da espera de sempre */
    if(manual || wanted() == on) return;
    schedule(on ? 0 : (uint32_t)settings()->perf_enter_s * 1000u);
}

perf_why_t perfui_why(void)
{
    if(perfmode_state() == PM_OFF && !on) return PERF_WHY_NONE;
    return manual ? PERF_WHY_MANUAL : PERF_WHY_VIDEO;
}

bool perfui_video_playing(void) { return video; }

/* ---------------------------------------------------------------- etiqueta flutuante */

static lv_obj_t *dlg_bd;                 /* fundo do diálogo da etiqueta (fecha se o modo desligar) */

static void dlg_gone(lv_event_t *e) { if(lv_event_get_target(e) == dlg_bd) dlg_bd = NULL; }

static void tag_answer(int b, void *ud)
{
    (void)ud;
    if(b == 0) perfui_disable();
    else if(b == 1) nav_go(SCR_PERFMODE, 0);
}

static void tag_click(lv_event_t *e)
{
    (void)e;
    perfui_dialog();
}

void perfui_dialog(void)
{
    if(dlg_bd) return;
    char body[300];
    snprintf(body, sizeof body, "%s\n%s", tr(manual ? S_PERFM_WHY_MANUAL : S_PERFM_WHY_VIDEO), tr(S_PERFM_TAG_BODY));
    static const char *b[3];
    b[0] = tr(S_PERFM_DISABLE);
    b[1] = tr(S_PERFM_CONFIGURE);
    b[2] = tr(S_PERFM_CLOSE);
    lv_obj_t *card = dialog_open(LV_SYMBOL_CHARGE, lv_color_hex(C_PERF), tr(S_SET_PERFMODE), body, b, 3, 1, 0,
                                 tag_answer, NULL);
    dlg_bd = lv_obj_get_parent(card);
    lv_obj_add_event_cb(dlg_bd, dlg_gone, LV_EVENT_DELETE, NULL);
}

static void tag_deleted(lv_event_t *e)
{
    if(lv_event_get_target(e) == tag) tag = tag_lbl = NULL;    /* o tick refaz */
}

static void tag_make(void)
{
    tag = lv_button_create(lv_layer_top());
    lv_obj_add_event_cb(tag, tag_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(tag);
    lv_obj_set_size(tag, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(tag, 12, 0);
    lv_obj_set_style_radius(tag, 15, 0);
    lv_obj_set_style_bg_color(tag, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_40, 0);           /* bem suave: não esconde a página */
    lv_obj_set_style_bg_opa(tag, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tag, 1, 0);
    lv_obj_set_style_border_color(tag, lv_color_hex(C_PERF), 0);
    lv_obj_set_style_border_opa(tag, LV_OPA_60, 0);
    lv_obj_set_ext_click_area(tag, 8);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    char t[64];
    snprintf(t, sizeof t, LV_SYMBOL_CHARGE " %s", tr(S_PERFM_TAG));
    tag_lbl = bf_label(tag, t, BF_FONT_S, lv_color_white());
    lv_obj_set_style_text_opa(tag_lbl, LV_OPA_90, 0);
    lv_obj_center(tag_lbl);
    lv_obj_add_event_cb(tag, tag_click, LV_EVENT_CLICKED, NULL);
}

void perfui_tag_hide(bool hide) { tag_hidden = hide; }
void perfui_tag_offset(int y) { tag_y = y; }

static void tick(lv_timer_t *t)
{
    (void)t;
    pm_update();
    if(!tag) tag_make();
    pm_state_t st = perfmode_state();
    if(dlg_bd && (st == PM_OFF || st == PM_LEAVING)) {   /* o modo desligou: o diálogo não vale mais */
        lv_obj_t *d = dlg_bd;
        dlg_bd = NULL;
        lv_obj_delete_async(d);
    }
    /* nada de etiqueta por cima da janela de progresso ou do diálogo (ficava acesa sobre o
     * fundo escurecido), nem em Ajustes > Modo performance (o cartão de estado já diz) */
    bool show = settings()->perf_tag && !tag_hidden && !pm_box && !dlg_bd && nav_current().scr != SCR_PERFMODE &&
                (st == PM_ON || st == PM_ENTERING);
    if(show) {
        lv_obj_set_pos(tag, 8, BF_TOPBAR_H + 6 + tag_y);
        if(lv_obj_has_flag(tag, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(tag, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(tag);
        }
    }
    else if(!lv_obj_has_flag(tag, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
}

void perfui_init(void)
{
    if(!tick_t) tick_t = lv_timer_create(tick, 200, NULL);
    if(perfmode_recover()) pm_open();            /* sobrou de um shell anterior: religa */
}
