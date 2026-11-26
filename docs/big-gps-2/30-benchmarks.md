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

## 25/11/2026 — com alertas, voz Faber e zoom gradual

Mesmo roteiro, mesmo aparelho, pacote de alertas do Brasil (169 mil pontos) e voz Faber no cartão.

| Fase | Quadros/s | CPU | Motor p50 / p95 | Laço p95 | RSS |
|---|---|---|---|---|---|
| Seguir | 19,6 | 44 % | 208 / 319 ms (desenho 193 ms; antes 162: ícones de semáforo e lombada no centro) | 44 ms | 8,7 MB |
| Navegação | 15,5 | 46 % | 104 / 463 ms | 79 ms | 15,3 MB (+0,8 MB: arquivos mapeados) |
| Arrasto | 17,6 | 82 % | — | 86 ms | |
| Rotação | 5,9 | 61 % | 112 / 147 ms | 62 ms | |

Sem regressão de fluidez. O zoom pela velocidade agora muda ~0,1 nível por quadro do motor,
em vez de saltar de uma vez.

## 26/11/2026: GPS procurando satélites e música pelo Bluetooth

Primeira medição nesse cenário. O FOSTON estava dentro de casa ("Procurando satélites") e
tocava música do celular por A2DP:
- `bfgps` com 29–39 % de CPU: o `gps.dll` original trabalhando na busca, sem laço ocioso; a
  thread passa a maior parte do tempo esperando num futex;
- `bluealsa` com 14–20 % (decodificação SBC).

O motor do mapa roda com nice 10. Com esses dois processos de nice 0 ocupando a CPU, ele recebe
pouco tempo.

| Fase | motor nice 10 (padrão) | nice 5 | nice 0 | nice 10, bfgps com nice 10 |
|---|---|---|---|---|
| seguir | 1,2–2,5 q/s | 12,3 | 10,1 | 12,3 |
| navegacao | 9,5–11,1 q/s | 6,2 | 5,8 | 8,8 |
| radar (placa e bipes) | 6,4–8,7 q/s | 8,0 | 7,0 | 8,6 |

- Mesmo cenário com o shell instalado, sem a placa do radar: `seguir` 2,5 e `navegacao` 10,6.
  A placa não mudou o desempenho.
- No simulador do PC, o mesmo roteiro dá `seguir` 22,9 e `radar` 23,0 q/s: o código anima.
- Baixar o nice do motor ajuda o `seguir`, mas piora a navegação (o motor passa a disputar
  com a animação). Por isso o padrão continua nice 10.
- Baixar a prioridade do `bfgps` também ajuda, mas ele lê a serial do GPS, que já perdeu dados
  quando atrasou. Fica como está.
- **Falta medir com o GPS fixado (no carro):** quanto o `bfgps` gasta quando já tem posição.
  Se continuar alto, o próximo passo é ajustar as prioridades com esse dado.
