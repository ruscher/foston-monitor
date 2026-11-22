/* scr_music.c — Multimídia: tocador de MP3.
 *
 * Motor em core/player.c (libmpg123 + OSS). Esta tela mostra capa simbólica,
 * título/artista/álbum das tags ID3, formato real (MPEG, kbps, kHz, canais),
 * espectro calculado do som que está tocando, barra de progresso com tempo
 * decorrido/total e avanço por toque/arraste, controles (aleatório, anterior,
 * tocar/pausar, próxima, repetir: desligado/todas/uma), lista de reprodução,
 * informações e estatísticas da sessão, e um modo tela cheia.
 * A reprodução continua fora da tela: um timer global cuida do fim da faixa.
 * Procura *.mp3 em /mnt/sd (cartão SD, só leitura) e em <dados>/music.
 *
 * Fonte Bluetooth: a música de um celular conectado (A2DP) toca pelo bluealsa-aplay;
 * esta tela mostra o que o celular informa por AVRCP (core/btstat: título, artista,
 * álbum, duração, posição, estado), manda anterior/tocar/pausar/próxima ao celular e
 * desenha o espectro com as amostras já decodificadas (core/btaudio). Quando o celular
 * começa a tocar, a fonte troca sozinha e a música do cartão pausa e solta o /dev/dsp;
 * tocar uma música do cartão pede pausa ao celular. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../core/player.h"
#include "../core/cover.h"
#include "../core/btstat.h"
#include "../core/btaudio.h"
#include "../widgets/dialog.h"
#include "../widgets/volpop.h"
#include "../widgets/bottombar.h"
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

/* fonte Bluetooth (vive fora da tela) */
static bool src_bt;
static bt_info_t btc;              /* estado do agente, relido no máximo a cada 500 ms */
static uint32_t btc_at;
static bool bt_was_live;
static char bt_pending[16];        /* comando que o celular pode recusar (repeat, shuffle) */

static const bt_info_t *bt_now(void)
{
    uint32_t now = lv_tick_get();
    if(!btc_at || now - btc_at >= 500) { bt_read(&btc); btc_at = now ? now : 1; }
    return &btc;
}

/* há um celular conectado que pode mandar música */
static bool bt_phone(void)
{
    const bt_info_t *b = bt_now();
    if(!b->available || !b->powered) return false;
    if(b->player || b->streaming) return true;
    for(int i = 0; i < b->ndev; i++)
        if(b->dev[i].connected && !strcmp(b->dev[i].icon, "phone")) return true;
    return false;
}

static const char *bt_dev_name(void)
{
    const bt_info_t *b = bt_now();
    if(b->pl_dev[0]) return b->pl_dev;
    for(int i = 0; i < b->ndev; i++) if(b->dev[i].connected) return b->dev[i].name;
    return tr(S_APP_BLUETOOTH);
}

/* ---------------- objetos da tela (NULL fora dela) ---------------- */

typedef struct {
    lv_obj_t *title, *artist, *album, *tech, *elapsed, *total, *slider, *play_lbl;
    lv_obj_t *shuffle, *repeat, *badge, *queue, *src;
    lv_obj_t *spec;               /* um só objeto desenha as PL_BANDS barras */
    uint8_t shown[PL_BANDS];      /* nível desenhado (0..25): só redesenha se mudar */
    int bar_h;
    int shown_sec;                /* segundo mostrado em "decorrido" */
} view_t;

static view_t v, vfull;          /* tela normal e tela cheia */
static lv_obj_t *root, *full, *queue_panel, *cover_img, *cover_note;
static lv_obj_t *fcover_img, *fcover_note, *opts_panel;   /* tela cheia */
static lv_color_t cover_avg;     /* cor média da capa (fundo "cor da capa") */
static bool cover_has;
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
    if(src_bt || btaudio_live()) bt_cmd("pause");    /* o cartão assume a saída de som */
    src_bt = false;
    player_suspend(false);
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

/* troca de fonte: o Bluetooth faz a música do cartão pausar e soltar o /dev/dsp */
static void set_source(bool bt)
{
    if(bt == src_bt) return;
    src_bt = bt;
    if(bt) {
        if(player_state() == PL_PLAYING) player_toggle();
        if(player_state() != PL_STOPPED) player_suspend(true);
    }
    else player_suspend(false);
    btc_at = 0;
    ui_refresh_track();
}

/* fim da faixa e tempo ouvido; roda sempre, mesmo fora da tela */
static void watch_cb(lv_timer_t *t)
{
    (void)t;
    bool live = btaudio_live();
    if(live && !bt_was_live) set_source(true);     /* o celular começou a tocar */
    bt_was_live = live;
    if(src_bt && !live && !bt_phone()) set_source(false);   /* celular desconectado */
    uint32_t now = lv_tick_get();
    if(player_state() == PL_PLAYING) listened_ms += (uint32_t)(now - last_tick);
    last_tick = now;
    if(player_take_finished()) play(next_index(false), true);
}

/* ---------------- ações ---------------- */

static void toggle_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) {
        bt_cmd("%s", !strcmp(bt_now()->pl_status, "playing") ? "pause" : "play");
        btc_at = 0;
        return;
    }
    if(player_state() == PL_STOPPED) play(cur >= 0 ? cur : 0, false);
    else player_toggle();
    ui_refresh_track();
}

static void prev_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) { bt_cmd("prev"); btc_at = 0; return; }
    if(player_state() != PL_STOPPED && player_pos_ms() > 3000) { player_seek_ms(0); return; }
    if(n_hist) { play(hist[--n_hist], false); return; }
    play(cur > 0 ? cur - 1 : 0, false);
}

static void next_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) { bt_cmd("next"); btc_at = 0; return; }
    play(next_index(true), true);
}

static void src_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) set_source(false);
    else if(bt_phone()) set_source(true);
}

static void shuffle_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) {
        const bt_info_t *b = bt_now();
        bt_cmd("shuffle %s", strcmp(b->pl_shuffle, "off") ? "off" : "alltracks");
        snprintf(bt_pending, sizeof bt_pending, "shuffle");
        btc_at = 0;
        return;
    }
    settings()->music_shuffle = !settings()->music_shuffle;
    settings_save();
    ui_refresh_track();
}

static void repeat_click(lv_event_t *e)
{
    (void)e;
    if(src_bt) {                           /* desligado -> todas -> uma -> desligado */
        const char *r = bt_now()->pl_repeat;
        bt_cmd("repeat %s", !strcmp(r, "off") ? "alltracks" : !strcmp(r, "alltracks") ? "singletrack" : "off");
        snprintf(bt_pending, sizeof bt_pending, "repeat");
        btc_at = 0;
        return;
    }
    settings()->music_repeat = (settings()->music_repeat + 1) % 3;
    settings_save();
    ui_refresh_track();
}

static void slider_event(lv_event_t *e)
{
    if(src_bt) return;                    /* o celular não aceita pular por aqui */
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
 * LVGL no ARM (~30 % da CPU); assim é uma área por quadro.
 * Estilos (Ajustes da tela cheia): barras, espelho, LED, linha; cores: coral,
 * tema, arco-íris, fogo (amarelo->vermelho) e gelo (ciano->azul). */
static lv_color_t band_color(int i, int level)
{
    switch(settings()->music_color) {
    case 1: return pal()->accent;
    case 2: return lv_color_hsv_to_rgb((uint16_t)(i * 300 / PL_BANDS), 75, 95);
    case 3: return lv_color_mix(lv_color_hex(0xffd23f), lv_color_hex(0xe5302b), (uint8_t)(level * 255 / 100));
    case 4: return lv_color_mix(lv_color_hex(0x7df3ff), lv_color_hex(0x2a5bd7), (uint8_t)(level * 255 / 100));
    default: return lv_color_hex(C_MEDIA);
    }
}

static void spectrum_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    view_t *vw = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int gap = w > 600 ? 4 : 2, bw = (w - gap * (PL_BANDS - 1)) / PL_BANDS;
    int style = settings()->music_vis;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = bw > 10 ? 3 : 1;
    if(style == 3) {                                   /* linha com área suave embaixo */
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.width = 3;
        ld.round_start = ld.round_end = 1;
        int px = 0, py = 0;
        for(int i = 0; i < PL_BANDS; i++) {
            int lvl = vw->shown[i] * 4;
            int x = a.x1 + i * (bw + gap) + bw / 2;
            int y = a.y2 - 2 - lvl * (h - 6) / 100;
            d.bg_color = band_color(i, lvl);
            d.bg_opa = LV_OPA_20;
            lv_area_t r = { x - bw / 2, y, x + bw / 2, a.y2 };
            lv_draw_rect(layer, &d, &r);
            if(i) {
                ld.color = band_color(i, lvl);
                ld.p1.x = px; ld.p1.y = py; ld.p2.x = x; ld.p2.y = y;
                lv_draw_line(layer, &ld);
            }
            px = x;
            py = y;
        }
        return;
    }
    for(int i = 0; i < PL_BANDS; i++) {
        int lvl = vw->shown[i] * 4;
        int x1 = a.x1 + i * (bw + gap), x2 = x1 + bw - 1;
        if(style == 1) {                               /* espelho: do meio para cima e para baixo */
            int mid = (a.y1 + a.y2) / 2;
            int hh = 1 + lvl * (h / 2 - 1) / 100;
            d.bg_color = band_color(i, lvl);
            d.bg_opa = LV_OPA_COVER;
            lv_area_t up = { x1, mid - hh, x2, mid };
            lv_draw_rect(layer, &d, &up);
            d.bg_opa = LV_OPA_40;
            lv_area_t dn = { x1, mid + 1, x2, mid + hh };
            lv_draw_rect(layer, &d, &dn);
        }
        else if(style == 2) {                          /* LED: células de 6 px */
            int cell = h > 150 ? 8 : 6, cg = 2, n = (h + cg) / (cell + cg);
            int lit = (lvl * n + 50) / 100;
            d.radius = 1;
            for(int k = 0; k < n; k++) {
                int y2 = a.y2 - k * (cell + cg), y1 = y2 - cell + 1;
                d.bg_color = band_color(i, k * 100 / (n > 1 ? n - 1 : 1));
                d.bg_opa = k < lit ? LV_OPA_COVER : LV_OPA_10;
                lv_area_t r = { x1, y1, x2, y2 };
                lv_draw_rect(layer, &d, &r);
            }
        }
        else {                                         /* barras */
            int bh = 2 + lvl * (h - 2) / 100;
            d.bg_color = band_color(i, lvl);
            d.bg_opa = LV_OPA_COVER;
            lv_area_t r = { x1, a.y2 - bh + 1, x2, a.y2 };
            lv_draw_rect(layer, &d, &r);
        }
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
static void open_opts(lv_event_t *e);

/* volume: abre o controle flutuante logo acima do botão */
static void vol_click(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    volpop_open((a.x1 + a.x2) / 2, a.y1 - 310);
}

static void build_controls(lv_obj_t *parent, bool compact)
{
    lv_obj_t *row = bf_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, BF_SP_S, 0);
    view_t *vw = compact ? &v : &vfull;

    lv_obj_t *sb = icon_button(row, LV_SYMBOL_SHUFFLE, shuffle_click, 0, 60, 56);
    icon_button(row, LV_SYMBOL_PREV, prev_click, 0, 68, 56);
    lv_obj_t *pb = icon_button(row, LV_SYMBOL_PLAY, toggle_click, 1, 92, 60);
    lv_obj_add_flag(pb, LV_OBJ_FLAG_USER_2);        /* mantém a cor de destaque na tela cheia */
    vw->play_lbl = lv_obj_get_child(pb, 0);
    icon_button(row, LV_SYMBOL_NEXT, next_click, 0, 68, 56);
    lv_obj_t *rb = icon_button(row, LV_SYMBOL_LOOP, repeat_click, 0, 60, 56);
    vw->shuffle = sb;
    vw->repeat = rb;
    vw->badge = bf_label(rb, "1", BF_FONT_S, pal()->text);   /* repetir uma */
    lv_obj_add_flag(vw->badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(vw->badge, LV_ALIGN_TOP_RIGHT, 4, -6);
    lv_obj_t *gap = bf_plain(row);
    lv_obj_set_size(gap, BF_SP_S, 1);
    icon_button(row, LV_SYMBOL_VOLUME_MAX, vol_click, 0, 60, 56);
    vw->queue = icon_button(row, LV_SYMBOL_LIST, open_queue, 0, 60, 56);
    vw->src = icon_button(row, LV_SYMBOL_BLUETOOTH, src_click, 0, 60, 56);   /* fonte Bluetooth */
    if(compact) {
        icon_button(row, BF_SYMBOL_INFO, open_info, 0, 60, 56);
        icon_button(row, LV_SYMBOL_UP, open_full, 0, 60, 56);
    }
    else {
        icon_button(row, LV_SYMBOL_SETTINGS, open_opts, 0, 60, 56);
        icon_button(row, LV_SYMBOL_DOWN, close_full, 0, 60, 56);
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

static void enable_obj(lv_obj_t *o, bool on)
{
    if(!o) return;
    lv_obj_set_style_opa(o, LV_OPA_40, LV_STATE_DISABLED);   /* desativado à vista */
    if(on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static void refresh_view_bt(view_t *vw)
{
    const bt_info_t *b = bt_now();
    char buf[300];
    const char *dev = bt_dev_name();
    if(b->pl_title[0]) set_text(vw->title, b->pl_title);
    else set_text(vw->title, tr(S_MUSIC_BT_PLAYING));
    const char *ar = b->pl_artist[0] ? b->pl_artist : dev;
    if(vw->album) {                        /* tela cheia: álbum · gênero · faixa N de M */
        set_text(vw->artist, ar);
        char al[220] = "";
        size_t o = 0;
        if(b->pl_album[0]) o += (size_t)snprintf(al, sizeof al, "%s", b->pl_album);
        if(b->pl_genre[0] && o < sizeof al) o += (size_t)snprintf(al + o, sizeof al - o, "%s%s", o ? "  ·  " : "", b->pl_genre);
        if(b->pl_trackno && o < sizeof al) {
            if(b->pl_ntracks) snprintf(al + o, sizeof al - o, "%s%u / %u", o ? "  ·  " : "", b->pl_trackno, b->pl_ntracks);
            else snprintf(al + o, sizeof al - o, "%s%u", o ? "  ·  " : "", b->pl_trackno);
        }
        set_text(vw->album, al);
    }
    else if(b->pl_album[0] && strcmp(b->pl_album, b->pl_title)) {
        snprintf(buf, sizeof buf, "%.140s  —  %.95s", ar, b->pl_album);
        set_text(vw->artist, buf);
    }
    else set_text(vw->artist, ar);
    if(vw->tech) {
        int n = snprintf(buf, sizeof buf, tr(S_MUSIC_BT_TECH), dev);
        if(b->pl_trackno && n > 0 && n < (int)sizeof buf) {
            if(b->pl_ntracks) snprintf(buf + n, sizeof buf - (size_t)n, "  ·  %u / %u", b->pl_trackno, b->pl_ntracks);
            else snprintf(buf + n, sizeof buf - (size_t)n, "  ·  %u", b->pl_trackno);
        }
        set_text(vw->tech, buf);
    }
    set_text(vw->play_lbl, !strcmp(b->pl_status, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_state(vw->shuffle, LV_STATE_CHECKED, b->pl_shuffle[0] && strcmp(b->pl_shuffle, "off"));
    lv_obj_set_state(vw->repeat, LV_STATE_CHECKED, b->pl_repeat[0] && strcmp(b->pl_repeat, "off"));
    if(!strcmp(b->pl_repeat, "singletrack")) lv_obj_remove_flag(vw->badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(vw->badge, LV_OBJ_FLAG_HIDDEN);
    int mx = b->pl_dur > 1000 ? (int)(b->pl_dur / 1000) : 1;
    if(lv_slider_get_max_value(vw->slider) != mx) lv_slider_set_range(vw->slider, 0, mx);
    fmt_time(buf, sizeof buf, (int)b->pl_dur);
    set_text(vw->total, b->pl_dur ? buf : "--:--");
}

static void refresh_view(view_t *vw)
{
    if(!vw->title) return;
    /* botões que só valem para o cartão; o da fonte só com um celular conectado */
    const bt_info_t *bi = bt_now();
    enable_obj(vw->shuffle, !src_bt || bi->pl_shuffle[0]);    /* o celular informa: pode tentar */
    enable_obj(vw->repeat, !src_bt || bi->pl_repeat[0]);
    enable_obj(vw->queue, !src_bt && n_tracks > 0);
    if(vw->src) {
        if(bt_phone() || src_bt) lv_obj_remove_flag(vw->src, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(vw->src, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_state(vw->src, LV_STATE_CHECKED, src_bt);
    }
    if(src_bt) { refresh_view_bt(vw); return; }
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
        if(vw->album) {                        /* tela cheia: artista e álbum em linhas próprias */
            set_text(vw->artist, ar);
            char al[220] = "";
            size_t o = 0;
            if(t.album[0]) o += (size_t)snprintf(al + o, sizeof al - o, "%s", t.album);
            if(t.year[0] && o < sizeof al) o += (size_t)snprintf(al + o, sizeof al - o, "%s%s", o ? "  ·  " : "", t.year);
            if(t.genre[0] && o < sizeof al) o += (size_t)snprintf(al + o, sizeof al - o, "%s%s", o ? "  ·  " : "", t.genre);
            if(o < sizeof al) snprintf(al + o, sizeof al - o, "%s%d / %d", o ? "  ·  " : "", cur + 1, n_tracks);
            set_text(vw->album, al);
        }
        else set_text(vw->artist, buf);
    }
    else {
        set_text(vw->title, tr(S_MUSIC_IDLE));
        snprintf(buf, sizeof buf, "%d %s", n_tracks, tr(S_MUSIC_TRACKS));
        set_text(vw->artist, buf);
        if(vw->album) set_text(vw->album, "");
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

/* cor média da capa (RGB565 COVER_PX x COVER_PX), para o fundo "cor da capa" */
static void cover_average(const lv_image_dsc_t *img)
{
    const uint16_t *px = (const uint16_t *)img->data;
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for(uint32_t y = 0; y < img->header.h; y += 4)
        for(uint32_t x = 0; x < img->header.w; x += 4, n++) {
            uint16_t c = px[y * img->header.w + x];
            r += c >> 11; g += (c >> 5) & 63; b += c & 31;
        }
    if(n) cover_avg = lv_color_make((uint8_t)(r / n * 255 / 31), (uint8_t)(g / n * 255 / 63), (uint8_t)(b / n * 255 / 31));
}

static void show_cover(lv_obj_t *im, lv_obj_t *note, const lv_image_dsc_t *img)
{
    if(!im) return;
    if(img) {
        /* cover_decode reusa o mesmo buffer: tira do cache e redesenha */
        lv_image_cache_drop(img);
        lv_image_set_src(im, img);
        lv_obj_invalidate(im);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_full_bg(void);

static void refresh_cover(void)
{
    int want = src_bt ? -3 : cur;            /* -3: capa do Bluetooth (o celular não manda imagem) */
    if((!cover_img && !fcover_img) || cover_for == want) return;
    cover_for = want;
    const char *sym = src_bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_AUDIO;
    if(cover_note) lv_label_set_text(cover_note, sym);
    if(fcover_note) lv_label_set_text(fcover_note, sym);
    if(src_bt) {
        cover_has = false;
        show_cover(cover_img, cover_note, NULL);
        show_cover(fcover_img, fcover_note, NULL);
        if(full) apply_full_bg();
        return;
    }
    const uint8_t *d;
    size_t n;
    const char *mime;
    const lv_image_dsc_t *img = NULL;
    if(cur >= 0 && player_cover(&d, &n, &mime)) img = cover_decode(d, n);
    cover_has = img != NULL;
    if(img) cover_average(img);
    show_cover(cover_img, cover_note, img);
    show_cover(fcover_img, fcover_note, img);
    if(full) apply_full_bg();
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
    int pos = src_bt ? (int)bt_position_ms(bt_now()) : player_pos_ms();
    tick_view(&v, lv, pos);
    tick_view(&vfull, lv, pos);
    pl_state_t s = player_state();
    /* o que o celular informa mudou (faixa, estado) ou o celular conectou/saiu */
    static char bt_sig[160];
    char sig[160];
    const bt_info_t *b = bt_now();
    if(bt_pending[0] && !strcmp(b->res_op, bt_pending) && strcmp(b->res_state, "andamento")) {
        if(!strcmp(b->res_state, "erro")) bottombar_toast(tr(S_MUSIC_BT_NOSET));
        bt_pending[0] = 0;
    }
    snprintf(sig, sizeof sig, "%d%d%.15s|%.80s|%u|%.12s%.12s%u", src_bt, bt_phone(), b->pl_status, b->pl_title, b->pl_dur,
             b->pl_repeat, b->pl_shuffle, b->pl_trackno);
    if(s != last || strcmp(sig, bt_sig)) {
        last = s;
        snprintf(bt_sig, sizeof bt_sig, "%s", sig);
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
    queue_panel = bf_card(full ? full : root);
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

    /* a lista rola dentro do painel; o cabeçalho com o X fica fixo */
    lv_obj_t *lst = bf_plain(queue_panel);
    lv_obj_set_width(lst, LV_PCT(100));
    lv_obj_set_flex_grow(lst, 1);
    lv_obj_set_flex_flow(lst, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lst, 2, 0);
    lv_obj_set_style_pad_right(lst, 12, 0);          /* espaço para a barra */
    bf_scrollable(lst);
    lv_obj_t *first = NULL;
    for(int i = 0; i < n_tracks; i++) {
        lv_obj_t *it = lv_button_create(lst);
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
        bf_label_ellipsis(l, plays[i] ? 320 : 370);
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

/* Cores próprias da tela cheia: modo claro/escuro independente do tema */
typedef struct { lv_color_t bg, bg2, text, dim, btn; } fpal_t;

static fpal_t full_pal(void)
{
    fpal_t f;
    bool light = settings()->music_light;
    lv_color_t base;
    if(settings()->music_bg == 1 && cover_has) base = cover_avg;
    else base = settings()->music_color == 1 ? pal()->accent : lv_color_hex(C_MEDIA);
    if(light) {
        f.bg = lv_color_mix(base, lv_color_hex(0xf6f7fb), 40);
        f.bg2 = lv_color_mix(base, lv_color_hex(0xffffff), 90);
        f.text = lv_color_hex(0x1b1f27);
        f.dim = lv_color_hex(0x4d5566);
        f.btn = lv_color_hex(0xffffff);
    }
    else {
        f.bg = lv_color_mix(base, lv_color_hex(0x0d0f14), 80);
        f.bg2 = lv_color_mix(base, lv_color_hex(0x0d0f14), 25);
        f.text = lv_color_hex(0xf2f4f8);
        f.dim = lv_color_hex(0xaab2c0);
        f.btn = lv_color_hex(0x2a2f3a);
    }
    return f;
}

static void style_full_tree(lv_obj_t *o, const fpal_t *f)
{
    /* botões e rótulos da tela cheia seguem o modo dela, não o tema */
    uint32_t n = lv_obj_get_child_count(o);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if(lv_obj_check_type(c, &lv_button_class) && !lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            lv_obj_set_style_bg_color(c, f->btn, 0);
            lv_obj_set_style_text_color(c, f->text, 0);
        }
        style_full_tree(c, f);
    }
}

static void apply_full_bg(void)
{
    if(!full) return;
    fpal_t f = full_pal();
    const lv_image_dsc_t *wp = settings()->music_bg == 2 ? theme_wallpaper() : NULL;
    if(wp) {
        lv_obj_set_style_bg_image_src(full, wp, 0);
        lv_obj_set_style_bg_image_tiled(full, false, 0);
        lv_obj_set_style_bg_color(full, f.bg, 0);
        lv_obj_set_style_bg_grad_dir(full, LV_GRAD_DIR_NONE, 0);
    }
    else {
        lv_obj_set_style_bg_image_src(full, NULL, 0);
        lv_obj_set_style_bg_color(full, f.bg2, 0);
        lv_obj_set_style_bg_grad_color(full, f.bg, 0);
        lv_obj_set_style_bg_grad_dir(full, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_set_style_text_color(full, f.text, 0);
}

static void close_opts(lv_event_t *e)
{
    (void)e;
    if(opts_panel) { lv_obj_delete(opts_panel); opts_panel = NULL; }
}

static void close_full(lv_event_t *e)
{
    (void)e;
    volpop_close();
    close_opts(NULL);
    if(queue_panel && full && lv_obj_get_parent(queue_panel) == full) queue_panel = NULL;
    if(full) { lv_obj_delete(full); full = NULL; }
    memset(&vfull, 0, sizeof vfull);
    fcover_img = fcover_note = NULL;
}

/* opções da tela cheia: espectro, cores, fundo e modo */
enum { OPT_VIS, OPT_COLOR, OPT_BG, OPT_MODE };
static void opt_pick(lv_event_t *e)
{
    int v_ = (int)(intptr_t)lv_event_get_user_data(e);
    int what = v_ >> 8, val = v_ & 0xff;
    bf_settings_t *st = settings();
    if(what == OPT_VIS) st->music_vis = val;
    else if(what == OPT_COLOR) st->music_color = val;
    else if(what == OPT_BG) st->music_bg = val;
    else st->music_light = val != 0;
    settings_save();
    /* refaz a tela cheia com as opções novas e reabre o painel */
    close_full(NULL);
    open_full(NULL);
    open_opts(NULL);
}

static void opt_row(lv_obj_t *parent, const char *label, int what, int cur_v, const char *const *names, int n)
{
    const bf_palette_t *p = pal();
    bf_label(parent, label, BF_FONT_S, p->text_dim);
    lv_obj_t *row = bf_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, BF_SP_XS, 0);
    lv_obj_set_style_pad_row(row, BF_SP_XS, 0);
    for(int i = 0; i < n; i++) {
        lv_obj_t *b = bf_button(row, names[i], NULL, i == cur_v ? 1 : 0);
        lv_obj_set_height(b, 44);
        lv_obj_add_event_cb(b, opt_pick, LV_EVENT_CLICKED, (void *)(intptr_t)(what << 8 | i));
    }
}

static void open_opts(lv_event_t *e)
{
    (void)e;
    if(opts_panel) { close_opts(NULL); return; }
    if(!full) return;
    opts_panel = bf_card(full);
    lv_obj_add_flag(opts_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(opts_panel, 470, LV_PCT(100));
    lv_obj_align(opts_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(opts_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(opts_panel, BF_SP_M, 0);
    lv_obj_set_style_pad_row(opts_panel, BF_SP_XS, 0);
    lv_obj_set_style_shadow_width(opts_panel, 24, 0);
    lv_obj_set_style_shadow_opa(opts_panel, LV_OPA_50, 0);
    bf_scrollable(opts_panel);
    lv_obj_t *hdr = bf_plain(opts_panel);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    lv_obj_t *ht = bf_label(hdr, tr(S_MUSIC_OPTIONS), BF_FONT_M, pal()->text);
    lv_obj_align(ht, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *x = icon_button(hdr, LV_SYMBOL_CLOSE, close_opts, 0, 48, 40);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    const char *vis[] = { tr(S_MUSIC_VIS_BARS), tr(S_MUSIC_VIS_MIRROR), tr(S_MUSIC_VIS_LED), tr(S_MUSIC_VIS_LINE) };
    const char *col[] = { tr(S_MUSIC_COL_CORAL), tr(S_MUSIC_COL_THEME), tr(S_MUSIC_COL_RAINBOW),
                          tr(S_MUSIC_COL_FIRE), tr(S_MUSIC_COL_ICE) };
    const char *bg[] = { tr(S_MUSIC_BG_COLOR), tr(S_MUSIC_BG_COVER), tr(S_MUSIC_BG_WALL) };
    const char *md[] = { tr(S_AP_DARK), tr(S_AP_LIGHT) };
    opt_row(opts_panel, tr(S_MUSIC_OPT_VIS), OPT_VIS, settings()->music_vis, vis, 4);
    opt_row(opts_panel, tr(S_MUSIC_OPT_COLOR), OPT_COLOR, settings()->music_color, col, 5);
    opt_row(opts_panel, tr(S_MUSIC_OPT_BG), OPT_BG, settings()->music_bg, bg, 3);
    opt_row(opts_panel, tr(S_MUSIC_OPT_MODE), OPT_MODE, settings()->music_light ? 1 : 0, md, 2);
}

static void open_full(lv_event_t *e)
{
    (void)e;
    if(full) return;
    queue_close(NULL);
    fpal_t f = full_pal();
    full = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(full);
    lv_obj_set_size(full, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(full, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(full, BF_SP_XL, 0);
    lv_obj_set_style_pad_ver(full, BF_SP_L, 0);
    lv_obj_set_style_pad_row(full, BF_SP_S, 0);
    lv_obj_set_flex_flow(full, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(full, LV_OBJ_FLAG_CLICKABLE);    /* não deixa o toque passar */
    lv_obj_remove_flag(full, LV_OBJ_FLAG_SCROLLABLE);

    /* capa grande + informações */
    lv_obj_t *top = bf_plain(full);
    lv_obj_set_size(top, LV_PCT(100), COVER_PX);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, BF_SP_XL, 0);
    lv_obj_t *cover = lv_obj_create(top);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, COVER_PX, COVER_PX);
    lv_obj_set_style_radius(cover, BF_RADIUS_L, 0);
    lv_obj_set_style_bg_color(cover, lv_color_lighten(lv_color_hex(C_MEDIA), 40), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_darken(lv_color_hex(C_MEDIA), 90), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_shadow_width(cover, 30, 0);
    lv_obj_set_style_shadow_opa(cover, LV_OPA_40, 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    fcover_note = bf_label(cover, LV_SYMBOL_AUDIO, BF_FONT_L, lv_color_white());
    lv_obj_center(fcover_note);
    fcover_img = lv_image_create(cover);
    lv_obj_set_size(fcover_img, COVER_PX, COVER_PX);
    lv_obj_center(fcover_img);
    lv_obj_add_flag(fcover_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *col = bf_plain(top);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 4, 0);
    bf_label(col, tr(S_HOME_NOW_PLAYING), BF_FONT_S, f.dim);
    vfull.title = bf_label(col, "", BF_FONT_L, f.text);
    bf_label_ellipsis(vfull.title, 540);
    vfull.artist = bf_label(col, "", BF_FONT_M, f.text);
    bf_label_ellipsis(vfull.artist, 540);
    vfull.album = bf_label(col, "", BF_FONT_S, f.dim);
    bf_label_ellipsis(vfull.album, 540);
    vfull.tech = bf_label(col, "", BF_FONT_S, f.dim);
    bf_label_ellipsis(vfull.tech, 540);

    build_spectrum(full, &vfull, 752, 104);
    build_progress(full, &vfull);
    lv_obj_set_style_text_color(vfull.elapsed, f.dim, 0);
    lv_obj_set_style_text_color(vfull.total, f.dim, 0);
    build_controls(full, false);
    style_full_tree(full, &f);
    cover_for = -2;                 /* força a capa na tela cheia */
    apply_full_bg();
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

    if(btaudio_live() || (src_bt && bt_phone())) src_bt = true;
    else if(src_bt) set_source(false);
    if(!player_available() || (!n_tracks && !src_bt && !bt_phone())) {
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

/* Controle a partir de outras telas (Painel da Início) */
bool music_has_queue(void) { return n_tracks > 0 || src_bt; }

bool music_now(char *title, size_t tn, char *sub, size_t sn, int *idx)
{
    if(src_bt) {
        const bt_info_t *b = bt_now();
        snprintf(title, tn, "%s", b->pl_title[0] ? b->pl_title : tr(S_MUSIC_BT_PLAYING));
        snprintf(sub, sn, "%s", b->pl_artist[0] ? b->pl_artist : bt_dev_name());
        if(idx) *idx = -3;
        return true;
    }
    if(cur < 0 || cur >= n_tracks) return false;
    pl_tags_t t;
    char name[200];
    player_tags(&t);
    file_title(name, sizeof name, tracks[cur]);
    snprintf(title, tn, "%s", t.title[0] ? t.title : name);
    if(t.artist[0] && t.album[0]) snprintf(sub, sn, "%s  —  %s", t.artist, t.album);
    else if(t.artist[0] || t.album[0]) snprintf(sub, sn, "%s", t.artist[0] ? t.artist : t.album);
    else snprintf(sub, sn, "%s", tr(S_MUSIC_UNKNOWN));
    if(idx) *idx = cur;
    return true;
}

void music_command(int cmd)
{
    if(!n_tracks && !src_bt) return;
    if(cmd == MUSIC_PREV) prev_click(NULL);
    else if(cmd == MUSIC_NEXT) next_click(NULL);
    else toggle_click(NULL);
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
    if(step == 4) { btc_at = 0; set_source(true); ui_refresh_track(); return; }   /* fonte Bluetooth */
    if(step == 0) play(0, false);
    else if(step == 1) open_queue(NULL);
    else if(step == 2) { queue_close(NULL); open_full(NULL); }
    else if(step == 4) open_opts(NULL);
    else if(step >= 5 && step <= 7) {        /* estilos para as capturas */
        static const int vis[] = { 2, 1, 3 }, col[] = { 3, 2, 4 }, bg[] = { 0, 1, 0 }, light[] = { 0, 0, 1 };
        bf_settings_t *st = settings();
        st->music_vis = vis[step - 5];
        st->music_color = col[step - 5];
        st->music_bg = bg[step - 5];
        st->music_light = light[step - 5];
        close_full(NULL);
        open_full(NULL);
    }
    else if(step == 8) {                     /* volta ao padrão */
        bf_settings_t *st = settings();
        st->music_vis = st->music_color = st->music_bg = 0;
        st->music_light = false;
        close_full(NULL);
    }
    else { close_full(NULL); player_stop(); cur = -1; ui_refresh_track(); }
}

const bf_screen_def_t scr_music_def = { create, destroy, S_APP_MULTIMEDIA, NULL };
