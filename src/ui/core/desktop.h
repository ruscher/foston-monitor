/* desktop.h — entradas de aplicativo estilo .desktop e listas persistentes
 * (histórico/favoritos) em arquivos de texto simples.
 *
 * Dois tipos de entrada:
 *   - comando: um .desktop solto em <dados>/apps ou /usr/share/bigfoston/apps (aparece no Run);
 *   - app instalado: um .desktop dentro da pasta do app, <dados>/apps/<id>/ (pacote .bfapp,
 *     core/pkg.c). Aparece também na grade de Apps, no grupo da categoria (Categories=).
 * Chaves além das do freedesktop.org:
 *   X-BigFoston-Screen=nome   abre uma tela do shell (ex.: sc-controller) em vez do Exec
 *   X-BigFoston-Color=#rrggbb cor do ícone na grade
 *   Icon=arquivo.svg          relativo à pasta do app; desenhado como os ícones do sistema
 *                             (silhueta branca sobre o degradê da cor) */
#ifndef BF_DESKTOP_H
#define BF_DESKTOP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char name[64];
    char comment[96];
    char exec[256];
    bool terminal;      /* Terminal=true => saída capturada e exibida */
    char id[48];        /* app instalado: o nome da pasta; comando: o do arquivo */
    char dir[200];      /* pasta do app instalado ("" = comando) */
    char icon[300];     /* caminho do ícone ("" = a inicial do nome) */
    char categories[128];
    char screen[32];    /* X-BigFoston-Screen */
    uint32_t color;     /* X-BigFoston-Color (0 = a cor do grupo) */
} bf_app_entry_t;

/* Varre $BIGFOSTON_APPS (pastas separadas por ':') ou, por padrão,
 * "$BIGFOSTON_DATA/apps:/usr/share/bigfoston/apps", e as subpastas (apps instalados).
 * Ordena por nome. Retorna a quantidade; *out deve ser liberado com free(). */
int  desktop_scan(bf_app_entry_t **out);
/* lê um .desktop (dir = pasta do app instalado, ou NULL) */
bool desktop_parse(const char *path, const char *dir, bf_app_entry_t *a);

/* ------------------------------------------------- grupos da grade de Apps */

/* a categoria principal do freedesktop.org decide o grupo (a primeira da lista que tiver um) */
typedef enum {
    BF_GRP_GAMES, BF_GRP_MEDIA, BF_GRP_INTERNET, BF_GRP_GRAPHICS, BF_GRP_OFFICE, BF_GRP_EDU,
    BF_GRP_DEV, BF_GRP_TOOLS, BF_GRP_SYSTEM, BF_GRP_OTHER, BF_GRP__N
} bf_group_t;

bf_group_t desktop_group(const char *categories);
const char *desktop_group_name(bf_group_t g);
uint32_t    desktop_group_color(bf_group_t g);
const void *desktop_group_icon(bf_group_t g);          /* const lv_image_dsc_t * */
/* "games", "media"...: o nome fixo (tela do grupo, testes) */
const char *desktop_group_key(bf_group_t g);

#define BF_LIST_MAX 50
typedef struct { char *item[BF_LIST_MAX]; int n; } bf_strlist_t;

void strlist_load(bf_strlist_t *l, const char *file);   /* arquivo em data_dir */
bool strlist_save(const bf_strlist_t *l, const char *file);
void strlist_push_front(bf_strlist_t *l, const char *s);   /* sem duplicatas */
bool strlist_remove(bf_strlist_t *l, const char *s);
bool strlist_contains(const bf_strlist_t *l, const char *s);
void strlist_free(bf_strlist_t *l);
#endif
