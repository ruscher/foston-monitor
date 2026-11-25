/* term.h — emulador de terminal VT100 (subconjunto) para o app Terminal.
 *
 * Sem LVGL: recebe a saída do PTY (term_feed) e mantém uma grade de células
 * com caractere (código Unicode) e atributo, cursor, região de rolagem e
 * tela alternativa. A interface desenha a partir da grade e das linhas
 * marcadas como sujas. Respostas ao programa (relatório de cursor) saem por
 * term_take_reply. */
#ifndef BF_TERM_H
#define BF_TERM_H
#include <stdbool.h>
#include <stdint.h>

#define TERM_MAX_COLS 100
#define TERM_MAX_ROWS 40

#define TA_INVERSE 1
#define TA_BOLD    2

typedef struct { uint32_t ch; uint8_t attr; } term_cell_t;

typedef struct {
    int cols, rows;
    int cx, cy;                       /* cursor */
    bool cursor_visible;
    bool app_cursor;                  /* ESC[?1h: setas mandam ESC O A (vi, less) */
    term_cell_t cell[TERM_MAX_ROWS][TERM_MAX_COLS];
    bool dirty[TERM_MAX_ROWS];
    /* interno */
    term_cell_t alt[TERM_MAX_ROWS][TERM_MAX_COLS];
    bool alt_active;
    int top, bot;                     /* região de rolagem [top, bot] */
    uint8_t attr;
    int saved_x, saved_y, alt_saved_x, alt_saved_y;
    bool wrap_pending;
    int state;                        /* parser */
    int params[16], nparams;
    bool priv;                        /* CSI ? ... */
    uint32_t utf8;
    int utf8_left;
    char reply[32];
    int reply_len;
} term_t;

void term_init(term_t *t, int cols, int rows);
void term_resize(term_t *t, int cols, int rows);
void term_feed(term_t *t, const char *data, int len);
/* bytes que o terminal precisa responder ao programa (ex.: ESC[6n) */
int  term_take_reply(term_t *t, char *out, int max);
#endif
