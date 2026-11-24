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
#include "../core/status.h"
#include "../core/app.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AREA_W 640                        /* área do vídeo; a coluna do controle fica à direita */
#define COL_W (BF_SCREEN_W - AREA_W)
#define FOOT_H 72                         /* rodapé: canal, região, sinal ou volume */
#define MAXCH 64
#define MAXREC 40

typedef struct { int ch; char mhz[16], name[64]; int snr; } tv_ch_t;
enum { SRC_TV, SRC_REC, SRC_AV };
enum { V_WATCH, V_CHANNELS, V_RECS, V_AV, V_SCAN, V_SETUP };

static lv_obj_t *root, *video, *panel, *col, *src_btn[3];
static lv_obj_t *play_btn, *stop_btn, *rec_btn, *rec_dot, *rec_txt, *chdn, *chup, *list_btn, *fs_btn, *volup, *voldn;
static lv_obj_t *foot, *ft_info, *ft_name, *ft_sub, *ft_bars, *vol_row, *vol_sl, *vol_pct;
static lv_obj_t *scan_bar, *scan_lbl, *fs_ov;
static bool stopped, fs_on;
static long vol_until;                    /* esconde a barra de volume do rodapé (ms) */
static int vid_w = 640, vid_h = 360, area_h = 360;
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
        if(l > 3 && !strcmp(e->d_name + l - 3, ".ts") && l < sizeof recs[0]) memcpy(recs[nrec++], e->d_name, l + 1);
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
    pid_t parent = getpid();
    pid_t p = fork();
    if(p == 0) {
        setsid();
        /* o shell morreu (atualizado, travou): o vídeo para junto, senão continua desenhando
         * por cima da interface nova sem ninguém para pará-lo */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if(getppid() != parent) _exit(0);
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
    char full[40];
    snprintf(rect, sizeof rect, "%d,%d,%d,%d", vid_x, vid_y, vid_w, vid_h);
    snprintf(full, sizeof full, "0,%d,%d,%d", (BF_SCREEN_H - BF_SCREEN_W * 9 / 16) / 2, BF_SCREEN_W, BF_SCREEN_W * 9 / 16);
    if(play_file[0]) {
        char *av[] = { bin, "-r", rect, "-F", full, "-s", (char *)state_path(), "-p", play_file, NULL };
        tv_spawn(av);
    } else if(cur_ch >= 0 && cur_ch < nch) {
        mkdir(dir, 0755);
        char *av[] = { bin, "-f", fw, "-r", rect, "-F", full, "-s", (char *)state_path(), "-W", dir, chs[cur_ch].mhz, NULL };
        tv_spawn(av);
    }
}

/* (re)começa a tocar: se o bftvd ainda está rodando, para e o tick começa quando ele sair */
static void tv_play(void)
{
    stopped = false;
    if(!have_player() || getenv("BIGFOSTON_TV_STATUS")) return;   /* simulador: sem bftvd */
    want_start = true;
    if(tv_pid > 0) tv_stop();
    else { want_start = false; tv_start_now(); }
}

static void tv_halt(void) { want_start = false; tv_stop(); }

/* ------------------------------------------------------------ peças (estilo controle remoto) */

/* Tudo sobre preto: só ícone e texto brancos, sem cartões, bordas, cantos ou cores. */
#define C_WHITE lv_color_white()
#define C_GRAY  lv_color_hex(0x8C8C8C)
#define C_LINE  lv_color_hex(0x262626)

static void show_view(int v);
static void update_ui(void);
static void fs_exit(void);
static long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

static lv_obj_t *lbl(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *box(lv_obj_t *p, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* monitor: tela com pé (Canais) */
static void ic_monitor(lv_obj_t *p)
{
    lv_obj_t *b = box(p, 30, 24);
    lv_obj_t *s = box(b, 28, 18);
    lv_obj_set_pos(s, 1, 0);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, C_WHITE, 0);
    lv_obj_set_style_radius(s, 3, 0);
    lv_obj_t *n = box(b, 2, 4);
    lv_obj_set_pos(n, 14, 18);
    lv_obj_set_style_bg_color(n, C_WHITE, 0);
    lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
    lv_obj_t *f = box(b, 14, 2);
    lv_obj_set_pos(f, 8, 22);
    lv_obj_set_style_bg_color(f, C_WHITE, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
}

/* entrada: moldura aberta à esquerda com a seta entrando (Entrada AV) */
static void ic_input(lv_obj_t *p)
{
    static const lv_point_precise_t shaft[] = { { 1, 12 }, { 19, 12 } };
    static const lv_point_precise_t head[] = { { 13, 6 }, { 19, 12 }, { 13, 18 } };
    lv_obj_t *b = box(p, 32, 24);
    lv_obj_t *r = box(b, 26, 22);
    lv_obj_set_pos(r, 5, 1);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_color(r, C_WHITE, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_t *gap = box(b, 4, 10);                       /* abertura da moldura (fundo preto) */
    lv_obj_set_pos(gap, 4, 7);
    lv_obj_set_style_bg_color(gap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gap, LV_OPA_COVER, 0);
    for(int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_line_create(b);
        lv_line_set_points(l, i ? head : shaft, i ? 3 : 2);
        lv_obj_set_style_line_width(l, 2, 0);
        lv_obj_set_style_line_color(l, C_WHITE, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_pos(l, 0, 0);
    }
}

/* ponto de gravar (vermelho só enquanto grava) */
static lv_obj_t *ic_dot(lv_obj_t *p)
{
    lv_obj_t *b = box(p, 24, 24);
    lv_obj_t *d = box(b, 16, 16);
    lv_obj_center(d);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, C_WHITE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

/* tecla: ícone (símbolo ou desenho) e o texto embaixo; apertada fica apagada */
static lv_obj_t *key(lv_obj_t *p, const char *sym, void (*draw)(lv_obj_t *), const char *txt, int w, int h,
                     lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(p, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 4, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    if(sym) lbl(b, sym, BF_FONT_M, C_WHITE);
    else if(draw) draw(b);
    if(txt) lbl(b, txt, BF_FONT_S, C_WHITE);
    if(cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void enable(lv_obj_t *b, bool on)
{
    if(!b) return;
    if(on) lv_obj_remove_state(b, LV_STATE_DISABLED);
    else lv_obj_add_state(b, LV_STATE_DISABLED);
}

static void set_text(lv_obj_t *l, const char *t) { if(l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }

/* ------------------------------------------------------------ painéis na área do vídeo */

static lv_obj_t *panel_page(const char *title, bool with_close)
{
    lv_obj_clean(panel);
    lv_obj_t *c = box(panel, AREA_W, area_h);
    lv_obj_set_style_pad_all(c, BF_SP_L, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    lv_obj_t *h = box(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, BF_SP_L, 0);
    lv_obj_t *t = lbl(h, title, BF_FONT_L, C_WHITE);
    lv_obj_set_flex_grow(t, 1);
    (void)with_close;
    return c;
}

static void on_back_watch(lv_event_t *e) { (void)e; show_view(V_WATCH); }

static void head_close(lv_obj_t *c)
{
    lv_obj_t *h = lv_obj_get_child(c, 0);
    key(h, LV_SYMBOL_CLOSE, NULL, NULL, 48, 44, on_back_watch, NULL);
}

static lv_obj_t *note(lv_obj_t *c, const char *txt)
{
    lv_obj_t *l = lbl(c, txt, BF_FONT_M, C_GRAY);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static lv_obj_t *list_box(lv_obj_t *c)
{
    lv_obj_t *l = box(c, LV_PCT(100), 0);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    bf_scrollable(l);
    return l;
}

/* linha de lista: ícone branco, título, detalhe cinza, fio fino embaixo */
static lv_obj_t *row(lv_obj_t *l, const char *sym, void (*draw)(lv_obj_t *), const char *title, const char *sub, bool marked)
{
    lv_obj_t *r = box(l, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_ver(r, 10, 0);
    lv_obj_set_style_pad_hor(r, 4, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, BF_SP_L, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, C_LINE, 0);
    lv_obj_set_style_opa(r, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_t *ib = box(r, 36, 28);
    if(sym) lv_obj_center(lbl(ib, sym, BF_FONT_M, C_WHITE));
    else if(draw) { draw(ib); lv_obj_center(lv_obj_get_child(ib, 0)); }
    lv_obj_t *cc = box(r, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(cc, 1);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = lbl(cc, title, BF_FONT_M, C_WHITE);
    bf_label_ellipsis(t, 440);
    if(sub && *sub) lbl(cc, sub, BF_FONT_S, C_GRAY);
    if(marked) lbl(r, LV_SYMBOL_PLAY, BF_FONT_S, C_WHITE);     /* o canal atual */
    return r;
}

static void on_scan(lv_event_t *e);

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
    lv_obj_t *c = panel_page(tr(S_TV_CHANNELS), true);
    lv_obj_t *h = lv_obj_get_child(c, 0);
    key(h, LV_SYMBOL_REFRESH, NULL, NULL, 48, 44, on_scan, NULL);
    head_close(c);
    if(!nch) { note(c, tr(S_TV_NO_CHANNELS)); return; }
    lv_obj_t *l = list_box(c);
    for(int i = 0; i < nch; i++) {
        char t[96], s[96];
        ch_label(i, t, sizeof t);
        snprintf(s, sizeof s, tr(S_TV_CH_SUB), chs[i].ch, chs[i].mhz);
        lv_obj_t *r = row(l, NULL, ic_monitor, t, s, i == cur_ch && src == SRC_TV);
        lv_obj_add_event_cb(r, on_pick_ch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* "tv-20261122-203015-c15.ts" -> "22/11/2026 20:30 · TV Brasil 1seg" */
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

static void build_recs(void)
{
    load_recs();
    lv_obj_t *c = panel_page(tr(S_TV_RECORDINGS), true);
    head_close(c);
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
        lv_obj_t *r = row(l, LV_SYMBOL_PLAY, NULL, t, s, false);
        lv_obj_add_event_cb(r, on_pick_rec, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void build_av(void)
{
    lv_obj_t *c = panel_page(tr(S_TV_AV), false);
    note(c, tr(S_TV_AV_NOTE));
}

static void build_setup(void)
{
    lv_obj_t *c = panel_page(tr(S_TV_SETUP), false);
    note(c, tr(S_TV_SETUP_NOTE));
}

static void build_stopped(void)
{
    lv_obj_clean(panel);
    lv_obj_t *c = box(panel, AREA_W, area_h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, BF_SP_S, 0);
    lbl(c, LV_SYMBOL_STOP, BF_FONT_L, C_WHITE);
    lbl(c, tr(S_TV_STOPPED_HINT), BF_FONT_S, C_GRAY);
}

static void on_scan_cancel(lv_event_t *e) { (void)e; tv_halt(); }

static void build_scan(void)
{
    lv_obj_t *c = panel_page(tr(S_TV_SCANNING), false);
    note(c, tr(S_TV_SCAN_NOTE));
    scan_bar = lv_bar_create(c);
    lv_obj_remove_style_all(scan_bar);
    lv_obj_set_size(scan_bar, LV_PCT(100), 6);
    lv_bar_set_range(scan_bar, 14, 69);
    lv_obj_set_style_bg_color(scan_bar, C_LINE, 0);
    lv_obj_set_style_bg_opa(scan_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scan_bar, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(scan_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    scan_lbl = lbl(c, "", BF_FONT_M, C_WHITE);
    key(c, LV_SYMBOL_CLOSE, NULL, tr(S_CANCEL), 120, 64, on_scan_cancel, NULL);
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    if(!have_player()) return;
    want_start = false;
    play_file[0] = 0;
    view = V_SCAN;
    build_scan();
    update_ui();
    if(getenv("BIGFOSTON_TV_STATUS")) return;            /* simulador */
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
    if(fs_on) fs_exit();
    view = v;
    scan_bar = scan_lbl = NULL;
    if(v == V_WATCH) {
        lv_obj_clean(panel);
        if(!have_player()) { view = V_SETUP; tv_halt(); build_setup(); }
        else if(src == SRC_AV) { view = V_AV; tv_halt(); build_av(); }
        else if(src == SRC_TV && cur_ch < 0) { view = V_CHANNELS; tv_halt(); build_channels(); }
        else if(stopped) { tv_halt(); build_stopped(); }
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
    update_ui();
}

/* ------------------------------------------------------------ tela cheia */

/* O vídeo ocupa a tela toda (800x450): uma camada preta cobre tudo e o LVGL para de desenhar
 * (nada da barra de cima por cima do vídeo); tocar em qualquer lugar volta. */
static void on_fs_tap(lv_event_t *e) { (void)e; fs_exit(); }

static void fs_enter(void)
{
    if(fs_on || tv_pid <= 0 || view != V_WATCH) return;
    fs_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(fs_ov);
    lv_obj_set_size(fs_ov, BF_SCREEN_W, BF_SCREEN_H);
    lv_obj_set_style_bg_color(fs_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fs_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(fs_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fs_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(fs_ov, on_fs_tap, LV_EVENT_CLICKED, NULL);
    lv_refr_now(NULL);
    lv_display_enable_invalidation(lv_display_get_default(), false);
    fs_on = true;
    kill(tv_pid, SIGUSR2);
}

static void fs_exit(void)
{
    if(!fs_on) return;
    fs_on = false;
    if(tv_pid > 0) { kill(tv_pid, SIGWINCH); usleep(150000); }   /* volta ao retângulo antes de redesenhar */
    lv_display_enable_invalidation(lv_display_get_default(), true);
    if(fs_ov) { lv_obj_delete(fs_ov); fs_ov = NULL; }
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

static void on_fs(lv_event_t *e) { (void)e; fs_enter(); }

/* ------------------------------------------------------------ volume no rodapé */

static void vol_show(int v)
{
    if(!vol_row) return;
    vol_until = ms_now() + 4000;
    lv_obj_add_flag(ft_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_HIDDEN);
    if(v < 0) v = status_provider()->volume_get();
    if(v < 0) v = 0;
    lv_slider_set_value(vol_sl, v, LV_ANIM_OFF);
    char b[16];
    snprintf(b, sizeof b, "%d%%", v);
    set_text(vol_pct, b);
}

static void vol_set(int v)
{
    if(v < 0) v = 0;
    if(v > 100) v = 100;
    status_provider()->volume_set(v);
    status_refresh();
    vol_show(v);
}

static void on_vol_key(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int v = status_provider()->volume_get();
    if(v < 0) return;
    vol_set(v + d);
}

static void on_vol_slider(lv_event_t *e) { vol_set((int)lv_slider_get_value(lv_event_get_target(e))); }

/* ícone de som da barra de cima: na TV a barra aparece no rodapé, sem cobrir o vídeo */
bool scr_tv_volume(void)
{
    if(!root || !vol_row) return false;
    if(!lv_obj_has_flag(vol_row, LV_OBJ_FLAG_HIDDEN)) { vol_until = 0; return true; }   /* 2º toque fecha */
    vol_show(-1);
    return true;
}

/* ------------------------------------------------------------ controle (coluna) */

static void on_src(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    src = s;
    stopped = false;
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
    stopped = false;
    save_last();
    show_view(V_WATCH);
}

static void on_chup(lv_event_t *e) { (void)e; zap(1); }
static void on_chdn(lv_event_t *e) { (void)e; zap(-1); }
static void on_list(lv_event_t *e) { (void)e; show_view(src == SRC_REC ? V_RECS : V_CHANNELS); }
static void on_play(lv_event_t *e) { (void)e; stopped = false; if(src == SRC_AV) src = SRC_TV; show_view(V_WATCH); }
static void on_stop(lv_event_t *e) { (void)e; stopped = true; show_view(V_WATCH); }

static void on_rec(lv_event_t *e)
{
    (void)e;
    if(play_file[0] || view != V_WATCH || tv_pid <= 0) return;
    kill(tv_pid, SIGUSR1);                                   /* começa/para de gravar sem parar a imagem */
}

static lv_obj_t *krow(lv_obj_t *p)
{
    lv_obj_t *r = box(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *kcol(lv_obj_t *p, int w)
{
    lv_obj_t *c = box(p, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 2, 0);
    return c;
}

static void build_column(int h)
{
    col = box(root, COL_W, h);
    lv_obj_set_pos(col, AREA_W, 0);
    lv_obj_set_style_pad_hor(col, 6, 0);
    lv_obj_set_style_pad_ver(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, h >= 340 ? 18 : 4, 0);
    int kh = h >= 340 ? 56 : 46;
    /* fonte: TV, gravações, AV */
    lv_obj_t *r = krow(col);
    src_btn[0] = key(r, BF_SYMBOL_TOWER, NULL, "TV", 34, kh, on_src, (void *)(intptr_t)SRC_TV);
    src_btn[1] = key(r, LV_SYMBOL_DIRECTORY, NULL, tr(S_TV_RECORDINGS), 80, kh, on_src, (void *)(intptr_t)SRC_REC);
    src_btn[2] = key(r, NULL, ic_input, "AV", 34, kh, on_src, (void *)(intptr_t)SRC_AV);
    /* play, stop, gravar */
    r = krow(col);
    play_btn = key(r, LV_SYMBOL_PLAY, NULL, "Play", 46, kh, on_play, NULL);
    stop_btn = key(r, LV_SYMBOL_STOP, NULL, "Stop", 46, kh, on_stop, NULL);
    rec_btn = key(r, NULL, NULL, NULL, 50, kh, on_rec, NULL);
    rec_dot = ic_dot(rec_btn);
    rec_txt = lbl(rec_btn, tr(S_TV_REC), BF_FONT_S, C_WHITE);
    /* volume | canais e tela cheia | canal */
    r = krow(col);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *v = kcol(r, 36);
    volup = key(v, LV_SYMBOL_PLUS, NULL, NULL, 36, 48, on_vol_key, (void *)(intptr_t)10);
    lbl(v, "VOL", BF_FONT_S, C_WHITE);
    voldn = key(v, LV_SYMBOL_MINUS, NULL, NULL, 36, 48, on_vol_key, (void *)(intptr_t)-10);
    lv_obj_t *m = kcol(r, 76);
    lv_obj_set_style_pad_row(m, 12, 0);
    list_btn = key(m, NULL, ic_monitor, tr(S_TV_CHANNELS), 76, kh, on_list, NULL);
    fs_btn = key(m, BF_SYMBOL_EXPAND, NULL, tr(S_TV_FULLSCREEN), 76, kh, on_fs, NULL);
    lv_obj_t *c = kcol(r, 36);
    chup = key(c, LV_SYMBOL_UP, NULL, NULL, 36, 48, on_chup, NULL);
    lbl(c, "CH", BF_FONT_S, C_WHITE);
    chdn = key(c, LV_SYMBOL_DOWN, NULL, NULL, 36, 48, on_chdn, NULL);
}

/* rodapé sob o vídeo: nome do canal; região e situação menores e mais escuras; sinal à direita */
static void build_footer(int y, int h)
{
    foot = box(root, BF_SCREEN_W, h);                    /* de ponta a ponta, sob o vídeo e o controle */
    lv_obj_set_pos(foot, 0, y);
    lv_obj_set_style_pad_hor(foot, BF_SP_L, 0);
    ft_info = box(foot, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ft_info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ft_info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = box(ft_info, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 2, 0);
    ft_name = lbl(t, "", BF_FONT_L, C_WHITE);
    bf_label_ellipsis(ft_name, 660);
    ft_sub = lbl(t, "", BF_FONT_S, C_GRAY);
    bf_label_ellipsis(ft_sub, 660);
    ft_bars = glass_bars(ft_info);
    for(uint32_t i = 0; i < lv_obj_get_child_count(ft_bars); i++)
        lv_obj_set_style_bg_color(lv_obj_get_child(ft_bars, (int32_t)i), C_WHITE, 0);
    /* volume (topo da barra de cima ou VOL +/-): no lugar do nome por 4 s */
    vol_row = box(foot, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, BF_SP_L, 0);
    key(vol_row, LV_SYMBOL_MINUS, NULL, NULL, 40, 48, on_vol_key, (void *)(intptr_t)-10);
    vol_sl = lv_slider_create(vol_row);
    lv_obj_remove_style_all(vol_sl);
    lv_obj_set_height(vol_sl, 6);
    lv_obj_set_flex_grow(vol_sl, 1);
    lv_slider_set_range(vol_sl, 0, 100);
    lv_obj_set_style_bg_color(vol_sl, C_LINE, 0);
    lv_obj_set_style_bg_opa(vol_sl, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(vol_sl, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vol_sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_sl, C_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(vol_sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(vol_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol_sl, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(vol_sl, 20);
    lv_obj_add_event_cb(vol_sl, on_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);
    key(vol_row, LV_SYMBOL_PLUS, NULL, NULL, 40, 48, on_vol_key, (void *)(intptr_t)10);
    vol_pct = lbl(vol_row, "", BF_FONT_M, C_WHITE);
    lv_obj_set_width(vol_pct, 64);
    lv_obj_add_flag(vol_row, LV_OBJ_FLAG_HIDDEN);
}

static void update_ui(void)
{
    if(!col) return;
    for(int i = 0; i < 3; i++)                               /* a fonte atual acesa; as outras apagadas */
        lv_obj_set_style_opa(src_btn[i], i == src ? LV_OPA_COVER : LV_OPA_50, 0);
    tv_state_t s;
    read_state(&s);
    char name[128], sub[200], b2[96];
    if(play_file[0]) {
        const char *f = strrchr(play_file, '/');
        rec_title(f ? f + 1 : play_file, name, sizeof name);
        snprintf(b2, sizeof b2, "%s", tr(S_TV_RECORDING_PLAY));
    } else if(src == SRC_AV) {
        snprintf(name, sizeof name, "%s", tr(S_TV_AV));
        b2[0] = 0;
    } else {
        ch_label(cur_ch, name, sizeof name);
        if(cur_ch >= 0 && chs[cur_ch].name[0]) snprintf(b2, sizeof b2, tr(S_TV_CHANNEL_N), chs[cur_ch].ch);
        else if(cur_ch >= 0) snprintf(b2, sizeof b2, "%s MHz", chs[cur_ch].mhz);
        else b2[0] = 0;
    }
    /* situação */
    const char *t = "";
    char tb[96];
    bool sim = getenv("BIGFOSTON_TV_STATUS") != NULL;
    if(view == V_SCAN) t = tr(S_TV_SCANNING);
    else if(view != V_WATCH) t = "";
    else if(stopped) t = tr(S_TV_ST_STOPPED);
    else if(tv_pid <= 0 && !want_start) t = sim ? tr(S_TV_ST_PLAYING) : tr(S_TV_ST_STOPPED);
    else if(!strcmp(s.state, "ligando")) t = tr(S_TV_ST_BOOT);
    else if(!strcmp(s.state, "sintonizando")) t = tr(S_TV_ST_TUNING);
    else if(!strcmp(s.state, "abrindo")) t = tr(S_TV_ST_OPENING);
    else if(!strcmp(s.state, "sem_imagem")) t = tr(S_TV_ST_NOVIDEO);
    else if(!strcmp(s.state, "sem_sinal")) t = tr(S_TV_ST_NOSIGNAL);
    else if(!strcmp(s.state, "tocando") && s.rec) { snprintf(tb, sizeof tb, tr(S_TV_ST_REC), s.rec_kb / 1024.0); t = tb; }
    else if(!strcmp(s.state, "tocando") && play_file[0] && s.dur > 0) {
        snprintf(tb, sizeof tb, "%d:%02d / %d:%02d", (int)s.pos / 60, (int)s.pos % 60, (int)s.dur / 60, (int)s.dur % 60);
        t = tb;
    }
    else if(!strcmp(s.state, "tocando")) t = tr(S_TV_ST_PLAYING);
    else if(!strcmp(s.state, "fim")) t = tr(S_TV_ST_END);
    else if(!strncmp(s.state, "erro", 4)) t = tr(S_TV_ST_ERROR);
    else t = tr(S_TV_ST_TUNING);
    snprintf(sub, sizeof sub, "%s", tr(S_TV_REGION));
    if(b2[0]) { size_t k = strlen(sub); snprintf(sub + k, sizeof sub - k, "  \xC2\xB7  %s", b2); }
    if(t[0]) { size_t k = strlen(sub); snprintf(sub + k, sizeof sub - k, "  \xC2\xB7  %s", t); }
    set_text(ft_name, name);
    set_text(ft_sub, sub);
    int lv = -1;                                              /* força do sinal (SNR) */
    if(!play_file[0] && src == SRC_TV && view == V_WATCH && !stopped) {
        if((tv_pid > 0 || sim) && (!strcmp(s.state, "tocando") || !strcmp(s.state, "abrindo") || !strcmp(s.state, "sem_imagem")))
            lv = s.snr >= 20 ? 4 : s.snr >= 14 ? 3 : s.snr >= 9 ? 2 : s.snr >= 4 ? 1 : 0;
        else if(tv_pid > 0 && !strcmp(s.state, "sem_sinal")) lv = 0;
    }
    glass_bars_set(ft_bars, lv);
    /* teclas */
    bool tv = src == SRC_TV && !play_file[0] && view != V_SCAN;
    bool playing = view == V_WATCH && !stopped && (tv_pid > 0 || want_start || sim) && src != SRC_AV;
    enable(chdn, tv && nch > 1);
    enable(chup, tv && nch > 1);
    enable(list_btn, view != V_SCAN && src != SRC_AV);
    set_text(lv_obj_get_child(list_btn, 1), tr(src == SRC_REC ? S_TV_RECORDINGS : S_TV_CHANNELS));
    enable(play_btn, view == V_WATCH && src != SRC_AV && (stopped || !playing) && (cur_ch >= 0 || play_file[0]));
    enable(stop_btn, playing);
    enable(fs_btn, playing && !strcmp(s.state, "tocando"));
    bool recording = tv_pid > 0 && s.rec && !play_file[0];
    enable(rec_btn, tv && view == V_WATCH && tv_pid > 0 && !strcmp(s.state, "tocando"));
    lv_obj_set_style_bg_color(rec_dot, recording ? lv_color_hex(0xFF3B30) : C_WHITE, 0);
    set_text(rec_txt, tr(recording ? S_TV_REC_STOP : S_TV_REC));
    bool vol = status_provider()->volume_get() >= 0;
    enable(volup, vol);
    enable(voldn, vol);
}

/* ------------------------------------------------------------ ciclo */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bool gone = tv_reap();
    if(gone && want_start) { want_start = false; tv_start_now(); }
    /* tela cheia: sai se o vídeo parou ou se algo precisa aparecer (chamada recebida, aviso) */
    if(fs_on && (tv_pid <= 0 || lv_obj_get_child_count(lv_layer_top()) > 1)) fs_exit();
    if(fs_on) return;                                    /* nada se redesenha por cima do vídeo */
    if(vol_row && vol_until && ms_now() > vol_until) {
        vol_until = 0;
        lv_obj_add_flag(vol_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ft_info, LV_OBJ_FLAG_HIDDEN);
    }
    if(vol_row && !vol_until && !lv_obj_has_flag(vol_row, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(vol_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ft_info, LV_OBJ_FLAG_HIDDEN);
    }
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
    } else if(gone && view == V_WATCH && play_file[0] && !stopped) {
        tv_state_t s;                                    /* gravação acabou: volta à lista */
        read_state(&s);
        if(!strcmp(s.state, "fim")) { show_view(V_RECS); return; }
    }
    update_ui();
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    app_wallpaper_visible(false);                        /* TV: tudo preto */
    root = bf_plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);
    int h = lv_obj_get_height(root);
    /* vídeo 16:9 o maior possível deixando o rodapé; 640x360 no Glass (432 px de altura) */
    area_h = h - FOOT_H;
    vid_h = area_h > 360 ? 360 : area_h & ~1;
    vid_w = vid_h * 16 / 9 & ~1;
    if(vid_w > AREA_W) { vid_w = AREA_W; vid_h = vid_w * 9 / 16 & ~1; }
    video = box(root, AREA_W, area_h);
    lv_obj_set_pos(video, 0, 0);
    lv_obj_set_style_bg_color(video, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(video, LV_OPA_COVER, 0);
    panel = box(video, AREA_W, area_h);
    build_column(area_h);                                /* o controle acaba onde começa o rodapé */
    build_footer(area_h, h - area_h);
    lv_obj_update_layout(root);
    lv_area_t a;
    lv_obj_get_coords(video, &a);
    vid_x = a.x1 + (AREA_W - vid_w) / 2;
    vid_y = a.y1 + (area_h - vid_h) / 2;
    load_channels();
    load_last();
    src = SRC_TV;
    stopped = false;
    play_file[0] = 0;
    show_view(V_WATCH);
    tick = lv_timer_create(tick_cb, 500, NULL);
}

static void destroy(void)
{
    if(tick) lv_timer_delete(tick);
    tick = NULL;
    fs_exit();
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
    app_wallpaper_visible(true);
    root = col = video = panel = foot = ft_info = ft_name = ft_sub = ft_bars = vol_row = vol_sl = vol_pct = NULL;
    play_btn = stop_btn = rec_btn = rec_dot = rec_txt = chdn = chup = list_btn = fs_btn = volup = voldn = NULL;
    memset(src_btn, 0, sizeof src_btn);
}

const bf_screen_def_t scr_tv_def = { create, destroy, S_APP_TV, NULL };

/* --screenshot: 0 assistindo, 1 canais, 2 gravações, 3 busca, 4 AV, 5 volume no rodapé */
void scr_tv_demo(int step)
{
    if(!root) return;
    if(step == 0) { src = SRC_TV; show_view(V_WATCH); }
    else if(step == 1) show_view(V_CHANNELS);
    else if(step == 2) { src = SRC_REC; show_view(V_RECS); }
    else if(step == 3) { view = V_SCAN; build_scan(); lv_bar_set_value(scan_bar, 34, LV_ANIM_OFF);
                         char b[96]; snprintf(b, sizeof b, tr(S_TV_SCAN_PROGRESS), 34, 5); set_text(scan_lbl, b); update_ui(); }
    else if(step == 4) { src = SRC_AV; show_view(V_AV); }
    else if(step == 5) { src = SRC_TV; show_view(V_WATCH); vol_show(60); }
}
