# Ferramentas e perfil de CPU

## Ferramentas criadas

- **`tools/bigfoston/bf-perf-monitor`** (C, estático, ~110 KB): lê só `/proc` a cada
  intervalo e imprime CPU (usr/sys/io/irq), trocas de contexto, interrupções,
  MemAvailable/Anon/Cached, faltas de página, swap e leitura/escrita do cartão, com as
  N threads que mais usaram CPU. Custo medido: ~3-4 % de CPU com `-t` (lê o `stat` de
  todas as threads a cada segundo), desprezível sem `-t`.
- **Roteiro do mapa** (`BIGFOSTON_BENCH=arquivo`, em `scr_map.c`): sem toque, num ponto
  público de Brasília, fases fixas (parado, arrasto, arrasto só do LVGL, zoom, rotação,
  seguir GPS simulado, navegação simulada) e uma linha por fase com quadros/s e
  p50/p95/máx do desenho do LVGL, de cada volta do laço e do quadro do motor
  (`core/perf.c`, histogramas de 1 ms; custo de um `clock_gettime` por evento).
- **Perfilador por amostragem** (`BIGFOSTON_PROF=arquivo`): `SIGPROF` a cada tick de CPU
  (10 ms, `HZ=100`) guarda PC e thread; `prof-resolve.py` soma por função com o `nm` do
  binário não reduzido. `bgview -p` faz o mesmo para o motor isolado.

## Achados

1. **GPS bloqueado no cartão**: a thread do `bfgps` vivia em `fat_file_release` (estado
   D). O motor abre `BEE.BIN`/`ARC.BIN` em leitura e escrita ~2 vezes/s (53 aberturas
   em 25 s, contadas com `bfgps -v`); o FAT é montado com `flush`, e cada fechamento de
   arquivo aberto para escrita dorme ~100 ms. Resultado: ~200 ms/s de bloqueio e ~20 %
   de iowait aparente em repouso. Corrigido com o `\ResidentFlash` em RAM
   (`/run/bfgps-flash`), gravado de volta no cartão quando muda (no máximo a cada 2 min
   e ao sair). Depois: **iowait 0 %**.
2. **Mapa**: perfil do arrasto só do LVGL — 40 % `memcpy`, 22 % preenchimento de cor,
   6 % mistura (antes das correções, 50 % `memcpy` depois). Ver [08](08-lvgl-optimization.md).
3. **Motor**: `span` 22 %, `fill_convex` 21 %, `chain_edge` 15 %, `slope_q15` 8 %,
   `transform` 8 % — o rasterizador já é enxuto; o ganho é desenhar menos ([09](09-gps-map-engine.md)).
4. **Repouso** (30 s, ticks por thread): `bfgps` leitura serial 2 % (quase tudo
   kernel: emula os tempos de espera de COM do WinCE), motor GPS 2,2 %, shell 2,9 %,
   `bigfoston-sd watch` ~1,3 % (fork de `sleep`/`grep` por segundo), kworkers ~1 %.
