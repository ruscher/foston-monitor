/* scr_gps.c — GPS: estado do receptor (bfgps), satélites com a força do sinal,
 * mapa do céu, posição, altitude, velocidade, rumo e hora UTC; liga/desliga o
 * serviço. Atualiza a cada segundo lendo /run/bigfoston-gps (core/gpsstat). */
#include "screens.h"
#include "../core/theme.h"
#include "../core/gpsstat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define C_GPS 0x2ec27e

static lv_obj_t *st_dot, *st_lbl, *st_sub, *pos_lbl, *alt_lbl, *spd_lbl, *crs_lbl, *utc_lbl, *dop_lbl;
static lv_obj_t *sky, *bars, *bars_note, *onoff_lbl, *onoff_btn;
static lv_timer_t *tick;
static gps_info_t cur;

/* ---------------------------------------------------------------- textos */

static void deg_text(char *out, size_t n, double v, char pos, char neg)
{
    char h = v < 0 ? neg : pos;
    v = fabs(v);
    int d = (int)v;
    double m = (v - d) * 60.0;
    snprintf(out, n, "%d° %06.3f' %c", d, m, h);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* -------------------------------------------- mapa do céu e barras de sinal */

static lv_color_t snr_color(int snr)
{
    if(snr < 0) return pal()->text_dim;
    if(snr < 20) return lv_color_hex(0xe5484d);
    if(snr < 30) return lv_color_hex(0xf5a524);
    return lv_color_hex(C_GPS);
}

static void sky_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = lv_area_get_width(&a) / 2 - 10;
    const bf_palette_t *p = pal();
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = p->text_dim;
    ad.opa = LV_OPA_50;
    ad.width = 1;
    ad.center.x = cx;
    ad.center.y = cy;
    ad.start_angle = 0;
    ad.end_angle = 360;
    for(int k = 1; k <= 3; k++) {            /* horizonte, 30° e 60° */
        ad.radius = (uint16_t)(r * k / 3);
        lv_draw_arc(layer, &ad);
    }
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = p->text_dim;
    ld.font = BF_FONT_S;
    lv_area_t na = { cx - 6, a.y1 - 2, cx + 8, a.y1 + 14 };
    ld.text = "N";
    lv_draw_label(layer, &ld, &na);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_opa = LV_OPA_COVER;
    for(int i = 0; i < cur.n_sats; i++) {
        const gps_sat_t *s = &cur.sats[i];
        if(s->elev < 0 || s->elev > 90) continue;
        double rr = r * (90 - s->elev) / 90.0, az = s->az * M_PI / 180.0;
        int x = cx + (int)(rr * sin(az)), y = cy - (int)(rr * cos(az));
        rd.bg_color = snr_color(s->snr);
        lv_area_t d = { x - 6, y - 6, x + 6, y + 6 };
        lv_draw_rect(layer, &rd, &d);
    }
}

static void bars_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int n = cur.n_sats > 16 ? 16 : cur.n_sats;
    if(!n) return;
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a) - 18;
    int slot = w / 16, bw = slot - 6;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 2;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = BF_FONT_S;
    ld.color = pal()->text_dim;
    ld.align = LV_TEXT_ALIGN_CENTER;
    for(int i = 0; i < n; i++) {
        const gps_sat_t *s = &cur.sats[i];
        int snr = s->snr < 0 ? 0 : s->snr > 50 ? 50 : s->snr;
        int bh = 2 + snr * (h - 2) / 50;
        int x = a.x1 + i * slot + 3;
        rd.bg_color = snr_color(s->snr);
        lv_area_t r = { x, a.y1 + h - bh, x + bw - 1, a.y1 + h - 1 };
        lv_draw_rect(layer, &rd, &r);
        char t[8];
        snprintf(t, sizeof t, "%d", s->prn);
        ld.text = t;
        ld.text_local = 1;
        lv_area_t la = { x - 4, a.y1 + h + 2, x + bw + 3, a.y2 };
        lv_draw_label(layer, &ld, &la);
    }
}

/* ---------------------------------------------------------------- estado */

static void refresh(void)
{
    gps_read(&cur);
    const bf_palette_t *p = pal();
    char b[96], b2[64];
    static const bf_str_t names[] = { S_GPS_OFF, S_GPS_STARTING, S_GPS_SEARCHING, S_GPS_FIX2D, S_GPS_FIX3D };
    lv_color_t dot = cur.state >= GPS_FIX2D ? lv_color_hex(C_GPS) :
                     cur.state >= GPS_STARTING ? lv_color_hex(0xf5a524) : p->text_dim;
    lv_obj_set_style_bg_color(st_dot, dot, 0);
    set_text(st_lbl, tr(gps_enabled() || cur.running ? names[cur.state] : S_GPS_DISABLED));
    if(!gps_available()) set_text(st_sub, tr(S_GPS_NO_DLL));
    else {
        snprintf(b, sizeof b, tr(S_GPS_SATS), cur.used, cur.view);
        set_text(st_sub, cur.running ? b : "");
    }
    bool fix = cur.state >= GPS_FIX2D;
    if(fix) {
        char la[32], lo[32];
        deg_text(la, sizeof la, cur.lat, 'N', 'S');
        deg_text(lo, sizeof lo, cur.lon, 'E', 'W');
        snprintf(b, sizeof b, "%s\n%s", la, lo);
    }
    else snprintf(b, sizeof b, "—\n—");
    set_text(pos_lbl, b);
    snprintf(b, sizeof b, "%s %s", tr(S_GPS_ALT), fix && cur.state == GPS_FIX3D ? "" : "—");
    if(fix && cur.state == GPS_FIX3D) snprintf(b, sizeof b, "%s %.0f m", tr(S_GPS_ALT), cur.alt);
    set_text(alt_lbl, b);
    if(fix) snprintf(b, sizeof b, "%s %.1f km/h", tr(S_GPS_SPEED), cur.speed_kmh);
    else snprintf(b, sizeof b, "%s —", tr(S_GPS_SPEED));
    set_text(spd_lbl, b);
    if(fix) snprintf(b, sizeof b, "%s %.0f°", tr(S_GPS_COURSE), cur.course);
    else snprintf(b, sizeof b, "%s —", tr(S_GPS_COURSE));
    set_text(crs_lbl, b);
    /* sem posição o motor ainda não tem a hora dos satélites (conta a partir de
     * um valor inicial): só mostra com posição válida */
    if(fix && strlen(cur.time) >= 6 && strlen(cur.date) >= 6)
        snprintf(b2, sizeof b2, "%.2s/%.2s/20%.2s %.2s:%.2s:%.2s UTC", cur.date, cur.date + 2, cur.date + 4,
                 cur.time, cur.time + 2, cur.time + 4);
    else snprintf(b2, sizeof b2, "—");
    snprintf(b, sizeof b, "%s %s", tr(S_GPS_UTC), b2);
    set_text(utc_lbl, b);
    if(cur.running && cur.hdop < 99) snprintf(b, sizeof b, "HDOP %.1f", cur.hdop);
    else snprintf(b, sizeof b, "HDOP —");
    set_text(dop_lbl, b);
    set_text(bars_note, cur.n_sats ? "" : tr(cur.running ? S_GPS_NO_SATS : S_GPS_NOT_RUNNING));
    set_text(onoff_lbl, tr(gps_enabled() ? S_GPS_TURN_OFF : S_GPS_TURN_ON));
    if(gps_available()) lv_obj_remove_state(onoff_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(onoff_btn, LV_STATE_DISABLED);
    lv_obj_invalidate(sky);
    lv_obj_invalidate(bars);
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

static void onoff_click(lv_event_t *e)
{
    (void)e;
    gps_set_enabled(!gps_enabled());
    refresh();
}

/* ---------------------------------------------------------------- tela */

static lv_obj_t *card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = bf_card(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_pad_all(c, BF_SP_M, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 4, 0);
    return c;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, BF_SP_M, 0);
    lv_obj_set_style_pad_row(root, BF_SP_S, 0);
    lv_obj_set_style_pad_column(root, BF_SP_S, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW_WRAP);

    /* estado + liga/desliga */
    lv_obj_t *c1 = card(root, 300, 160);
    lv_obj_t *row = bf_plain(c1);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    st_dot = lv_obj_create(row);
    lv_obj_remove_style_all(st_dot);
    lv_obj_set_size(st_dot, 14, 14);
    lv_obj_set_style_radius(st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(st_dot, LV_OPA_COVER, 0);
    st_lbl = bf_label(row, "", BF_FONT_M, p->text);
    st_sub = bf_label(c1, "", BF_FONT_S, p->text_dim);
    lv_label_set_long_mode(st_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(st_sub, LV_PCT(100));
    dop_lbl = bf_label(c1, "", BF_FONT_S, p->text_dim);
    onoff_btn = bf_button(c1, LV_SYMBOL_POWER, "", 0);
    lv_obj_set_height(onoff_btn, 44);
    onoff_lbl = lv_obj_get_child(onoff_btn, lv_obj_get_child_count(onoff_btn) - 1);
    lv_obj_add_event_cb(onoff_btn, onoff_click, LV_EVENT_CLICKED, NULL);

    /* posição */
    lv_obj_t *c2 = card(root, 284, 160);
    bf_label(c2, tr(S_GPS_POSITION), BF_FONT_S, p->text_dim);
    pos_lbl = bf_label(c2, "", BF_FONT_M, p->text);
    alt_lbl = bf_label(c2, "", BF_FONT_S, p->text);
    spd_lbl = bf_label(c2, "", BF_FONT_S, p->text);
    crs_lbl = bf_label(c2, "", BF_FONT_S, p->text);

    /* mapa do céu */
    lv_obj_t *c3 = card(root, 168, 160);
    lv_obj_set_style_pad_all(c3, 4, 0);
    sky = lv_obj_create(c3);
    lv_obj_remove_style_all(sky);
    lv_obj_set_size(sky, 152, 152);
    lv_obj_remove_flag(sky, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sky, sky_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* força do sinal por satélite */
    lv_obj_t *c4 = card(root, LV_PCT(100), 160);
    lv_obj_t *hdr = bf_plain(c4);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *ht = bf_label(hdr, tr(S_GPS_SIGNAL), BF_FONT_S, p->text_dim);
    lv_obj_align(ht, LV_ALIGN_LEFT_MID, 0, 0);
    utc_lbl = bf_label(hdr, "", BF_FONT_S, p->text_dim);
    lv_obj_align(utc_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    bars = lv_obj_create(c4);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, LV_PCT(100), 110);
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bars, bars_draw, LV_EVENT_DRAW_MAIN, NULL);
    bars_note = bf_label(bars, "", BF_FONT_S, p->text_dim);
    lv_obj_center(bars_note);

    refresh();
    tick = lv_timer_create(tick_cb, 1000, NULL);
}

static void destroy(void)
{
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    st_dot = st_lbl = st_sub = pos_lbl = alt_lbl = spd_lbl = crs_lbl = utc_lbl = dop_lbl = NULL;
    sky = bars = bars_note = onoff_lbl = onoff_btn = NULL;
}

const bf_screen_def_t scr_gps_def = { create, destroy, S_APP_GPS, NULL };
