/* theme.h — design system do BigFoston (identidade BigLinux). */
#ifndef BF_THEME_H
#define BF_THEME_H

#include "lvgl.h"
#include <stdbool.h>

/* Geometria (px). Alvos de toque nunca menores que BF_TOUCH_MIN. */
#define BF_SCREEN_W   800
#define BF_SCREEN_H   480
#define BF_TOPBAR_H    48
#define BF_BOTBAR_H    72
#define BF_CONTENT_H  (BF_SCREEN_H - BF_TOPBAR_H - BF_BOTBAR_H)   /* 360 */
#define BF_TOUCH_MIN   64
#define BF_SP_XS        4
#define BF_SP_S         8
#define BF_SP_M        12
#define BF_SP_L        16
#define BF_SP_XL       24
#define BF_RADIUS_S    10
#define BF_RADIUS_M    14
#define BF_RADIUS_L    20

/* Cores fixas da marca */
#define BF_BLUE    0x3584e4
#define BF_CYAN    0x33c7de
#define BF_GREEN   0x2ec27e
#define BF_AMBER   0xf5a524
#define BF_RED     0xe5484d

typedef struct {
    bool dark;
    lv_color_t bg;        /* fundo da tela */
    lv_color_t surface;   /* cartões, barras */
    lv_color_t surface2;  /* elevação 2, botões neutros */
    lv_color_t border;
    lv_color_t text;
    lv_color_t text_dim;
    lv_color_t primary;   /* azul BigLinux */
    lv_color_t accent;    /* ciano */
    lv_color_t success, warning, danger;
} bf_palette_t;

/* Fontes */
#define BF_FONT_S    (&bf_font_14)
#define BF_FONT_M    (&bf_font_20)
#define BF_FONT_L    (&bf_font_28)
#define BF_FONT_MONO (&bf_mono_16)

/* Símbolos extras incluídos nas fontes (Font Awesome 5) */
#define BF_SYMBOL_TERMINAL  "\xEF\x84\xA0"  /* F120 */
#define BF_SYMBOL_UNDO      "\xEF\x83\xA2"  /* F0E2 */
#define BF_SYMBOL_INFO      "\xEF\x81\x9A"  /* F05A */
#define BF_SYMBOL_ALERT     "\xEF\x81\xB1"  /* F071 */
#define BF_SYMBOL_CLOCK     "\xEF\x80\x97"  /* F017 */
#define BF_SYMBOL_NET       "\xEF\x9B\xBF"  /* F6FF network-wired */
#define BF_SYMBOL_SATELLITE "\xEF\x9F\x80"  /* F7C0 satellite-dish */
#define BF_SYMBOL_SUN       "\xEF\x86\x85"  /* F185 */
#define BF_SYMBOL_MOON      "\xEF\x86\x86"  /* F186 */
#define BF_SYMBOL_USB_FA    "\xEF\x8A\x87"  /* F287 */
#define BF_SYMBOL_SEARCH    "\xEF\x80\x82"  /* F002 */
#define BF_SYMBOL_EXPAND    "\xEF\x81\xA5"  /* F065 */
#define BF_SYMBOL_BOOKMARK  "\xEF\x80\xAE"  /* F02E */
#define BF_SYMBOL_TOWER     "\xEF\x94\x99"  /* F519 */
#define BF_SYMBOL_KEY       "\xEF\x82\x84"  /* F084 */
#define BF_SYMBOL_LANGUAGE  "\xEF\x86\xAB"  /* F1AB */
#define BF_SYMBOL_CALENDAR  "\xEF\x81\xB3"  /* F073 */
#define BF_SYMBOL_CROSSHAIR "\xEF\x81\x9B"  /* F05B */
#define BF_SYMBOL_WRENCH    "\xEF\x82\xAD"  /* F0AD */
#define BF_SYMBOL_ROCKET    "\xEF\x84\xB5"  /* F135 */
#define BF_SYMBOL_STAR      "\xEF\x80\x85"  /* F005 */
#define BF_SYMBOL_HDD       "\xEF\x82\xA0"  /* F0A0 */

void theme_init(lv_display_t *disp, bool dark);
const bf_palette_t *pal(void);

/* Estilos compartilhados (recriados a cada troca de tema) */
extern lv_style_t bf_st_screen;   /* fundo da tela */
extern lv_style_t bf_st_card;     /* cartão arredondado */
extern lv_style_t bf_st_btn;      /* botão neutro */
extern lv_style_t bf_st_btn_pr;   /* botão neutro pressionado */
extern lv_style_t bf_st_btn_primary;
extern lv_style_t bf_st_btn_danger;
extern lv_style_t bf_st_plain;    /* contêiner transparente sem borda/padding */

/* Helpers */
lv_obj_t *bf_plain(lv_obj_t *parent);                       /* contêiner neutro */
lv_obj_t *bf_card(lv_obj_t *parent);
lv_obj_t *bf_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color);
lv_obj_t *bf_button(lv_obj_t *parent, const char *symbol, const char *txt, int kind); /* kind: 0 neutro, 1 primário, 2 perigo */
/* Rótulo de uma linha com reticências: largura fixa (px ou LV_PCT) e altura de uma linha. */
void bf_label_ellipsis(lv_obj_t *label, int32_t width);
lv_obj_t *bf_pill(lv_obj_t *parent, const char *txt, lv_color_t color);

#endif
