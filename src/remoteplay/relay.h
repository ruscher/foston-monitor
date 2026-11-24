/* relay.h — o bfrp no PC (relay.c) e o cliente no FOSTON (client.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BFRP_RELAY_H
#define BFRP_RELAY_H
#include <stdint.h>

/* PC */
void relay_attach(int fd);                               /* o FOSTON conectado (-1: nenhum) */
int  relay_send(char type, const void *data, uint32_t len);
void relay_ack(void);                                    /* o FOSTON desenhou o quadro */
void relay_set_target(int w);                            /* largura do quadro enviado */
void relay_stats(unsigned long *decoded, unsigned long *sent, unsigned long long *bytes);

/* FOSTON: conecta no relay e faz a ponte stdin/stdout <-> relay; 0 = terminou, -1 = sem relay */
int  relay_client_main(const char *host, int port);
#endif
