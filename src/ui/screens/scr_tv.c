/* scr_tv.c — TV: TV digital aberta (ISDB-T 1seg) pelo sintonizador Siano do FOSTON, gravações e
 * a entrada AV.
 *
 * Quem mostra a imagem e toca o som é o bftvd (src/tv/bftvd.c), no cartão (BigFoston/tv/, com o
 * firmware do sintonizador extraído do próprio aparelho): ele desenha o vídeo 320x180 ampliado 2x
 * direto no framebuffer, num retângulo de 640x360 à esquerda que esta tela deixa vazio (preto) e
 * nunca redesenha enquanto o vídeo toca; os controles ficam na coluna da direita. Tudo que
 * cobriria o vídeo (lista de canais, gravações, busca) para o vídeo antes.
 *
 * Estado do bftvd em /run/bigfoston-tv (tocando, sem_sinal, busca, ...); canais da busca em
 * BigFoston/tv/canais.tsv (canal, MHz, nome, SNR, RSSI); gravações em BigFoston/tv/gravacoes/.
 * Detalhes: docs/tv/00-visao-geral.md. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "../core/theme.h"
#include "../core/settings.h"
#include "../widgets/glass.h"
#include "../widgets/dialog.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VID_W 640
#define VID_H 360
#define COL_W (BF_SCREEN_W - VID_W)
#define MAXCH 64
#define MAXREC 40

typedef struct { int ch; char mhz[16], name[64]; int snr; } tv_ch_t;
enum { SRC_TV, SRC_REC, SRC_AV };
enum { V_WATCH, V_CHANNELS, V_RECS, V_AV, V_SCAN, V_SETUP };

static lv_obj_t *root, *video, *panel, *col, *src_btn[3];
static lv_obj_t *ch_name, *ch_sub, *ch_bars, *st_lbl, *rec_btn, *rec_lbl, *chdn, *chup, *list_btn;
static lv_obj_t *strip_name, *strip_st;   /* Foston Glass: faixa sob o vídeo (há 72 px a mais) */
static lv_obj_t *scan_bar, *scan_lbl;
static lv_timer_t *tick;
static tv_ch_t chs[MAXCH];
static int nch, cur_ch = -1;
static int src = SRC_TV, view = V_WATCH;
static pid_t tv_pid;
static bool want_start;                   /* bftvd parando: começa de novo quando sair */
static char play_file[420];               /* tocando uma gravação */
static char recs[MAXREC][96];
static int nrec;
static int vid_x, vid_y;

/* ------------------------------------------------------------ caminhos */

static const char *tv_dir(void)
{
    const char *e = getenv("BIGFOSTON_TV_DIR");
    return e && *e ? e : "/mnt/sd/BigFoston/tv";
}

static const char *state_path(void)
{
    const char *e = getenv("BIGFOSTON_TV_STATUS");
    return e && *e ? e : "/run/bigfoston-tv";
}

static void tv_file(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", tv_dir(), name); }

static bool have_player(void)
{
    char p[300], f[300];
    tv_file(p, sizeof p, "bftvd");
    tv_file(f, sizeof f, "isdbt_nova_12mhz_b0.inp");
    return access(p, X_OK) == 0 && access(f, R_OK) == 0;
}

/* ------------------------------------------------------------ estado do bftvd */

typedef struct { char state[24]; int locked, snr, rssi, scan_ch, scan_found, rec_kb; double fps, pos, dur; bool rec; } tv_state_t;

static void read_state(tv_state_t *s)
{
    memset(s, 0, sizeof *s);
    s->snr = -99;
    FILE *f = fopen(state_path(), "r");
    if(!f) return;
    char l[320];
    while(fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        char *v = strchr(l, '=');
        if(!v) continue;
        *v++ = 0;
        if(!strcmp(l, "state")) snprintf(s->state, sizeof s->state, "%s", v);
        else if(!strcmp(l, "locked")) s->locked = atoi(v);
        else if(!strcmp(l, "snr")) s->snr = atoi(v);
        else if(!strcmp(l, "rssi")) s->rssi = atoi(v);
        else if(!strcmp(l, "fps")) s->fps = atof(v);
        else if(!strcmp(l, "scan_ch")) s->scan_ch = atoi(v);
        else if(!strcmp(l, "scan_found")) s->scan_found = atoi(v);
        else if(!strcmp(l, "rec_kb")) { s->rec_kb = atoi(v); s->rec = true; }
        else if(!strcmp(l, "pos")) s->pos = atof(v);
        else if(!strcmp(l, "dur")) s->dur = atof(v);
    }
    fclose(f);
}

/* ------------------------------------------------------------ canais e gravações */

static void load_channels(void)
{
    nch = 0;
    char p[300];
    tv_file(p, sizeof p, "canais.tsv");
    FILE *f = fopen(p, "r");
    if(!f) return;
    char l[256];
    while(nch < MAXCH && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        tv_ch_t *c = &chs[nch];
        memset(c, 0, sizeof *c);
        char *t[5] = { l, NULL, NULL, NULL, NULL };
        for(int i = 1; i < 5; i++) { t[i] = t[i - 1] ? strchr(t[i - 1], '\t') : NULL; if(t[i]) *t[i]++ = 0; }
        c->ch = atoi(t[0]);
        if(c->ch < 7 || c->ch > 69 || !t[1]) continue;
        snprintf(c->mhz, sizeof c->mhz, "%s", t[1]);
        snprintf(c->name, sizeof c->name, "%s", t[2] && *t[2] ? t[2] : "");
        c->snr = t[3] ? atoi(t[3]) : 0;
        nch++;
    }
    fclose(f);
}

static void pref_path(char *out, size_t n) { data_path(out, n, "tv-canal"); }

static void save_last(void)
{
    if(cur_ch < 0 || cur_ch >= nch) return;
    char p[300];
    pref_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if(f) { fprintf(f, "%d\n", chs[cur_ch].ch); fclose(f); }
}

static void load_last(void)
{
    cur_ch = nch ? 0 : -1;
    char p[300];
    pref_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    int ch = 0;
    if(f) { if(fscanf(f, "%d", &ch) != 1) ch = 0; fclose(f); }
    for(int i = 0; i < nch; i++) if(chs[i].ch == ch) cur_ch = i;
}

static int rec_cmp(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static void load_recs(void)
{
    nrec = 0;
    char d[300];
    tv_file(d, sizeof d, "gravacoes");
    DIR *dir = opendir(d);
    if(!dir) return;
    struct dirent *e;
    while((e = readdir(dir)) && nrec < MAXREC) {
        size_t l = strlen(e->d_name);
        if(l > 3 && !strcmp(e->d_name + l - 3, ".ts") && l < sizeof recs[0]) snprintf(recs[nrec++], sizeof recs[0], "%s", e->d_name);
    }
    closedir(dir);
    qsort(recs, (size_t)nrec, sizeof recs[0], rec_cmp);          /* mais novas primeiro */
}

static const char *ch_label(int i, char *b, size_t n)
{
    if(i < 0 || i >= nch) { snprintf(b, n, "%s", tr(S_TV_NO_CHANNEL)); return b; }
    if(chs[i].name[0]) snprintf(b, n, "%s", chs[i].name);
    else snprintf(b, n, tr(S_TV_CHANNEL_N), chs[i].ch);
    return b;
}

/* ------------------------------------------------------------ processo bftvd */

static void tv_spawn(char *const argv[])
{
    pid_t p = fork();
    if(p == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);               /* nada do player no log do shell */
        if(nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(argv[0], argv);
        _exit(127);
    }
    if(p > 0) tv_pid = p;
}

/* termina o bftvd (sem esperar): o tick recolhe e, com want_start, começa de novo */
static void tv_stop(void)
{
    if(tv_pid > 0) kill(tv_pid, SIGTERM);
}

static bool tv_reap(void)
{
    if(tv_pid <= 0) return true;
    int st;
    pid_t r = waitpid(tv_pid, &st, WNOHANG);
    if(r == tv_pid || (r < 0 && errno == ECHILD)) { tv_pid = 0; return true; }
    return false;
}

static void tv_start_now(void)
{
    char bin[300], fw[300], rect[40], dir[300];
    tv_file(bin, sizeof bin, "bftvd");
    tv_file(fw, sizeof fw, "isdbt_nova_12mhz_b0.inp");
    tv_file(dir, sizeof dir, "gravacoes");
    snprintf(rect, sizeof rect, "%d,%d,2", vid_x, vid_y);
    if(play_file[0]) {
        char *av[] = { bin, "-r", rect, "-s", (char *)state_path(), "-p", play_file, NULL };
        tv_spawn(av);
    } else if(cur_ch >= 0 && cur_ch < nch) {
        mkdir(dir, 0755);
        char *av[] = { bin, "-f", fw, "-r", rect, "-s", (char *)state_path(), "-W", dir, chs[cur_ch].mhz, NULL };
        tv_spawn(av);
    }
}

/* (re)começa a tocar: se o bftvd ainda está rodando, para e o tick começa quando ele sair */
static void tv_play(void)
{
    if(!have_player() || getenv("BIGFOSTON_TV_STATUS")) return;   /* simulador: sem bftvd */
    want_start = true;
    if(tv_pid > 0) tv_stop();
    else { want_start = false; tv_start_now(); }
}

static void tv_halt(void) { want_start = false; tv_stop(); }

/* ------------------------------------------------------------ painel sobre o vídeo */

static void show_view(int v);
static void update_side(void);

static lv_obj_t *panel_card(void)
{
    lv_obj_clean(panel);
    lv_obj_t *c = glass_card(panel, 12, 12, VID_W - 24, VID_H - 24, 20);
    lv_obj_set_style_bg_opa(c, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(c, BF_SP_L, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    return c;
}

static lv_obj_t *gb(lv_obj_t *p, const char *sym, const char *txt, uint32_t top, uint32_t bottom)
{
    lv_obj_t *b = bf_button(p, sym, txt, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, lv_color_hex(GL_WHITE), 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_min_height(b, 0, 0);
    return b;
}

static lv_obj_t *head(lv_obj_t *c, const char *sym, const char *title)
{
    lv_obj_t *h = bf_plain(c);
    lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, BF_SP_M, 0);
    glass_tile(h, 40, 12, GL_LAVENDER, GL_LAVBLUE, NULL, sym, BF_FONT_M);
    lv_obj_t *t = glass_label(h, title, BF_FONT_L, GL_WHITE);
    lv_obj_set_flex_grow(t, 1);
    return h;
}

static lv_obj_t *note(lv_obj_t *c, const char *txt)
{
    lv_obj_t *l = glass_label(c, txt, BF_FONT_M, GL_TEXT2);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void on_scan(lv_event_t *e);
static void on_back_watch(lv_event_t *e) { (void)e; show_view(V_WATCH); }

static lv_obj_t *list_box(lv_obj_t *c)
{
    lv_obj_t *l = bf_plain(c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 6, 0);
    bf_scrollable(l);
    return l;
}

static lv_obj_t *row(lv_obj_t *l, const char *sym, uint32_t top, uint32_t bottom, const char *title, const char *sub,
                     bool marked)
{
    lv_obj_t *r = glass_card(l, 0, 0, LV_PCT(100), LV_SIZE_CONTENT, 14);
    lv_obj_set_style_pad_all(r, 8, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, BF_SP_M, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(r, LV_OPA_90, LV_STATE_PRESSED);
    if(marked) { lv_obj_set_style_border_color(r, lv_color_hex(GL_ACTIVE), 0); lv_obj_set_style_border_opa(r, LV_OPA_COVER, 0); }
    glass_tile(r, 40, 12, top, bottom, NULL, sym, BF_FONT_M);
    lv_obj_t *cc = bf_plain(r);
    lv_obj_set_height(cc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(cc, 1);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = glass_label(cc, title, BF_FONT_M, GL_WHITE);
    bf_label_ellipsis(t, 440);
    if(sub && *sub) glass_label(cc, sub, BF_FONT_S, GL_TEXT2);
    return r;
}

static void on_pick_ch(lv_event_t *e)
{
    cur_ch = (int)(intptr_t)lv_event_get_user_data(e);
    play_file[0] = 0;
    save_last();
    src = SRC_TV;
    show_view(V_WATCH);
}

static void build_channels(void)
{
    lv_obj_t *c = panel_card();
    lv_obj_t *h = head(c, LV_SYMBOL_LIST, tr(S_TV_CHANNELS));
    lv_obj_t *sc = gb(h, LV_SYMBOL_REFRESH, tr(S_TV_SCAN), GL_LIGHT, GL_BLUE);
    lv_obj_set_height(sc, 44);
    lv_obj_add_event_cb(sc, on_scan, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bk = gb(h, LV_SYMBOL_CLOSE, NULL, GL_LIGHT, GL_BLUE);
    lv_obj_set_size(bk, 52, 44);
    lv_obj_add_event_cb(bk, on_back_watch, LV_EVENT_CLICKED, NULL);
    if(!nch) { note(c, tr(S_TV_NO_CHANNELS)); return; }
    lv_obj_t *l = list_box(c);
    for(int i = 0; i < nch; i++) {
        char t[96], s[96];
        ch_label(i, t, sizeof t);
        snprintf(s, sizeof s, tr(S_TV_CH_SUB), chs[i].ch, chs[i].mhz);
        lv_obj_t *r = row(l, LV_SYMBOL_VIDEO, GL_SKY, 0x3E86C9, t, s, i == cur_ch);
        lv_obj_add_event_cb(r, on_pick_ch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void on_pick_rec(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= nrec) return;
    char d[300];
    tv_file(d, sizeof d, "gravacoes");
    snprintf(play_file, sizeof play_file, "%s/%.95s", d, recs[i]);
    src = SRC_REC;
    show_view(V_WATCH);
}

/* "tv-20261122-203015-c15.ts" -> "22/11/2026 20:30 · canal 15" */
static void rec_title(const char *f, char *out, size_t n)
{
    int Y, M, D, h, m, s, ch = 0;
    if(sscanf(f, "tv-%4d%2d%2d-%2d%2d%2d-c%d", &Y, &M, &D, &h, &m, &s, &ch) >= 6) {
        const char *name = NULL;
        for(int i = 0; i < nch; i++) if(chs[i].ch == ch && chs[i].name[0]) name = chs[i].name;
        char who[80];
        if(name) snprintf(who, sizeof who, "%s", name);
        else snprintf(who, sizeof who, tr(S_TV_CHANNEL_N), ch);
        snprintf(out, n, "%02d/%02d/%04d %02d:%02d \xC2\xB7 %s", D, M, Y, h, m, who);
    } else snprintf(out, n, "%s", f);
}

static void build_recs(void)
{
    load_recs();
    lv_obj_t *c = panel_card();
    lv_obj_t *h = head(c, LV_SYMBOL_DIRECTORY, tr(S_TV_RECORDINGS));
    lv_obj_t *bk = gb(h, LV_SYMBOL_CLOSE, NULL, GL_LIGHT, GL_BLUE);
    lv_obj_set_size(bk, 52, 44);
    lv_obj_add_event_cb(bk, on_back_watch, LV_EVENT_CLICKED, NULL);
    if(!nrec) { note(c, tr(S_TV_NO_RECS)); return; }
    lv_obj_t *l = list_box(c);
    char d[300];
    tv_file(d, sizeof d, "gravacoes");
    for(int i = 0; i < nrec; i++) {
        char t[160], s[64] = "", p[420];
        rec_title(recs[i], t, sizeof t);
        snprintf(p, sizeof p, "%s/%.95s", d, recs[i]);
        struct stat stt;
        if(!stat(p, &stt)) {
            long mins = (long)(stt.st_size / (364000 / 8) / 60);    /* ~364 kbit/s */
            snprintf(s, sizeof s, "%.1f MB \xC2\xB7 ~%ld min", stt.st_size / 1048576.0, mins < 1 ? 1 : mins);
        }
        lv_obj_t *r = row(l, LV_SYMBOL_PLAY, 0xFF8E8E, 0xD04545, t, s, false);
        lv_obj_add_event_cb(r, on_pick_rec, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void build_av(void)
{
    lv_obj_t *c = panel_card();
    head(c, LV_SYMBOL_SHUFFLE, tr(S_TV_AV));
    note(c, tr(S_TV_AV_NOTE));
}

static void build_setup(void)
{
    lv_obj_t *c = panel_card();
    head(c, BF_SYMBOL_INFO, tr(S_TV_SETUP));
    note(c, tr(S_TV_SETUP_NOTE));
}

static void on_scan_cancel(lv_event_t *e) { (void)e; tv_halt(); }

static void build_scan(void)
{
    lv_obj_t *c = panel_card();
    head(c, LV_SYMBOL_REFRESH, tr(S_TV_SCANNING));
    note(c, tr(S_TV_SCAN_NOTE));
    scan_bar = lv_bar_create(c);
    lv_obj_set_size(scan_bar, LV_PCT(100), 16);
    lv_bar_set_range(scan_bar, 14, 69);
    lv_obj_set_style_bg_color(scan_bar, lv_color_hex(GL_DEEP), 0);
    lv_obj_set_style_bg_opa(scan_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scan_bar, lv_color_hex(GL_ACTIVE), LV_PART_INDICATOR);
    scan_lbl = glass_label(c, "", BF_FONT_M, GL_WHITE);
    lv_obj_t *cn = gb(c, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0xFF8E8E, 0xD04545);
    lv_obj_set_size(cn, 200, 52);
    lv_obj_add_event_cb(cn, on_scan_cancel, LV_EVENT_CLICKED, NULL);
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    if(!have_player()) return;
    want_start = false;
    play_file[0] = 0;
    view = V_SCAN;
    build_scan();
    update_side();
    char bin[300], fw[300], out[300];
    tv_file(bin, sizeof bin, "bftvd");
    tv_file(fw, sizeof fw, "isdbt_nova_12mhz_b0.inp");
    tv_file(out, sizeof out, "canais.tsv");
    /* o vídeo para antes da busca (o sintonizador é um só) */
    if(tv_pid > 0) { tv_stop(); for(int i = 0; i < 40 && !tv_reap(); i++) usleep(50000); }
    if(tv_pid > 0) return;
    char *av[] = { bin, "-f", fw, "-s", (char *)state_path(), "-S", out, NULL };
    tv_spawn(av);
}

static void show_view(int v)
{
    view = v;
    scan_bar = scan_lbl = NULL;
    if(v == V_WATCH) {
        lv_obj_clean(panel);
        if(!have_player()) { view = V_SETUP; build_setup(); }
        else if(src == SRC_AV) { view = V_AV; build_av(); }
        else if(src == SRC_TV && cur_ch < 0) { view = V_CHANNELS; build_channels(); }
        else {
            /* a área fica preta: o bftvd desenha por cima */
            lv_obj_invalidate(video);
            lv_refr_now(NULL);
            tv_play();
        }
    } else {
        tv_halt();
        if(v == V_CHANNELS) build_channels();
        else if(v == V_RECS) build_recs();
        else if(v == V_AV) build_av();
        else if(v == V_SETUP) build_setup();
    }
    update_side();
}

/* ------------------------------------------------------------ coluna da direita */

static void on_src(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    src = s;
    if(s == SRC_TV) { play_file[0] = 0; show_view(V_WATCH); }
    else if(s == SRC_REC) show_view(V_RECS);
    else show_view(V_AV);
}

static void zap(int d)
{
    if(!nch) return;
    cur_ch = ((cur_ch < 0 ? 0 : cur_ch) + d + nch) % nch;
    play_file[0] = 0;
    src = SRC_TV;
    save_last();
    show_view(V_WATCH);
}

static void on_chup(lv_event_t *e) { (void)e; zap(1); }
static void on_chdn(lv_event_t *e) { (void)e; zap(-1); }
static void on_list(lv_event_t *e) { (void)e; show_view(src == SRC_REC ? V_RECS : V_CHANNELS); }

static void on_rec(lv_event_t *e)
{
    (void)e;
    if(play_file[0] || view != V_WATCH || tv_pid <= 0) return;
    kill(tv_pid, SIGUSR1);                                   /* começa/para de gravar sem parar a imagem */
}

static lv_obj_t *side_btn(lv_obj_t *p, const char *sym, const char *txt, int w, uint32_t top, uint32_t bottom)
{
    lv_obj_t *b = gb(p, sym, txt, top, bottom);
    lv_obj_set_size(b, w, 52);
    return b;
}

static void build_column(void)
{
    col = bf_plain(root);
    lv_obj_set_pos(col, VID_W, 0);
    lv_obj_set_size(col, COL_W, LV_PCT(100));
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    /* fonte: TV, gravações, AV */
    lv_obj_t *seg = glass_card(col, 0, 0, LV_PCT(100), LV_SIZE_CONTENT, 16);
    lv_obj_set_style_pad_all(seg, 3, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const sy[3] = { LV_SYMBOL_VIDEO, LV_SYMBOL_DIRECTORY, LV_SYMBOL_SHUFFLE };
    for(int i = 0; i < 3; i++) {
        src_btn[i] = gb(seg, sy[i], NULL, GL_BLUE, GL_DEEP);
        lv_obj_set_size(src_btn[i], 44, 40);
        lv_obj_set_style_bg_opa(src_btn[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(src_btn[i], 0, 0);
        lv_obj_set_style_bg_color(src_btn[i], lv_color_hex(GL_SKY), LV_STATE_CHECKED);
        lv_obj_set_style_bg_grad_color(src_btn[i], lv_color_hex(0x3E86C9), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(src_btn[i], LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_add_event_cb(src_btn[i], on_src, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    /* canal: nome, número, sinal */
    lv_obj_t *info = glass_card(col, 0, 0, LV_PCT(100), LV_SIZE_CONTENT, 16);
    lv_obj_set_style_pad_all(info, 8, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 2, 0);
    ch_name = glass_label(info, "", BF_FONT_M, GL_WHITE);
    lv_obj_set_width(ch_name, LV_PCT(100));
    lv_label_set_long_mode(ch_name, LV_LABEL_LONG_DOT);
    lv_obj_t *r = bf_plain(info);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    ch_sub = glass_label(r, "", BF_FONT_S, GL_TEXT2);
    ch_bars = glass_bars(r);
    st_lbl = glass_label(info, "", BF_FONT_S, GL_TEXT2);
    lv_obj_set_width(st_lbl, LV_PCT(100));
    lv_label_set_long_mode(st_lbl, LV_LABEL_LONG_DOT);
    /* canal - / +  */
    lv_obj_t *zr = bf_plain(col);
    lv_obj_set_size(zr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(zr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int half = (COL_W - 12 - 6) / 2;
    chdn = side_btn(zr, LV_SYMBOL_DOWN, NULL, half, GL_LIGHT, GL_BLUE);
    chup = side_btn(zr, LV_SYMBOL_UP, NULL, half, GL_LIGHT, GL_BLUE);
    lv_obj_add_event_cb(chdn, on_chdn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(chup, on_chup, LV_EVENT_CLICKED, NULL);
    list_btn = side_btn(col, LV_SYMBOL_LIST, tr(S_TV_CHANNELS), LV_PCT(100), GL_LAVENDER, GL_LAVBLUE);
    lv_obj_add_event_cb(list_btn, on_list, LV_EVENT_CLICKED, NULL);
    rec_btn = side_btn(col, NULL, NULL, LV_PCT(100), 0xFF8E8E, 0xD04545);
    lv_obj_t *dot = lv_obj_create(rec_btn);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    rec_lbl = glass_label(rec_btn, tr(S_TV_REC), BF_FONT_M, GL_WHITE);
    lv_obj_add_event_cb(rec_btn, on_rec, LV_EVENT_CLICKED, NULL);
}

static void set_text(lv_obj_t *l, const char *t) { if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }

static void enable(lv_obj_t *b, bool on)
{
    if(on) lv_obj_remove_state(b, LV_STATE_DISABLED);
    else lv_obj_add_state(b, LV_STATE_DISABLED);
    lv_obj_set_style_opa(b, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void update_side(void)
{
    if(!col) return;
    for(int i = 0; i < 3; i++) {
        if(i == src) lv_obj_add_state(src_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(src_btn[i], LV_STATE_CHECKED);
    }
    tv_state_t s;
    read_state(&s);
    char b[128], b2[96];
    if(play_file[0]) {
        const char *f = strrchr(play_file, '/');
        rec_title(f ? f + 1 : play_file, b, sizeof b);
        set_text(ch_name, tr(S_TV_RECORDING_PLAY));
        set_text(ch_sub, b);
        glass_bars_set(ch_bars, -1);
    } else if(src == SRC_AV) {
        set_text(ch_name, tr(S_TV_AV));
        set_text(ch_sub, "");
        glass_bars_set(ch_bars, -1);
    } else {
        set_text(ch_name, ch_label(cur_ch, b, sizeof b));
        if(cur_ch >= 0 && chs[cur_ch].name[0]) snprintf(b2, sizeof b2, tr(S_TV_CHANNEL_N), chs[cur_ch].ch);
        else if(cur_ch >= 0) snprintf(b2, sizeof b2, "%s MHz", chs[cur_ch].mhz);   /* sem nome: o título já é "Canal N" */
        else b2[0] = 0;
        set_text(ch_sub, b2);
        int lv = -1;
        if(tv_pid > 0 && !strcmp(s.state, "tocando")) lv = s.snr >= 20 ? 4 : s.snr >= 14 ? 3 : s.snr >= 9 ? 2 : s.snr >= 4 ? 1 : 0;
        else if(tv_pid > 0 && !strcmp(s.state, "sem_sinal")) lv = 0;
        glass_bars_set(ch_bars, lv);
    }
    /* situação */
    const char *t = "";
    if(view == V_SCAN) t = tr(S_TV_SCANNING);
    else if(view != V_WATCH) t = "";
    else if(tv_pid <= 0 && !want_start) t = getenv("BIGFOSTON_TV_STATUS") ? tr(S_TV_ST_PLAYING) : tr(S_TV_ST_STOPPED);
    else if(!strcmp(s.state, "ligando")) t = tr(S_TV_ST_BOOT);
    else if(!strcmp(s.state, "sintonizando")) t = tr(S_TV_ST_TUNING);
    else if(!strcmp(s.state, "abrindo")) t = tr(S_TV_ST_OPENING);
    else if(!strcmp(s.state, "sem_imagem")) t = tr(S_TV_ST_NOVIDEO);
    else if(!strcmp(s.state, "sem_sinal")) t = tr(S_TV_ST_NOSIGNAL);
    else if(!strcmp(s.state, "tocando") && s.rec) { snprintf(b, sizeof b, tr(S_TV_ST_REC), s.rec_kb / 1024.0); t = b; }
    else if(!strcmp(s.state, "tocando") && play_file[0] && s.dur > 0) {
        snprintf(b, sizeof b, "%d:%02d / %d:%02d", (int)s.pos / 60, (int)s.pos % 60, (int)s.dur / 60, (int)s.dur % 60);
        t = b;
    }
    else if(!strcmp(s.state, "tocando")) t = tr(S_TV_ST_PLAYING);
    else if(!strcmp(s.state, "fim")) t = tr(S_TV_ST_END);
    else if(!strncmp(s.state, "erro", 4)) t = tr(S_TV_ST_ERROR);
    else t = tr(S_TV_ST_TUNING);
    set_text(st_lbl, t);
    if(strip_name) {
        set_text(strip_name, lv_label_get_text(ch_name));
        char sb[160];
        const char *sub2 = lv_label_get_text(ch_sub);
        snprintf(sb, sizeof sb, "%s%s%s", sub2, sub2[0] && t[0] ? "  \xC2\xB7  " : "", t);
        set_text(strip_st, sb);
    }
    bool tv = src == SRC_TV && !play_file[0] && view != V_SCAN;
    enable(chdn, tv && nch > 1);
    enable(chup, tv && nch > 1);
    enable(list_btn, view != V_SCAN && src != SRC_AV);
    set_text(lv_obj_get_child(list_btn, 1), tr(src == SRC_REC ? S_TV_RECORDINGS : S_TV_CHANNELS));
    bool recording = tv_pid > 0 && s.rec && !play_file[0];
    enable(rec_btn, tv && view == V_WATCH && tv_pid > 0 && !strcmp(s.state, "tocando"));
    set_text(rec_lbl, tr(recording ? S_TV_REC_STOP : S_TV_REC));
}

/* ------------------------------------------------------------ ciclo */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bool gone = tv_reap();
    if(gone && want_start) { want_start = false; tv_start_now(); }
    if(view == V_SCAN) {
        tv_state_t s;
        read_state(&s);
        if(scan_bar && s.scan_ch) lv_bar_set_value(scan_bar, s.scan_ch, LV_ANIM_OFF);
        if(scan_lbl && s.scan_ch) {
            char b[96];
            snprintf(b, sizeof b, tr(S_TV_SCAN_PROGRESS), s.scan_ch, s.scan_found);
            set_text(scan_lbl, b);
        }
        if(gone && !getenv("BIGFOSTON_TV_STATUS")) {   /* a busca terminou (ou foi cancelada) */
            load_channels();
            load_last();
            if(cur_ch < 0 && nch) cur_ch = 0;
            show_view(V_CHANNELS);
            return;
        }
    } else if(gone && view == V_WATCH && play_file[0]) {
        tv_state_t s;                                    /* gravação acabou: volta à lista */
        read_state(&s);
        if(!strcmp(s.state, "fim")) { show_view(V_RECS); return; }
    }
    update_side();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* a área do vídeo: preta, sem nada que se redesenhe por cima */
    video = lv_obj_create(root);
    lv_obj_remove_style_all(video);
    lv_obj_set_pos(video, 0, 0);
    lv_obj_set_size(video, VID_W, VID_H);
    lv_obj_set_style_bg_color(video, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(video, LV_OPA_COVER, 0);
    lv_obj_remove_flag(video, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    panel = bf_plain(video);
    lv_obj_set_size(panel, VID_W, VID_H);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    build_column();
    lv_obj_update_layout(root);
    if(lv_obj_get_height(root) >= VID_H + 60) {          /* Glass: nome do canal grande sob o vídeo */
        lv_obj_t *sc = glass_card(root, 8, VID_H + 6, VID_W - 16, lv_obj_get_height(root) - VID_H - 12, 18);
        lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(sc, BF_SP_L, 0);
        lv_obj_set_style_pad_column(sc, BF_SP_L, 0);
        glass_tile(sc, 40, 12, GL_SKY, 0x3E86C9, NULL, LV_SYMBOL_VIDEO, BF_FONT_M);
        strip_name = glass_label(sc, "", BF_FONT_L, GL_WHITE);
        bf_label_ellipsis(strip_name, 300);
        strip_st = glass_label(sc, "", BF_FONT_M, GL_TEXT2);
        lv_obj_set_flex_grow(strip_st, 1);
        lv_label_set_long_mode(strip_st, LV_LABEL_LONG_DOT);
    }
    lv_obj_update_layout(root);
    lv_area_t a;
    lv_obj_get_coords(video, &a);
    vid_x = a.x1;
    vid_y = a.y1;
    load_channels();
    load_last();
    src = SRC_TV;
    play_file[0] = 0;
    show_view(V_WATCH);
    tick = lv_timer_create(tick_cb, 500, NULL);
}

static void destroy(void)
{
    if(tick) lv_timer_delete(tick);
    tick = NULL;
    want_start = false;
    /* sair da TV: para o vídeo e desliga o sintonizador (economia) */
    if(tv_pid > 0) { tv_stop(); for(int i = 0; i < 40 && !tv_reap(); i++) usleep(50000); }
    if(tv_pid > 0) { kill(tv_pid, SIGKILL); waitpid(tv_pid, NULL, 0); tv_pid = 0; }
    if(have_player() && !getenv("BIGFOSTON_TV_STATUS")) {
        char bin[300];
        tv_file(bin, sizeof bin, "bftvd");
        char *av[] = { bin, "-O", NULL };
        tv_spawn(av);
        if(tv_pid > 0) { waitpid(tv_pid, NULL, 0); tv_pid = 0; }
    }
    root = col = video = panel = strip_name = strip_st = NULL;
}

const bf_screen_def_t scr_tv_def = { create, destroy, S_APP_TV, NULL };

/* --screenshot: 0 assistindo, 1 canais, 2 gravações, 3 busca, 4 AV */
void scr_tv_demo(int step)
{
    if(!root) return;
    if(step == 0) { src = SRC_TV; show_view(V_WATCH); }
    else if(step == 1) show_view(V_CHANNELS);
    else if(step == 2) { src = SRC_REC; show_view(V_RECS); }
    else if(step == 3) { view = V_SCAN; build_scan(); lv_bar_set_value(scan_bar, 34, LV_ANIM_OFF);
                         char b[96]; snprintf(b, sizeof b, tr(S_TV_SCAN_PROGRESS), 34, 5); set_text(scan_lbl, b); update_side(); }
    else if(step == 4) { src = SRC_AV; show_view(V_AV); }
}
