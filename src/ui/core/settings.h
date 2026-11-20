/* settings.h — ajustes persistidos em $BIGFOSTON_DATA/settings.conf (chave=valor). */
#ifndef BF_SETTINGS_H
#define BF_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include "i18n.h"

typedef struct {
    bf_lang_t lang;
    int  mode;          /* BF_MODE_*: escuro (padrão), claro ou automático */
    int  palette;       /* tema de cores (core/theme.h, BF_PAL_*) */
    int  wallpaper;     /* BF_WALL_*: liso, degradê, degradê com o símbolo */
    bool anim;          /* animações de transição */
    bool sounds;        /* sons de toque */
    bool boot_sound;    /* som ao iniciar a interface */
    int  dim_min;       /* escurecer a tela após N min sem toque (0 = nunca) */
    int  backlight;     /* 5..100 % */
    int  volume;        /* 0..100 % */
    bool show_date;     /* data na barra superior */
    bool clock_24h;
    int  music_repeat;  /* 0 desligado, 1 todas, 2 uma */
    bool music_shuffle;
    int  book_font;     /* Livros: 0..2 tamanho da letra */
    int  book_space;    /* 0..2 espaçamento entre linhas */
    int  book_margin;   /* 0..2 margens */
    int  book_theme;    /* 0 escuro, 1 sépia, 2 claro */
} bf_settings_t;

enum { BF_MODE_DARK, BF_MODE_LIGHT, BF_MODE_AUTO, BF_MODE__N };
enum { BF_WALL_PLAIN, BF_WALL_GRADIENT, BF_WALL_LOGO, BF_WALL__N };
#define BF_DIM_CHOICES { 0, 1, 2, 5, 10 }

bf_settings_t *settings(void);
void settings_defaults(bf_settings_t *s);
void settings_load(void);
bool settings_save(void);

/* Pasta de dados: $BIGFOSTON_DATA ou o padrão do build
 * (/data/bigfoston no alvo, ./data no simulador). Criada se não existir. */
void        data_dir_init(const char *override);
const char *data_dir(void);
/* true quando a pasta de dados está na RAM (tmpfs/initramfs): os ajustes se
 * perdem ao desligar. */
bool        data_dir_volatile(void);
/* Monta "<data_dir>/<name>" em out. */
void        data_path(char *out, size_t n, const char *name);

#endif
