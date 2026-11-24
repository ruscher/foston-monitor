# Etapa 3 — TV sem travar

Queixa: "mesmo com o sinal bom ou excelente a imagem trava bastante, som e imagem ficam picotando
e falhando o tempo todo". A primeira medida do player (22/11) foi na TV Brasil: 15 qps em
Baseline, 13,9 qps, ~70 % de CPU. Gravando 12–16 s de cada canal hoje e analisando no PC com
`ffprobe`, o quadro é outro:

| Canal | Vídeo | Quadros |
|---|---|---|
| Globo 1SEG (515 MHz) | H.264 Constrained Baseline 320×180, **29,97 qps** | I e P |
| TV Brasil 1seg (479 MHz) | Constrained Baseline 320×180, **29,97 qps** | I e P (com legenda A53) |
| Record 1seg (527 MHz) | Constrained Baseline 320×180, **29,97 qps** | I e P |
| SBT 1Seg (533 MHz) | **H.264 Main com CABAC e quadros B** em pirâmide, 29,97 qps | 85 % B, 12 % P, 3 % I |

Hoje todos os canais mandam o dobro de quadros. A CPU fica a 100 % e, a partir daí, três falhas
do player faziam o resto.

## Falha 1: o TS se perdia uma vez por segundo (todo canal, qualquer sinal)

A thread do sintonizador pede o sinal (`stats()`) a cada segundo. Durante a espera pela
resposta, trocava o tratador de mensagens por `store`, que **jogava fora o TS** que chegava
nesse intervalo.

Na gravação do SBT (SNR 17–19): 52 descontinuidades em 17 s, em todos os PIDs, espaçadas de
~1 s. O resultado eram blocos quebrados e "reference picture missing" no decodificador.

**Correção:** `on_data` já passa a `store` o que não é TS, então `stats()` não troca mais o
tratador (`siano.c`).

| Gravação | Depois da correção |
|---|---|
| Globo (SNR 18) | **0** descontinuidades |
| TV Brasil (SNR 13), Record (SNR 15) | 14–16, em rajadas, sem o bit de erro de transporte: queda do sinal fraco, não do player |

## Falha 2: a fila de quadros era de 1 s a 30 qps

A fila entre o decodificador e o mostrador tinha 32 quadros: 2 s a 15 qps, mas só ~1 s a 30
qps. Nos canais que mandam o vídeo à frente do áudio no fluxo, o efeito era em cadeia:
1. a fila enchia;
2. o decodificador parava sem entregar o áudio;
3. o relógio do som parava;
4. o mostrador seguia sozinho a 15 qps;
5. o decodificador consumia o TS à metade da velocidade;
6. o anel de 1 MB transbordava (`drops`).

**Correção:** 64 quadros (5,5 MB). Na Globo: de 7–8 qps para **20–26 qps, 0 drops**.

## Falha 3: na primeira abertura depois de ligar, o SPI ficava lento

O download do firmware do sintonizador é feito com `clk_div = 15` e o valor **não voltava**
para 7. A primeira TV depois de ligar o aparelho (o caso normal) lia o TS com o dobro de espera
ativa no SPI. **Correção:** `clk_div` volta ao valor de tocar depois do download.

## A escada de decodificação

O ARM11 não decodifica 30 qps de H.264 Main com quadros B junto com o som, o sintonizador e o
desenho. No SBT, medido no PC com 15 s do canal:

| Degrau | Decodifica | Quadros | Custo |
|---|---|---|---|
| 0 | tudo | 456 | 100 % |
| 1 | sem os que não são referência (`AVDISCARD_NONREF`) | 401 | 76 % |
| 2 | só I e P (`AVDISCARD_BIDIR`) | 69 | 38 % |

Como a escada funciona:
- **Sobe um degrau** quando o TS se acumula no anel (> 48 KB, ~1 s atrás do ar, e sem
  diminuir), o som está a ponto de acabar (< 30 ms no buffer) ou um quadro chegou atrasado no
  último segundo.
- **Desce** depois de 8 s sem aperto. Se precisou subir de novo logo depois de descer, espera o
  dobro (até 64 s).
- **Só troca num quadro-chave.** Pular um B de referência no meio do grupo deixaria os
  seguintes sem referência.
- **No degrau 2**, o decodificador segura os quadros para reordenar (até ~0,9 s a ~5 qps). O
  mostrador aceita até 1 s de atraso em vez de descartar.
- **Nos degraus 1 e 2**, a tela mostra até ~15 qps: cada quadro ampliado para 640×360 custa CPU,
  e 15 qps é o padrão do 1seg.

Nos canais Baseline (sem B), a escada não tira quadros da decodificação; só limita a tela a 15
qps quando aperta.

## Som

- **Mono decodificado:** a fdk-aac faz a mistura (`AAC_PCM_MAX_OUTPUT_CHANNELS = 1`), o que pula
  o estéreo paramétrico do HE-AAC v2.
- **SBR:** QMF real, de baixo consumo (`AAC_QMF_LOWPOWER`).
- **Limitador:** desligado.
- **Buffer:** a primeira tentativa desta rodada foi um buffer de ~750 ms (12×4 KB). No mesmo
  canal ele mostrou 10 qps contra 15,6 do de 190 ms: o decodificador corria à frente do som e a
  fila enchia. Voltou ao de 190 ms (6×2 KB). `BFTV_FRAG` permite testar outros.

## Testados e descartados

| Tentativa | Resultado |
|---|---|
| Consultar o sintonizador a cada 8 ou 15 ms (em vez de 3) | perde TS: a FIFO do chip é pequena |
| SPI mais rápido (`clk_div` 5) | −10 % na thread; com 3 perde dados. Fica 7, o do WinCE |
| `skip_idct` nos quadros B | o decodificador H.264 do ffmpeg ignora |
| `has_b_frames = 0` no degrau 2 | o SPS repõe 4 |
| Escrever dois pontos por vez no framebuffer (32 bits) | ficou (correto), mas a thread do mostrador não mudou: o limite é a banda da memória dividida com a varredura do LCD |

## Resultado no aparelho (pelo app TV, com o modo performance)

| Canal | Quadros na tela | TS perdido | drops | CPU |
|---|---|---|---|---|
| SBT (Main, B) | 3–5 qps, degrau 2, imagem limpa | 0–4 | 0 | 58–84 % |
| Globo (Baseline) | 12–26 qps | 0 | 0 | 100 % |
| TV Brasil (Baseline, SNR 13) | 13–28 qps | rajadas do sinal | 0–12 | 100 % |

No SBT: 1 falha de som em 30 s.

O SBT a 30 qps com quadros B está além do ARM11. A escolha foi imagem limpa e som contínuo a
~5 qps, em vez de 10–15 qps com blocos quebrados e som picotando.

## Novo no estado (`/run/bigfoston-tv`)

| Campo | O que conta |
|---|---|
| `lost` | pacotes TS perdidos pelo contador de continuidade (sinal ou sintonizador) |
| `drops` | o anel do TS transbordou (CPU) |
| `shown` | quadros na tela |
| `thin` | quadros não mostrados pelo limite de 15 qps |
| `late` | quadros atrasados descartados |
| `skip` | degrau da escada |
| `underruns` | o som secou |

O `fps` antigo é uma janela de 2 s e engana: para medir, use a diferença de `shown` no tempo.
