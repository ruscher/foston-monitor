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
