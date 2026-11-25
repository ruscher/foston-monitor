# Benchmarks no aparelho

Como rodar: igual a [docs/performance/10-rendering-fps.md](../performance/10-rendering-fps.md)
(binário de teste em `/tmp`, o shell instalado é parado e volta no fim):

```
BIGFOSTON_DATA=/data/bigfoston BIGFOSTON_BENCH=/tmp/bench.txt /tmp/shell-perf --open map
```

"Quadros/s" é o número de redesenhos do LVGL por segundo (`render n / tempo`). Agora ele inclui
redesenhos pequenos, só do marcador (p50 de 1–8 ms), que também são imagem nova. Quadros
repetidos não entram mais (ver [01-auditoria.md](01-auditoria.md)).

## 25/11/2026 — fase 2 (movimento separado do GPS)

FOSTON FS-707DC, mapa do DF, posições simuladas, detalhe automático (econômico ao navegar).
"Antes" é o BigFoston OS 0.5.0 instalado; "depois" é a branch `feat/big-gps-apple-maps-experience`.

| Fase | Quadros/s antes → depois | CPU antes → depois | Motor p50 antes → depois | Laço da interface p95 |
|---|---|---|---|---|
| Seguir (1 Hz, mapa no rumo) | 2,9 → **19,1** | 22 % → 43 % | 140 → 170 ms | 7 → 49 ms |
| Navegação simulada | 2,5 → **14,3** | 20 % → 46 % | 69 → 94 ms | 24 → 80 ms |
| Arrasto | 18,0 → 18,3 | 83 % → 88 % | — | 77 → 67 ms |
| Rotação | 12,7* → 6,1 | 76 % → 61 % | 168 → 113 ms | 70 → 60 ms |
| RSS na navegação | 14,9 → 14,5 MB | | | |

\* inflado por redesenhos da mesma imagem.

Tentativa intermediária, descartada: invalidar a tela a cada passo de 50 ms deu 14,9 quadros/s
em "seguir", mas com 89 % de CPU e quadros do motor de 403 ms (p50). O motor perdia a CPU para
redesenhos que não mudavam nada.

Ainda não medido (fases seguintes): com Bluetooth A2DP tocando, com a voz falando, no
cache frio e no tema claro, e um percurso real gravado.
