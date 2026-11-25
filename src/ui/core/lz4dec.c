/* lz4dec.c — ver lz4dec.h. Implementação própria do formato de bloco LZ4
 * (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "lz4dec.h"
#include <string.h>

/* comprimento estendido: 15 + bytes seguintes até um < 255 */
static int ext_len(const uint8_t **ip, const uint8_t *ie, size_t *len)
{
    unsigned b;
    do {
        if(*ip >= ie) return -1;
        b = *(*ip)++;
        *len += b;
    } while(b == 255);
    return 0;
}

long lz4_block_decode(const uint8_t *src, size_t sn, uint8_t *dst, size_t dn)
{
    const uint8_t *ip = src, *ie = src + sn;
    uint8_t *op = dst, *oe = dst + dn;
    while(ip < ie) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if(lit == 15 && ext_len(&ip, ie, &lit)) return -1;
        if((size_t)(ie - ip) < lit || (size_t)(oe - op) < lit) return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if(ip == ie) break;                           /* a última sequência só tem literais */
        if(ie - ip < 2) return -1;
        size_t off = (size_t)ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        if(!off || off > (size_t)(op - dst)) return -1;
        size_t ml = (tok & 15u) + 4;
        if((tok & 15u) == 15 && ext_len(&ip, ie, &ml)) return -1;
        if((size_t)(oe - op) < ml) return -1;
        const uint8_t *m = op - off;
        if(off >= ml) {
            memcpy(op, m, ml);
            op += ml;
        }
        else if(off == 1) {                           /* um byte repetido (o que não mudou no delta) */
            memset(op, *m, ml);
            op += ml;
        }
        else {                                        /* sobreposto: o padrão de 'off' bytes se repete;
                                                       * cada cópia dobra a distância (sempre múltipla de off) */
            while(ml) {
                size_t n = (size_t)(op - m);
                if(n > ml) n = ml;
                memcpy(op, m, n);
                op += n;
                ml -= n;
            }
        }
    }
    return (long)(op - dst);
}
