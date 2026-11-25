/* test_lz4dec.c — confere core/lz4dec.c contra a liblz4 oficial (só no PC).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../core/lz4dec.h"
#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void roundtrip(const char *name, const uint8_t *in, int n, int accel)
{
    int cap = LZ4_compressBound(n);
    char *z = malloc((size_t)cap);
    int zn = LZ4_compress_fast((const char *)in, z, n, cap, accel);
    uint8_t *out = malloc((size_t)n + 1);
    long got = lz4_block_decode((const uint8_t *)z, (size_t)zn, out, (size_t)n);
    if(got != n || memcmp(out, in, (size_t)n)) { printf("FALHA %s (accel %d): %ld de %d\n", name, accel, got, n); fails++; }
    /* destino pequeno demais: recusa, sem escrever fora */
    if(n > 10 && lz4_block_decode((const uint8_t *)z, (size_t)zn, out, (size_t)n - 1) != -1) { printf("FALHA %s: destino curto aceito\n", name); fails++; }
    /* bloco truncado ou com bytes trocados: nunca passa dos limites (o ASan avisaria) */
    for(int cut = 1; cut < zn && cut < 64; cut += 7) lz4_block_decode((const uint8_t *)z, (size_t)(zn - cut), out, (size_t)n);
    for(int i = 0; i < 200 && zn > 0; i++) {
        char *bad = malloc((size_t)zn);
        memcpy(bad, z, (size_t)zn);
        bad[rand() % zn] ^= (char)(1 + rand() % 255);
        lz4_block_decode((const uint8_t *)bad, (size_t)zn, out, (size_t)n);
        free(bad);
    }
    free(out);
    free(z);
}

int main(void)
{
    srand(7);
    enum { N = 800 * 64 * 2 };
    uint8_t *buf = malloc(N);
    memset(buf, 0, N);                                          /* tudo igual (página branca) */
    roundtrip("zeros", buf, N, 1);
    for(int i = 0; i < N; i++) buf[i] = (uint8_t)rand();        /* ruído (vídeo) */
    roundtrip("ruido", buf, N, 1);
    for(int i = 0; i < N; i++) buf[i] = (uint8_t)((i / 2) % 7 == 0 ? rand() : i % 13);   /* misto */
    roundtrip("misto", buf, N, 1);
    roundtrip("misto8", buf, N, 8);
    for(int i = 0; i < N; i += 2) { buf[i] = 0x1f; buf[i + 1] = (uint8_t)(i % 6 ? 0xf8 : 0x07); }   /* padrões curtos */
    roundtrip("padrao", buf, N, 1);
    roundtrip("um", buf, 1, 1);
    roundtrip("vazio13", buf, 13, 1);
    free(buf);
    printf("%s\n", fails ? "ERROS" : "lz4dec OK");
    return fails != 0;
}
