# zram

**Decisão: o kernel tem zram (LZ4, LZO, LZO-RLE, ZSTD), mas ele não é ativado no boot.**

## Por que não

- Com o mapa nacional, rotas e navegação, o `MemAvailable` mínimo foi **77,5-84,8 MB** de
  119 MB ([04](04-memory-analysis.md), [14](14-benchmarks-before-after.md)). Nenhuma página
  foi para swap e não houve falta maior de página. Não há o que comprimir.
- PSI de memória no aparelho depois do roteiro do mapa: `some avg10=0.00 avg60=0.03`.
- Neste processador, comprimir e descomprimir custa caro (medida abaixo). Uma página de
  4 KB leva ~0,3 ms para voltar com LZ4 e ~0,8 ms com ZSTD. Se o shell precisasse de
  páginas comprimidas durante o arrasto, cada quadro ficaria mais lento. É o oposto do
  objetivo deste projeto.

## Medida no aparelho (21/11/2026)

Script `zrambench.sh` (só RAM, nada no cartão): amostra de 24 MB com quadros RGB565 da
tela, o binário do shell, 8 MB do `brasil.bgm` (tiles já compactados) e 10 % de dados
aleatórios. Ele grava em `/dev/zram0` com `dd bs=64k conv=fsync`, limpa o cache e lê de
volta. Fiz duas execuções, que deram o mesmo resultado com variação abaixo de 10 %.

| Compressor | Comprime (MB/s) | Descomprime (MB/s) | RAM usada para 24 MB | Taxa |
|---|---|---|---|---|
| lz4 | 4,5-5,0 | 12,0-12,8 | 9,3 MB | 2,57 |
| lzo-rle | 5,4-5,5 | 12,2-12,6 | 9,1 MB | 2,64 |
| lzo | 5,3-5,5 | 12,6-12,9 | 9,0 MB | 2,66 |
| zstd | 1,7 | 4,9-5,0 | 7,3 MB | 3,28 |

As vazões incluem o caminho de bloco (`dd`, 64 KB por vez) e são limitadas pela banda de
memória do aparelho (58 MB/s em `memcpy`, [01](01-hardware-baseline.md)). Neste ARM1176,
o LZ4 não é mais rápido que o LZO.

## Se um dia faltar memória

Use **lzo-rle**, com o disco do tamanho de ~25 % da RAM e prioridade acima de qualquer
swap:

```sh
echo lzo-rle > /sys/block/zram0/comp_algorithm
echo 32M > /sys/block/zram0/disksize
mkswap /dev/zram0 && swapon -p 100 /dev/zram0
```

Antes, meça com `bf-perf-monitor` e `/proc/pressure/memory` e confira que a pressão de
memória é real. Swap no cartão FAT32 continua descartado ([06](06-swap-sd.md)).
