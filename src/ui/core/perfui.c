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
#include "gamepad.h"
#include "btstat.h"
#include "app.h"
#include <sys/resource.h>
#include "../widgets/dialog.h"
#include <stdio.h>
#include <string.h>

#define C_PERF 0xe66100

static bool manual, video, suppressed;   /* suppressed: desligado pelo usuário com o vídeo tocando */
static bool pad, pad_suppressed;         /* controle de jogo conectado; desligado à mão com ele */
static bool tv, tv_suppressed;           /* app TV aberto; desligado à mão com ela */
static bool stream, stream_suppressed;   /* sessão com imagem do Big Remote Play */
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

/* um motivo "forte": vídeo, TV, controle ou o botão (pausa tudo o que está marcado) */
static bool strong(void)
{
    return manual || (video && !suppressed && settings()->perf_auto) || (pad && !pad_suppressed && settings()->perf_pad) ||
           (tv && !tv_suppressed && settings()->perf_tv) || (stream && !stream_suppressed && settings()->perf_stream);
}

static bool wanted(void) { return strong() || settings()->perf_always; }

/* o que pausar: só pelo plano de energia, GPS e tarefas (Bluetooth e telefone continuam: o
 * celular precisa conectar e as chamadas, chegar) */
static unsigned svc_now(void)
{
    unsigned s = settings()->perf_svc;
    /* Big Remote Play: o controle Bluetooth precisa conectar e ficar (pode chegar depois de a
     * sessão começar) e as chamadas, chegar: nunca pausa Bluetooth nem telefone */
    if(stream && !stream_suppressed && settings()->perf_stream) s &= ~(PM_SVC_BT | PM_SVC_PHONE);
    return strong() ? s : s & (PM_SVC_GPS | PM_SVC_CRON);
}

static void apply(void)
{
    bool w = wanted();
    if(w == on) return;
    on = w;
    if(w) {
        if(settings()->perf_quiet) fx_quiet(true);
        /* o Bluetooth de teclado/controle e o app aberto: perfmode_hold (tick) */
        perfmode_enter(svc_now(), false);
    }
    else {
        fx_quiet(false);
        perfmode_leave();
    }
    /* a janela que segura os toques só para o vídeo e o botão: com o controle a pessoa vai
     * jogar agora, e a TV já mostra "sintonizando"; a etiqueta avisa */
    if(manual || (video && !suppressed && settings()->perf_auto)) pm_open();
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
    if(!m && pad) pad_suppressed = true;         /* nem com este controle */
    if(!m && tv) tv_suppressed = true;           /* nem com a TV aberta agora */
    if(!m && stream) stream_suppressed = true;
    schedule(0);
}

void perfui_disable(void)
{
    manual = false;
    if(video) suppressed = true;
    if(pad) pad_suppressed = true;
    if(tv) tv_suppressed = true;
    if(stream) stream_suppressed = true;
    schedule(0);
}

void perfui_tv(bool open)
{
    if(open == tv) return;
    tv = open;
    if(!open) tv_suppressed = false;
    if(manual) return;
    schedule(0);                                 /* abre: já; fecha: sai já (o app saiu) */
}

void perfui_stream(bool o)
{
    if(o == stream) return;
    stream = o;
    if(!o) stream_suppressed = false;
    if(manual) return;
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
    if(manual) return PERF_WHY_MANUAL;
    if(video && !suppressed && settings()->perf_auto) return PERF_WHY_VIDEO;
    if(tv && !tv_suppressed && settings()->perf_tv) return PERF_WHY_TV;
    if(stream && !stream_suppressed && settings()->perf_stream) return PERF_WHY_STREAM;
    if(pad && !pad_suppressed && settings()->perf_pad) return PERF_WHY_PAD;
    return settings()->perf_always ? PERF_WHY_PLAN : PERF_WHY_VIDEO;
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
    perf_why_t w = perfui_why();
    snprintf(body, sizeof body, "%s\n%s", tr(w == PERF_WHY_MANUAL ? S_PERFM_WHY_MANUAL : w == PERF_WHY_PAD ? S_PERFM_WHY_PAD
                                              : w == PERF_WHY_TV ? S_PERFM_WHY_TV : w == PERF_WHY_STREAM ? S_PERFM_WHY_STREAM : w == PERF_WHY_PLAN ? S_PERFM_WHY_PLAN
                                              : S_PERFM_WHY_VIDEO), tr(S_PERFM_TAG_BODY));
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

/* ---------------------------------------------------------------- apps e controle */

#define HOLD_RELEASE_MS 3000             /* saiu do app: pausa de novo depois disso */
#define PAD_ENTER_MS    1000             /* controle conectou: liga depois disso */

/* o que o app aberto precisa que esteja rodando */
static unsigned app_needs(bf_screen_t s)
{
    switch(s) {
    /* Big GPS: navegando, nada essencial pausa — GPS, o som e a música do celular (Bluetooth)
     * e as chamadas (docs/big-gps-2/25-modo-performance.md) */
    case SCR_MAP: return PM_SVC_GPS | PM_SVC_BT | PM_SVC_PHONE;
    case SCR_GPS: return PM_SVC_GPS;
    case SCR_PHONE: return PM_SVC_PHONE | PM_SVC_BT;
    case SCR_BLUETOOTH: return PM_SVC_BT;
    case SCR_BRP: return PM_SVC_BT;                  /* o controle Bluetooth do jogo */
    default: return 0;
    }
}

static unsigned hold_now;
static bool hold_dropping;               /* o app largou algo: pausa de novo em hold_drop_at */
static uint32_t hold_drop_at;
static bool prio_now, bt_boosted;
static uint32_t boost_at;

/* algum aparelho Bluetooth conectado (celular, fone, teclado, controle)? Lido a cada 1 s */
static bool bt_in_use(void)
{
    static uint32_t at;
    static bool used;
    if(lv_tick_elaps(at) < 1000) return used;
    at = lv_tick_get();
    static bt_info_t b;
    bt_read(&b);
    used = false;
    for(int i = 0; i < b.ndev; i++) if(b.dev[i].connected) used = true;
    return used;
}

static void holds_update(void)
{
    unsigned need = app_needs(nav_current().scr);
    /* com um aparelho Bluetooth conectado o Bluetooth nunca pausa: pausar derrubava o celular
     * (e a internet e o som dele) a cada vídeo, e o teclado/controle */
    if(hwkbd_bt_connected() || bt_in_use()) need |= PM_SVC_BT;
    unsigned add = need & ~hold_now, drop = hold_now & ~need;
    if(add) {                                    /* o app está esperando: já */
        hold_now |= add;
        perfmode_hold(hold_now);
    }
    if(!drop) { hold_dropping = false; return; }
    if(!hold_dropping) {                         /* pode voltar ao app logo: espera um pouco */
        hold_dropping = true;
        hold_drop_at = lv_tick_get() + HOLD_RELEASE_MS;
    } else if((int32_t)(lv_tick_get() - hold_drop_at) >= 0) {
        hold_dropping = false;
        hold_now = need;
        perfmode_hold(hold_now);
    }
}

/* prioridade: a interface com o modo ligado (perf_prio) ou com um controle; o bluetoothd com
 * um controle (ele lê o canal HID do Bluetooth e entrega ao kernel pelo uhid) */
static void prio_update(void)
{
    bool want = (on && settings()->perf_prio) || pad;
    if(want != prio_now && setpriority(PRIO_PROCESS, 0, want ? -5 : 0) == 0) {
        prio_now = want;
        FILE *f = fopen("/proc/self/oom_score_adj", "w");   /* a interface nunca é morta por falta de memória */
        if(f) { fprintf(f, "%d\n", want ? -1000 : 0); fclose(f); }
    }
    /* de novo a cada 5 s: o bluetoothd pode ter reiniciado */
    if(pad && (!bt_boosted || (int32_t)(lv_tick_get() - boost_at) >= 5000)) {
        bt_boosted = perfmode_renice("bluetoothd", -10);
        boost_at = lv_tick_get();
    } else if(!pad && bt_boosted) {
        perfmode_renice("bluetoothd", 0);
        bt_boosted = false;
    }
}

static void pad_update(void)
{
    bool now = gamepad_count() > 0;
    if(now == pad) return;
    pad = now;
    if(!pad) pad_suppressed = false;             /* o próximo controle pode ligar de novo */
    if(manual || (video && !suppressed && settings()->perf_auto)) return;   /* outro motivo segura */
    /* o controle saiu: desliga já (esperar perf_exit_s só pausava e religava serviços à toa) */
    schedule(pad ? PAD_ENTER_MS : 0);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    pad_update();
    holds_update();
    prio_update();
    /* fundo liso enquanto ligado: o papel de parede sai do que é redesenhado */
    static bool plain;
    pm_state_t ps = perfmode_state();
    bool want_plain = settings()->fx_level == BF_FX_MIN ||
                      (settings()->perf_wall && strong() && (ps == PM_ON || ps == PM_ENTERING));
    if(on) perfmode_enter(svc_now(), false);       /* o motivo mudou (o vídeo começou com o plano) */
    if(want_plain != plain) { plain = want_plain; app_wallpaper_plain(plain); }
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
    /* ligado pelo controle (jogando, por muito tempo): a etiqueta avisa por 6 s e some, para
     * não ficar por cima das telas; Ajustes > Modo performance continua mostrando */
    static pm_state_t last_st = PM_OFF;
    static uint32_t on_since;
    if(st == PM_ON && last_st != PM_ON) on_since = lv_tick_get();
    last_st = st;
    bool brief = (perfui_why() == PERF_WHY_PAD || perfui_why() == PERF_WHY_TV || perfui_why() == PERF_WHY_STREAM) && st == PM_ON &&
                 lv_tick_elaps(on_since) > 6000;
    bool show = settings()->perf_tag && !tag_hidden && !pm_box && !dlg_bd && nav_current().scr != SCR_PERFMODE &&
                perfui_why() != PERF_WHY_PLAN &&
                nav_current().scr != SCR_SCC && nav_current().scr != SCR_BRP && nav_current().scr != SCR_TV &&
                (st == PM_ON || st == PM_ENTERING) && !brief;
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
