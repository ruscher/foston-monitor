/* settings.h — ajustes persistidos em $BIGFOSTON_DATA/settings.conf (chave=valor). */
#ifndef BF_SETTINGS_H
#define BF_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include "i18n.h"

typedef struct {
    bf_lang_t lang;
    bool dark;          /* tema escuro (padrão) */
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

bf_settings_t *settings(void);
void settings_defaults(bf_settings_t *s);
void settings_load(void);
bool settings_save(void);

/* Pasta de dados: $BIGFOSTON_DATA ou o padrão do build
 * (/data/bigfoston no alvo, ./data no simulador). Criada se não existir. */
void        data_dir_init(const char *override);
const char *data_dir(void);
/* Monta "<data_dir>/<name>" em out. */
void        data_path(char *out, size_t n, const char *name);

#endif
