/* sdctl.h — controle do cartão SD pela interface (/usr/sbin/bigfoston-sd).
 * Executa com fork/exec e argumentos fixos (sem shell), numa thread: montar
 * pode passar pelo fsck.fat e ejetar espera o sync. */
#ifndef BF_SDCTL_H
#define BF_SDCTL_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { SD_ABSENT, SD_RO, SD_RW, SD_EJECTED, SD_MOUNTING, SD_UNKNOWN } sd_state_t;

/* rápido (só lê /proc/mounts e /dev, sem processo): dá para chamar a cada
 * segundo. UNKNOWN sem o programa (simulador). MOUNTING: o cartão está no slot,
 * sem montar e sem ter sido ejetado — o observador está montando. */
sd_state_t sdctl_status(void);
bool sdctl_available(void);
/* começa "eject" ou "mount"; false se já há uma operação */
bool sdctl_start(bool eject);
/* true quando terminou: rc (0 ok, 2 ocupado) e a última linha da saída */
bool sdctl_done(int *rc, char *msg, size_t n);
bool sdctl_busy(void);
#endif
