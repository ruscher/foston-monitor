# Big GPS — análise do hardware (fase 1)

Medições feitas no próprio FOSTON FS-707DC, rodando BigFoston OS (Linux 6.12.117) com a
interface (`bigfoston-shell`) e o serviço de GPS (`bfgps`) ativos, em 2026-11-21.
Programas de medição: [tools/big-gps/bench/](../../tools/big-gps/bench/).

## Orçamento do aparelho

| Recurso | Medido | Observação |
|---|---|---|
| CPU | ARM1176JZ-S, `BogoMIPS 466.94`, ARMv6 | `Features: half thumb fastmult edsp java tls` — **sem VFP** |
| RAM total para o Linux | 121 988 kB | 128 MB físicos |
| RAM livre (sistema completo) | 103 540 kB livres, `MemAvailable` 104 480 kB | interface 2,0 MB RSS, bfgps 2,7 MB RSS |
| Swap | nenhuma | `overcommit_memory = 0` |
| CPU em uso sem navegação | bfgps ~12 % | `top` com a tela parada |
| Cartão SD | 29,7 GB, 29,6 GB livres (FAT, gravação validada pelo driver) | |
| SD, leitura aleatória 4 KiB | **2,70 ms** por leitura (500 leituras, cache limpo) | `iobench` |
| SD, leitura sequencial | **6,94 MB/s** | arquivo de 64 MB |
| Tela | 800×480, RGB565, `/dev/fb0` | toque resistivo de um ponto |
| Áudio | ALSA `mt3351-audio` (`/dev/snd/pcmC0D0p`), 22 050 Hz estéreo testado | sem alsa-lib no rootfs estático |

## Ponto flutuante (o fator decisivo)

O ARM1176**JZ-S** não tem unidade de ponto flutuante: toda conta em `float`/`double` é feita
por rotinas de software da libgcc. Medição com `fpbench` (2 milhões de iterações, `-O2`,
estático, musl):

| Operação | ns por operação |
|---|---|
| `double` multiplicação + soma | 318,5 |
| `float` multiplicação + soma | 151,8 |
| inteiro 32 bits (ponto fixo: mul 64, deslocamento, soma) | 34,0 |
| `sqrt` (double) | 970,0 |
| `sin` (double) | 7 854,1 |

Consequência para o projeto: qualquer laço por vértice (projeção, recorte, desenho de linhas,
custo de aresta no roteamento, casamento com a via) precisa ser **inteiro / ponto fixo**. Um
motor escrito em `double` paga ~10× no mínimo, e trigonometria por ponto (projeção Mercator
com `sin`/`log`) é proibitiva: 10 000 vértices na tela × 8 µs = 80 ms só de `sin`.

## O que já existe e será reaproveitado

| Item | Onde | Uso no Big GPS |
|---|---|---|
| Serviço GPS (`bfgps`, `S60gps`) | [src/gps/](../../src/gps/), [docs/gps.md](../gps.md) | posição real; não será substituído |
| Estado do GPS | `/run/bigfoston-gps` (arquivo de chaves) | leitura simples a 1 Hz |
| NMEA cru | `/run/gps-nmea` (PTY) | velocidade, rumo, satélites, hora |
| Interface | LVGL 9 em C ([src/ui/](../../src/ui/)), tela "GPS" atual (`scr_gps.c`) | o Big GPS será uma tela/aplicativo deste shell |
| Áudio | ALSA do kernel, já usado por `fx.c` (sons da interface) e pelo player | voz das instruções |
| SD | `/mnt/sd`, montado com gravação após validação do cartão | dados em `/mnt/sd/BigFoston/big-gps/` |

## Limites para o desenho

- **RAM:** ~100 MB livres, sem swap. Meta: o aplicativo de mapa+rota em < 40 MB RSS, para
  sobrar folga para música e o restante do sistema.
- **Disco:** sobra espaço (29 GB); o gargalo é a latência aleatória de 2,7 ms — 100 leituras
  aleatórias = 0,27 s. Formatos precisam agrupar dados por área (tiles) e ler blocos grandes.
- **CPU:** ~470 MHz sem FPU, já com ~12 % gastos pelo GPS. Pré-processar tudo o que puder no
  PC (projeção, simplificação por zoom, grafo de rotas com hierarquia).
