/* webdata.h — dados do app Internet: favoritos, histórico, WebApps e o texto da barra
 * de endereço (URL ou pesquisa). Arquivos TSV em <dados>/web/, gravados só quando
 * mudam (favoritos, WebApps) ou em lote (histórico), por troca atômica (.new + rename).
 * Tudo aqui é texto vindo de páginas: validado antes de gravar e ao ler. */
#ifndef BF_WEBDATA_H
#define BF_WEBDATA_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define WEB_URL_MAX    1024
#define WEB_TITLE_MAX  160
#define WEB_MARKS_MAX  300
#define WEB_HIST_MAX   300
#define WEB_APPS_MAX   24

typedef struct { char title[WEB_TITLE_MAX]; char url[WEB_URL_MAX]; time_t when; } web_item_t;
typedef struct { char name[48]; char url[WEB_URL_MAX]; unsigned color; } web_app_t;

/* motores de pesquisa (o primeiro é o padrão: funciona sem JavaScript) */
int         web_search_count(void);
const char *web_search_name(int i);

/* barra de endereço -> URL. Domínio sem esquema vira https://; IP e localhost viram
 * http://; texto com espaço ou sem ponto vira pesquisa. Recusa javascript: e outros
 * esquemas perigosos (false, out vazio). */
bool web_normalize(const char *text, int search_engine, char *out, size_t n);
/* esquemas que o motor pode abrir */
bool web_scheme_allowed(const char *url);
/* "https://www.exemplo.com/a/b" -> "exemplo.com" (para mostrar na barra) */
void web_host(const char *url, char *out, size_t n);

void web_data_load(void);            /* lê os três arquivos (chamar ao abrir o app) */
void web_data_flush(void);           /* grava o histórico pendente */

int               web_marks_count(void);
const web_item_t *web_mark(int i);
int               web_mark_find(const char *url);   /* índice ou -1 */
bool              web_mark_add(const char *title, const char *url);
void              web_mark_remove(int i);
bool              web_mark_edit(int i, const char *title, const char *url);

bool              web_history_enabled(void);
void              web_history_set_enabled(bool on);
int               web_history_count(void);          /* mais recente primeiro */
const web_item_t *web_history(int i);
void              web_history_add(const char *title, const char *url);
void              web_history_set_title(const char *url, const char *title);
void              web_history_remove(int i);
void              web_history_clear(void);

void              web_apps_load(void);              /* só os WebApps (tela de apps) */
int               web_apps_count(void);
const web_app_t  *web_app(int i);
int               web_app_find(const char *url);    /* índice ou -1 */
bool              web_app_add(const char *name, const char *url);   /* false: inválido, cheio ou já existe */
void              web_app_remove(int i);
bool              web_app_rename(int i, const char *name);

/* nome para mostrar (o nome, ou o domínio se o nome for vazio ou um endereço) e a
 * inicial para o ícone (o primeiro caractere UTF-8 inteiro, em maiúscula se for ASCII) */
const char *web_display_name(const char *name, const char *url, char *host, size_t n);
void              web_initial(const char *text, char *out, size_t n);

/* para testes no PC: pasta dos arquivos (padrão: data_path("web")) */
void web_data_set_dir(const char *dir);
#endif
