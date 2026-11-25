/* lz4dec.h — descompressão de um bloco LZ4 (formato de bloco, sem o cabeçalho de quadro).
 * Usado pela imagem do modo completo do app Internet: no ARM1176 do FOSTON um quadro de
 * vídeo de 800x380 sai em ~57 ms, contra ~107 ms da zlib e ~225 ms do inflate do lodepng.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_LZ4DEC_H
#define BF_LZ4DEC_H
#include <stddef.h>
#include <stdint.h>

/* bytes escritos em dst, ou -1 se o bloco estiver corrompido ou não couber em dn
 * (nunca lê fora de src nem escreve fora de dst) */
long lz4_block_decode(const uint8_t *src, size_t sn, uint8_t *dst, size_t dn);
#endif
