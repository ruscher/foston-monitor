/* cefmt.h — printf do Windows CE lido de um vetor de palavras: os argumentos
 * vêm como o código do CE os empilha (doubles em 2 palavras, palavra baixa
 * primeiro, sem alinhamento de 8 bytes). Saída em UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BF_CEFMT_H
#define BF_CEFMT_H
#include <stddef.h>
#include <stdint.h>

/* fmt em UTF-8; wide: %s é UTF-16 (wsprintfW/NKDbgPrintfW). Avança *argp. */
size_t ce_vformat(char *out, size_t n, const char *fmt, int wide, const uint32_t **argp);
size_t w16_len(const uint16_t *s);
size_t w16_to_utf8(char *out, size_t n, const uint16_t *s);
size_t utf8_to_w16(uint16_t *out, size_t n, const char *s);
#endif
