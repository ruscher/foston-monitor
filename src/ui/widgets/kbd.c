/* kbd.c — ver kbd.h. Os mapas vieram do Telefone (scr_phone.c). */
#include "kbd.h"
#include <string.h>

#define ACC "\xC3\xA1\xC3\xA3"      /* "áã" */

static const char *const kb_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xA7", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "?", "\n",
    LV_SYMBOL_KEYBOARD, "1#", ACC, " ", LV_SYMBOL_OK, ""
};
static const char *const kb_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x87", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "!", "\n",
    LV_SYMBOL_KEYBOARD, "1#", ACC, " ", LV_SYMBOL_OK, ""
};
static const char *const kb_acc[] = {
    "\xC3\xA1", "\xC3\xA0", "\xC3\xA2", "\xC3\xA3", "\xC3\xA9", "\xC3\xAA", "\xC3\xAD", "\xC3\xB3", "\xC3\xB4", "\xC3\xB5", "\n",
    "\xC3\xBA", "\xC3\xBC", "\xC3\xA7", "\xC3\x81", "\xC3\x83", "\xC3\x89", "\xC3\x8D", "\xC3\x93", "\xC3\x95", "\xC3\x87", "\n",
    "abc", "-", "'", "\"", ":", ";", "(", ")", "@", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, "1#", " ", LV_SYMBOL_OK, ""
};
#define KF LV_KEYBOARD_CTRL_BUTTON_FLAGS
static const lv_buttonmatrix_ctrl_t kb_ctrl_txt[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 4, KF | 4, KF | 4, 18, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_acc[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KF | 6, 4, 4, 4, 4, 4, 4, 4, 4, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
    KF | 4, KF | 4, 22, LV_BUTTONMATRIX_CTRL_CHECKED | 6,
};

typedef struct { kbd_sink_cb cb; void *ud; } sink_t;

static bool is_mode_key(const char *t)
{
    return !strcmp(t, "ABC") || !strcmp(t, "abc") || !strcmp(t, "1#");
}

/* "áã" troca para os acentos; sem campo, as teclas vão ao callback; com campo, o
 * tratamento normal do LVGL */
static void kb_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if(id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *t = lv_buttonmatrix_get_button_text(kb, id);
    if(!t) return;
    if(!strcmp(t, ACC)) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1); return; }
    sink_t *s = lv_event_get_user_data(e);
    if(!s || !s->cb || lv_keyboard_get_textarea(kb)) { lv_keyboard_def_event_cb(e); return; }
    if(is_mode_key(t)) { lv_keyboard_def_event_cb(e); return; }   /* troca de mapa: o LVGL faz */
    if(!strcmp(t, LV_SYMBOL_BACKSPACE)) s->cb(KBD_BACKSPACE, NULL, s->ud);
    else if(!strcmp(t, LV_SYMBOL_OK) || !strcmp(t, LV_SYMBOL_NEW_LINE)) s->cb(KBD_ENTER, NULL, s->ud);
    else if(!strcmp(t, LV_SYMBOL_LEFT)) s->cb(KBD_LEFT, NULL, s->ud);
    else if(!strcmp(t, LV_SYMBOL_RIGHT)) s->cb(KBD_RIGHT, NULL, s->ud);
    else if(!strcmp(t, LV_SYMBOL_KEYBOARD) || !strcmp(t, LV_SYMBOL_CLOSE)) s->cb(KBD_CLOSE, NULL, s->ud);
    else s->cb(KBD_TEXT, t, s->ud);
    /* maiúscula vale para uma letra, como no celular */
    if(lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER && strlen(t) <= 2 && !is_mode_key(t))
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void kb_delete(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

lv_obj_t *kbd_create(lv_obj_t *parent, lv_obj_t *ta)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lc, kb_ctrl_txt);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_uc, kb_ctrl_txt);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_acc, kb_ctrl_acc);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    sink_t *s = lv_malloc_zeroed(sizeof *s);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_VALUE_CHANGED, s);
    lv_obj_add_event_cb(kb, kb_delete, LV_EVENT_DELETE, s);
    lv_keyboard_set_textarea(kb, ta);
    return kb;
}

void kbd_set_sink(lv_obj_t *kb, kbd_sink_cb cb, void *ud)
{
    /* o sink_t é o user_data do primeiro callback de VALUE_CHANGED */
    for(uint32_t i = 0; i < lv_obj_get_event_count(kb); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(kb, i);
        if(lv_event_dsc_get_cb(d) == kb_event) {
            sink_t *s = lv_event_dsc_get_user_data(d);
            if(s) { s->cb = cb; s->ud = ud; }
            return;
        }
    }
}
