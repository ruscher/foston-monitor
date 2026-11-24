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
#include "../widgets/dialog.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define C_BRP     0x6a3df0
#define HOST_USB  "192.168.7.1"
#define MAXAPPS   32

/* Moonlight: botões do controle (Limelight.h) */
enum { F_UP = 0x0001, F_DOWN = 0x0002, F_LEFT = 0x0004, F_RIGHT = 0x0008, F_START = 0x0010, F_BACK = 0x0020,
       F_LS = 0x0040, F_RS = 0x0080, F_LB = 0x0100, F_RB = 0x0200, F_GUIDE = 0x0400,
       F_A = 0x1000, F_B = 0x2000, F_X = 0x4000, F_Y = 0x8000,
       F_LT = 0x10000, F_RT = 0x20000 };                   /* gatilhos: só aqui, viram 0/255 */

enum { V_HOME, V_PIN, V_PLAY };

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

/* ------------------------------------------------------------ estado do controle */

static int v_held, v_lock;                     /* botões do toque: segurado agora / travados */
static bool lock_mode;
static int stick_x, stick_y;                   /* analógico esquerdo virtual */
static bool stick_locked;
static int sent_b = -1, sent_lt, sent_rt, sent_lx, sent_ly, sent_rx, sent_ry;
static char phys_name[64];

static void pad_flush(bool force)
{
    if(!streaming) return;
    int b = v_held | v_lock;
    int lt = (b & F_LT) ? 255 : 0, rt = (b & F_RT) ? 255 : 0;
    int lx = stick_x, ly = stick_y, rx = 0, ry = 0;
    phys_name[0] = 0;
    gp_info_t gi;
    if(gamepad_count() > 0 && gamepad_info(0, &gi)) {     /* controle físico: soma ao virtual */
        snprintf(phys_name, sizeof phys_name, "%s", gi.name);
        static const struct { int pc, f; } m[] = {
            { PC_A, F_A }, { PC_B, F_B }, { PC_X, F_X }, { PC_Y, F_Y }, { PC_BACK, F_BACK }, { PC_GUIDE, F_GUIDE },
            { PC_START, F_START }, { PC_LSTICK, F_LS }, { PC_RSTICK, F_RS }, { PC_LSHOULDER, F_LB },
            { PC_RSHOULDER, F_RB }, { PC_DPUP, F_UP }, { PC_DPDOWN, F_DOWN }, { PC_DPLEFT, F_LEFT },
            { PC_DPRIGHT, F_RIGHT } };
        for(unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if(gi.btn[m[i].pc]) b |= m[i].f;
        int ax = gi.ax[PC_LEFTX - PC__NBTN], ay = gi.ax[PC_LEFTY - PC__NBTN];
        if(ax || ay) { lx = ax * 32767 / 1000; ly = -ay * 32767 / 1000; }   /* Moonlight: para cima é + */
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

static void start_app(int id)
{
    cur_app = id;
    starting = true;
    sent_b = -1;
    v_held = v_lock = 0;
    stick_x = stick_y = 0;
    stick_locked = false;
    /* Somente Controle: o menor fluxo de vídeo (descartado sem decodificar) */
    eng_send("START %d 640 360 30 500 0", id);
    show(V_PLAY);
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
        for(int i = 0; i < napps; i++)
            page_choice(pg, LV_SYMBOL_PLAY, apps[i].name, tr(S_BRP_ONLY_CTRL), false, on_app, (void *)(intptr_t)apps[i].id);
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

/* ---- controle na tela ---- */

typedef struct { lv_obj_t *o; int flag; } vbtn_t;
#define NVB 17
static vbtn_t vb[NVB];
static int nvb;
static lv_obj_t *lock_btn, *stick_area, *stick_knob;

static void vb_paint(void)
{
    const bf_palette_t *p = pal();
    for(int i = 0; i < nvb; i++) {
        bool on = (v_held | v_lock) & vb[i].flag;
        bool locked = v_lock & vb[i].flag;
        lv_obj_set_style_bg_color(vb[i].o, on ? lv_color_hex(C_BRP) : p->surface, 0);
        lv_obj_set_style_bg_opa(vb[i].o, on ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_border_width(vb[i].o, locked ? 3 : 1, 0);
        lv_obj_set_style_border_color(vb[i].o, locked ? p->warning : p->border, 0);
    }
    if(lock_btn) {
        lv_obj_set_style_bg_color(lock_btn, lock_mode ? p->warning : p->surface, 0);
        lv_obj_set_style_text_color(lock_btn, lock_mode ? lv_color_black() : p->text, 0);
    }
}

static void on_vb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        if(lock_mode) v_lock ^= f;                       /* travar: liga/desliga e fica */
        else v_held |= f;
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        v_held &= ~f;
    } else return;
    vb_paint();
    pad_flush(false);
}

static lv_obj_t *vbutton(lv_obj_t *par, const char *txt, int flag, int x, int y, int w, int h, bool round)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, round ? LV_RADIUS_CIRCLE : BF_RADIUS_M, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = bf_label(o, txt, BF_FONT_M, pal()->text);
    lv_obj_center(l);
    lv_obj_add_event_cb(o, on_vb, LV_EVENT_ALL, (void *)(intptr_t)flag);
    if(nvb < NVB) { vb[nvb].o = o; vb[nvb].flag = flag; nvb++; }
    return o;
}

static void stick_paint(void)
{
    if(!stick_knob) return;
    int r = lv_obj_get_width(stick_area) / 2 - lv_obj_get_width(stick_knob) / 2;
    lv_obj_align(stick_knob, LV_ALIGN_CENTER, stick_x * r / 32767, -stick_y * r / 32767);
    lv_obj_set_style_border_color(stick_area, stick_locked ? pal()->warning : pal()->border, 0);
}

/* analógico num ponto só: arrastar orienta; soltar volta ao centro (com Travar, fica onde está) */
static void on_stick(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        lv_area_t a;
        lv_obj_get_coords(stick_area, &a);
        int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, rad = (a.x2 - a.x1) / 2;
        int dx = pt.x - cx, dy = cy - pt.y;
        int dz = rad * 15 / 100;                         /* zona morta: tremor do resistivo */
        if(dx * dx + dy * dy < dz * dz) dx = dy = 0;
        if(dx > rad) dx = rad;
        if(dx < -rad) dx = -rad;
        if(dy > rad) dy = rad;
        if(dy < -rad) dy = -rad;
        stick_x = dx * 32767 / rad;
        stick_y = dy * 32767 / rad;
        stick_locked = false;
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(lock_mode && (stick_x || stick_y)) stick_locked = true;
        else stick_x = stick_y = 0;
    } else return;
    stick_paint();
    pad_flush(false);
}

static void release_all(void)
{
    v_held = v_lock = 0;
    stick_x = stick_y = 0;
    stick_locked = false;
    eng_send("RELEASE");
    sent_b = -1;
    vb_paint();
    stick_paint();
}

static void on_lock(lv_event_t *e) { (void)e; lock_mode = !lock_mode; vb_paint(); }
static void on_release(lv_event_t *e) { (void)e; release_all(); }
static void on_end(lv_event_t *e)
{
    (void)e;
    release_all();
    eng_send("STOP");
    streaming = starting = false;
    show(V_HOME);
}

static void build_play(void)
{
    const bf_palette_t *p = pal();
    nvb = 0;
    int W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *pad = bf_plain(body);
    lv_obj_set_size(pad, W, H);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    /* faixa de cima: estado e as ações da sessão */
    lv_obj_t *top = row(pad);
    lv_obj_set_width(top, W - 16);
    lv_obj_set_pos(top, 8, 4);
    st_lbl = bf_label(top, starting ? tr(S_BRP_STARTING) : "", BF_FONT_S, p->text_dim);
    lv_obj_set_flex_grow(st_lbl, 1);
    lock_btn = bf_button(top, BF_SYMBOL_LOCK, tr(S_BRP_LOCK), 0);
    lv_obj_set_height(lock_btn, 40);
    lv_obj_add_event_cb(lock_btn, on_lock, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rb = bf_button(top, LV_SYMBOL_REFRESH, tr(S_BRP_RELEASE), 0);
    lv_obj_set_height(rb, 40);
    lv_obj_add_event_cb(rb, on_release, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eb = bf_button(top, LV_SYMBOL_STOP, tr(S_BRP_END), 2);
    lv_obj_set_height(eb, 40);
    lv_obj_add_event_cb(eb, on_end, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(top);
    int y0 = lv_obj_get_y2(top) + 8, ah = H - y0 - 6;    /* área do controle: abaixo da faixa real */
    int s = ah * 30 / 100;                                /* botão do direcional / da face */
    if(s > 70) s = 70;
    int cyc = y0 + ah / 2 + 10;
    /* esquerda: LB LT em cima, direcional */
    vbutton(pad, "LB", F_LB, 16, y0, 3 * s / 2, 40, false);
    vbutton(pad, "LT", F_LT, 16 + 3 * s / 2 + 8, y0, 3 * s / 2, 40, false);
    int dx = 16, dy = cyc - 3 * s / 2 + 22;
    vbutton(pad, LV_SYMBOL_UP, F_UP, dx + s, dy, s, s, false);
    vbutton(pad, LV_SYMBOL_LEFT, F_LEFT, dx, dy + s, s, s, false);
    vbutton(pad, LV_SYMBOL_RIGHT, F_RIGHT, dx + 2 * s, dy + s, s, s, false);
    vbutton(pad, LV_SYMBOL_DOWN, F_DOWN, dx + s, dy + 2 * s, s, s, false);
    /* direita: RT RB em cima, A B X Y em losango (posição do Xbox) */
    vbutton(pad, "RT", F_RT, W - 16 - 3 * s - 8, y0, 3 * s / 2, 40, false);
    vbutton(pad, "RB", F_RB, W - 16 - 3 * s / 2, y0, 3 * s / 2, 40, false);
    int fx = W - 16 - 3 * s, fy = dy;
    vbutton(pad, "Y", F_Y, fx + s, fy, s, s, true);
    vbutton(pad, "X", F_X, fx, fy + s, s, s, true);
    vbutton(pad, "B", F_B, fx + 2 * s, fy + s, s, s, true);
    vbutton(pad, "A", F_A, fx + s, fy + 2 * s, s, s, true);
    /* centro: Back Guia Start, e o analógico esquerdo */
    int mid = W / 2;
    vbutton(pad, "Back", F_BACK, mid - 140, y0, 84, 40, false);
    vbutton(pad, LV_SYMBOL_HOME, F_GUIDE, mid - 42, y0, 84, 40, false);
    vbutton(pad, "Start", F_START, mid + 56, y0, 84, 40, false);
    int sd = ah - 56;
    if(sd > 3 * s) sd = 3 * s;
    if(sd > W - 2 * (16 + 3 * s) - 24) sd = W - 2 * (16 + 3 * s) - 24;   /* entre o direcional e a face */
    stick_area = lv_obj_create(pad);
    lv_obj_remove_style_all(stick_area);
    lv_obj_set_size(stick_area, sd, sd);
    lv_obj_set_pos(stick_area, mid - sd / 2, y0 + 50);
    lv_obj_set_style_radius(stick_area, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stick_area, p->surface, 0);
    lv_obj_set_style_bg_opa(stick_area, LV_OPA_60, 0);
    lv_obj_set_style_border_width(stick_area, 2, 0);
    lv_obj_add_flag(stick_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(stick_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(stick_area, on_stick, LV_EVENT_ALL, NULL);
    stick_knob = lv_obj_create(stick_area);
    lv_obj_remove_style_all(stick_knob);
    lv_obj_set_size(stick_knob, sd / 3, sd / 3);
    lv_obj_set_style_radius(stick_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stick_knob, lv_color_hex(C_BRP), 0);
    lv_obj_set_style_bg_opa(stick_knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(stick_knob, LV_OBJ_FLAG_CLICKABLE);
    vb_paint();
    stick_paint();
}

static void build(void)
{
    if(!body) return;
    lv_obj_clean(body);
    st_lbl = pin_lbl = lock_btn = stick_area = stick_knob = NULL;
    nvb = 0;
    gamepad_capture(view == V_PLAY, NULL);              /* na sessão o controle físico vai ao PC */
    if(view == V_PLAY) build_play();
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
    if(!strncmp(l, "STAGE ", 6)) { if(view == V_PLAY) set_status(tr(S_BRP_STARTING)); return; }
    if(!strcmp(l, "STARTED")) {
        streaming = true;
        starting = false;
        pad_flush(true);
        if(view == V_PLAY) set_status(tr(S_BRP_CONNECTED));
        return;
    }
    if(!strncmp(l, "TERMINATED", 10) || !strncmp(l, "FAILED", 6) || !strncmp(l, "ERR start", 9) ||
       !strncmp(l, "ERR conexão", 12)) {
        bool was = streaming || starting;
        streaming = starting = false;
        v_held = v_lock = stick_x = stick_y = 0;
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
    if(view == V_PLAY && streaming && st_lbl) {
        static char last[96];
        char b[96];
        if(phys_name[0]) snprintf(b, sizeof b, tr(S_BRP_PHYS), phys_name);
        else snprintf(b, sizeof b, "%s", tr(S_BRP_CONNECTED));
        if(strcmp(b, last)) { snprintf(last, sizeof last, "%s", b); lv_label_set_text(st_lbl, b); }
    }
}

/* roteiro de capturas (core/screenshot.c): linhas como se viessem do motor; "+play" abre a
 * sessão como se tivesse conectado */
void scr_brp_demo(const char *line)
{
    if(!body) return;
    if(!strcmp(line, "+play")) { view = V_PLAY; streaming = true; starting = false; build(); set_status(tr(S_BRP_CONNECTED)); return; }
    if(!strcmp(line, "+lock")) { lock_mode = true; v_lock = F_RIGHT | F_A; stick_x = 20000; stick_y = 12000; stick_locked = true; vb_paint(); stick_paint(); return; }
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
    lock_mode = false;
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
    streaming = starting = false;
    eng_stop();                                          /* RELEASE + STOP + EOF: nada preso no PC */
    root = body = st_lbl = pin_lbl = lock_btn = stick_area = stick_knob = NULL;
    nvb = 0;
}

const bf_screen_def_t scr_brp_def = { create, destroy, S_BRP_TITLE, NULL };
