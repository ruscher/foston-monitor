/* scr_music.c — Multimídia: tocador de MP3.
 *
 * Motor em core/player.c (libmpg123 + OSS). Esta tela mostra capa simbólica,
 * título/artista/álbum das tags ID3, formato real (MPEG, kbps, kHz, canais),
 * espectro calculado do som que está tocando, barra de progresso com tempo
 * decorrido/total e avanço por toque/arraste, controles (aleatório, anterior,
 * tocar/pausar, próxima, repetir: desligado/todas/uma), lista de reprodução,
 * informações e estatísticas da sessão, e um modo tela cheia.
 * A reprodução continua fora da tela: um timer global cuida do fim da faixa.
 * Procura *.mp3 em /mnt/sd (cartão SD, só leitura) e em <dados>/music. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/player.h"
#include "../core/cover.h"
#include "../widgets/dialog.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_TRACKS 300
#define HIST       32
#define C_MEDIA    0xe5534b

/* ---------------- estado global (vive fora da tela) ---------------- */

static char *tracks[MAX_TRACKS];
static int n_tracks, cur = -1;
static int plays[MAX_TRACKS];
static int hist[HIST], n_hist;
static long long listened_ms;
static lv_timer_t *watch;
static uint32_t last_tick;

enum { REP_OFF, REP_ALL, REP_ONE };

/* ---------------- objetos da tela (NULL fora dela) ---------------- */

typedef struct {
    lv_obj_t *title, *artist, *tech, *elapsed, *total, *slider, *play_lbl;
    lv_obj_t *shuffle, *repeat, *badge;
    lv_obj_t *spec;               /* um só objeto desenha as PL_BANDS barras */
    uint8_t shown[PL_BANDS];      /* nível desenhado (0..25): só redesenha se mudar */
    int bar_h;
    int shown_sec;                /* segundo mostrado em "decorrido" */
} view_t;

static view_t v, vfull;          /* tela normal e tela cheia */
static lv_obj_t *root, *full, *queue_panel, *cover_img, *cover_note;
static int cover_for = -2;       /* faixa cuja capa está na tela */
static lv_timer_t *ui_timer;
static bool dragging;

static void ui_refresh_track(void);

/* ---------------- lista de faixas ---------------- */

static void free_tracks(void)
{
    for(int i = 0; i < n_tracks; i++) free(tracks[i]);
    n_tracks = 0;
}

static bool is_mp3(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".mp3");
}

static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if(!d) return;
    struct dirent *e;
    char p[512];
    while(n_tracks < MAX_TRACKS && (e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if(stat(p, &st)) continue;
        if(S_ISDIR(st.st_mode)) { if(depth < 3) scan(p, depth + 1); }
        else if(S_ISREG(st.st_mode) && is_mp3(e->d_name)) tracks[n_tracks++] = strdup(p);
    }
    closedir(d);
}

static int cmp_track(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* nome do arquivo sem ".mp3", para quando não há tag de título */
static void file_title(char *out, size_t n, const char *path)
{
    snprintf(out, n, "%s", base_name(path));
    size_t l = strlen(out);
    if(l > 4 && !strcasecmp(out + l - 4, ".mp3")) out[l - 4] = 0;
}

static void fmt_time(char *out, size_t n, int ms)
{
    if(ms < 0) ms = 0;
    int s = ms / 1000;
    if(s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/* ---------------- reprodução ---------------- */

static void play(int idx, bool remember)
{
    if(idx < 0 || idx >= n_tracks) {
        player_stop();
        cur = -1;
        ui_refresh_track();
        return;
    }
    if(remember && cur >= 0 && cur != idx) {
        if(n_hist == HIST) { memmove(hist, hist + 1, sizeof hist[0] * (HIST - 1)); n_hist--; }
        hist[n_hist++] = cur;
    }
    cur = idx;
    if(player_open(tracks[idx])) plays[idx]++;
    ui_refresh_track();
}

static int next_index(bool user)
{
    if(!n_tracks) return -1;
    if(!user && settings()->music_repeat == REP_ONE) return cur;
    if(settings()->music_shuffle && n_tracks > 1) {
        int r;
        do r = rand() % n_tracks; while(r == cur);
        return r;
    }
    if(cur + 1 < n_tracks) return cur + 1;
    return settings()->music_repeat == REP_ALL || user ? 0 : -1;
}

/* fim da faixa e tempo ouvido; roda sempre, mesmo fora da tela */
static void watch_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    if(player_state() == PL_PLAYING) listened_ms += (uint32_t)(now - last_tick);
    last_tick = now;
    if(player_take_finished()) play(next_index(false), true);
}

/* ---------------- ações ---------------- */

static void toggle_click(lv_event_t *e)
{
    (void)e;
    if(player_state() == PL_STOPPED) play(cur >= 0 ? cur : 0, false);
    else player_toggle();
    ui_refresh_track();
}

static void prev_click(lv_event_t *e)
{
    (void)e;
    if(player_state() != PL_STOPPED && player_pos_ms() > 3000) { player_seek_ms(0); return; }
    if(n_hist) { play(hist[--n_hist], false); return; }
    play(cur > 0 ? cur - 1 : 0, false);
}

static void next_click(lv_event_t *e) { (void)e; play(next_index(true), true); }

static void shuffle_click(lv_event_t *e)
{
    (void)e;
    settings()->music_shuffle = !settings()->music_shuffle;
    settings_save();
    ui_refresh_track();
}

static void repeat_click(lv_event_t *e)
{
    (void)e;
    settings()->music_repeat = (settings()->music_repeat + 1) % 3;
    settings_save();
    ui_refresh_track();
}

static void slider_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *s = lv_event_get_target(e);
    if(c == LV_EVENT_PRESSED) dragging = true;
    else if(c == LV_EVENT_VALUE_CHANGED && dragging) {
        char b[16];
        fmt_time(b, sizeof b, (int)lv_slider_get_value(s) * 1000);
        if(v.elapsed && s == v.slider) lv_label_set_text(v.elapsed, b);
        if(vfull.elapsed && s == vfull.slider) lv_label_set_text(vfull.elapsed, b);
    }
    else if(c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        dragging = false;
        v.shown_sec = vfull.shown_sec = -1;
        if(player_state() != PL_STOPPED) player_seek_ms((int)lv_slider_get_value(s) * 1000);
    }
}

/* ---------------- peças da interface ---------------- */

static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, int kind, int w, int h)
{
    lv_obj_t *b = bf_button(parent, sym, NULL, kind);
    lv_obj_set_size(b, w, h);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* Espectro: um objeto com desenho próprio. Barras separadas geravam ~1000
 * áreas invalidadas por segundo, e cada área custa o pipeline inteiro do
 * LVGL no ARM (~30 % da CPU); assim é uma área por quadro. */
static void spectrum_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    view_t *vw = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int gap = 2, bw = (w - gap * (PL_BANDS - 1)) / PL_BANDS;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(C_MEDIA);
    d.bg_opa = LV_OPA_COVER;
    for(int i = 0; i < PL_BANDS; i++) {
        int bh = 2 + vw->shown[i] * 4 * (h - 2) / 100;
        lv_area_t r = { a.x1 + i * (bw + gap), a.y2 - bh + 1, a.x1 + i * (bw + gap) + bw - 1, a.y2 };
        lv_draw_rect(layer, &d, &r);
    }
}

static void build_spectrum(lv_obj_t *parent, view_t *vw, int w, int h)
{
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, w, h);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sp, spectrum_draw, LV_EVENT_DRAW_MAIN, vw);
    memset(vw->shown, 0, sizeof vw->shown);
    vw->spec = sp;
    vw->bar_h = h;
    vw->shown_sec = -1;
}

static void build_progress(lv_obj_t *parent, view_t *vw)
{
    const bf_palette_t *p = pal();
    lv_obj_t *row = bf_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_M, 0);
    vw->elapsed = bf_label(row, "0:00", BF_FONT_S, p->text_dim);
    lv_obj_set_width(vw->elapsed, 56);
    lv_obj_set_style_text_align(vw->elapsed, LV_TEXT_ALIGN_RIGHT, 0);
    vw->slider = lv_slider_create(row);
    lv_obj_set_flex_grow(vw->slider, 1);
    lv_obj_set_height(vw->slider, 10);
    lv_obj_set_ext_click_area(vw->slider, 16);
    lv_obj_set_style_bg_color(vw->slider, p->surface2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vw->slider, lv_color_hex(C_MEDIA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vw->slider, p->text, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vw->slider, 6, LV_PART_KNOB);
    lv_slider_set_range(vw->slider, 0, 1);
    lv_obj_add_event_cb(vw->slider, slider_event, LV_EVENT_ALL, NULL);
    vw->total = bf_label(row, "--:--", BF_FONT_S, p->text_dim);
    lv_obj_set_width(vw->total, 56);
}

static void close_full(lv_event_t *e);
static void open_full(lv_event_t *e);
static void open_queue(lv_event_t *e);
static void open_info(lv_event_t *e);

static void build_controls(lv_obj_t *parent, bool compact)
{
    lv_obj_t *row = bf_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    view_t *vw = compact ? &v : &vfull;

    lv_obj_t *sb = icon_button(row, LV_SYMBOL_SHUFFLE, shuffle_click, 0, 64, 56);
    icon_button(row, LV_SYMBOL_PREV, prev_click, 0, 72, 56);
    lv_obj_t *pb = icon_button(row, LV_SYMBOL_PLAY, toggle_click, 1, 96, 60);
    vw->play_lbl = lv_obj_get_child(pb, 0);
    icon_button(row, LV_SYMBOL_NEXT, next_click, 0, 72, 56);
    lv_obj_t *rb = icon_button(row, LV_SYMBOL_LOOP, repeat_click, 0, 64, 56);
    vw->shuffle = sb;
    vw->repeat = rb;
    vw->badge = bf_label(rb, "1", BF_FONT_S, pal()->text);   /* repetir uma */
    lv_obj_add_flag(vw->badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(vw->badge, LV_ALIGN_TOP_RIGHT, 4, -6);
    if(compact) {
        lv_obj_t *gap = bf_plain(row);
        lv_obj_set_size(gap, BF_SP_M, 1);
        icon_button(row, LV_SYMBOL_LIST, open_queue, 0, 64, 56);
        icon_button(row, BF_SYMBOL_INFO, open_info, 0, 64, 56);
        icon_button(row, LV_SYMBOL_UP, open_full, 0, 64, 56);
    }
    else {
        lv_obj_t *gap = bf_plain(row);
        lv_obj_set_size(gap, BF_SP_L, 1);
        icon_button(row, LV_SYMBOL_DOWN, close_full, 0, 64, 56);
    }
}

/* ---------------- atualização ---------------- */

/* lv_label_set_text redesenha mesmo com o mesmo texto: só troca se mudou */
static void set_text(lv_obj_t *l, const char *t)
{
    if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void tech_line(char *out, size_t n)
{
    pl_info_t in;
    player_info(&in);
    if(!in.rate) { out[0] = 0; return; }
    static const char *const ver[] = { "1", "2", "2.5" };
    snprintf(out, n, "MPEG-%s Layer %s  ·  %d kbps%s  ·  %d,%d kHz  ·  %s%s",
             ver[in.version >= 0 && in.version < 3 ? in.version : 0],
             in.layer == 1 ? "I" : in.layer == 2 ? "II" : "III",
             in.vbr && player_len_ms() > 0 ? (int)(in.file_bytes * 8 / player_len_ms()) : in.bitrate_kbps,
             in.vbr ? " VBR" : "", in.rate / 1000, in.rate % 1000 / 100,
             in.channels == 2 ? tr(S_MUSIC_STEREO) : tr(S_MUSIC_MONO),
             in.audio_out ? "" : tr(S_MUSIC_NOOUT));
}

static void refresh_view(view_t *vw)
{
    if(!vw->title) return;
    pl_tags_t t;
    char buf[300], name[200];
    player_tags(&t);
    if(cur >= 0) {
        file_title(name, sizeof name, tracks[cur]);
        set_text(vw->title, t.title[0] ? t.title : name);
        const char *ar = t.artist[0] ? t.artist : tr(S_MUSIC_UNKNOWN);
        if(t.album[0] && t.year[0]) snprintf(buf, sizeof buf, "%s  —  %s (%s)", ar, t.album, t.year);
        else if(t.album[0]) snprintf(buf, sizeof buf, "%s  —  %s", ar, t.album);
        else snprintf(buf, sizeof buf, "%s", ar);
        set_text(vw->artist, buf);
    }
    else {
        set_text(vw->title, tr(S_MUSIC_IDLE));
        snprintf(buf, sizeof buf, "%d %s", n_tracks, tr(S_MUSIC_TRACKS));
        set_text(vw->artist, buf);
    }
    if(vw->tech) { tech_line(buf, sizeof buf); set_text(vw->tech, buf); }
    int r = settings()->music_repeat;
    lv_obj_set_state(vw->shuffle, LV_STATE_CHECKED, settings()->music_shuffle);
    lv_obj_set_state(vw->repeat, LV_STATE_CHECKED, r != REP_OFF);
    if(r == REP_ONE) lv_obj_remove_flag(vw->badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(vw->badge, LV_OBJ_FLAG_HIDDEN);
    set_text(vw->play_lbl, player_state() == PL_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    int len = player_len_ms(), mx = len > 1000 ? len / 1000 : 1;
    if(lv_slider_get_max_value(vw->slider) != mx) lv_slider_set_range(vw->slider, 0, mx);
    fmt_time(buf, sizeof buf, len);
    set_text(vw->total, len ? buf : "--:--");
}

static void refresh_cover(void)
{
    if(!cover_img || cover_for == cur) return;
    cover_for = cur;
    const uint8_t *d;
    size_t n;
    const char *mime;
    const lv_image_dsc_t *img = NULL;
    if(cur >= 0 && player_cover(&d, &n, &mime)) img = cover_decode(d, n);
    if(img) {
        lv_image_set_src(cover_img, NULL);
        lv_image_set_src(cover_img, img);
        lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cover_note, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cover_note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_refresh_track(void)
{
    refresh_cover();
    refresh_view(&v);
    refresh_view(&vfull);
}

static void tick_view(view_t *vw, const uint8_t *lv, int pos)
{
    if(!vw->title) return;
    bool changed = false;
    for(int i = 0; i < PL_BANDS; i++) {
        uint8_t q = (uint8_t)(lv[i] / 4);          /* 25 degraus bastam */
        if(q != vw->shown[i]) { vw->shown[i] = q; changed = true; }
    }
    if(changed) lv_obj_invalidate(vw->spec);
    if(!dragging && pos / 1000 != vw->shown_sec) {    /* muda 1x por segundo */
        char b[16];
        vw->shown_sec = pos / 1000;
        fmt_time(b, sizeof b, pos);
        set_text(vw->elapsed, b);
        lv_slider_set_value(vw->slider, pos / 1000, LV_ANIM_OFF);
    }
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    static pl_state_t last = PL_STOPPED;
    uint8_t lv[PL_BANDS];
    player_spectrum(lv);
    int pos = player_pos_ms();
    tick_view(&v, lv, pos);
    tick_view(&vfull, lv, pos);
    pl_state_t s = player_state();
    if(s != last) {
        last = s;
        ui_refresh_track();
    }
}

/* ---------------- lista de reprodução ---------------- */

static void queue_close(lv_event_t *e)
{
    (void)e;
    if(queue_panel) { lv_obj_delete(queue_panel); queue_panel = NULL; }
}

static void queue_pick(lv_event_t *e)
{
    play((int)(intptr_t)lv_event_get_user_data(e), true);
    queue_close(NULL);
}

static void open_queue(lv_event_t *e)
{
    (void)e;
    const bf_palette_t *p = pal();
    if(queue_panel) { queue_close(NULL); return; }
    queue_panel = bf_card(root);
    lv_obj_add_flag(queue_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(queue_panel, 430, LV_PCT(100));
    lv_obj_align(queue_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(queue_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(queue_panel, BF_SP_S, 0);
    lv_obj_set_style_pad_row(queue_panel, 2, 0);
    lv_obj_set_style_shadow_width(queue_panel, 24, 0);
    lv_obj_set_style_shadow_opa(queue_panel, LV_OPA_50, 0);

    lv_obj_t *hdr = bf_plain(queue_panel);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    lv_obj_t *ht = bf_label(hdr, tr(S_MUSIC_QUEUE), BF_FONT_M, p->text);
    lv_obj_align(ht, LV_ALIGN_LEFT_MID, BF_SP_S, 0);
    lv_obj_t *x = icon_button(hdr, LV_SYMBOL_CLOSE, queue_close, 0, 48, 40);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *first = NULL;
    for(int i = 0; i < n_tracks; i++) {
        lv_obj_t *it = lv_button_create(queue_panel);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, LV_PCT(100), 44);
        lv_obj_set_style_radius(it, BF_RADIUS_S, 0);
        lv_obj_set_style_bg_color(it, p->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(it, BF_SP_M, 0);
        lv_obj_add_event_cb(it, queue_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char name[200];
        file_title(name, sizeof name, tracks[i]);
        bool now = i == cur;
        lv_obj_t *l = bf_label(it, name, BF_FONT_S, now ? lv_color_hex(C_MEDIA) : p->text);
        bf_label_ellipsis(l, plays[i] ? 330 : 380);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, now ? 22 : 0, 0);
        if(now) {
            lv_obj_t *ic = bf_label(it, LV_SYMBOL_VOLUME_MAX, BF_FONT_S, lv_color_hex(C_MEDIA));
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
            first = it;
        }
        if(plays[i]) {
            char c[16];
            snprintf(c, sizeof c, "%d×", plays[i]);
            lv_obj_t *pl = bf_label(it, c, BF_FONT_S, p->text_dim);
            lv_obj_align(pl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
    if(first) lv_obj_scroll_to_view(first, LV_ANIM_OFF);
}

/* ---------------- informações e estatísticas ---------------- */

static void open_info(lv_event_t *e)
{
    (void)e;
    static char body[900];
    const char *bt[] = { tr(S_CLOSE) };
    char tl[24], ln[24], tech[200];
    int total_plays = 0;
    for(int i = 0; i < n_tracks; i++) total_plays += plays[i];
    fmt_time(ln, sizeof ln, (int)listened_ms);
    if(cur >= 0) {
        pl_info_t in;
        player_info(&in);
        fmt_time(tl, sizeof tl, player_len_ms());
        tech_line(tech, sizeof tech);
        snprintf(body, sizeof body,
                 "%s: %s\n%s: %.1f MB\n%s: %s\n%s: %s\n\n%s: %d  ·  %s: %s  ·  %s: %d",
                 tr(S_MUSIC_FILE), base_name(tracks[cur]),
                 tr(S_MUSIC_SIZE), (double)in.file_bytes / (1024.0 * 1024.0),
                 tr(S_MUSIC_FORMAT), tech,
                 tr(S_MUSIC_DURATION), player_len_ms() ? tl : "--:--",
                 tr(S_MUSIC_PLAYS), plays[cur], tr(S_MUSIC_LISTENED), ln,
                 tr(S_MUSIC_TRACKS), n_tracks);
    }
    else {
        snprintf(body, sizeof body, "%s: %d  ·  %s: %d  ·  %s: %s",
                 tr(S_MUSIC_TRACKS), n_tracks, tr(S_MUSIC_PLAYS), total_plays,
                 tr(S_MUSIC_LISTENED), ln);
    }
    dialog_open(BF_SYMBOL_INFO, lv_color_hex(C_MEDIA), tr(S_MUSIC_INFO), body, bt, 1, 0, -1, NULL, NULL);
}

/* ---------------- tela cheia ---------------- */

static void close_full(lv_event_t *e)
{
    (void)e;
    if(full) { lv_obj_delete(full); full = NULL; }
    memset(&vfull, 0, sizeof vfull);
}

static void open_full(lv_event_t *e)
{
    (void)e;
    const bf_palette_t *p = pal();
    if(full) return;
    full = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(full);
    lv_obj_set_size(full, LV_PCT(100), LV_PCT(100));
    /* cor sólida: o fundo é redesenhado atrás do espectro a cada quadro */
    lv_obj_set_style_bg_color(full, lv_color_mix(lv_color_hex(C_MEDIA), p->bg, 28), 0);
    lv_obj_set_style_bg_opa(full, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(full, BF_SP_XL, 0);
    lv_obj_set_style_pad_row(full, BF_SP_S, 0);
    lv_obj_set_flex_flow(full, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(full, LV_OBJ_FLAG_CLICKABLE);    /* não deixa o toque passar */
    lv_obj_remove_flag(full, LV_OBJ_FLAG_SCROLLABLE);

    vfull.title = bf_label(full, "", BF_FONT_L, p->text);
    bf_label_ellipsis(vfull.title, 752);
    vfull.artist = bf_label(full, "", BF_FONT_M, p->text_dim);
    bf_label_ellipsis(vfull.artist, 752);
    build_spectrum(full, &vfull, 752, 210);
    build_progress(full, &vfull);
    build_controls(full, false);
    ui_refresh_track();
}

/* ---------------- tela ---------------- */

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    char music[300];

    if(!watch) {
        watch = lv_timer_create(watch_cb, 200, NULL);
        last_tick = lv_tick_get();
        srand((unsigned)time(NULL));
    }
    if(player_state() == PL_STOPPED) {        /* parado: relê o cartão e a pasta */
        free_tracks();
        memset(plays, 0, sizeof plays);
        n_hist = 0;
        scan("/mnt/sd", 0);
        data_path(music, sizeof music, "music");
        scan(music, 0);
        qsort(tracks, n_tracks, sizeof tracks[0], cmp_track);
        cur = -1;
    }

    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, BF_SP_M, 0);
    lv_obj_set_style_pad_row(root, BF_SP_XS, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    if(!player_available() || !n_tracks) {
        lv_obj_t *box = bf_card(root);
        lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(box, BF_SP_XL, 0);
        bf_label(box, LV_SYMBOL_AUDIO "  ", BF_FONT_L, lv_color_hex(C_MEDIA));
        lv_obj_t *t = bf_label(box, !player_available() ? tr(S_MUSIC_NOPLAYER) : tr(S_MUSIC_EMPTY),
                               BF_FONT_M, p->text);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, 640);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 56, 0);
        return;
    }

    /* capa + texto + espectro */
    lv_obj_t *top = bf_plain(root);
    lv_obj_set_size(top, LV_PCT(100), 186);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, BF_SP_L, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cover = lv_obj_create(top);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, COVER_PX, COVER_PX);
    lv_obj_set_style_radius(cover, BF_RADIUS_L, 0);
    lv_obj_set_style_bg_color(cover, lv_color_lighten(lv_color_hex(C_MEDIA), 40), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_darken(lv_color_hex(C_MEDIA), 90), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    cover_note = bf_label(cover, LV_SYMBOL_AUDIO, BF_FONT_L, lv_color_white());
    lv_obj_center(cover_note);
    cover_img = lv_image_create(cover);
    lv_obj_set_size(cover_img, COVER_PX, COVER_PX);
    lv_obj_center(cover_img);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
    cover_for = -2;

    lv_obj_t *col = bf_plain(top);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    v.title = bf_label(col, "", BF_FONT_L, p->text);
    bf_label_ellipsis(v.title, 560);
    v.artist = bf_label(col, "", BF_FONT_M, p->text_dim);
    bf_label_ellipsis(v.artist, 560);
    v.tech = bf_label(col, "", BF_FONT_S, p->text_dim);
    bf_label_ellipsis(v.tech, 560);
    build_spectrum(col, &v, 560, 78);

    build_progress(root, &v);
    build_controls(root, true);
    ui_refresh_track();

    ui_timer = lv_timer_create(ui_tick, 100, NULL);
    ui_tick(ui_timer);
}

static void destroy(void)
{
    /* a música continua; só a interface some */
    if(ui_timer) { lv_timer_delete(ui_timer); ui_timer = NULL; }
    close_full(NULL);
    queue_panel = NULL;
    memset(&v, 0, sizeof v);
    root = cover_img = cover_note = NULL;
    cover_for = -2;
    dragging = false;
}

/* Arquivos: toca a pasta de `path` a partir dele e abre o Multimídia */
void music_play_file(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if(!slash) return;
    *slash = 0;
    if(!watch) {
        watch = lv_timer_create(watch_cb, 200, NULL);
        last_tick = lv_tick_get();
        srand((unsigned)time(NULL));
    }
    player_stop();
    free_tracks();
    memset(plays, 0, sizeof plays);
    n_hist = 0;
    scan(dir[0] ? dir : "/", 3);          /* só a própria pasta */
    qsort(tracks, n_tracks, sizeof tracks[0], cmp_track);
    int at = 0;
    for(int i = 0; i < n_tracks; i++)
        if(!strcmp(tracks[i], path)) { at = i; break; }
    play(at, false);
    nav_go(SCR_MUSIC, 0);
}

void scr_music_demo(int step)
{
    if(step == 0) play(0, false);
    else if(step == 1) open_queue(NULL);
    else if(step == 2) { queue_close(NULL); open_full(NULL); }
    else { close_full(NULL); player_stop(); cur = -1; ui_refresh_track(); }
}

const bf_screen_def_t scr_music_def = { create, destroy, S_APP_MULTIMEDIA, NULL };
