/* desktop.h — entradas de aplicativo estilo .desktop e listas persistentes
 * (histórico/favoritos) em arquivos de texto simples. */
#ifndef BF_DESKTOP_H
#define BF_DESKTOP_H
#include <stdbool.h>

typedef struct {
    char name[64];
    char comment[96];
    char exec[256];
    bool terminal;      /* Terminal=true => saída capturada e exibida */
} bf_app_entry_t;

/* Varre $BIGFOSTON_APPS (pastas separadas por ':') ou, por padrão,
 * "$BIGFOSTON_DATA/apps:/usr/share/bigfoston/apps". Ordena por nome.
 * Retorna a quantidade; *out deve ser liberado com free(). */
int  desktop_scan(bf_app_entry_t **out);

#define BF_LIST_MAX 50
typedef struct { char *item[BF_LIST_MAX]; int n; } bf_strlist_t;

void strlist_load(bf_strlist_t *l, const char *file);   /* arquivo em data_dir */
bool strlist_save(const bf_strlist_t *l, const char *file);
void strlist_push_front(bf_strlist_t *l, const char *s);   /* sem duplicatas */
bool strlist_remove(bf_strlist_t *l, const char *s);
bool strlist_contains(const bf_strlist_t *l, const char *s);
void strlist_free(bf_strlist_t *l);
#endif
