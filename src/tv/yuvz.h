/* yuvz.h — quadros YUV 4:2:0 comprimidos para a rede USB, o mesmo formato do relay do Big Remote
 * Play (src/remoteplay/relay.c, proto.h 'V'):
 *   l a (u16 LE) + flags (bit 0 delta, bit 1 soma) [+ soma u32 LE] [+ máscara dos pares de
 *   linhas, 1 bit por par, se delta] + LZ4.
 * Inteiro = Y, U, V contíguos; delta = só os pares de linhas que mudaram (2 linhas de Y, a de U e
 * a de V que elas usam), em XOR com o quadro anterior, em sequência.
 * O codificador (PC) quantiza o brilho em 6 bits e a cor em 5 (a tela RGB565 não mostra mais),
 * com histerese contra o quadro anterior, e comprime com LZ4 HC; o decodificador (FOSTON) usa o
 * LZ4 de src/ui/core/lz4dec.c. Largura múltipla de 8.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BF_YUVZ_H
#define BF_YUVZ_H
#include <stddef.h>
#include <stdint.h>

static inline size_t yuvz_size(int w, int h) { return (size_t)w * h + 2 * (size_t)(w / 2) * ((h + 1) / 2); }

/* ---- PC */
typedef struct {
    uint8_t *cur, *prev, *x, *zk, *zd, *out;
    size_t cap, zk_cap, zd_cap, out_cap;
    int w, h, have_prev, since_key;
    unsigned long n;
} yuvz_enc_t;
/* Codifica o quadro (Y, U, V contíguos, w x h). force_key: inteiro. Devolve o tamanho da mensagem
 * em *out (válida até a próxima chamada) ou -1. */
long yuvz_encode(yuvz_enc_t *e, const uint8_t *yuv, int w, int h, int force_key, const uint8_t **out);
void yuvz_enc_free(yuvz_enc_t *e);

/* ---- FOSTON */
typedef struct {
    uint8_t *yuv, *x;
    size_t cap, x_cap;
    int w, h;                       /* quadro em yuv (0: nenhum) */
    unsigned long bad;              /* somas que não bateram */
} yuvz_dec_t;
/* Aplica uma mensagem: 1 quadro novo em d->yuv; 0 nada mudou; -1 sem a base ou soma errada (peça
 * um inteiro). */
int yuvz_decode(yuvz_dec_t *d, const uint8_t *p, size_t n);
void yuvz_dec_free(yuvz_dec_t *d);
#endif
