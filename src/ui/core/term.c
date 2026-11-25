/* term.c — emulador VT100 (ver term.h).
 *
 * Cobre o que o BusyBox usa (edição de linha do ash, top, vi, less, ls):
 * caracteres UTF-8, CR/LF/BS/TAB/BEL, ESC 7/8/D/E/M/c, CSI A B C D E F G H f
 * J K L M P @ X d r s u m n h l (incl. ?25 cursor e ?1049/?47/?1047 tela
 * alternativa). Cores SGR são aceitas e ignoradas (só negrito e inverso). */
#include "term.h"
#include <stdio.h>
#include <string.h>

enum { S_NORMAL, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET };

static void clear_cells(term_t *t, int y, int x0, int x1)
{
    for(int x = x0; x < x1 && x < t->cols; x++) t->cell[y][x] = (term_cell_t){ ' ', 0 };
    t->dirty[y] = true;
}

static void dirty_all(term_t *t)
{
    for(int y = 0; y < t->rows; y++) t->dirty[y] = true;
}

void term_init(term_t *t, int cols, int rows)
{
    memset(t, 0, sizeof *t);
    t->cols = cols > TERM_MAX_COLS ? TERM_MAX_COLS : cols;
    t->rows = rows > TERM_MAX_ROWS ? TERM_MAX_ROWS : rows;
    t->bot = t->rows - 1;
    t->cursor_visible = true;
    for(int y = 0; y < TERM_MAX_ROWS; y++) clear_cells(t, y, 0, TERM_MAX_COLS);
    dirty_all(t);
}

/* ao encolher, sobe o conteúdo para manter a linha do cursor visível */
void term_resize(term_t *t, int cols, int rows)
{
    if(cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if(rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if(rows < t->rows && t->cy >= rows) {
        int sh = t->cy - rows + 1;
        memmove(&t->cell[0], &t->cell[sh], sizeof t->cell[0] * (TERM_MAX_ROWS - sh));
        for(int y = TERM_MAX_ROWS - sh; y < TERM_MAX_ROWS; y++)
            for(int x = 0; x < TERM_MAX_COLS; x++) t->cell[y][x] = (term_cell_t){ ' ', 0 };
        t->cy -= sh;
    }
    for(int y = rows > t->rows ? t->rows : rows; y < rows; y++) clear_cells(t, y, 0, TERM_MAX_COLS);
    t->cols = cols;
    t->rows = rows;
    t->top = 0;
    t->bot = rows - 1;
    if(t->cx >= cols) t->cx = cols - 1;
    if(t->cy >= rows) t->cy = rows - 1;
    t->wrap_pending = false;
    dirty_all(t);
}

static void scroll_up(term_t *t, int top, int bot, int n)
{
    if(n <= 0) return;
    if(n > bot - top + 1) n = bot - top + 1;
    memmove(&t->cell[top], &t->cell[top + n], sizeof t->cell[0] * (bot - top + 1 - n));
    for(int y = bot - n + 1; y <= bot; y++) clear_cells(t, y, 0, TERM_MAX_COLS);
    for(int y = top; y <= bot; y++) t->dirty[y] = true;
}

static void scroll_down(term_t *t, int top, int bot, int n)
{
    if(n <= 0) return;
    if(n > bot - top + 1) n = bot - top + 1;
    memmove(&t->cell[top + n], &t->cell[top], sizeof t->cell[0] * (bot - top + 1 - n));
    for(int y = top; y < top + n; y++) clear_cells(t, y, 0, TERM_MAX_COLS);
    for(int y = top; y <= bot; y++) t->dirty[y] = true;
}

static void line_feed(term_t *t)
{
    if(t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if(t->cy < t->rows - 1) t->cy++;
}

static void put_char(term_t *t, uint32_t ch)
{
    if(t->wrap_pending) {
        t->wrap_pending = false;
        t->cx = 0;
        line_feed(t);
    }
    t->cell[t->cy][t->cx] = (term_cell_t){ ch, t->attr };
    t->dirty[t->cy] = true;
    if(t->cx == t->cols - 1) t->wrap_pending = true;
    else t->cx++;
}

static int p(term_t *t, int i, int def)
{
    return i < t->nparams && t->params[i] > 0 ? t->params[i] : def;
}

static void clamp_cursor(term_t *t)
{
    if(t->cx < 0) t->cx = 0;
    if(t->cx >= t->cols) t->cx = t->cols - 1;
    if(t->cy < 0) t->cy = 0;
    if(t->cy >= t->rows) t->cy = t->rows - 1;
    t->wrap_pending = false;
}

static void set_alt(term_t *t, bool on)
{
    if(on == t->alt_active) return;
    term_cell_t tmp[TERM_MAX_COLS];
    for(int y = 0; y < TERM_MAX_ROWS; y++) {           /* troca as duas telas */
        memcpy(tmp, t->cell[y], sizeof tmp);
        memcpy(t->cell[y], t->alt[y], sizeof tmp);
        memcpy(t->alt[y], tmp, sizeof tmp);
    }
    if(on) {
        t->alt_saved_x = t->cx;
        t->alt_saved_y = t->cy;
        for(int y = 0; y < TERM_MAX_ROWS; y++) clear_cells(t, y, 0, TERM_MAX_COLS);
    }
    else {
        t->cx = t->alt_saved_x;
        t->cy = t->alt_saved_y;
        clamp_cursor(t);
    }
    t->alt_active = on;
    dirty_all(t);
}

static void reply(term_t *t, const char *s)
{
    int n = (int)strlen(s);
    if(t->reply_len + n <= (int)sizeof t->reply) {
        memcpy(t->reply + t->reply_len, s, n);
        t->reply_len += n;
    }
}

static void csi(term_t *t, char f)
{
    int n = p(t, 0, 1);
    switch(f) {
    case 'A': t->cy -= n; if(t->cy < t->top && t->cy + n >= t->top) t->cy = t->top; clamp_cursor(t); break;
    case 'B': case 'e': t->cy += n; if(t->cy > t->bot && t->cy - n <= t->bot) t->cy = t->bot; clamp_cursor(t); break;
    case 'C': case 'a': t->cx += n; clamp_cursor(t); break;
    case 'D': t->cx -= n; clamp_cursor(t); break;
    case 'E': t->cy += n; t->cx = 0; clamp_cursor(t); break;
    case 'F': t->cy -= n; t->cx = 0; clamp_cursor(t); break;
    case 'G': case '`': t->cx = n - 1; clamp_cursor(t); break;
    case 'd': t->cy = n - 1; clamp_cursor(t); break;
    case 'H': case 'f': t->cy = p(t, 0, 1) - 1; t->cx = p(t, 1, 1) - 1; clamp_cursor(t); break;
    case 'J': {
        int m = t->nparams ? t->params[0] : 0;
        if(m == 0) {
            clear_cells(t, t->cy, t->cx, t->cols);
            for(int y = t->cy + 1; y < t->rows; y++) clear_cells(t, y, 0, t->cols);
        }
        else if(m == 1) {
            for(int y = 0; y < t->cy; y++) clear_cells(t, y, 0, t->cols);
            clear_cells(t, t->cy, 0, t->cx + 1);
        }
        else for(int y = 0; y < t->rows; y++) clear_cells(t, y, 0, t->cols);
        break;
    }
    case 'K': {
        int m = t->nparams ? t->params[0] : 0;
        if(m == 0) clear_cells(t, t->cy, t->cx, t->cols);
        else if(m == 1) clear_cells(t, t->cy, 0, t->cx + 1);
        else clear_cells(t, t->cy, 0, t->cols);
        break;
    }
    case 'L': if(t->cy >= t->top && t->cy <= t->bot) scroll_down(t, t->cy, t->bot, n); break;
    case 'M': if(t->cy >= t->top && t->cy <= t->bot) scroll_up(t, t->cy, t->bot, n); break;
    case 'P':                                   /* apaga n caracteres */
        if(n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(&t->cell[t->cy][t->cx], &t->cell[t->cy][t->cx + n], sizeof(term_cell_t) * (t->cols - t->cx - n));
        clear_cells(t, t->cy, t->cols - n, t->cols);
        break;
    case '@':                                   /* insere n espaços */
        if(n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(&t->cell[t->cy][t->cx + n], &t->cell[t->cy][t->cx], sizeof(term_cell_t) * (t->cols - t->cx - n));
        clear_cells(t, t->cy, t->cx, t->cx + n);
        break;
    case 'X': clear_cells(t, t->cy, t->cx, t->cx + n); break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'r': {
        int top = p(t, 0, 1) - 1, bot = p(t, 1, t->rows) - 1;
        if(top < bot && bot < t->rows) { t->top = top; t->bot = bot; }
        t->cx = 0;
        t->cy = 0;
        t->wrap_pending = false;
        break;
    }
    case 's': t->saved_x = t->cx; t->saved_y = t->cy; break;
    case 'u': t->cx = t->saved_x; t->cy = t->saved_y; clamp_cursor(t); break;
    case 'm':
        if(!t->nparams) { t->attr = 0; break; }
        for(int i = 0; i < t->nparams; i++) {
            int v = t->params[i];
            if(v == 0) t->attr = 0;
            else if(v == 1) t->attr |= TA_BOLD;
            else if(v == 22) t->attr &= (uint8_t)~TA_BOLD;
            else if(v == 7) t->attr |= TA_INVERSE;
            else if(v == 27) t->attr &= (uint8_t)~TA_INVERSE;
            else if(v == 38 || v == 48) i += (i + 1 < t->nparams && t->params[i + 1] == 5) ? 2 : 4;
        }
        break;
    case 'n':
        if(p(t, 0, 0) == 6) {
            char b[40];
            snprintf(b, sizeof b, "\033[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, b);
        }
        else if(p(t, 0, 0) == 5) reply(t, "\033[0n");
        break;
    case 'c': if(!t->priv) reply(t, "\033[?1;0c"); break;     /* VT100 */
    case 'h': case 'l': {
        bool on = f == 'h';
        if(!t->priv) break;
        for(int i = 0; i < t->nparams; i++) {
            int v = t->params[i];
            if(v == 1) t->app_cursor = on;
            else if(v == 25) t->cursor_visible = on;
            else if(v == 47 || v == 1047) set_alt(t, on);
            else if(v == 1049) {
                if(on) { t->saved_x = t->cx; t->saved_y = t->cy; }
                set_alt(t, on);
                if(!on) { t->cx = t->saved_x; t->cy = t->saved_y; clamp_cursor(t); }
            }
        }
        t->dirty[t->cy] = true;
        break;
    }
    default: break;
    }
}

static void control(term_t *t, unsigned char c)
{
    switch(c) {
    case '\r': t->cx = 0; t->wrap_pending = false; break;
    case '\n': case '\v': case '\f': line_feed(t); t->wrap_pending = false; break;
    case '\b': if(t->cx > 0) t->cx--; t->wrap_pending = false; break;
    case '\t': {
        int nx = (t->cx / 8 + 1) * 8;
        t->cx = nx >= t->cols ? t->cols - 1 : nx;
        break;
    }
    case 0x1b: t->state = S_ESC; break;
    default: break;                          /* BEL e outros: ignora */
    }
    t->dirty[t->cy] = true;
}

void term_feed(term_t *t, const char *data, int len)
{
    for(int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch(t->state) {
        case S_NORMAL:
            if(t->utf8_left) {
                if((c & 0xc0) == 0x80) {
                    t->utf8 = (t->utf8 << 6) | (c & 0x3f);
                    if(--t->utf8_left == 0) put_char(t, t->utf8);
                    continue;
                }
                t->utf8_left = 0;
                put_char(t, '?');
            }
            if(c < 0x20 || c == 0x7f) { if(c != 0x7f) control(t, c); }
            else if(c < 0x80) put_char(t, c);
            else if((c & 0xe0) == 0xc0) { t->utf8 = c & 0x1f; t->utf8_left = 1; }
            else if((c & 0xf0) == 0xe0) { t->utf8 = c & 0x0f; t->utf8_left = 2; }
            else if((c & 0xf8) == 0xf0) { t->utf8 = c & 0x07; t->utf8_left = 3; }
            else put_char(t, '?');
            break;
        case S_ESC:
            t->state = S_NORMAL;
            switch(c) {
            case '[': t->state = S_CSI; t->nparams = 0; t->priv = false; memset(t->params, 0, sizeof t->params); break;
            case ']': t->state = S_OSC; break;
            case '(': case ')': t->state = S_CHARSET; break;
            case '7': t->saved_x = t->cx; t->saved_y = t->cy; break;
            case '8': t->cx = t->saved_x; t->cy = t->saved_y; clamp_cursor(t); break;
            case 'D': line_feed(t); break;
            case 'E': t->cx = 0; line_feed(t); break;
            case 'M':
                if(t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
                else if(t->cy > 0) t->cy--;
                break;
            case 'c': term_init(t, t->cols, t->rows); break;
            default: break;
            }
            break;
        case S_CSI:
            if(c == '?') t->priv = true;
            else if(c >= '0' && c <= '9') {
                if(!t->nparams) t->nparams = 1;
                if(t->params[t->nparams - 1] < 10000) t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0');
            }
            else if(c == ';') { if(!t->nparams) t->nparams = 1; if(t->nparams < 16) t->nparams++; }
            else if(c >= 0x40 && c <= 0x7e) { csi(t, (char)c); t->state = S_NORMAL; }
            else if(c < 0x20) control(t, c);       /* controles dentro de CSI valem */
            break;
        case S_OSC:                                 /* título da janela: ignora até BEL ou ST */
            if(c == 0x07) t->state = S_NORMAL;
            else if(c == 0x1b) t->state = S_OSC_ESC;
            break;
        case S_OSC_ESC: t->state = c == '\\' ? S_NORMAL : S_OSC; break;
        case S_CHARSET: t->state = S_NORMAL; break;
        }
    }
}

int term_take_reply(term_t *t, char *out, int max)
{
    int n = t->reply_len < max ? t->reply_len : max;
    memcpy(out, t->reply, n);
    memmove(t->reply, t->reply + n, t->reply_len - n);
    t->reply_len -= n;
    return n;
}
