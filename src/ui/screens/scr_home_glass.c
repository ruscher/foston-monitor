/* scr_home_glass.c — tela inicial Foston Glass (Ajustes > Aparência > Tela inicial).
 *
 * Cartões de vidro sobre o papel de parede (configuração global): relógio e data, Conexão
 * (Bluetooth e GPS reais), player da fonte ativa da Multimídia (cartão ou celular) e o dock com
 * GPS, Rádio, Apps, Bluetooth e Telefone (a TV fica na grade Apps). A navegação (Voltar, Início, Recentes, Ajustes) e o status
 * ficam na barra superior (widgets/topbar.c). Geometria em 800x480: conteúdo de y = 48 a 480.
 *
 * Desempenho: objetos criados uma vez; os timers só trocam textos e valores que mudaram (um
 * texto igual não é redesenhado); o espectro (100 ms) só anda com música tocando. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/clock.h"
#include "../core/player.h"
#include "../core/cover.h"
#include "../core/btstat.h"
#include "../core/gpsstat.h"
#include "../core/phonestat.h"
#include "../widgets/glass.h"
#include "../assets/icons/bf_icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLR(x) lv_color_hex(x)

static lv_timer_t *tick_t, *spec_t;
static lv_obj_t *clk, *date_l, *wday_l;
static lv_obj_t *bt_dev, *bt_st, *bt_dot, *gps_st, *gps_dot, *gps_bars, *ph_op, *ph_bars, *ph_bat;
static lv_obj_t *cov_box, *cov_img, *cov_note, *song, *artist, *spec, *pos_l, *len_l, *prog, *play_l;
static lv_image_dsc_t thumb;
static int thumb_for = -2, last_min = -1, last_day = -1;
static uint8_t spec_lv[PL_BANDS];
static bool playing;
static int shape = -1;                    /* player: 1 com música, 0 vazio (forma dos textos) */

static void set_text_if(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void fmt_ms(char *out, size_t n, int ms)
{
    int s = ms < 0 ? 0 : ms / 1000;
    if(s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%02d:%02d", s / 60, s % 60);
}

/* ---------------------------------------------------------------- relógio */

static void clock_card(lv_obj_t *p)
{
    lv_obj_t *c = glass_card(p, 30, 21, 446, 88, 24);      /* folga à direita da data */
    clk = glass_label(c, "--:--", BF_FONT_CLOCK, GL_WHITE);
    lv_obj_align(clk, LV_ALIGN_LEFT_MID, 22, 2);
    lv_obj_t *div = lv_obj_create(c);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, 52);
    lv_obj_align(div, LV_ALIGN_LEFT_MID, 212, 0);
    lv_obj_set_style_bg_color(div, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_40, 0);
    lv_obj_t *cal = glass_label(c, BF_SYMBOL_CALENDAR, BF_FONT_L, GL_TEXT2);
    lv_obj_align(cal, LV_ALIGN_LEFT_MID, 232, -2);
    date_l = glass_label(c, "", BF_FONT_L, GL_WHITE);
    lv_obj_set_pos(date_l, 274, 12);
    wday_l = glass_label(c, "", BF_FONT_M, GL_TEXT2);
    lv_obj_set_pos(wday_l, 276, 48);
}

static void gl_clock_tick(void)
{
    time_t n = time(NULL);
    struct tm tm;
    localtime_r(&n, &tm);
    if(!clock_valid()) {                  /* sem hora acertada: nada de data inventada */
        set_text_if(clk, "--:--");
        set_text_if(date_l, "--/--/----");
        set_text_if(wday_l, tr(S_DATE_SRC_UNSET));
        last_min = last_day = -1;
        return;
    }
    if(tm.tm_min != last_min) {
        char b[16];
        bool h24 = settings()->clock_24h;
        snprintf(b, sizeof b, "%02d:%02d", h24 ? tm.tm_hour : (tm.tm_hour + 11) % 12 + 1, tm.tm_min);
        set_text_if(clk, b);
        last_min = tm.tm_min;
    }
    if(tm.tm_yday != last_day) {          /* virada do dia */
        static const char *const wd[LANG_COUNT][7] = {
            { "domingo", "segunda-feira", "terça-feira", "quarta-feira", "quinta-feira", "sexta-feira", "sábado" },
            { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
            { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" } };
        bf_lang_t l = i18n_get_lang();
        char b[32];
        if(l == LANG_EN) snprintf(b, sizeof b, "%02d/%02d/%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
        else snprintf(b, sizeof b, "%02d/%02d/%d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        set_text_if(date_l, b);
        set_text_if(wday_l, wd[l][tm.tm_wday]);
        last_day = tm.tm_yday;
    }
}

/* ---------------------------------------------------------------- conexão */

static void open_bt(lv_event_t *e) { (void)e; nav_go(SCR_BLUETOOTH, 0); }
/* o card abre o mapa de navegação (Big GPS); a busca de satélites é o ícone GPS da barra do topo */
static void open_gps(lv_event_t *e) { (void)e; nav_go(SCR_MAP, 0); }

#define CONN_W 446                         /* largura do card Conexão (alinhado ao do relógio) */

/* a linha inteira é o botão: os filhos não pegam o toque */
static void unclick_children(lv_obj_t *o)
{
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        unclick_children(c);
    }
}

static lv_obj_t *row(lv_obj_t *card, int y, int h, lv_event_cb_t cb)
{
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, 6, y);
    lv_obj_set_size(r, CONN_W - 14, h);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    return r;
}

static lv_obj_t *dot(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, CLR(GL_ACTIVE), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static void conn_card(lv_obj_t *p)
{
    lv_obj_t *c = glass_card(p, 30, 117, CONN_W, 156, 22);  /* folga abaixo do GPS */
    lv_obj_t *t = glass_label(c, tr(S_GL_CONNECTION), BF_FONT_M, GL_WHITE);
    lv_obj_set_pos(t, 18, 8);

    lv_obj_t *r = row(c, 36, 60, open_bt);
    lv_obj_t *ic = glass_tile(r, 40, 12, GL_LIGHT, GL_BLUE, NULL, LV_SYMBOL_BLUETOOTH, BF_FONT_M);
    lv_obj_set_pos(ic, 10, 8);
    lv_obj_t *n = glass_label(r, tr(S_APP_BLUETOOTH), BF_FONT_M, GL_WHITE);
    lv_obj_set_pos(n, 62, 2);
    bt_dev = glass_label(r, "", BF_FONT_S, GL_TEXT2);
    lv_obj_set_pos(bt_dev, 62, 24);
    lv_label_set_long_mode(bt_dev, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bt_dev, 186);
    bt_dot = dot(r);
    lv_obj_set_pos(bt_dot, 63, 45);
    bt_st = glass_label(r, "", BF_FONT_S, GL_ACTIVE);
    lv_obj_set_pos(bt_st, 76, 40);
    /* celular conectado: operadora, antena (sinal 0-5 em 4 barras) e bateria */
    ph_op = glass_label(r, "", BF_FONT_S, GL_WHITE);
    lv_obj_set_width(ph_op, 160);
    lv_label_set_long_mode(ph_op, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ph_op, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(ph_op, CONN_W - 14 - 170, 6);
    ph_bars = glass_bars(r);
    lv_obj_set_pos(ph_bars, CONN_W - 14 - 100, 30);
    ph_bat = glass_label(r, "", BF_FONT_S, GL_WHITE);
    lv_obj_set_pos(ph_bat, CONN_W - 14 - 62, 34);
    glass_bars_set(ph_bars, -1);
    unclick_children(r);

    lv_obj_t *line = lv_obj_create(c);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, CONN_W - 36, 1);
    lv_obj_set_pos(line, 18, 100);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_30, 0);

    r = row(c, 106, 42, open_gps);
    ic = glass_tile(r, 36, 12, GL_LAVENDER, GL_LAVBLUE, NULL, LV_SYMBOL_GPS, BF_FONT_M);
    lv_obj_set_pos(ic, 12, 3);
    n = glass_label(r, tr(S_APP_GPS), BF_FONT_M, GL_WHITE);
    lv_obj_set_pos(n, 62, 0);
    gps_dot = dot(r);
    lv_obj_set_pos(gps_dot, 63, 27);
    gps_st = glass_label(r, "", BF_FONT_S, GL_ACTIVE);
    lv_obj_set_pos(gps_st, 76, 22);
    gps_bars = glass_bars(r);
    lv_obj_set_pos(gps_bars, CONN_W - 14 - 46, 8);
    unclick_children(r);
}

static void status_color(lv_obj_t *dot_, lv_obj_t *lbl, bool on)
{
    lv_color_t c = CLR(on ? GL_ACTIVE : GL_TEXT2);
    lv_obj_set_style_bg_color(dot_, c, 0);
    lv_obj_set_style_bg_opa(dot_, on ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_color(lbl, c, 0);
}

static void conn_tick(void)
{
    bt_info_t b;
    bt_read(&b);
    const char *dev = "", *st;
    bool on = false;
    int nconn = 0;
    for(int i = 0; i < b.ndev; i++)
        if(b.dev[i].connected) { if(!nconn) dev = b.dev[i].name; nconn++; }
    char devb[96];
    if(!b.available) st = tr(S_GL_UNAVAILABLE);
    else if(!b.powered) st = tr(S_BT_ST_OFF);
    else if(nconn) {
        st = tr(S_BT_CONNECTED);
        on = true;
        if(nconn > 1) { snprintf(devb, sizeof devb, tr(S_GL_N_MORE), dev, nconn - 1); dev = devb; }
    }
    else if(b.discovering) st = tr(S_GL_SEARCHING_DEV);
    else { st = tr(S_GL_NO_DEVICE); dev = b.name; }
    set_text_if(bt_dev, dev);
    set_text_if(bt_st, st);
    status_color(bt_dot, bt_st, on);

    /* celular pelo canal de ligações (HFP): operadora, sinal e bateria dele */
    ph_info_t ph;
    ph_read(&ph);
    if(ph.link && ph.signal >= 0) {
        set_text_if(ph_op, ph.op[0] ? ph.op : "");
        glass_bars_set(ph_bars, ph.service == 0 ? 0 : (ph.signal * 4 + 4) / 5);
        char bb[24];
        snprintf(bb, sizeof bb, "%s %d/5", ph.battery >= 4 ? LV_SYMBOL_BATTERY_FULL : ph.battery >= 3 ? LV_SYMBOL_BATTERY_3 :
                 ph.battery >= 2 ? LV_SYMBOL_BATTERY_2 : ph.battery >= 1 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY,
                 ph.battery < 0 ? 0 : ph.battery);
        set_text_if(ph_bat, ph.battery >= 0 ? bb : "");
    } else {
        set_text_if(ph_op, "");
        set_text_if(ph_bat, "");
        glass_bars_set(ph_bars, -1);
    }

    gps_info_t g;
    gps_read(&g);
    int lvl = -1;                         /* sem barras quando não há medida */
    bf_str_t gs;
    if(!gps_available()) gs = S_GL_UNAVAILABLE;
    else if(!gps_enabled() && !g.running) gs = S_GPS_DISABLED;
    else if(g.state <= GPS_STARTING) gs = S_GPS_STARTING;
    else if(g.state == GPS_SEARCHING) { gs = g.view ? S_GL_GPS_SEARCH : S_GL_GPS_NOSIG; lvl = 0; }
    else {
        /* qualidade pela geometria e pelos satélites usados (o que o receptor informa) */
        if(g.used >= 8 && g.hdop > 0 && g.hdop <= 1.5) { gs = S_GL_GPS_STRONG; lvl = 4; }
        else if(g.used >= 6 && g.hdop <= 2.5) { gs = S_GL_GPS_GOOD; lvl = 3; }
        else if(g.used >= 4) { gs = S_GL_GPS_MOD; lvl = 2; }
        else { gs = S_GL_GPS_WEAK; lvl = 1; }
    }
    set_text_if(gps_st, tr(gs));
    status_color(gps_dot, gps_st, lvl >= 1);
    glass_bars_set(gps_bars, lvl);
}

/* ------------------------------------------------------------------ player */

static void open_music(lv_event_t *e) { (void)e; nav_go(SCR_MUSIC, 0); }

static void media_click(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    music_status_t m;
    music_status(&m);
    if(!m.active && !music_has_queue()) { nav_go(SCR_MUSIC, 0); return; }
    music_command(cmd);
}

static void prog_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    music_status_t m;
    music_status(&m);
    if(m.can_seek && m.len_ms > 0) music_seek_ms((int)((long long)lv_slider_get_value(prog) * m.len_ms / 1000));
}

static void spec_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int h = lv_area_get_height(&a);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 2;
    for(int i = 0; i < 16; i++) {
        int lv = spec_lv[i * PL_BANDS / 16];
        int bh = 3 + lv * (h - 3) / 100;
        rd.bg_color = i % 3 == 2 ? CLR(GL_LAVENDER) : CLR(GL_LIGHT);
        lv_area_t r = { a.x1 + i * 9, a.y2 - bh + 1, a.x1 + i * 9 + 4, a.y2 };
        lv_draw_rect(layer, &rd, &r);
    }
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, int cmd, int w, int h, bool main_)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, main_ ? LV_RADIUS_CIRCLE : 14, 0);
    lv_obj_set_style_bg_color(b, CLR(main_ ? GL_BLUE : GL_DEEP), 0);
    lv_obj_set_style_bg_opa(b, main_ ? LV_OPA_70 : LV_OPA_40, 0);
    lv_obj_set_style_border_color(b, CLR(main_ ? GL_SKY : 0xffffff), 0);
    lv_obj_set_style_border_width(b, main_ ? 2 : 1, 0);
    lv_obj_set_style_border_opa(b, main_ ? LV_OPA_80 : LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *l = glass_label(b, sym, main_ ? BF_FONT_L : BF_FONT_M, GL_WHITE);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, media_click, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return b;
}

static void player_card(lv_obj_t *p)
{
    lv_obj_t *c = glass_card(p, 508, 18, 272, 233, 24);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, open_music, LV_EVENT_CLICKED, NULL);

    cov_box = lv_obj_create(c);
    lv_obj_remove_style_all(cov_box);
    lv_obj_set_pos(cov_box, 16, 16);
    lv_obj_set_size(cov_box, 92, 92);
    lv_obj_set_style_radius(cov_box, 14, 0);
    lv_obj_set_style_clip_corner(cov_box, true, 0);
    lv_obj_set_style_bg_color(cov_box, CLR(GL_LAVENDER), 0);
    lv_obj_set_style_bg_grad_color(cov_box, CLR(GL_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(cov_box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cov_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cov_box, lv_color_white(), 0);
    lv_obj_set_style_border_width(cov_box, 1, 0);
    lv_obj_set_style_border_opa(cov_box, LV_OPA_40, 0);
    lv_obj_remove_flag(cov_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    cov_note = glass_label(cov_box, LV_SYMBOL_AUDIO, BF_FONT_L, GL_WHITE);
    lv_obj_center(cov_note);
    cov_img = lv_image_create(cov_box);
    lv_obj_center(cov_img);
    lv_obj_add_flag(cov_img, LV_OBJ_FLAG_HIDDEN);

    song = glass_label(c, "", BF_FONT_M, GL_WHITE);
    lv_obj_set_pos(song, 120, 22);
    lv_label_set_long_mode(song, LV_LABEL_LONG_DOT);
    lv_obj_set_size(song, 140, 24);
    artist = glass_label(c, "", BF_FONT_S, GL_TEXT2);
    lv_obj_set_pos(artist, 120, 48);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(artist, 140, 18);
    spec = lv_obj_create(c);
    lv_obj_remove_style_all(spec);
    lv_obj_set_pos(spec, 120, 74);
    lv_obj_set_size(spec, 140, 32);
    lv_obj_remove_flag(spec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(spec, spec_draw, LV_EVENT_DRAW_MAIN, NULL);

    pos_l = glass_label(c, "", BF_FONT_S, GL_TEXT2);
    lv_obj_set_pos(pos_l, 16, 116);
    len_l = glass_label(c, "", BF_FONT_S, GL_TEXT2);
    lv_obj_align(len_l, LV_ALIGN_TOP_RIGHT, -16, 116);
    prog = lv_slider_create(c);
    lv_obj_set_pos(prog, 22, 142);
    lv_obj_set_size(prog, 228, 5);
    lv_slider_set_range(prog, 0, 1000);
    lv_obj_set_ext_click_area(prog, 14);
    lv_obj_set_style_bg_color(prog, CLR(GL_TEXT2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(prog, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(prog, CLR(GL_SKY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(prog, CLR(GL_WHITE), LV_PART_KNOB);
    lv_obj_set_style_pad_all(prog, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(prog, prog_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *prev = round_btn(c, LV_SYMBOL_PREV, MUSIC_PREV, 56, 42, false);
    lv_obj_t *pb = round_btn(c, LV_SYMBOL_PLAY, MUSIC_TOGGLE, 62, 62, true);
    lv_obj_t *nb = round_btn(c, LV_SYMBOL_NEXT, MUSIC_NEXT, 56, 42, false);
    lv_obj_set_pos(prev, 26, 170);
    lv_obj_set_pos(pb, 105, 160);
    lv_obj_set_pos(nb, 190, 170);
    play_l = lv_obj_get_child(pb, 0);
}

static void player_tick(void)
{
    music_status_t m;
    music_status(&m);
    char b[32];
    playing = m.active && m.playing;
    if(shape != (int)m.active) {
        shape = m.active;
        lv_label_set_long_mode(song, m.active ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
        lv_obj_set_height(song, m.active ? 24 : LV_SIZE_CONTENT);
        lv_label_set_long_mode(artist, m.active ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
        lv_obj_set_height(artist, m.active ? 18 : LV_SIZE_CONTENT);
        lv_obj_set_y(artist, m.active ? 48 : 72);      /* vazio: abaixo das duas linhas do título */
        if(m.active) lv_obj_remove_flag(spec, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(spec, LV_OBJ_FLAG_HIDDEN);
    }
    if(m.active) {
        set_text_if(song, m.title);
        set_text_if(artist, m.sub);
        fmt_ms(b, sizeof b, m.pos_ms);
        set_text_if(pos_l, b);
        if(m.len_ms > 0) fmt_ms(b, sizeof b, m.len_ms);
        set_text_if(len_l, m.len_ms > 0 ? b : "--:--");
        int v = m.len_ms > 0 ? (int)((long long)m.pos_ms * 1000 / m.len_ms) : 0;
        if(!lv_obj_has_state(prog, LV_STATE_PRESSED) && lv_slider_get_value(prog) != v)
            lv_slider_set_value(prog, v, LV_ANIM_OFF);
    } else {
        set_text_if(song, tr(S_GL_NO_MUSIC));
        set_text_if(artist, tr(S_HOME_OPEN_HINT));
        set_text_if(pos_l, "");
        set_text_if(len_l, "");
        if(lv_slider_get_value(prog) != 0) lv_slider_set_value(prog, 0, LV_ANIM_OFF);
    }
    if(m.can_seek) lv_obj_add_flag(prog, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(prog, LV_OBJ_FLAG_CLICKABLE);     /* celular: sem busca na faixa */
    set_text_if(play_l, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    int want = m.active ? m.idx : -1;
    if(want != thumb_for) {               /* faixa nova: capa do cartão; celular: símbolo */
        thumb_for = want;
        lv_obj_add_flag(cov_img, LV_OBJ_FLAG_HIDDEN);
        jpeg_free(&thumb);
        const uint8_t *d;
        size_t n;
        const char *mime;
        bool ok = want >= 0 && player_cover(&d, &n, &mime) && jpeg_fit(d, n, 92, 92, &thumb);
        lv_label_set_text(cov_note, want == -3 ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_AUDIO);
        if(ok) {
            lv_image_set_src(cov_img, &thumb);
            lv_obj_center(cov_img);
            lv_obj_remove_flag(cov_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cov_note, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_remove_flag(cov_note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void spec_tick(lv_timer_t *t)
{
    (void)t;
    static bool was;
    if(!playing && !was) return;          /* parado: nada a redesenhar */
    uint8_t lv[PL_BANDS];
    player_spectrum(lv);
    if(!playing) memset(lv, 0, sizeof lv);
    bool changed = false;
    for(int i = 0; i < PL_BANDS; i++)
        if(lv[i] / 6 != spec_lv[i] / 6) { changed = true; break; }
    memcpy(spec_lv, lv, sizeof lv);
    if(changed) lv_obj_invalidate(spec);
    was = playing;
}

/* -------------------------------------------------------------------- dock */

static void dock_click(lv_event_t *e)
{
    switch((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0: nav_go(SCR_TV, 0); break;
    case 1: nav_go(SCR_FM, 0); break;
    case 2: nav_go(SCR_HOME, HOME_ARG_APPS); break;
    case 3: nav_go(SCR_GALLERY, 0); break;
    case 4: nav_go(SCR_PHONE, 0); break;
    }
}

static lv_obj_t *dock_item(lv_obj_t *dock, int cx, int w, int h, int i)
{
    lv_obj_t *it = lv_obj_create(dock);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, w, h);
    lv_obj_set_pos(it, cx - w / 2, 0);
    lv_obj_set_style_radius(it, 18, 0);
    lv_obj_set_style_bg_color(it, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_remove_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(it, dock_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return it;
}

static void apps_squares(lv_obj_t *t)
{
    for(int k = 0; k < 4; k++) {          /* 2x2 quadrados brancos */
        lv_obj_t *q = lv_obj_create(t);
        lv_obj_remove_style_all(q);
        lv_obj_set_size(q, 24, 24);
        lv_obj_set_style_radius(q, 7, 0);
        lv_obj_set_style_bg_color(q, CLR(GL_WHITE), 0);
        lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
        lv_obj_remove_flag(q, LV_OBJ_FLAG_CLICKABLE);   /* o toque é do botão Apps inteiro */
        lv_obj_align(q, LV_ALIGN_CENTER, k % 2 ? 15 : -15, k / 2 ? 15 : -15);
    }
}

static void dock(lv_obj_t *p)
{
    lv_obj_t *d = glass_card(p, 24, 287, 752, 124, 28);
    static const struct { const lv_image_dsc_t *icon; bf_str_t name; uint32_t top, bot; } it[5] = {
        { &bf_icon_tv, S_APP_TV, GL_LIGHT, GL_BLUE },
        { &bf_icon_fm, S_GL_RADIO, GL_LAVENDER, GL_LAVBLUE },
        { NULL, S_GL_APPS, GL_SKY, GL_BLUE },
        { &bf_icon_gallery, S_APP_GALLERY, GL_LIGHT, GL_BLUE },
        { &bf_icon_phone, S_APP_PHONE, GL_LAVENDER, GL_LAVBLUE },
    };
    static const int cx[5] = { 85, 231, 376, 521, 667 };
    for(int i = 0; i < 5; i++) {
        if(i == 2) continue;                  /* Apps: peça própria, por cima do dock */
        lv_obj_t *item = dock_item(d, cx[i], 142, 122, i);   /* 142: cabe "FM Transmitir" */
        lv_obj_t *t = glass_tile(item, 72, 20, it[i].top, it[i].bot, it[i].icon, NULL, NULL);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *l = glass_label(item, tr(it[i].name), BF_FONT_M, GL_WHITE);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 88);
    }
    /* Apps: quadrado arredondado de 96 px que vaza 18 px acima do dock; filho da tela (não
     * do dock), para nada cortar a parte de fora; o rótulo alinha com os outros (y 375) */
    lv_obj_t *item = dock_item(p, 24 + cx[2], 132, 140, 2);
    lv_obj_set_y(item, 263);
    lv_obj_t *t = glass_tile(item, 96, 24, it[2].top, it[2].bot, NULL, NULL, NULL);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_border_color(t, CLR(GL_SKY), 0);
    lv_obj_set_style_border_opa(t, LV_OPA_80, 0);
    apps_squares(t);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *l = glass_label(item, tr(it[2].name), BF_FONT_M, GL_WHITE);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 112);
}

/* -------------------------------------------------------------------- tela */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    gl_clock_tick();
    conn_tick();
    player_tick();
}

void home_glass_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BF_SCREEN_W, BF_CONTENT_H);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    clock_card(root);
    conn_card(root);
    player_card(root);
    dock(root);
    last_min = last_day = -1;
    shape = -1;
    thumb_for = -2;
    memset(spec_lv, 0, sizeof spec_lv);
    tick_cb(NULL);
    tick_t = lv_timer_create(tick_cb, 500, NULL);
    spec_t = lv_timer_create(spec_tick, 100, NULL);
}

void home_glass_destroy(void)
{
    if(tick_t) { lv_timer_delete(tick_t); tick_t = NULL; }
    if(spec_t) { lv_timer_delete(spec_t); spec_t = NULL; }
    jpeg_free(&thumb);
    thumb_for = -2;
    clk = date_l = wday_l = bt_dev = bt_st = bt_dot = gps_st = gps_dot = gps_bars = ph_op = ph_bars = ph_bat = NULL;
    cov_box = cov_img = cov_note = song = artist = spec = pos_l = len_l = prog = play_l = NULL;
}
