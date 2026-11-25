/* scr_term.c — Terminal: shell interativo num pseudo-terminal.
 *
 * /bin/sh -l num PTY (forkpty) com TERM=vt100, emulação em core/term.c.
 * Mesma regra do Run: se o shell gráfico roda como root, o terminal usa
 * $BIGFOSTON_RUN_USER (padrão "bigfoston"); root só com BIGFOSTON_ALLOW_ROOT=1.
 * A sessão continua ao sair da tela (um timer global lê o PTY); a grade é
 * desenhada por um único objeto que só redesenha as linhas que mudaram.
 * Teclado próprio: letras/números, símbolos, Esc, Tab, Ctrl (fixo), setas. Com um teclado
 * físico (core/hwkbd.c) todas as teclas vão para o shell e o da tela some na primeira. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/term.h"
#include "../core/runner.h"
#include "../core/hwkbd.h"
#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define KB_H     204
#define BOTTOM_H 72          /* barra inferior do shell, coberta pelo teclado */

static term_t term;
static int master = -1;
static pid_t child;
static bool ended;
static char who[64];
static lv_timer_t *pump_timer;

static lv_obj_t *grid, *kb, *ended_lbl;
static int char_w, line_h;
static bool kb_shown = true, shift, ctrl, symbols;

/* ---------------- processo ---------------- */

static void set_winsize(void)
{
    if(master < 0) return;
    struct winsize ws = { .ws_row = (unsigned short)term.rows, .ws_col = (unsigned short)term.cols };
    ioctl(master, TIOCSWINSZ, &ws);
}

static void send_bytes(const char *s, int n)
{
    if(master < 0 || ended) return;
    while(n > 0) {
        ssize_t w = write(master, s, (size_t)n);
        if(w < 0) { if(errno == EINTR) continue; if(errno == EAGAIN) { usleep(2000); continue; } return; }
        s += w;
        n -= (int)w;
    }
}

static void invalidate_dirty(void)
{
    if(!grid) return;
    lv_area_t a;
    lv_obj_get_coords(grid, &a);
    for(int y = 0; y < term.rows; y++) {
        if(!term.dirty[y]) continue;
        lv_area_t r = { a.x1, a.y1 + y * line_h, a.x2, a.y1 + (y + 1) * line_h - 1 };
        lv_obj_invalidate_area(grid, &r);
    }
}

static void pump(lv_timer_t *t)
{
    (void)t;
    if(master < 0) return;
    char buf[4096];
    int cx = term.cx, cy = term.cy;
    for(int k = 0; k < 4; k++) {                /* até 16 KB por tique */
        ssize_t n = read(master, buf, sizeof buf);
        if(n > 0) { term_feed(&term, buf, (int)n); continue; }
        if(n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        /* EIO/0: o shell saiu */
        if(!ended) {
            ended = true;
            waitpid(child, NULL, WNOHANG);
            if(ended_lbl) lv_obj_remove_flag(ended_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    }
    char rep[32];
    int rn = term_take_reply(&term, rep, sizeof rep);
    if(rn) send_bytes(rep, rn);
    if(cy != term.cy || cx != term.cx) { term.dirty[cy] = true; term.dirty[term.cy] = true; }
    invalidate_dirty();
    if(!grid) for(int y = 0; y < term.rows; y++) term.dirty[y] = false;   /* fora da tela */
}

static bool start_shell(char *msg, size_t msgn)
{
    uid_t uid;
    gid_t gid;
    char home[256] = "/", name[64] = "";
    int rc = runner_resolve_user(&uid, &gid, home, sizeof home, name, sizeof name, msg, msgn);
    if(rc) return false;
    struct winsize ws = { .ws_row = (unsigned short)term.rows, .ws_col = (unsigned short)term.cols };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if(pid < 0) { snprintf(msg, msgn, "forkpty: %s", strerror(errno)); return false; }
    if(pid == 0) {
        if(uid != (uid_t)-1) runner_child_setup(uid, gid, home, name);
        else { signal(SIGPIPE, SIG_DFL); signal(SIGCHLD, SIG_DFL); }
        setenv("TERM", "vt100", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        execl("/bin/sh", "sh", "-l", (char *)NULL);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    master = fd;
    child = pid;
    ended = false;
    snprintf(who, sizeof who, "%s", uid != (uid_t)-1 ? name : (geteuid() ? (getenv("USER") ? getenv("USER") : "?") : "root"));
    if(!pump_timer) pump_timer = lv_timer_create(pump, 40, NULL);
    return true;
}

static void restart_shell(void)
{
    if(master >= 0) { close(master); master = -1; }
    if(child > 0) { kill(child, SIGHUP); waitpid(child, NULL, 0); child = 0; }
    term_init(&term, term.cols, term.rows);
    char msg[160];
    if(!start_shell(msg, sizeof msg)) term_feed(&term, msg, (int)strlen(msg));
    if(ended_lbl) lv_obj_add_flag(ended_lbl, LV_OBJ_FLAG_HIDDEN);
    if(grid) lv_obj_invalidate(grid);
}

/* ---------------- desenho ---------------- */

static void grid_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const bf_palette_t *p = pal();
    lv_area_t a, clip;
    lv_obj_get_coords(obj, &a);
    clip = layer->_clip_area;

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = BF_FONT_MONO;
    ld.text_local = 1;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;

    for(int y = 0; y < term.rows; y++) {
        int y1 = a.y1 + y * line_h, y2 = y1 + line_h - 1;
        if(y2 < clip.y1 || y1 > clip.y2) continue;
        /* séries de células com o mesmo atributo */
        int x = 0;
        while(x < term.cols) {
            uint8_t at = term.cell[y][x].attr;
            bool cur = term.cursor_visible && y == term.cy && x == term.cx && !ended;
            bool inv = ((at & TA_INVERSE) != 0) != cur;
            int x0 = x;
            char txt[TERM_MAX_COLS * 4 + 1];
            int tl = 0;
            while(x < term.cols) {
                uint8_t at2 = term.cell[y][x].attr;
                bool cur2 = term.cursor_visible && y == term.cy && x == term.cx && !ended;
                bool inv2 = ((at2 & TA_INVERSE) != 0) != cur2;
                if(inv2 != inv) break;
                if((at2 & TA_BOLD) != (at & TA_BOLD)) break;
                uint32_t c = term.cell[y][x].ch;
                if(c < 0x80) txt[tl++] = (char)(c ? c : ' ');
                else if(c < 0x800) { txt[tl++] = (char)(0xc0 | c >> 6); txt[tl++] = (char)(0x80 | (c & 0x3f)); }
                else if(c < 0x10000) { txt[tl++] = (char)(0xe0 | c >> 12); txt[tl++] = (char)(0x80 | ((c >> 6) & 0x3f)); txt[tl++] = (char)(0x80 | (c & 0x3f)); }
                else txt[tl++] = '?';
                x++;
            }
            txt[tl] = 0;
            lv_area_t r = { a.x1 + x0 * char_w, y1, a.x1 + x * char_w - 1, y2 };
            if(inv) {
                rd.bg_color = p->text;
                lv_draw_rect(layer, &rd, &r);
                ld.color = p->bg;
            }
            else ld.color = (at & TA_BOLD) ? p->text : lv_color_mix(p->text, p->text_dim, 200);
            /* espaços no fim de uma série normal não precisam ser desenhados */
            int end = tl;
            if(!inv) while(end > 0 && txt[end - 1] == ' ') end--;
            if(end) {
                txt[end] = 0;
                ld.text = txt;
                lv_draw_label(layer, &ld, &r);
            }
        }
        term.dirty[y] = false;
    }
}

static void fit_grid(void)
{
    if(!grid) return;
    int h = BF_CONTENT_H - (kb_shown ? KB_H - BOTTOM_H : 0) - 4;
    int rows = h / line_h, cols = (BF_SCREEN_W - 8) / char_w;
    lv_obj_set_size(grid, cols * char_w, rows * line_h);
    if(rows != term.rows || cols != term.cols) {
        term_resize(&term, cols, rows);
        set_winsize();
    }
    lv_obj_invalidate(grid);
}

/* ---------------- teclado ---------------- */

static const char *const map_abc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "-", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", LV_SYMBOL_NEW_LINE, "\n",
    "Shift", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "\n",
    "Ctrl", "Esc", "Tab", "?123", " ", LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_UP, LV_SYMBOL_RIGHT, LV_SYMBOL_KEYBOARD, "",
};
static const char *const map_sym[] = {
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", LV_SYMBOL_BACKSPACE, "\n",
    "~", "`", "|", "\\", "{", "}", "[", "]", "<", ">", "=", "\n",
    "+", "_", "\"", "'", ":", "?", "/", ".", ",", "-", LV_SYMBOL_NEW_LINE, "\n",
    "Shift", "ç", "ã", "é", "ó", "á", "í", "ú", "õ", "ê", "à", "\n",
    "Ctrl", "Esc", "Tab", "abc", " ", LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_UP, LV_SYMBOL_RIGHT, LV_SYMBOL_KEYBOARD, "",
};
static const lv_buttonmatrix_ctrl_t ctrl_map[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
    LV_BUTTONMATRIX_CTRL_CHECKABLE | 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_BUTTONMATRIX_CTRL_CHECKABLE | 2, 2, 2, 2, 5, 1, 1, 1, 1, 2,
};

static void set_map(void)
{
    lv_buttonmatrix_set_map(kb, symbols ? map_sym : map_abc);
    lv_buttonmatrix_set_ctrl_map(kb, ctrl_map);
}

static void kb_event(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *k = lv_buttonmatrix_get_button_text(kb, id);
    if(!k) return;
    (void)e;
    if(!strcmp(k, "Shift")) { shift = !shift; return; }
    if(!strcmp(k, "Ctrl")) { ctrl = !ctrl; return; }
    if(!strcmp(k, "?123") || !strcmp(k, "abc")) {
        symbols = !symbols;
        set_map();
        shift = ctrl = false;
        return;
    }
    if(!strcmp(k, LV_SYMBOL_KEYBOARD)) {
        kb_shown = false;
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        fit_grid();
        return;
    }
    if(ended) { restart_shell(); return; }
    const char *seq = NULL;
    char one[8];
    if(!strcmp(k, LV_SYMBOL_BACKSPACE)) seq = "\x7f";
    else if(!strcmp(k, LV_SYMBOL_NEW_LINE)) seq = "\r";
    else if(!strcmp(k, "Esc")) seq = "\x1b";
    else if(!strcmp(k, "Tab")) seq = "\t";
    else if(!strcmp(k, LV_SYMBOL_UP)) seq = term.app_cursor ? "\x1bOA" : "\x1b[A";
    else if(!strcmp(k, LV_SYMBOL_DOWN)) seq = term.app_cursor ? "\x1bOB" : "\x1b[B";
    else if(!strcmp(k, LV_SYMBOL_RIGHT)) seq = term.app_cursor ? "\x1bOC" : "\x1b[C";
    else if(!strcmp(k, LV_SYMBOL_LEFT)) seq = term.app_cursor ? "\x1bOD" : "\x1b[D";
    else {
        snprintf(one, sizeof one, "%s", k);
        if(one[0] && !one[1]) {
            char c = one[0];
            if(shift && c >= 'a' && c <= 'z') c = (char)(c - 32);
            if(ctrl) {
                if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
                else if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
                else if(c == '[') c = 0x1b;
                else if(c == '\\') c = 0x1c;
            }
            one[0] = c;
        }
        seq = one;
    }
    send_bytes(seq, (int)strlen(seq));
    /* modificadores valem para uma tecla */
    if(shift || ctrl) {
        shift = ctrl = false;
        for(uint32_t i = 0;; i++) {
            const char *t = lv_buttonmatrix_get_button_text(kb, i);
            if(!t) break;
            if(!strcmp(t, "Shift") || !strcmp(t, "Ctrl")) lv_buttonmatrix_clear_button_ctrl(kb, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }
}

/* teclado físico: a tecla vira os bytes que um terminal VT100 mandaria */
static bool hw_key(const hwkey_t *k)
{
    if(ended) { if(!k->repeat) restart_shell(); return true; }
    if(kb_shown && kb) {                              /* digitando no teclado de verdade: mais linhas */
        kb_shown = false;
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        fit_grid();
    }
    char buf[16];
    const char *seq = NULL;
    bool ac = term.app_cursor;
    switch(k->code) {
    case KEY_ENTER: seq = "\r"; break;
    case KEY_BACKSPACE: seq = (k->mods & HK_ALT) ? "\x1b\x7f" : "\x7f"; break;
    case KEY_TAB: seq = (k->mods & HK_SHIFT) ? "\x1b[Z" : "\t"; break;
    case KEY_ESC: seq = "\x1b"; break;
    case KEY_UP: seq = ac ? "\x1bOA" : "\x1b[A"; break;
    case KEY_DOWN: seq = ac ? "\x1bOB" : "\x1b[B"; break;
    case KEY_RIGHT: seq = ac ? "\x1bOC" : "\x1b[C"; break;
    case KEY_LEFT: seq = ac ? "\x1bOD" : "\x1b[D"; break;
    case KEY_HOME: seq = "\x1b[1~"; break;
    case KEY_END: seq = "\x1b[4~"; break;
    case KEY_INSERT: seq = "\x1b[2~"; break;
    case KEY_DELETE: seq = "\x1b[3~"; break;
    case KEY_PAGEUP: seq = "\x1b[5~"; break;
    case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
    case KEY_F1: seq = "\x1bOP"; break;
    case KEY_F2: seq = "\x1bOQ"; break;
    case KEY_F3: seq = "\x1bOR"; break;
    case KEY_F4: seq = "\x1bOS"; break;
    default: break;
    }
    if(!seq && k->text[0]) seq = k->text;
    if(!seq && (k->mods & (HK_CTRL | HK_ALT))) {
        /* Ctrl+letra = 1..26; Ctrl+[ \ ] espaço; Alt+tecla = ESC antes */
        static const char letters[] = "qwertyuiopasdfghjklzxcvbnm";
        static const uint16_t codes[] = { KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
                                          KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
                                          KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M };
        char c = 0;
        for(size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) if(codes[i] == k->code) c = letters[i];
        if(!c && k->code >= KEY_1 && k->code <= KEY_0) c = "1234567890"[k->code - KEY_1];
        if(!c && k->code == KEY_SPACE) c = ' ';
        if(!c && k->code == KEY_MINUS) c = '-';
        if(!c && k->code == KEY_DOT) c = '.';
        if(!c && k->code == KEY_SLASH) c = '/';
        if(!c) return false;
        int n = 0;
        if(k->mods & HK_ALT) buf[n++] = 0x1b;
        if(k->mods & HK_CTRL) {
            if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
            else if(c == ' ' || c == '2') c = 0;
            else if(c == '3') c = 0x1b;
            else if(c == '4') c = 0x1c;
            else if(c == '5') c = 0x1d;
            else if(c == '6') c = 0x1e;
            else if(c == '7' || c == '/' || c == '-') c = 0x1f;
            else if(c == '8') c = 0x7f;
        }
        else if(k->mods & HK_SHIFT && c >= 'a' && c <= 'z') c = (char)(c - 32);
        buf[n++] = c;
        send_bytes(buf, n);
        return true;
    }
    if(!seq) return false;
    send_bytes(seq, (int)strlen(seq));
    return true;
}

static void grid_click(lv_event_t *e)
{
    (void)e;
    if(ended) { restart_shell(); return; }
    kb_shown = !kb_shown;
    if(kb_shown) lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    fit_grid();
}

/* ---------------- tela ---------------- */

static const char *title_fn(int arg)
{
    (void)arg;
    static char t[96];
    snprintf(t, sizeof t, "%s  —  %s", tr(S_APP_TERMINAL), who[0] ? who : "?");
    return t;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    char_w = lv_font_get_glyph_width(BF_FONT_MONO, 'M', 0);
    line_h = lv_font_get_line_height(BF_FONT_MONO);

    lv_obj_t *bg = bf_plain(parent);
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bg, lv_color_mix(p->bg, lv_color_black(), 160), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    grid = lv_obj_create(bg);
    lv_obj_remove_style_all(grid);
    lv_obj_set_pos(grid, 4, 2);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grid, grid_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(grid, grid_click, LV_EVENT_CLICKED, NULL);

    ended_lbl = bf_label(bg, tr(S_TERM_ENDED), BF_FONT_S, p->warning);
    lv_obj_align(ended_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_add_flag(ended_lbl, LV_OBJ_FLAG_HIDDEN);

    kb = lv_buttonmatrix_create(lv_layer_top());
    lv_obj_set_size(kb, BF_SCREEN_W, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, p->surface, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, p->surface2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, p->text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, BF_FONT_S, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, p->primary, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, p->primary, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_VALUE_CHANGED, NULL);
    set_map();
    if(!kb_shown) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    if(master < 0) {                          /* primeira vez: inicia o shell */
        int rows = (BF_CONTENT_H - (kb_shown ? KB_H - BOTTOM_H : 0) - 4) / line_h;
        term_init(&term, (BF_SCREEN_W - 8) / char_w, rows);
        char msg[160];
        if(!start_shell(msg, sizeof msg)) {
            term_feed(&term, msg, (int)strlen(msg));
            ended = true;
        }
    }
    if(ended) lv_obj_remove_flag(ended_lbl, LV_OBJ_FLAG_HIDDEN);
    fit_grid();
    hwkbd_set_handler(hw_key);
}

static void destroy(void)
{
    /* a sessão continua; só a interface some */
    hwkbd_set_handler(NULL);
    if(kb) { lv_obj_delete(kb); kb = NULL; }
    grid = ended_lbl = NULL;
    shift = ctrl = false;
}

/* modo --screenshot: digita uma linha no shell */
void scr_term_demo_type(const char *s)
{
    send_bytes(s, (int)strlen(s));
}

const bf_screen_def_t scr_term_def = { create, destroy, S_APP_TERMINAL, title_fn };
