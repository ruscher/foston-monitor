/* proto.h — mensagens entre o relay do PC (relay.c) e o FOSTON (client.c), pela rede USB:
 * tipo (1 byte) + tamanho (u32 LE) + conteúdo, como no modo PC do Internet (webremote.c).
 *
 *   FOSTON -> PC   K linha de comando (os mesmos de bfrp.c: HOST, START, MABS, PAD...)
 *                  Q quadro desenhado (controle de fluxo: o PC manda o próximo, sempre o mais novo)
 *                  R a soma não bateu: o próximo quadro vai inteiro
 *   PC -> FOSTON   E linha de evento (HOST, APP, STARTED, VSTATS...)
 *                  V l a (u16 LE) + flags (bit 0 delta, bit 1 soma) [+ soma u32 LE] [+ máscara
 *                    dos pares de linhas, 1 bit por par, se delta] + LZ4: inteiro = Y, U, V do
 *                    quadro YUV 4:2:0 já cortado e reduzido; delta = só os pares que mudaram
 *                    (2 linhas de Y, a de U, a de V) em XOR com o anterior (relay.c)
 *                  A PCM S16_LE mono 32 kHz
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BFRP_PROTO_H
#define BFRP_PROTO_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define RELAY_PORT 48120

/* bytes de um quadro YUV 4:2:0 contíguo; altura ímpar: a cor tem (h + 1) / 2 linhas */
static inline size_t yuv_size(int w, int h) { return (size_t)w * h + 2 * (size_t)(w / 2) * ((h + 1) / 2); }
/* soma de conferência do quadro reconstruído (n múltiplo de 4) */
static inline uint32_t yuv_sum(const uint8_t *p, size_t n)
{
    const uint32_t *w = (const uint32_t *)(const void *)p;
    uint32_t a = 1, b = 0;
    for(size_t i = 0; i < n / 4; i++) { a += w[i]; b += a; }
    return a ^ (b << 7 | b >> 25);
}
#define RELAY_MAX_MSG (4u << 20)

static inline int proto_write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while(n) {
        ssize_t w = write(fd, b, n);
        if(w < 0) { if(errno == EINTR) continue; return -1; }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static inline int proto_send(int fd, char type, const void *data, uint32_t len)
{
    uint8_t h[5] = { (uint8_t)type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    if(proto_write_all(fd, h, 5)) return -1;
    return len ? proto_write_all(fd, data, len) : 0;
}

/* leitura incremental: acumula até ter uma mensagem inteira */
typedef struct { uint8_t *buf; size_t n, cap; } proto_rx_t;

/* 1: mensagem pronta em *type/*data/*len (vale até a próxima chamada); 0: falta; -1: erro */
static inline int proto_take(proto_rx_t *r, char *type, uint8_t **data, uint32_t *len, size_t *consumed)
{
    if(r->n < 5) return 0;
    uint32_t l = (uint32_t)r->buf[1] | (uint32_t)r->buf[2] << 8 | (uint32_t)r->buf[3] << 16 | (uint32_t)r->buf[4] << 24;
    if(l > RELAY_MAX_MSG) return -1;
    if(r->n < 5 + (size_t)l) return 0;
    *type = (char)r->buf[0];
    *data = r->buf + 5;
    *len = l;
    *consumed = 5 + (size_t)l;
    return 1;
}
#endif
