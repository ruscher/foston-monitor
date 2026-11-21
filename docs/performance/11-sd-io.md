# Cartão SD

- Leitura sequencial: 7,0 MB/s (`sdlat`, arquivo de rotas, cache frio).
- **Efeito na interface:** com o cartão lendo sem parar, uma thread que dorme 10 ms (como o
  laço da interface) acordou com atraso p50 0,22 ms, p95 1,6 ms, máx 16 ms (repouso: máx
  2,5 ms). O driver por PIO não segura a CPU por tempo perceptível.
- **O problema real era o `flush`:** cada `close()` de arquivo aberto para escrita no FAT
  dorme ~100 ms. Quem fazia isso o tempo todo era o GPS (`BEE.BIN`/`ARC.BIN`), corrigido
  com o espelho em RAM ([03](03-cpu-profiling.md)). A opção `flush` continua (protege dados
  se o cartão sair).
- Mapas: o motor lê com `pread` e cache de tiles; leituras do cartão por quadro ficam em
  0 bytes com o cache quente (`bgview`: "lidos 0 bytes"); o pior caso no roteiro (navegação,
  rota nova) foi 215 ms de leitura num quadro.
