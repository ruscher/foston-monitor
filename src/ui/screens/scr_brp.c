/* scr_brp.c — Big Remote Play (grupo Jogos): o FOSTON como cliente do Sunshine do computador
 * (docs/remote-play). Quem fala o protocolo é o motor bfrp (src/remoteplay/bfrp.c), que vem no
 * pacote do app (<dados>/apps/big-remote-play/bfrp); esta tela conversa com ele por linhas.
 *
 * Vistas:
 *  - início: o computador (pelo cabo USB, 192.168.7.1), o estado do Sunshine e do pareamento,
 *    Parear / Atualizar e a lista de jogos e apps que o computador oferece;
 *  - PIN: o código que o usuário aprova no Big Remote Play do computador;
 *  - sessão Somente Controle: a imagem fica no monitor do computador e o FOSTON é o controle.
 *
 * Controle virtual num touchscreen de UM ponto (o resistivo do FOSTON não vê dois dedos:
 * docs/remote-play/11): com "Travar" ligado, cada toque liga/desliga um botão que fica
 * pressionado (direcional travado + A chegam juntos ao PC). Sem travar, o botão vale enquanto o
 * dedo está nele. O controle Bluetooth físico é somado ao virtual (vários botões de verdade).
 * Nada fica pressionado no PC: Soltar tudo, Encerrar, sair da tela e a queda da conexão soltam
 * tudo; o motor também solta tudo se a entrada dele fechar.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../widgets/page.h"
#include "../core/apps.h"
#include "../core/gamepad.h"
#include "../core/padmap.h"
#include "../core/settings.h"
#include "../core/perfui.h"
#include "../widgets/dialog.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C_BRP     0x6a3df0
#define HOST_USB  "192.168.7.1"
#define MAXAPPS   32

/* Moonlight: botões do controle (Limelight.h) */
enum { F_UP = 0x0001, F_DOWN = 0x0002, F_LEFT = 0x0004, F_RIGHT = 0x0008, F_START = 0x0010, F_BACK = 0x0020,
       F_LS = 0x0040, F_RS = 0x0080, F_LB = 0x0100, F_RB = 0x0200, F_GUIDE = 0x0400,
       F_A = 0x1000, F_B = 0x2000, F_X = 0x4000, F_Y = 0x8000,
       F_LT = 0x10000, F_RT = 0x20000 };                   /* gatilhos: só aqui, viram 0/255 */

enum { V_HOME, V_PIN, V_VIDEO };

/* qualidade da imagem: o que o FOSTON decodifica de verdade (medido em 24/11/2026 pelo USB, H.264
 * do Sunshine com o filtro de desblocagem, ampliado suave para 800x450): 320x180 ~19 qps
 * (52 ms/quadro), 480x270 ~9 qps (105 ms), 640x360 ~5 qps (200 ms) */
static const struct { int w, h, fps, kbps; bf_str_t name, sub; } qual[] = {
    { 320, 180, 30, 1000, S_BRP_Q_FLUID, S_BRP_Q_FLUID_SUB },      /* baixa: ~19 qps em tela cheia */
    { 480, 270, 20, 2000, S_BRP_Q_SHARP, S_BRP_Q_SHARP_SUB },      /* média: ~9 qps */
    { 640, 360, 15, 3000, S_BRP_Q_MAX, S_BRP_Q_MAX_SUB },          /* alta: ~5 qps */
};
#define NQUAL (int)(sizeof qual / sizeof qual[0])
static int quality;
static bool session_video;                     /* a sessão atual tem imagem */
static lv_obj_t *video_box;
static lv_area_t vr;
static const int zooms[] = { 100, 150, 200, 300, 400 };
static int zoom_i;                               /* índice em zooms (lupa) */                             /* área do vídeo na tela (janela ou tela cheia) */

static lv_obj_t *root, *body, *st_lbl, *pin_lbl;
static int view;
static lv_timer_t *io_t, *pad_t;

/* ------------------------------------------------------------ motor */

static pid_t eng_pid = -1;
static int to_eng = -1, from_eng = -1;
static char inbuf[4096];
static int inlen;

static struct { int id; char name[64]; } apps[MAXAPPS];
static int napps;
static bool host_ok, host_paired, host_busy, apps_done, streaming, starting;
static char host_err[120], host_ver[32];
static int cur_app = -1;

static void eng_send(const char *fmt, ...)
{
    if(to_eng < 0) return;
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if(write(to_eng, b, (size_t)n) != n) { /* motor saiu: o tick percebe */ }
}

static bool eng_start(char *err, size_t errn)
{
    const bf_app_entry_t *a = apps_find("big-remote-play");
    char bin[300], keys[256];
    snprintf(bin, sizeof bin, "%s/bfrp", a ? a->dir : ".");
    if(access(bin, X_OK)) { snprintf(err, errn, "%s", tr(S_BRP_NO_ENGINE)); return false; }
    data_path(keys, sizeof keys, "remoteplay");
    int in[2], out[2];
    if(pipe2(in, O_CLOEXEC)) return false;
    if(pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return false; }
    pid_t p = fork();
    if(p < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return false; }
    if(p == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        /* diagnóstico: o stderr do motor em /tmp (RAM, só root); detalhado só com o arquivo
         * <dados>/remoteplay/debug (o log guarda as respostas do pareamento) */
        int lg = open("/tmp/bfrp.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if(lg >= 0) dup2(lg, 2);
        char dbg[300];
        snprintf(dbg, sizeof dbg, "%s/debug", keys);
        if(!access(dbg, F_OK)) setenv("BFRP_DEBUG", "1", 1);
        /* a interface primeiro: decodificando, o motor ocupa a CPU e, com a mesma prioridade,
         * o toque e a barra da tela cheia chegavam segundos atrasados (medido 24/11/2026) */
        setpriority(PRIO_PROCESS, 0, 5);
        char *av[] = { bin, "--keys", keys, NULL };
        execv(bin, av);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    to_eng = in[1];
    from_eng = out[0];
    fcntl(from_eng, F_SETFL, fcntl(from_eng, F_GETFL) | O_NONBLOCK);
    eng_pid = p;
    inlen = 0;
    return true;
}

static void eng_stop(void)
{
    if(to_eng >= 0) {
        eng_send("RELEASE");
        eng_send("STOP");
        close(to_eng);                                 /* EOF: o motor solta tudo e sai */
        to_eng = -1;
    }
    if(from_eng >= 0) { close(from_eng); from_eng = -1; }
    if(eng_pid > 0) {
        for(int i = 0; i < 30 && waitpid(eng_pid, NULL, WNOHANG) == 0; i++) usleep(20000);
        if(waitpid(eng_pid, NULL, WNOHANG) == 0) { kill(eng_pid, SIGTERM); waitpid(eng_pid, NULL, 0); }
        eng_pid = -1;
    }
}

/* ------------------------------------------------------------ controle físico */

/* o controle Bluetooth (ou USB) vai ao computador durante a sessão; sem controle na tela */
static int sent_b = -1, sent_lt, sent_rt, sent_lx, sent_ly, sent_rx, sent_ry;
static char phys_name[64];

static void pad_flush(bool force)
{
    if(!streaming) return;
    int b = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    phys_name[0] = 0;
    gp_info_t gi;
    if(gamepad_count() > 0 && gamepad_info(0, &gi)) {
        snprintf(phys_name, sizeof phys_name, "%s", gi.name);
        static const struct { int pc, f; } m[] = {
            { PC_A, F_A }, { PC_B, F_B }, { PC_X, F_X }, { PC_Y, F_Y }, { PC_BACK, F_BACK }, { PC_GUIDE, F_GUIDE },
            { PC_START, F_START }, { PC_LSTICK, F_LS }, { PC_RSTICK, F_RS }, { PC_LSHOULDER, F_LB },
            { PC_RSHOULDER, F_RB }, { PC_DPUP, F_UP }, { PC_DPDOWN, F_DOWN }, { PC_DPLEFT, F_LEFT },
            { PC_DPRIGHT, F_RIGHT } };
        for(unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if(gi.btn[m[i].pc]) b |= m[i].f;
        int ax = gi.ax[PC_LEFTX - PC__NBTN], ay = gi.ax[PC_LEFTY - PC__NBTN];
        lx = ax * 32767 / 1000; ly = -ay * 32767 / 1000;    /* Moonlight: para cima é + */
        rx = gi.ax[PC_RIGHTX - PC__NBTN] * 32767 / 1000;
        ry = -gi.ax[PC_RIGHTY - PC__NBTN] * 32767 / 1000;
        int plt = gi.ax[PC_LTRIGGER - PC__NBTN] * 255 / 1000, prt = gi.ax[PC_RTRIGGER - PC__NBTN] * 255 / 1000;
        if(plt > lt) lt = plt;
        if(prt > rt) rt = prt;
    }
    int bb = b & 0xffff;
    if(!force && bb == sent_b && lt == sent_lt && rt == sent_rt && lx == sent_lx && ly == sent_ly &&
       rx == sent_rx && ry == sent_ry) return;
    sent_b = bb; sent_lt = lt; sent_rt = rt; sent_lx = lx; sent_ly = ly; sent_rx = rx; sent_ry = ry;
    eng_send("PAD %d %d %d %d %d %d %d", bb, lt, rt, lx, ly, rx, ry);
}

static void release_all(void);

/* ------------------------------------------------------------ vistas */

static void build(void);
static void show(int v) { view = v; build(); }

static void set_status(const char *s) { if(st_lbl) lv_label_set_text(st_lbl, s); }

static void on_refresh(lv_event_t *e)
{
    (void)e;
    host_ok = host_paired = apps_done = false;
    napps = 0;
    host_err[0] = 0;
    eng_send("HOST %s", HOST_USB);
    show(V_HOME);
}

static void on_pair(lv_event_t *e) { (void)e; eng_send("PAIR"); }

static void quality_load(void)
{
    char f[256];
    data_path(f, sizeof f, "remoteplay/quality");
    FILE *fp = fopen(f, "r");
    quality = 0;
    if(fp) { if(fscanf(fp, "%d", &quality) != 1) quality = 0; fclose(fp); }
    if(quality < 0 || quality >= NQUAL) quality = 0;
}

static void quality_save(void)
{
    char f[256];
    data_path(f, sizeof f, "remoteplay/quality");
    FILE *fp = fopen(f, "w");
    if(fp) { fprintf(fp, "%d\n", quality); fclose(fp); }
}

static void on_quality(lv_event_t *e)
{
    quality = (int)(intptr_t)lv_event_get_user_data(e);
    quality_save();
    lv_obj_t *pg = lv_obj_get_child(body, 0);
    int32_t y = pg ? lv_obj_get_scroll_y(pg) : 0;
    build();
    pg = lv_obj_get_child(body, 0);
    if(pg) { lv_obj_update_layout(pg); lv_obj_scroll_to_y(pg, y, LV_ANIM_OFF); }
}

static void start_app(int id)
{
    cur_app = id;
    session_video = true;
    starting = true;
    zoom_i = 0;
    sent_b = -1;
    show(V_VIDEO);                                       /* a caixa preta onde o motor desenha */
    eng_send("START %d %d %d %d %d 1 %d %d %d %d 1", id, qual[quality].w, qual[quality].h, qual[quality].fps,
             qual[quality].kbps, vr.x1, vr.y1, vr.x2 - vr.x1 + 1, vr.y2 - vr.y1 + 1);   /* 1: desblocagem */
}

static void on_app(lv_event_t *e) { start_app((int)(intptr_t)lv_event_get_user_data(e)); }

static void quit_answer(int b, void *ud)
{
    (void)ud;
    if(b == 0) { eng_send("QUITAPP"); }
}

static void on_quit_busy(lv_event_t *e)
{
    (void)e;
    const char *b[2] = { tr(S_BRP_QUIT_YES), tr(S_CANCEL) };
    dialog_open(BF_SYMBOL_ALERT, lv_color_hex(C_BRP), tr(S_BRP_QUIT_BUSY), tr(S_BRP_QUIT_BODY), b, 2, 1, 0,
                quit_answer, NULL);
}

static lv_obj_t *row(lv_obj_t *p)
{
    lv_obj_t *r = bf_plain(p);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, BF_SP_S, 0);
    return r;
}

static void build_home(void)
{
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(body);
    page_header(pg, LV_SYMBOL_SHUFFLE, lv_color_hex(C_BRP), tr(S_BRP_TITLE), tr(S_BRP_HINT));

    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(c, BF_SP_S, 0);
    lv_obj_t *r = row(c);
    page_round_icon(r, LV_SYMBOL_USB, lv_color_hex(C_BRP), 44);
    lv_obj_t *col = bf_plain(r);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    char b[200];
    snprintf(b, sizeof b, "%s  ·  %s", tr(S_BRP_HOST_USB), HOST_USB);
    bf_label(col, b, BF_FONT_M, p->text);
    if(host_err[0]) snprintf(b, sizeof b, tr(S_BRP_NO_HOST), HOST_USB);
    else if(!host_ok) snprintf(b, sizeof b, "%s", tr(S_BRP_CHECKING));
    else snprintf(b, sizeof b, "Sunshine %s  ·  %s%s", host_ver, host_paired ? tr(S_BRP_PAIRED) : tr(S_BRP_NOT_PAIRED),
                  host_busy ? tr(S_BRP_BUSY) : "");
    lv_obj_t *l = st_lbl = bf_label(col, b, BF_FONT_S, host_err[0] ? p->warning : p->text_dim);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);

    lv_obj_t *br = row(c);
    if(host_ok && !host_paired) {
        lv_obj_t *bp = bf_button(br, BF_SYMBOL_KEY, tr(S_BRP_PAIR), 1);
        lv_obj_add_event_cb(bp, on_pair, LV_EVENT_CLICKED, NULL);
    }
    if(host_ok && host_paired && host_busy) {
        lv_obj_t *bq = bf_button(br, LV_SYMBOL_STOP, tr(S_BRP_QUIT_BUSY), 2);
        lv_obj_add_event_cb(bq, on_quit_busy, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *bu = bf_button(br, LV_SYMBOL_REFRESH, tr(S_BRP_REFRESH), 0);
    lv_obj_add_event_cb(bu, on_refresh, LV_EVENT_CLICKED, NULL);

    if(host_ok && host_paired) {
        lv_obj_t *t = bf_label(pg, tr(S_BRP_APPS), BF_FONT_S, p->text_dim);
        lv_obj_set_style_pad_top(t, BF_SP_M, 0);
        lv_obj_t *n = bf_label(pg, tr(S_BRP_ONLY_CTRL_SUB), BF_FONT_S, p->text_dim);
        lv_obj_set_width(n, LV_PCT(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        if(!apps_done) bf_label(pg, tr(S_BRP_CHECKING), BF_FONT_M, p->text_dim);
        for(int i = 0; i < napps; i++) {
            lv_obj_t *ac = bf_card(pg);
            lv_obj_set_size(ac, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_t *ar = row(ac);
            lv_obj_t *an = bf_label(ar, apps[i].name, BF_FONT_M, p->text);
            lv_obj_set_flex_grow(an, 1);
            lv_obj_t *bv = bf_button(ar, LV_SYMBOL_PLAY, tr(S_BRP_OPEN), 1);
            lv_obj_add_event_cb(bv, on_app, LV_EVENT_CLICKED, (void *)(intptr_t)apps[i].id);
        }
        lv_obj_t *qt = bf_label(pg, tr(S_BRP_Q_TITLE), BF_FONT_S, p->text_dim);
        lv_obj_set_style_pad_top(qt, BF_SP_M, 0);
        for(int i = NQUAL - 1; i >= 0; i--)              /* alta, média, baixa */
            page_choice(pg, NULL, tr(qual[i].name), tr(qual[i].sub), i == quality, on_quality, (void *)(intptr_t)i);
        lv_obj_t *tip = bf_label(pg, tr(S_BRP_Q_TIP), BF_FONT_S, p->text_dim);
        lv_obj_set_width(tip, LV_PCT(100));
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    }
}

static void build_pin(const char *pin)
{
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(body);
    page_header(pg, BF_SYMBOL_KEY, lv_color_hex(C_BRP), tr(S_BRP_PIN_TITLE), tr(S_BRP_PIN_HELP));
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    pin_lbl = bf_label(c, pin, BF_FONT_CLOCK, lv_color_hex(C_BRP));
    lv_obj_center(pin_lbl);
    st_lbl = bf_label(pg, tr(S_BRP_PIN_WAIT), BF_FONT_M, p->text_dim);
    lv_obj_t *bu = bf_button(pg, LV_SYMBOL_CLOSE, tr(S_CANCEL), 0);
    lv_obj_add_event_cb(bu, on_refresh, LV_EVENT_CLICKED, NULL);
}

static void release_all(void)
{
    eng_send("RELEASE");                               /* botões, teclas e mouse soltos no PC */
    sent_b = -1;
}

static void on_end(lv_event_t *e)
{
    (void)e;
    release_all();
    eng_send("STOP");
    streaming = starting = false;
    perfui_stream(false);
    show(V_HOME);
}

/* ---- imagem do computador com o mouse ----
 * Janela: o vídeo em 16:9 o maior possível e, embaixo, uma barra pequena com as ações.
 * Tela cheia: o vídeo em 800x450 numa camada por cima de tudo; tocar na imagem clica no PC e faz a
 * barra subir de baixo por 4 s. Como na TV, a interface não se redesenha por baixo do vídeo
 * (lv_display_enable_invalidation); só enquanto a barra está à mostra, e o motor não pinta as
 * linhas que ela cobre (CLIP). */
#define BAR_H 48

static bool right_next, drag_mode, pressing;
static lv_point_t last_pt;                       /* para arrastar a imagem com zoom */
static double fps_now;
static lv_point_t press_pt;
static uint32_t press_t;
static int moved;
static lv_obj_t *rclick_btn, *drag_btn, *fs_btn_lbl;
static bool fs_on, bar_up;
static lv_obj_t *fs_ov, *fs_bar;
static lv_timer_t *bar_t;

static void send_rect(void)
{
    eng_send("RECT %d %d %d %d", vr.x1, vr.y1, vr.x2 - vr.x1 + 1, vr.y2 - vr.y1 + 1);
}

static void mouse_to(lv_point_t pt)
{
    eng_send("MABS %d %d %d %d", (int)(pt.x - vr.x1), (int)(pt.y - vr.y1), vr.x2 - vr.x1 + 1, vr.y2 - vr.y1 + 1);
}

static void side_paint(void)
{
    const bf_palette_t *p = pal();
    if(rclick_btn) lv_obj_set_style_bg_color(rclick_btn, right_next ? p->warning : p->surface2, 0);
    if(drag_btn) lv_obj_set_style_bg_color(drag_btn, drag_mode ? p->warning : p->surface2, 0);
}

static void bar_show(void);

/* tocar: o ponteiro vai até lá; toque curto clica (esquerdo, ou direito se armado), toque longo
 * parado é clique direito; com Arrastar, o botão esquerdo fica apertado enquanto o dedo anda */
static void on_video_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    static lv_point_t prev;
    last_pt = prev;
    prev = pt;
    if(code == LV_EVENT_PRESSED) {
        last_pt = pt;
        pressing = true;
        press_pt = pt;
        press_t = lv_tick_get();
        moved = 0;
        mouse_to(pt);
        if(drag_mode) eng_send("MBTN 1 1");
        if(fs_on) bar_show();
    } else if(code == LV_EVENT_PRESSING && pressing) {
        int d = abs(pt.x - press_pt.x) + abs(pt.y - press_pt.y);
        if(d > moved) moved = d;
        if(zoom_i > 0 && !drag_mode) {                   /* com zoom: a imagem acompanha o dedo */
            if(moved > 14 && (pt.x != last_pt.x || pt.y != last_pt.y))
                eng_send("PAN %d %d %d %d", (int)(pt.x - last_pt.x), (int)(pt.y - last_pt.y),
                         vr.x2 - vr.x1 + 1, vr.y2 - vr.y1 + 1);
        } else mouse_to(pt);
    } else if((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && pressing) {
        pressing = false;
        if(drag_mode) { eng_send("MBTN 1 0"); return; }
        if(code == LV_EVENT_PRESS_LOST || moved > 14) return;
        if(zoom_i > 0) mouse_to(pt);                     /* com zoom o ponteiro não seguiu o dedo */
        int b = right_next || lv_tick_elaps(press_t) > 600 ? 3 : 1;   /* 3: direito (Moonlight) */
        eng_send("MBTN %d 1", b);
        eng_send("MBTN %d 0", b);
        if(right_next) { right_next = false; side_paint(); }
    }
}

static void bar_touched(void) { if(bar_t) lv_timer_reset(bar_t); }   /* mexeu na barra: fica mais */

static void on_rclick(lv_event_t *e) { (void)e; right_next = !right_next; side_paint(); bar_touched(); }
static void on_drag(lv_event_t *e) { (void)e; drag_mode = !drag_mode; if(!drag_mode) eng_send("MBTN 1 0"); side_paint(); bar_touched(); }
static void on_scroll(lv_event_t *e) { eng_send("SCROLL %d", (int)(intptr_t)lv_event_get_user_data(e)); bar_touched(); }
static void on_release_bar(lv_event_t *e) { (void)e; release_all(); bar_touched(); }
static void fs_exit(void);

static void status_paint(void)
{
    if(!st_lbl) return;
    char b[48];
    if(zoom_i) snprintf(b, sizeof b, "%d q/s · %d%%", (int)(fps_now + 0.5), zooms[zoom_i]);
    else snprintf(b, sizeof b, tr(S_BRP_FPS), (int)(fps_now + 0.5));
    lv_label_set_text(st_lbl, b);
}

static void on_zoom(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int n = (int)(sizeof zooms / sizeof zooms[0]);
    zoom_i += d;
    if(zoom_i < 0) zoom_i = 0;
    if(zoom_i >= n) zoom_i = n - 1;
    eng_send("ZOOM %d", zooms[zoom_i]);
    status_paint();
    bar_touched();
}
static void on_end_video(lv_event_t *e) { fs_exit(); on_end(e); }

static lv_obj_t *bar_btn(lv_obj_t *par, const char *sym, bf_str_t txt, int kind, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = bf_button(par, NULL, NULL, kind);
    lv_obj_set_height(b, BAR_H - 8);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    char t[64];
    snprintf(t, sizeof t, "%s%s%s", sym ? sym : "", sym && txt ? " " : "", txt ? tr(txt) : "");
    lv_obj_t *l = bf_label(b, t, BF_FONT_S, pal()->text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if(txt == S_BRP_FULL || txt == S_BRP_WINDOW) fs_btn_lbl = l;
    return b;
}

static void on_fullscreen(lv_event_t *e);

/* a barra: igual na janela e na tela cheia */
static lv_obj_t *make_bar(lv_obj_t *par, int y, int w)
{
    lv_obj_t *bar = bf_plain(par);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_size(bar, w, BAR_H);
    lv_obj_set_style_bg_color(bar, pal()->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    bar_btn(bar, BF_SYMBOL_EXPAND, fs_on ? S_BRP_WINDOW : S_BRP_FULL, 1, on_fullscreen, NULL);
    rclick_btn = bar_btn(bar, NULL, S_BRP_RCLICK, 0, on_rclick, NULL);
    drag_btn = bar_btn(bar, NULL, S_BRP_DRAG, 0, on_drag, NULL);
    bar_btn(bar, LV_SYMBOL_UP, 0, 0, on_scroll, (void *)(intptr_t)2);
    bar_btn(bar, LV_SYMBOL_DOWN, 0, 0, on_scroll, (void *)(intptr_t)-2);
    bar_btn(bar, BF_SYMBOL_SEARCH " -", 0, 0, on_zoom, (void *)(intptr_t)-1);   /* lupa: afastar */
    bar_btn(bar, BF_SYMBOL_SEARCH " +", 0, 0, on_zoom, (void *)(intptr_t)1);    /* lupa: aproximar */
    bar_btn(bar, LV_SYMBOL_REFRESH, S_BRP_RELEASE_SHORT, 0, on_release_bar, NULL);
    bar_btn(bar, LV_SYMBOL_STOP, S_BRP_END, 2, on_end_video, NULL);
    st_lbl = bf_label(bar, starting ? "..." : "", BF_FONT_S, pal()->text_dim);
    side_paint();
    if(!starting) status_paint();
    return bar;
}

/* ---- tela cheia ---- */

/* o redesenho da interface por baixo do vídeo: o LVGL conta liga/desliga (inv_en_cnt); aqui
 * cada lado é chamado uma vez só, para o contador nunca passar de "desligado uma vez" */
static bool inv_off;

static void ui_draw(bool on)
{
    /* no aparelho, religar o redesenho não trazia a barra de volta (no simulador, sim); a camada
     * preta da tela cheia já cobre a interface, então o redesenho fica sempre ligado: no máximo
     * o relógio do alto, uma vez por minuto, pisca um pedaço preto até o quadro seguinte */
    (void)on;
    inv_off = false;
}

static void bar_y_anim(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }

static int bar_overlap(void)                     /* linhas do vídeo cobertas pela barra */
{
    int over = vr.y2 + 1 - (BF_SCREEN_H - BAR_H);
    return over > 0 ? over : 0;
}

static void bar_hidden(lv_anim_t *a)
{
    (void)a;
    if(!fs_bar) return;
    lv_obj_add_flag(fs_bar, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);                            /* preto embaixo antes de parar de desenhar */
    ui_draw(false);
    eng_send("CLIP 0");
    bar_up = false;
}

static void bar_hide(lv_timer_t *t)
{
    (void)t;
    if(bar_t) { lv_timer_delete(bar_t); bar_t = NULL; }
    if(!fs_bar || !bar_up) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, fs_bar);
    lv_anim_set_exec_cb(&a, bar_y_anim);
    lv_anim_set_values(&a, 0, BAR_H);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_completed_cb(&a, bar_hidden);
    lv_anim_start(&a);
}

static void bar_show(void)
{
    if(!fs_on || !fs_bar) return;
    if(bar_t) lv_timer_reset(bar_t);
    else bar_t = lv_timer_create(bar_hide, 4000, NULL);
    if(bar_up) return;
    bar_up = true;
    eng_send("CLIP %d", bar_overlap());           /* o motor não pinta onde a barra fica */
    ui_draw(true);
    lv_anim_delete(fs_bar, bar_y_anim);
    lv_obj_remove_flag(fs_bar, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;                                  /* sobe de baixo para cima */
    lv_anim_init(&a);
    lv_anim_set_var(&a, fs_bar);
    lv_anim_set_exec_cb(&a, bar_y_anim);
    lv_anim_set_values(&a, BAR_H, 0);
    lv_anim_set_duration(&a, 200);
    lv_anim_start(&a);
}

static void fs_exit(void)
{
    if(!fs_on) return;
    fs_on = bar_up = false;
    if(bar_t) { lv_timer_delete(bar_t); bar_t = NULL; }
    eng_send("CLIP 0");
    ui_draw(true);
    if(fs_bar) { lv_anim_delete(fs_bar, bar_y_anim); lv_obj_delete(fs_bar); }
    if(fs_ov) lv_obj_delete(fs_ov);
    fs_ov = fs_bar = NULL;
    lv_obj_invalidate(lv_screen_active());
}

static void build_video(void);

static void on_fullscreen(lv_event_t *e)
{
    (void)e;
    if(fs_on) {                                   /* volta à janela */
        fs_exit();
        build();
        return;
    }
    fs_on = true;
    fs_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(fs_ov);
    lv_obj_set_size(fs_ov, BF_SCREEN_W, BF_SCREEN_H);
    lv_obj_set_style_bg_color(fs_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fs_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(fs_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fs_ov, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(fs_ov, on_video_touch, LV_EVENT_ALL, NULL);
    int vh = BF_SCREEN_W * 9 / 16;                /* 800x450 */
    vr.x1 = 0; vr.x2 = BF_SCREEN_W - 1;
    vr.y1 = (BF_SCREEN_H - vh) / 2; vr.y2 = vr.y1 + vh - 1;
    /* a barra direto na camada de cima (irmã da camada preta), como o botão da tela cheia do
     * Internet: filha da camada preta ela não voltava a aparecer no aparelho durante a sessão */
    fs_bar = make_bar(lv_layer_top(), BF_SCREEN_H - BAR_H, BF_SCREEN_W);
    lv_obj_add_flag(fs_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(fs_bar, BAR_H, 0);
    send_rect();
    lv_refr_now(NULL);
    ui_draw(false);
}

static void build_video(void)
{
    int W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    /* a área toda acima da barra: o motor corta as faixas pretas e cobre a área sem esticar */
    int vw = W & ~3, vh = (H - BAR_H) & ~1, ah = vh;
    lv_obj_t *pane = bf_plain(body);
    lv_obj_set_size(pane, W, H);
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(pane, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pane, LV_OPA_COVER, 0);

    video_box = lv_obj_create(pane);
    lv_obj_remove_style_all(video_box);
    lv_obj_set_pos(video_box, (W - vw) / 2 & ~1, (ah - vh) / 2);
    lv_obj_set_size(video_box, vw, vh);
    lv_obj_set_style_bg_color(video_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(video_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(video_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(video_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(video_box, on_video_touch, LV_EVENT_ALL, NULL);
    make_bar(pane, H - BAR_H, W);
    lv_obj_update_layout(pane);
    lv_obj_get_coords(video_box, &vr);
    if(streaming) send_rect();                    /* voltou da tela cheia ou do controle */
}

static void build(void)
{
    if(!body) return;
    lv_obj_clean(body);
    if(view != V_VIDEO) fs_exit();
    st_lbl = pin_lbl = video_box = rclick_btn = drag_btn = fs_btn_lbl = NULL;
    pressing = false;
    /* na sessão o controle físico vai ao PC; o vídeo só é desenhado na vista dele */
    gamepad_capture(view == V_VIDEO, NULL);
    eng_send("SHOW %d", view == V_VIDEO ? 1 : 0);
    if(view == V_VIDEO) build_video();
    else if(view == V_HOME) build_home();
}

/* ------------------------------------------------------------ linhas do motor */

static void on_line(char *l)
{
    if(!strcmp(l, "READY")) { eng_send("HOST %s", HOST_USB); return; }
    if(!strncmp(l, "HOST ", 5)) {
        int paired = 0, busy = 0, cur = 0;
        char *f[5] = { 0 };
        char *s = l + 5;
        for(int i = 0; i < 5 && s; i++) { f[i] = s; s = strchr(s, '|'); if(s) *s++ = 0; }
        if(f[1]) paired = atoi(f[1]);
        if(f[2]) busy = atoi(f[2]);
        if(f[3]) cur = atoi(f[3]);
        (void)cur;
        snprintf(host_ver, sizeof host_ver, "%s", f[4] ? f[4] : "");
        char *dot = strstr(host_ver, ".-1");               /* "7.1.431.-1" */
        if(dot) *dot = 0;
        host_ok = true;
        host_paired = paired;
        host_busy = busy;
        host_err[0] = 0;
        napps = 0;
        apps_done = false;
        if(paired) eng_send("APPS");
        if(view == V_HOME) build();
        return;
    }
    if(!strncmp(l, "ERR host", 8)) {
        snprintf(host_err, sizeof host_err, "%s", l + 4);
        host_ok = false;
        if(view == V_HOME) build();
        return;
    }
    if(!strncmp(l, "PIN ", 4)) { view = V_PIN; lv_obj_clean(body); build_pin(l + 4); return; }
    if(!strncmp(l, "PAIR ok", 7)) {
        host_paired = true;
        eng_send("APPS");
        show(V_HOME);
        return;
    }
    if(!strncmp(l, "PAIR ", 5)) {
        if(st_lbl) { char b[160]; snprintf(b, sizeof b, tr(S_BRP_PAIR_FAIL), l + 5); lv_label_set_text(st_lbl, b); }
        return;
    }
    if(!strncmp(l, "APP ", 4)) {
        char *bar = strchr(l + 4, '|');
        if(bar && napps < MAXAPPS) {
            *bar = 0;
            apps[napps].id = atoi(l + 4);
            snprintf(apps[napps].name, sizeof apps[napps].name, "%s", bar + 1);
            napps++;
        }
        return;
    }
    if(!strcmp(l, "APPS fim")) { apps_done = true; if(view == V_HOME) build(); return; }
    if(!strcmp(l, "QUITAPP ok")) { eng_send("HOST %s", HOST_USB); return; }
    if(!strncmp(l, "STAGE ", 6)) { if(view == V_VIDEO) set_status(tr(S_BRP_STARTING)); return; }
    if(!strncmp(l, "VSTATS ", 7)) {
        fps_now = atof(l + 7);
        if(view == V_VIDEO) status_paint();
        return;
    }
    if(!strcmp(l, "STARTED")) {
        streaming = true;
        starting = false;
        pad_flush(true);
        perfui_stream(true);                             /* o modo performance com a imagem */
        if(view == V_VIDEO) set_status(tr(S_BRP_CONNECTED));
        return;
    }
    if(!strncmp(l, "TERMINATED", 10) || !strncmp(l, "FAILED", 6) || !strncmp(l, "ERR start", 9) ||
       !strncmp(l, "ERR conexão", 12)) {
        bool was = streaming || starting;
        streaming = starting = false;
        perfui_stream(false);
        if(was) {
            char b[160];
            snprintf(b, sizeof b, tr(S_BRP_ENDED), l);
            show(V_HOME);
            set_status(b);
        }
        return;
    }
}

static void io_tick(lv_timer_t *t)
{
    (void)t;
    if(from_eng < 0) return;
    for(;;) {
        ssize_t n = read(from_eng, inbuf + inlen, sizeof inbuf - 1 - (size_t)inlen);
        if(n <= 0) {
            if(n == 0 || (errno != EAGAIN && errno != EINTR)) {   /* o motor saiu */
                close(from_eng);
                from_eng = -1;
                streaming = starting = false;
                snprintf(host_err, sizeof host_err, "motor");
                host_ok = false;
                if(view != V_HOME) show(V_HOME);
                else build();
            }
            return;
        }
        inlen += (int)n;
        inbuf[inlen] = 0;
        char *s = inbuf, *nl;
        while((nl = strchr(s, '\n'))) { *nl = 0; if(*s) on_line(s); s = nl + 1; }
        inlen = (int)strlen(s);
        memmove(inbuf, s, (size_t)inlen + 1);
        if(inlen > (int)sizeof inbuf - 256) inlen = 0;
    }
}

static void pad_tick(lv_timer_t *t)
{
    (void)t;
    pad_flush(false);                                    /* controle físico: o estado a cada 8 ms */
    if(view == V_VIDEO) {                                /* algo visível por cima: o vídeo para */
        static int shown = -1;
        lv_obj_t *top = lv_layer_top();
        int busy = 0;
        for(uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
            lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
            if(c != fs_ov && c != fs_bar && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) busy = 1;
        }
        if(busy == shown) { /* igual */ }
        else { shown = busy; eng_send("SHOW %d", !busy); if(!busy && video_box) lv_obj_invalidate(video_box); }
    }
}

/* roteiro de capturas (core/screenshot.c) e ctl "brp ...": linhas como se viessem do motor;
 * "+video" abre a sessão como se tivesse conectado, "+full"/"+bar"/"+window" a tela cheia */
void scr_brp_demo(const char *line)
{
    if(!body) return;
    if(!strcmp(line, "+video")) { session_video = true; view = V_VIDEO; streaming = true; starting = false; build(); on_line((char[]){"VSTATS 27.5 0 36"}); return; }
    if(!strcmp(line, "+full")) { on_fullscreen(NULL); return; }
    if(!strcmp(line, "+bar")) { bar_show(); return; }
    if(!strcmp(line, "+window")) { fs_exit(); build(); return; }
    char b[256];
    snprintf(b, sizeof b, "%s", line);
    on_line(b);
}

/* ------------------------------------------------------------ tela */

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    root = parent;
    body = bf_plain(parent);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_update_layout(body);
    view = V_HOME;
    host_ok = host_paired = host_busy = apps_done = streaming = starting = false;
    napps = 0;
    host_err[0] = host_ver[0] = 0;
    session_video = right_next = drag_mode = false;
    quality_load();
    char err[160] = "";
    if(!eng_start(err, sizeof err)) snprintf(host_err, sizeof host_err, "%s", err[0] ? err : "motor");
    io_t = lv_timer_create(io_tick, 30, NULL);
    pad_t = lv_timer_create(pad_tick, 8, NULL);
    build();
}

static void destroy(void)
{
    if(io_t) { lv_timer_delete(io_t); io_t = NULL; }
    if(pad_t) { lv_timer_delete(pad_t); pad_t = NULL; }
    gamepad_capture(false, NULL);
    fs_exit();
    streaming = starting = false;
    eng_stop();                                          /* RELEASE + STOP + EOF: nada preso no PC */
    root = body = st_lbl = pin_lbl = video_box = rclick_btn = drag_btn = NULL;
    perfui_stream(false);
}

const bf_screen_def_t scr_brp_def = { create, destroy, S_BRP_TITLE, NULL };
