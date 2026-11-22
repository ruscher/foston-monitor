# Antes e depois (medido no aparelho)

Roteiro `BIGFOSTON_BENCH` (mapa nacional `brasil.bgm`, Brasília -15,7975 / -47,8919 z15,
rotas DF), mesmo aparelho, mesmo cartão, GPS ligado em ambiente fechado (sem posição),
21/11/2026. "Antes" = firmware de 21/11 18:05 (commit `686c0d2`); "depois" = commit
`a46d9c0` + detalhe automático. Tempos em ms; "laço" é uma volta de `lv_timer_handler`
(o atraso máximo para atender um toque). Os quadros do motor ficam mais longos no
relógio *depois* porque a thread do motor passou a ter prioridade menor de propósito:
a interface vem primeiro.

| Métrica | Antes | Depois | Diferença |
|---|---|---|---|
| Arrasto: quadros/s da tela | 3,6 | **17,6** | 4,9× |
| Arrasto: desenho do LVGL médio / p95 | 256 / 357 | **48 / 76** | −81 % / −79 % |
| Arrasto: volta do laço média / máx | 261 / 408 | **29 / 109** | −89 % / −73 % |
| Arrasto só do LVGL (sem motor): desenho médio | 129 | **41** | −68 % |
| Rotação: quadros/s | 5,1 | **12,6** | 2,5× |
| Rotação: volta do laço máx | 320 | **106** | −67 % |
| Seguir GPS: desenho p95 / laço máx | 209 / 246 | **74 / 115** | −65 % / −53 % |
| Navegação: desenho p95 / laço máx | 342 / 722 | **85 / 235** | −75 % / −67 % |
| Navegação: quadro do motor médio | 178 | **156** (econômico) | −12 % |
| Zoom: desenho p95 / laço máx | 129 / 172 | **38 / 66** | −71 % / −62 % |
| Duração do roteiro (fases somam 59,5 s nominais) | 97 s | **62 s** | as fases deixaram de atrasar |
| Motor isolado (`bgview`), quadro médio z12 / z13 / z14 / z15 / z16 — detalhado | 274 / 157 / 355 / 173 / 100 | — | — |
| idem — econômico | — | **118 / 95 / 130 / 95 / 70** | 2-3× |
| Repouso: iowait aparente | ~20 % | **0 %** | GPS sem bloqueio |
| Repouso: acordadas/s do shell | 57 | **4** | −93 % |
| Repouso: CPU do shell | ~3 % | **~1,9 %** | −37 % |
| MemAvailable mínimo no roteiro | 87,0 MB | 84,8 MB | buffer de tela inteira (+1,5 MB) |
| RSS do shell (navegação) | 12,4 MB | 14,1 MB | idem |
| Swap usada / faltas maiores | 0 / 0 | 0 / 0 | — |

## Firmware instalado (conferência)

Mesmo roteiro com o firmware instalado (`/usr/bin/bigfoston-shell`, atualização aplicada
às 21:47 de 21/11/2026, kernel com zram e PSI):

| Fase | Quadros/s | Desenho médio / p95 | Laço médio / máx |
|---|---|---|---|
| arrasto | 17,4 | 49,6 / 89 | 40,9 / 126,7 |
| arrasto só do LVGL | 17,6 | 46,2 / 86 | 34,8 / 115,5 |
| rotação | 12,8 | 44,6 / 80 | 25,8 / 112,5 |
| seguir GPS | 2,9 | 19,1 / 67 | 3,0 / 78,9 |
| navegação | 2,7 | 36,4 / 104 | 6,0 / 316,1 |
| zoom | 1,9 | 12,9 / 53 | 1,7 / 120,1 |

O roteiro levou 63 s. O `MemAvailable` mínimo foi 77,5 MB, sem swap e sem falta maior de
página. Na navegação, um quadro do motor levou 1,3 s, quando uma rota nova foi carregada
do cartão. Ele roda em outra thread; a interface teve no máximo 316 ms numa volta.

Com o sistema parado, a CPU total é de ~11 % ([20](20-final-report.md)).

## Etapas (arrasto, quadros/s da tela)

| Etapa | Quadros/s | Desenho do LVGL médio |
|---|---|---|
| base | 3,6 | 256 |
| motor com nice 10 | 6,0 | 160 |
| rótulos compostos no quadro | 7,7 | 125 |
| LVGL pula as telas cobertas (patch 0005) | 12,0 | 79 |
| buffer de tela inteira + cache de sombras | 16,5 | 55 |
| fundo só nas faixas descobertas, controles opacos | 17,5 | 47 |

Os arquivos de cada execução ficam em `/tmp/bench-*.txt` no aparelho durante a sessão; as
linhas acima foram copiadas deles.

## Ainda não medido

Latência do toque ao primeiro pixel (precisaria de câmera ou de um sensor), navegação real
com GPS em movimento e uso por horas ([15](15-stress-tests.md)).
