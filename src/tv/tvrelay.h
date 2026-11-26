/* tvrelay.h — a TV do FOSTON decodificada no PC (como o relay do Big Remote Play).
 *
 * O PC (bftv-relay) conecta no FOSTON (bftvd escuta em TVRELAY_PORT na rede USB; o firewall do
 * PC não precisa abrir nada) e troca mensagens no formato de src/remoteplay/proto.h:
 * tipo (1 byte) + tamanho (u32 LE) + conteúdo.
 *
 *   FOSTON -> PC   H "BFTV1 l a": começo; l a = tamanho do quadro que o FOSTON quer (metade do
 *                    retângulo do vídeo: ele amplia 2x; 0 0 = o do canal)
 *                  T bytes do TS, como chegam da antena
 *                  S "l a": o retângulo mudou (tela cheia / janela)
 *                  R a soma não bateu: o próximo quadro vai inteiro
 *   PC -> FOSTON   V pts (i64 LE, µs) + quadro yuvz (src/tv/yuvz.h)
 *                  A pts (i64 LE, µs) + PCM S16_LE mono 32 kHz
 *
 * O PC decodifica o H.264 inteiro (todos os quadros, com o filtro de desblocagem) e o HE-AAC, e
 * reamostra com o swresample; o FOSTON só descomprime, desenha na hora do relógio do som e toca.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BF_TVRELAY_H
#define BF_TVRELAY_H
#include <stdint.h>
#include "../remoteplay/proto.h"

#define TVRELAY_PORT 48122
#define TVRELAY_RATE 32000

static inline void tvrelay_put_pts(uint8_t *p, int64_t v) { for(int i = 0; i < 8; i++) p[i] = (uint8_t)((uint64_t)v >> 8 * i); }
static inline int64_t tvrelay_get_pts(const uint8_t *p)
{
    uint64_t v = 0;
    for(int i = 0; i < 8; i++) v |= (uint64_t)p[i] << 8 * i;
    return (int64_t)v;
}
#endif
