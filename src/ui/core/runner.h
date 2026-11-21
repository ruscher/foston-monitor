/* runner.h — execução de aplicativos/comandos via fork/exec.
 *
 * - Sem shell por padrão: o texto é dividido em argumentos (aspas simples e
 *   duplas respeitadas) e passado direto ao execvp.
 * - Modo terminal: /bin/sh -c "<texto>", saída (stdout+stderr) capturada.
 * - Nunca roda como root implicitamente: se o shell for root, o filho troca
 *   para $BIGFOSTON_RUN_USER (padrão "bigfoston"); se não existir, recusa —
 *   a menos que BIGFOSTON_ALLOW_ROOT=1 esteja definido explicitamente.
 * - Um job capturado por vez; leitura não bloqueante por lv_timer (100 ms)
 *   que só existe enquanto o processo roda (sem custo em repouso). */
#ifndef BF_RUNNER_H
#define BF_RUNNER_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*runner_out_cb)(const char *data, int len, void *ud);
typedef void (*runner_exit_cb)(int exit_code, int signal, void *ud);

/* Retorna o PID (>0) ou -errno. Em erro, msg recebe a explicação. */
int  runner_exec(char *const argv[], bool capture, runner_out_cb out, runner_exit_cb done,
                 void *ud, char *msg, size_t msgn);
int  runner_shell(const char *cmd, runner_out_cb out, runner_exit_cb done, void *ud,
                  char *msg, size_t msgn);
bool runner_busy(void);
void runner_stop(void);             /* SIGTERM no job atual */
void runner_forget(void);           /* desliga callbacks (tela fechada); mata o job */
/* Mesma regra de usuário para o Terminal: se o shell é root, escolhe
 * $BIGFOSTON_RUN_USER (uid != -1); erro -EPERM se não existir e
 * BIGFOSTON_ALLOW_ROOT != 1. runner_child_setup roda no filho. */
#include <sys/types.h>
int  runner_resolve_user(uid_t *uid, gid_t *gid, char *home, size_t nh, char *name, size_t nn,
                         char *msg, size_t msgn);
void runner_child_setup(uid_t uid, gid_t gid, const char *home, const char *name);
/* Divide uma linha em argv (modifica buf). Retorna argc. */
int  runner_split(char *buf, char **argv, int max);

#endif
