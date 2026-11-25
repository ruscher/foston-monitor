/* test_kbdmap.c — layouts do teclado físico (core/kbdmap.c): ABNT2, EUA e EUA internacional,
 * teclas mortas, AltGr, Caps Lock e teclado numérico. */
#include "core/kbdmap.h"
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

static int fails, total;
static kbdmap_t st;

/* digita uma sequência de (código, modificadores) e compara o texto junto */
typedef struct { uint16_t code; uint8_t mods; } k_t;

static void check(const char *what, int lay, const k_t *keys, int n, const char *want)
{
    char all[64] = "";
    memset(&st, 0, sizeof st);
    st.numlock = true;
    for(int i = 0; i < n; i++) {
        char t[KBDMAP_TEXT];
        uint16_t c = keys[i].code;
        kbdmap_translate(&st, lay, &c, keys[i].mods, false, t);
        strncat(all, t, sizeof all - strlen(all) - 1);
    }
    total++;
    if(strcmp(all, want)) { fails++; printf("FALHOU %s: \"%s\" (esperado \"%s\")\n", what, all, want); }
}

#define K(...) (const k_t[]){ __VA_ARGS__ }, (int)(sizeof((const k_t[]){ __VA_ARGS__ }) / sizeof(k_t))
#define S HK_SHIFT
#define G HK_ALTGR

int main(void)
{
    const int AB = BF_KBD_ABNT2, US = BF_KBD_US, IN = BF_KBD_US_INTL;
    /* ABNT2 */
    check("abnt2 letras", AB, K({ KEY_O, 0 }, { KEY_L, 0 }, { KEY_A, S }), "olA");
    check("abnt2 ç", AB, K({ KEY_SEMICOLON, 0 }, { KEY_SEMICOLON, S }), "\xC3\xA7\xC3\x87");
    check("abnt2 ´a", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_A, 0 }), "\xC3\xA1");
    check("abnt2 ´c", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_C, 0 }), "\xC3\xA7");
    check("abnt2 ´E", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_E, S }), "\xC3\x89");
    check("abnt2 ~a", AB, K({ KEY_APOSTROPHE, 0 }, { KEY_A, 0 }), "\xC3\xA3");
    check("abnt2 ~O", AB, K({ KEY_APOSTROPHE, 0 }, { KEY_O, S }), "\xC3\x95");
    check("abnt2 ^e", AB, K({ KEY_APOSTROPHE, S }, { KEY_E, 0 }), "\xC3\xAA");
    check("abnt2 `a", AB, K({ KEY_LEFTBRACE, S }, { KEY_A, 0 }), "\xC3\xA0");
    check("abnt2 ¨u", AB, K({ KEY_6, S }, { KEY_U, 0 }), "\xC3\xBC");
    check("abnt2 ~ espaço", AB, K({ KEY_APOSTROPHE, 0 }, { KEY_SPACE, 0 }), "~");
    check("abnt2 ´ espaço", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_SPACE, 0 }), "\xC2\xB4");
    check("abnt2 ~x", AB, K({ KEY_APOSTROPHE, 0 }, { KEY_X, 0 }), "~x");
    check("abnt2 ~~", AB, K({ KEY_APOSTROPHE, 0 }, { KEY_APOSTROPHE, 0 }), "~");
    check("abnt2 ´~a", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_APOSTROPHE, 0 }, { KEY_A, 0 }), "\xC2\xB4\xC3\xA3");
    check("abnt2 pontuação", AB, K({ KEY_SLASH, 0 }, { KEY_SLASH, S }, { KEY_RO, 0 }, { KEY_RO, S },
                                   { KEY_GRAVE, 0 }, { KEY_GRAVE, S }, { KEY_RIGHTBRACE, 0 }, { KEY_BACKSLASH, 0 },
                                   { KEY_102ND, 0 }, { KEY_2, S }, { KEY_COMMA, 0 }, { KEY_DOT, S }), ";:/?'\"[]\\@,>");
    check("abnt2 AltGr", AB, K({ KEY_Q, G }, { KEY_W, G }, { KEY_2, G }, { KEY_EQUAL, G }), "/?\xC2\xB2\xC2\xA7");
    check("abnt2 numérico", AB, K({ KEY_KP1, 0 }, { KEY_KPDOT, 0 }, { KEY_KP5, 0 }, { KEY_KPASTERISK, 0 }), "1,5*");
    /* EUA */
    check("us pontuação", US, K({ KEY_SEMICOLON, 0 }, { KEY_APOSTROPHE, 0 }, { KEY_APOSTROPHE, S }, { KEY_6, S },
                                { KEY_GRAVE, 0 }, { KEY_GRAVE, S }, { KEY_LEFTBRACE, 0 }, { KEY_SLASH, S }), ";'\"^`~[?");
    check("us AltGr nada", US, K({ KEY_A, G }), "");
    check("us numérico", US, K({ KEY_KPDOT, 0 }), ".");
    /* EUA internacional */
    check("intl 'e", IN, K({ KEY_APOSTROPHE, 0 }, { KEY_E, 0 }), "\xC3\xA9");
    check("intl 'c", IN, K({ KEY_APOSTROPHE, 0 }, { KEY_C, 0 }), "\xC3\xA7");
    check("intl \"u", IN, K({ KEY_APOSTROPHE, S }, { KEY_U, 0 }), "\xC3\xBC");
    check("intl ' espaço", IN, K({ KEY_APOSTROPHE, 0 }, { KEY_SPACE, 0 }), "'");
    check("intl 't", IN, K({ KEY_APOSTROPHE, 0 }, { KEY_T, 0 }), "'t");
    check("intl ^o", IN, K({ KEY_6, S }, { KEY_O, 0 }), "\xC3\xB4");
    check("intl ~n", IN, K({ KEY_GRAVE, S }, { KEY_N, 0 }), "\xC3\xB1");
    check("intl `a", IN, K({ KEY_GRAVE, 0 }, { KEY_A, 0 }), "\xC3\xA0");
    check("intl AltGr", IN, K({ KEY_COMMA, G }, { KEY_COMMA, G | S }, { KEY_E, G }, { KEY_5, G }),
          "\xC3\xA7\xC3\x87\xC3\xA9\xE2\x82\xAC");
    /* Ctrl/Alt não escrevem e cancelam o acento */
    check("ctrl", AB, K({ KEY_A, HK_CTRL }, { KEY_C, HK_ALT }), "");
    check("ctrl cancela acento", AB, K({ KEY_LEFTBRACE, 0 }, { KEY_A, HK_CTRL }, { KEY_A, 0 }), "a");

    /* Caps Lock, Backspace no acento, teclado numérico sem Num Lock, repetição de morta */
    total++;
    {
        char t[KBDMAP_TEXT];
        uint16_t c;
        memset(&st, 0, sizeof st);
        st.caps = true;
        c = KEY_A; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(strcmp(t, "A")) { fails++; printf("FALHOU caps a: %s\n", t); }
        c = KEY_A; kbdmap_translate(&st, AB, &c, S, false, t);
        if(strcmp(t, "a")) { fails++; printf("FALHOU caps+shift a: %s\n", t); }
        c = KEY_SEMICOLON; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(strcmp(t, "\xC3\x87")) { fails++; printf("FALHOU caps ç: %s\n", t); }
        c = KEY_1; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(strcmp(t, "1")) { fails++; printf("FALHOU caps 1: %s\n", t); }
        st.caps = false;
        c = KEY_LEFTBRACE; kbdmap_translate(&st, AB, &c, 0, false, t);
        c = KEY_BACKSPACE; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(c != 0 || t[0] || st.dead) { fails++; printf("FALHOU backspace cancela o acento\n"); }
        c = KEY_BACKSPACE; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(c != KEY_BACKSPACE || t[0]) { fails++; printf("FALHOU backspace normal\n"); }
        st.numlock = false;
        c = KEY_KP8; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(c != KEY_UP || t[0]) { fails++; printf("FALHOU KP8 sem Num Lock\n"); }
        c = KEY_KPDOT; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(c != KEY_DELETE || t[0]) { fails++; printf("FALHOU KPDOT sem Num Lock\n"); }
        c = KEY_KPENTER; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(c != KEY_ENTER || t[0]) { fails++; printf("FALHOU KPENTER\n"); }
        c = KEY_KPMINUS; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(strcmp(t, "-")) { fails++; printf("FALHOU KPMINUS sem Num Lock\n"); }
        c = KEY_LEFTBRACE; kbdmap_translate(&st, AB, &c, 0, false, t);
        c = KEY_LEFTBRACE; kbdmap_translate(&st, AB, &c, 0, true, t);    /* morta segurada */
        c = KEY_E; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(strcmp(t, "\xC3\xA9")) { fails++; printf("FALHOU morta repetida: %s\n", t); }
        c = KEY_F1; kbdmap_translate(&st, AB, &c, 0, false, t);
        if(t[0]) { fails++; printf("FALHOU F1 escreveu\n"); }
    }
    printf("kbdmap: %d/%d ok\n", total - fails, total);
    return fails != 0;
}
