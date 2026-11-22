/* nav.h — pilha de navegação. A tela fixa tem barra superior, área de
 * conteúdo (800x360) e barra inferior; cada "tela" desenha só o conteúdo. */
#ifndef BF_NAV_H
#define BF_NAV_H

#include "lvgl.h"
#include "i18n.h"

typedef enum {
    SCR_HOME, SCR_SETTINGS, SCR_SYSINFO, SCR_RUN, SCR_MONITOR, SCR_CALIBRATE,
    SCR_BACKLIGHT, SCR_VOLUME, SCR_LANGUAGE, SCR_DATE, SCR_POWER, SCR_RESET,
    SCR_SSH, SCR_ADVANCED, SCR_FEATURE, SCR_MUSIC, SCR_FILES, SCR_TERM, SCR_BOOK,
    SCR_APPEARANCE, SCR_BIGSCREEN, SCR_GALLERY, SCR_GPS, SCR_MAP, SCR_PERF, SCR_BLUETOOTH, SCR_PHONE,
    SCR__COUNT
} bf_screen_t;

typedef struct {
    void (*create)(lv_obj_t *parent, int arg);  /* desenha no contêiner de conteúdo */
    void (*destroy)(void);                      /* opcional: timers, overlays */
    bf_str_t title;                             /* título na barra superior */
    const char *(*title_fn)(int arg);           /* opcional: título dinâmico */
} bf_screen_def_t;

typedef struct { bf_screen_t scr; int arg; } bf_nav_entry_t;

void nav_init(lv_obj_t *content);
void nav_set_content(lv_obj_t *content);       /* após reconstrução da UI */
void nav_go(bf_screen_t scr, int arg);          /* assíncrono (seguro em eventos) */
void nav_back(void);
void nav_home(void);
void nav_show_now(void);
void nav_teardown(void);                        /* destrói a tela atual (antes de reconstruir a UI) */                        /* recria a tela do topo (síncrono) */
bf_nav_entry_t nav_current(void);
const char *nav_title(bf_screen_t scr, int arg);
bool nav_is_return(void);                      /* tela atual foi aberta via Voltar */
int nav_recents(bf_nav_entry_t *out, int max);  /* mais recente primeiro */

#endif
