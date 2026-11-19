/* i18n.h — tabela de textos por ID (pt-BR padrão, en, es). */
#ifndef BF_I18N_H
#define BF_I18N_H

typedef enum { LANG_PT = 0, LANG_EN, LANG_ES, LANG_COUNT } bf_lang_t;

typedef enum {
#define X(id, key, pt, en, es) id,
#include "i18n_strings.def"
#undef X
    S__COUNT
} bf_str_t;

void        i18n_set_lang(bf_lang_t lang);
bf_lang_t   i18n_get_lang(void);
const char *tr(bf_str_t id);                 /* texto no idioma atual */
const char *i18n_key(bf_str_t id);           /* chave estável (msgid) */
const char *i18n_lang_code(bf_lang_t lang);  /* "pt_BR", "en", "es" */
const char *i18n_lang_name(bf_lang_t lang);  /* nome nativo */
bf_lang_t   i18n_lang_from_code(const char *code);
/* Carrega um catálogo "chave = texto" (uma por linha, # comenta) que
 * sobrescreve os textos do idioma atual. Retorna nº de entradas aplicadas. */
int         i18n_load_catalog(const char *path);

#endif
