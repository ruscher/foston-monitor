/* fileops.h — operações de arquivo do app Arquivos (copiar, mover, apagar,
 * renomear, criar pasta) com progresso, numa thread.
 *
 * Só grava dentro das raízes permitidas (fo_writable): o cartão SD montado com
 * gravação e a pasta de dados do shell. O sistema (/) nunca é alterado. */
#ifndef BF_FILEOPS_H
#define BF_FILEOPS_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { FO_COPY, FO_MOVE, FO_DELETE } fo_op_t;
typedef enum { FO_IDLE, FO_RUNNING, FO_DONE, FO_FAILED, FO_CANCELLED } fo_state_t;

typedef struct {
    fo_state_t state;
    fo_op_t op;
    long long done, total;         /* bytes (apagar: itens) */
    char current[256];             /* nome do arquivo em curso */
    char err[160];
    char result[1024];             /* caminho final (copiar/mover) */
} fo_status_t;

/* o caminho está num local gravável (SD com gravação ou pasta de dados)? */
bool fo_writable(const char *path);
/* nome válido para FAT/Linux (sem / \ : * ? " < > |, sem só pontos, <= 200 bytes) */
bool fo_valid_name(const char *name);

/* operações rápidas, síncronas: 0 ou mensagem em err */
bool fo_mkdir(const char *dir, const char *name, char *err, size_t n);
bool fo_rename(const char *path, const char *new_name, char *err, size_t n);

/* operações longas: começam a thread; false se já há uma em curso ou se o
 * pedido é inválido (err explica) */
bool fo_start(fo_op_t op, const char *src, const char *dst_dir, char *err, size_t n);
void fo_cancel(void);
/* cópia do estado atual (thread-safe) */
void fo_get(fo_status_t *out);
/* depois de DONE/FAILED/CANCELLED: volta a IDLE */
void fo_reset(void);
#endif
