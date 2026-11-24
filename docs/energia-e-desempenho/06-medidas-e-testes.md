# Etapa 6 — Medidas e testes

## Testes automáticos

- `make test` em `src/ui`: kbdmap 33, padmap 32, pkg 22, todos ok.
- `make VARIANT=headless` sem avisos (`-Wall -Wextra -Werror`).
- O roteiro de capturas (`--screenshot`, build com ASan) roda as 146 telas sem erro de
  memória. Capturas novas:
  - `17b-potencia-plano`;
  - `17c-desempenho` e `17d-desempenho-fim`;
  - `17e-modo-performance` e `17f-modo-performance-fim`.

## Comandos no aparelho

FIFO de controle `/run/bigfoston/ctl`:

| Comando | Faz |
|---|---|
| `app power`, `app perf`, `app perfmode`, `app tv` | abre a tela |
| `plan 0`, `plan 1`, `plan 2` | aplica Economia, Equilibrado, Desempenho |
| `fx 0`, `fx 1`, `fx 2` | efeitos completos, reduzidos, mínimos |
| `perf on`, `perf off` | modo performance manual |

## CPU por processo

Diferença de `utime + stime` em `/proc/*/task/*/stat`, com HZ = 100. Assim se vê a CPU de cada
thread, inclusive as três do `bftvd`:
- principal: demux, H.264 e áudio;
- sintonizador: SPI;
- mostrador: YUV para RGB565 ampliado.

| Situação | Medida |
|---|---|
| Parado, Equilibrado | `bfgps` 9,9 %, shell 3,3 %, resto < 1 % |
| TV na Globo | principal ~62 %, sintonizador ~18 %, mostrador ~18 %, shell ~5 % |

## TV

O roteiro mede em janelas de 5 s a diferença de `shown`, `late`, `lost`, `drops` e `underruns`
no estado da TV, com o degrau da escada.

Para um canal sem o app, direto pelo binário (estado em `/tmp/tvst`):

```
bftvd -f BigFoston/tv/isdbt_nova_12mhz_b0.inp -r 0,48,640,360 -s /tmp/tvst 515.143
```

Variáveis para experimentar:

| Variável | Para quê |
|---|---|
| `BFTV_NOSKIP=1` | sem a escada |
| `BFTV_FRAG=0xNNNNMMMM` | fragmentos do /dev/dsp |
| `BFTV_IDLE_MS` | espera do sintonizador sem dados (3; mais perde TS) |
| `BFTV_CLKDIV` | divisor do SPI (7; 3 perde dados) |

Para analisar a imagem, grave com `-w ARQ.ts` e use no PC:
- `ffprobe -show_frames` para o tipo dos quadros;
- `ffmpeg -benchmark -skip_frame bidir|noref` para o custo relativo.

A versão anterior do player fica no cartão como `BigFoston/tv/bftvd.anterior`.
