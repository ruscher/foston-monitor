# Auditoria: de onde vinham os ~2 FPS na navegação

Medido em 25/11/2026 no FOSTON FS-707DC (BigFoston OS 0.5.0 instalado), com o roteiro de bench
que já existia (`BIGFOSTON_BENCH`, [docs/performance/10-rendering-fps.md](../performance/10-rendering-fps.md)).
Posições simuladas num ponto público de Brasília; nada da localização do usuário.

## Números da versão instalada

| Fase do bench | Quadros de tela/s | CPU do shell | Quadro do motor (p50 / p95) |
|---|---|---|---|
| Arrasto do mapa | 18,0 | 83 % | 317 / 779 ms (cache frio) |
| Rotação (6° a cada 100 ms) | 12,7 | 76 % | 168 / 321 ms |
| Seguir (GPS 1 Hz, mapa no rumo) | **2,9** | 22 % | 140 / 261 ms |
| Navegação simulada | **2,5** | 20 % | 69 / 1426 ms |

O usuário via "uns 2 FPS" ao navegar, e o número confere. **O renderizador não era o gargalo**:
na navegação a CPU ficava 80 % ociosa.

## Causa

[`scr_map.c`](../../src/ui/screens/scr_map.c), antes desta mudança:

1. O `tick_cb` lê o GPS uma vez por segundo (`gps_ms >= 1000`) e chama `on_fix`.
2. `on_fix` coloca o centro do mapa na posição nova e pede ao motor um quadro inteiro.
   Na navegação o mapa gira no rumo ("direção para cima" é ligada sozinha), então a rotação
   também muda.
3. Enquanto o quadro novo não fica pronto, `update_offset` desloca a imagem anterior, mas
   **só quando a rotação é exatamente a mesma**. Com o mapa no rumo isso quase nunca acontecia,
   então a tela ficava parada.
4. Resultado: um quadro novo por segundo, e o carro e o mapa saltavam 14 m de uma vez a 50 km/h.
   O documento antigo descrevia isso como comportamento esperado
   ("a tela redesenha quando o quadro do motor chega").

Além disso, o marcador do carro era posto na posição crua do GPS, e a câmera no ponto
casado na via (`N.st.x/y`). Os dois podiam divergir alguns metros.

## O que o bench antigo inflava

Na fase "rotação", os 12,7 FPS contavam também redesenhos da **mesma** imagem:
`update_offset` invalidava a tela toda a cada chamada, mesmo sem nada mudar. Depois da correção
são 6,1 FPS nessa fase, com mais quadros realmente novos do motor (25 contra 17 no mesmo
tempo). O número menor é o honesto.

## Estado do resto (para as fases seguintes)

- Motor de mapas: renderizador próprio em C (RGB565, `raster.c`/`bgrender.c`), uma thread com
  `nice 10`, duplo buffer, rótulos compostos no quadro. Bom e medido, então foi mantido.
- Roteamento: `bgrouter.c`/`bgroute.c` (Contraction Hierarchies). Ainda sem paradas
  intermediárias nem rotas alternativas.
- Navegação: `bgnav.c` (casamento com a rota, manobras, rotatórias com número da saída).
- Busca: `bgsearch.c` (índice binário, thread própria).
- **Voz: eSpeak NG**, um processo por aviso (`core/voice.c`), 5,6 MB em
  `BigFoston/big-gps/voz` no cartão. **Não há RHVoice no projeto**: não há o que remover dele.
  A troca pela Piper Faber é a fase 8 ([22-piper-faber.md](22-piper-faber.md), a escrever).
- Branches: o trabalho de mapas estável está no `main` (0.5.1). `perf/bigfoston-extreme-optimization`
  já foi integrada. Esta frente fica em `feat/big-gps-apple-maps-experience`, criada do `main`.
