/* webdl.h — downloads do app Internet, feitos pelo bfdl (src/web/bfdl.c).
 *
 * O motor (bfweb, sem privilégios) só pede: "DLREQ url\tnome\ttamanho\tmime\torigem\tcookie".
 * Aqui o nome é higienizado e tornado único em BigFoston/Downloads, e um bfdl por
 * arquivo grava "<nome>.part" e só renomeia no fim. Pausar encerra o bfdl mantendo o
 * .part; continuar retoma com HTTP Range (If-Range: o arquivo não pode ter mudado).
 * Até 2 ao mesmo tempo; os outros esperam na fila. A lista (sem o progresso) fica em
 * <dados>/web/downloads.tsv, então um download pausado continua depois de reiniciar.
 * Downloads seguem com o app Internet fechado. Nada baixado é aberto sozinho. */
#ifndef BF_WEBDL_H
#define BF_WEBDL_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { WEBDL_QUEUED, WEBDL_RUNNING, WEBDL_PAUSED, WEBDL_DONE, WEBDL_FAILED } webdl_state_t;

typedef struct {
    int id;
    char name[200], path[512], url[1024], referer[1024], cookie[2048];
    long long total, done, speed;    /* bytes; speed em B/s */
    webdl_state_t state;
    char error[96];
} webdl_t;

typedef void (*webdl_listener)(void *ud);

void webdl_init(void);                       /* lê a lista (uma vez) */
const char *webdl_dir(void);
bool webdl_request(const char *dlreq, char *name_out, size_t n);
int  webdl_count(void);
const webdl_t *webdl_at(int i);
const webdl_t *webdl_find(int id);
int  webdl_active(void);                     /* baixando ou na fila */
void webdl_pause(int id);
void webdl_resume(int id);
void webdl_cancel(int id);                   /* apaga o .part (o arquivo pronto fica) */
void webdl_set_listener(webdl_listener cb, void *ud);   /* muda de estado ou progresso */
void webdl_open_file(const char *path);      /* abre no app certo; senão nos Arquivos */
/* testes: nome seguro para FAT32 (sem caminho, sem caracteres proibidos) */
void webdl_safe_name(const char *in, char *out, size_t n);
#endif
