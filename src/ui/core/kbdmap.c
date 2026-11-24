/* kbdmap.c — ver kbdmap.h (sem LVGL: testado em tests/test_kbdmap.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kbdmap.h"
#include <linux/input.h>
#include <stddef.h>


#define DEAD(c) (0x80000000u | (uint32_t)(c))     /* tecla morta; c = o acento sozinho */
#define IS_DEAD(u) ((u) & 0x80000000u)

typedef struct { uint16_t code; uint32_t n, s, g, gs; } kmap_t;  /* normal, Shift, AltGr, AltGr+Shift */

/* comuns a todos (posição QWERTY); letras e números à parte */
static const char letters_row1[] = "qwertyuiop";      /* KEY_Q..KEY_P   */
static const char letters_row2[] = "asdfghjkl";       /* KEY_A..KEY_L   */
static const char letters_row3[] = "zxcvbnm";         /* KEY_Z..KEY_M   */

static const kmap_t map_us[] = {
    { KEY_MINUS, '-', '_', 0, 0 }, { KEY_EQUAL, '=', '+', 0, 0 },
    { KEY_LEFTBRACE, '[', '{', 0, 0 }, { KEY_RIGHTBRACE, ']', '}', 0, 0 },
    { KEY_SEMICOLON, ';', ':', 0, 0 }, { KEY_APOSTROPHE, '\'', '"', 0, 0 },
    { KEY_GRAVE, '`', '~', 0, 0 }, { KEY_BACKSLASH, '\\', '|', 0, 0 },
    { KEY_COMMA, ',', '<', 0, 0 }, { KEY_DOT, '.', '>', 0, 0 }, { KEY_SLASH, '/', '?', 0, 0 },
    { KEY_102ND, '\\', '|', 0, 0 }, { KEY_RO, '/', '?', 0, 0 },
    { KEY_6, '6', '^', 0, 0 },
    { 0, 0, 0, 0, 0 }
};

/* EUA internacional: ' " ` ~ ^ são teclas mortas ('+c = ç); AltGr com as letras acentuadas */
static const kmap_t map_intl[] = {
    { KEY_MINUS, '-', '_', 0xA5, 0 }, { KEY_EQUAL, '=', '+', 0xD7, 0xF7 },
    { KEY_LEFTBRACE, '[', '{', 0xAB, 0 }, { KEY_RIGHTBRACE, ']', '}', 0xBB, 0 },
    { KEY_SEMICOLON, ';', ':', 0xB6, 0xB0 }, { KEY_APOSTROPHE, DEAD('\''), DEAD('"'), 0xB4, 0xA8 },
    { KEY_GRAVE, DEAD('`'), DEAD('~'), 0, 0 }, { KEY_BACKSLASH, '\\', '|', 0xAC, 0xA6 },
    { KEY_COMMA, ',', '<', 0xE7, 0xC7 }, { KEY_DOT, '.', '>', 0, 0 }, { KEY_SLASH, '/', '?', 0xBF, 0 },
    { KEY_102ND, '\\', '|', 0, 0 }, { KEY_RO, '/', '?', 0, 0 },
    { KEY_1, '1', '!', 0xA1, 0xB9 }, { KEY_2, '2', '@', 0xB2, 0 }, { KEY_3, '3', '#', 0xB3, 0 },
    { KEY_4, '4', '$', 0xA4, 0xA3 }, { KEY_5, '5', '%', 0x20AC, 0 }, { KEY_6, '6', DEAD('^'), 0xBC, 0 },
    { KEY_7, '7', '&', 0xBD, 0 }, { KEY_8, '8', '*', 0xBE, 0 },
    { KEY_Q, 'q', 'Q', 0xE4, 0xC4 }, { KEY_W, 'w', 'W', 0xE5, 0xC5 }, { KEY_E, 'e', 'E', 0xE9, 0xC9 },
    { KEY_Y, 'y', 'Y', 0xFC, 0xDC }, { KEY_U, 'u', 'U', 0xFA, 0xDA }, { KEY_I, 'i', 'I', 0xED, 0xCD },
    { KEY_O, 'o', 'O', 0xF3, 0xD3 }, { KEY_P, 'p', 'P', 0xF6, 0xD6 }, { KEY_A, 'a', 'A', 0xE1, 0xC1 },
    { KEY_S, 's', 'S', 0xDF, 0xA7 }, { KEY_N, 'n', 'N', 0xF1, 0xD1 }, { KEY_C, 'c', 'C', 0xA9, 0xA2 },
    { 0, 0, 0, 0, 0 }
};

/* ABNT2 (Brasil): ´` e ~^ mortas ao lado do Ç, ¨ no Shift+6 */
static const kmap_t map_abnt2[] = {
    { KEY_MINUS, '-', '_', 0, 0 }, { KEY_EQUAL, '=', '+', 0xA7, 0 },
    { KEY_LEFTBRACE, DEAD(0xB4), DEAD('`'), 0, 0 }, { KEY_RIGHTBRACE, '[', '{', 0xAA, 0 },
    { KEY_SEMICOLON, 0xE7, 0xC7, 0, 0 }, { KEY_APOSTROPHE, DEAD('~'), DEAD('^'), 0, 0 },
    { KEY_GRAVE, '\'', '"', 0, 0 }, { KEY_BACKSLASH, ']', '}', 0xBA, 0 },
    { KEY_COMMA, ',', '<', 0, 0 }, { KEY_DOT, '.', '>', 0, 0 }, { KEY_SLASH, ';', ':', 0, 0 },
    { KEY_102ND, '\\', '|', 0, 0 }, { KEY_RO, '/', '?', 0xB0, 0 },
    { KEY_1, '1', '!', 0xB9, 0 }, { KEY_2, '2', '@', 0xB2, 0 }, { KEY_3, '3', '#', 0xB3, 0 },
    { KEY_4, '4', '$', 0xA3, 0 }, { KEY_5, '5', '%', 0xA2, 0 }, { KEY_6, '6', DEAD(0xA8), 0xAC, 0 },
    { KEY_Q, 'q', 'Q', '/', 0 }, { KEY_W, 'w', 'W', '?', 0 }, { KEY_E, 'e', 'E', 0x20AC, 0 },
    { 0, 0, 0, 0, 0 }
};

/* composição das teclas mortas */
enum { AC_NONE, AC_ACUTE, AC_GRAVE, AC_CIRC, AC_TILDE, AC_DIAER };

static int accent_kind(uint32_t spacing)
{
    switch(spacing) {
    case '\'': case 0xB4: return AC_ACUTE;
    case '`': return AC_GRAVE;
    case '^': return AC_CIRC;
    case '~': return AC_TILDE;
    case '"': case 0xA8: return AC_DIAER;
    default: return AC_NONE;
    }
}

static const struct { uint8_t kind; char base; uint16_t out; } compose_tab[] = {
    { AC_ACUTE, 'a', 0xE1 }, { AC_ACUTE, 'e', 0xE9 }, { AC_ACUTE, 'i', 0xED }, { AC_ACUTE, 'o', 0xF3 },
    { AC_ACUTE, 'u', 0xFA }, { AC_ACUTE, 'y', 0xFD }, { AC_ACUTE, 'c', 0xE7 },
    { AC_ACUTE, 'A', 0xC1 }, { AC_ACUTE, 'E', 0xC9 }, { AC_ACUTE, 'I', 0xCD }, { AC_ACUTE, 'O', 0xD3 },
    { AC_ACUTE, 'U', 0xDA }, { AC_ACUTE, 'Y', 0xDD }, { AC_ACUTE, 'C', 0xC7 },
    { AC_GRAVE, 'a', 0xE0 }, { AC_GRAVE, 'e', 0xE8 }, { AC_GRAVE, 'i', 0xEC }, { AC_GRAVE, 'o', 0xF2 },
    { AC_GRAVE, 'u', 0xF9 }, { AC_GRAVE, 'A', 0xC0 }, { AC_GRAVE, 'E', 0xC8 }, { AC_GRAVE, 'I', 0xCC },
    { AC_GRAVE, 'O', 0xD2 }, { AC_GRAVE, 'U', 0xD9 },
    { AC_CIRC, 'a', 0xE2 }, { AC_CIRC, 'e', 0xEA }, { AC_CIRC, 'i', 0xEE }, { AC_CIRC, 'o', 0xF4 },
    { AC_CIRC, 'u', 0xFB }, { AC_CIRC, 'A', 0xC2 }, { AC_CIRC, 'E', 0xCA }, { AC_CIRC, 'I', 0xCE },
    { AC_CIRC, 'O', 0xD4 }, { AC_CIRC, 'U', 0xDB },
    { AC_TILDE, 'a', 0xE3 }, { AC_TILDE, 'o', 0xF5 }, { AC_TILDE, 'n', 0xF1 },
    { AC_TILDE, 'A', 0xC3 }, { AC_TILDE, 'O', 0xD5 }, { AC_TILDE, 'N', 0xD1 },
    { AC_DIAER, 'a', 0xE4 }, { AC_DIAER, 'e', 0xEB }, { AC_DIAER, 'i', 0xEF }, { AC_DIAER, 'o', 0xF6 },
    { AC_DIAER, 'u', 0xFC }, { AC_DIAER, 'y', 0xFF }, { AC_DIAER, 'A', 0xC4 }, { AC_DIAER, 'E', 0xCB },
    { AC_DIAER, 'I', 0xCF }, { AC_DIAER, 'O', 0xD6 }, { AC_DIAER, 'U', 0xDC },
};

static uint32_t compose(uint32_t spacing, uint32_t base)
{
    int k = accent_kind(spacing);
    for(size_t i = 0; i < sizeof compose_tab / sizeof compose_tab[0]; i++)
        if(compose_tab[i].kind == k && (uint32_t)(unsigned char)compose_tab[i].base == base) return compose_tab[i].out;
    return 0;
}

static int utf8_put(char *out, uint32_t c)
{
    if(c < 0x80) { out[0] = (char)c; return 1; }
    if(c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if(c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F)); return 4;
}



static char letter_of(uint16_t code)
{
    if(code >= KEY_Q && code <= KEY_P) return letters_row1[code - KEY_Q];
    if(code >= KEY_A && code <= KEY_L) return letters_row2[code - KEY_A];
    if(code >= KEY_Z && code <= KEY_M) return letters_row3[code - KEY_Z];
    return 0;
}

/* o caractere da tecla (Unicode, DEAD(...) ou 0) pelo layout e modificadores */
static uint32_t lookup(int lay, bool caps, uint16_t code, uint8_t mods)
{
    const kmap_t *m = lay == BF_KBD_ABNT2 ? map_abnt2 : lay == BF_KBD_US_INTL ? map_intl : map_us;
    bool shift = mods & HK_SHIFT, altgr = mods & HK_ALTGR;
    char l = letter_of(code);
    for(; m->code; m++) {
        if(m->code != code) continue;
        if(altgr) {
            uint32_t g = shift && m->gs ? m->gs : m->g;
            /* letras com AltGr: o Caps Lock também vale (á/Á) */
            if(g && l && caps && !shift && m->gs) g = m->gs;
            if(g) return g;
            return 0;
        }
        if(l) break;                          /* letra: só o AltGr é especial */
        /* letra fora do A-Z (o Ç do ABNT2): o Caps Lock também vale */
        bool sh = shift;
        if(caps && m->n >= 0xE0 && m->n <= 0xFE && m->n != 0xF7 && m->s == m->n - 0x20) sh = !sh;
        return sh ? m->s : m->n;
    }
    if(altgr) return 0;
    if(l) return (shift != caps) ? (uint32_t)(l - 32) : (uint32_t)l;
    if(code >= KEY_1 && code <= KEY_0) {
        static const char us_shift[] = "!@#$%^&*()";
        return shift ? (uint32_t)us_shift[code - KEY_1] : (uint32_t)"1234567890"[code - KEY_1];
    }
    if(code == KEY_SPACE) return ' ';
    return 0;
}

/* teclado numérico: com Num Lock, números; sem, navegação */
static uint16_t keypad(int lay, bool numlock, uint16_t code, uint32_t *ch)
{
    static const struct { uint16_t kp, nav; char c; } t[] = {
        { KEY_KP7, KEY_HOME, '7' }, { KEY_KP8, KEY_UP, '8' }, { KEY_KP9, KEY_PAGEUP, '9' },
        { KEY_KP4, KEY_LEFT, '4' }, { KEY_KP5, 0, '5' }, { KEY_KP6, KEY_RIGHT, '6' },
        { KEY_KP1, KEY_END, '1' }, { KEY_KP2, KEY_DOWN, '2' }, { KEY_KP3, KEY_PAGEDOWN, '3' },
        { KEY_KP0, KEY_INSERT, '0' }, { KEY_KPDOT, KEY_DELETE, '.' },
    };
    *ch = 0;
    if(code == KEY_KPENTER) return KEY_ENTER;
    if(code == KEY_KPSLASH) { *ch = '/'; return code; }
    if(code == KEY_KPASTERISK) { *ch = '*'; return code; }
    if(code == KEY_KPMINUS) { *ch = '-'; return code; }
    if(code == KEY_KPPLUS) { *ch = '+'; return code; }
    if(code == KEY_KPCOMMA) { *ch = lay == BF_KBD_ABNT2 ? '.' : ','; return code; }
    for(size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if(t[i].kp != code) continue;
        if(numlock) {
            *ch = (code == KEY_KPDOT && lay == BF_KBD_ABNT2) ? ',' : (uint32_t)t[i].c;
            return code;
        }
        return t[i].nav;
    }
    return code;
}

void kbdmap_translate(kbdmap_t *st, int lay, uint16_t *code_io, uint8_t mods, bool repeat, char text[KBDMAP_TEXT])
{
    uint32_t ch = 0;
    uint16_t code = keypad(lay, st->numlock, *code_io, &ch);
    *code_io = code;
    text[0] = 0;
    if(!ch) ch = lookup(lay, st->caps, code, mods);
    if(mods & (HK_CTRL | HK_ALT | HK_META)) {         /* atalho: sem texto */
        st->dead = 0;
        return;
    }
    if(!ch) {
        if(st->dead && code == KEY_BACKSPACE) { st->dead = 0; *code_io = 0; }   /* só cancela o acento */
        else if(st->dead) st->dead = 0;
        return;
    }
    char *o = text;
    if(IS_DEAD(ch)) {
        uint32_t sp = ch & 0x7FFFFFFFu;
        if(repeat) return;
        if(!st->dead) { st->dead = sp; return; }     /* espera a letra */
        /* duas mortas: a primeira sai sozinha; a mesma duas vezes sai uma vez */
        o += utf8_put(o, st->dead);
        st->dead = st->dead == sp ? 0 : sp;
        *o = 0;
        return;
    }
    if(st->dead) {
        uint32_t c = compose(st->dead, ch);
        if(c) ch = c;
        else if(ch == ' ') ch = st->dead;            /* acento + espaço = o acento */
        else o += utf8_put(o, st->dead);             /* sem composição: os dois */
        st->dead = 0;
    }
    o += utf8_put(o, ch);
    *o = 0;
}
