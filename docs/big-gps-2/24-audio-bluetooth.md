# Voz da navegação junto com a música do celular (Bluetooth)

## Como está

- A música do celular chega por A2DP. O `bluealsa-aplay --pcm=hw:0,0` toca e **segura** a saída
  de som.
- Quando o Big GPS fala (`core/voice.c`):
  - `btaudio_hold(true)` manda `SIGUSR1` ao `bluealsa-aplay` (patch do BigFoston), que solta a
    saída;
  - a voz Faber toca pelo `/dev/dsp`;
  - `SIGUSR2` devolve a saída à música.
- O Multimídia e o app Internet fazem o mesmo (`player_suspend`, `webaudio_hold`).
- O Bluetooth **não é desligado nem reiniciado**: a ligação A2DP continua de pé, e só o
  `bluealsa-aplay` para de escrever durante a fala. Com a voz pré-gerada, a fala começa na hora
  e dura só o necessário (sem síntese no meio).

## Agora: a música abaixa durante a fala

Sem `dmix` e sem mexer no resto da cadeia de som. O `bluealsa-aplay` continua dono da saída e
mistura a voz ele mesmo (patch
`firmware/buildroot/patches/bluez-alsa/0002-aplay-bigfoston-voice-mix.patch`):

1. O shell escreve a voz (mono, 22.050 Hz) numa fila em memória compartilhada,
   `/run/bluealsa-voice` (32.768 amostras, `btaudio_say` em `core/btaudio.c`).
2. A cada bloco de música, o aplay leva a voz à taxa da música (44,1 ou 48 kHz, interpolação
   linear em inteiros), abaixa a música para 1/4 (-12 dB) e soma.
3. A música desce numa rampa de ~50 ms **antes** de a voz entrar (sem estouro) e volta ~330 ms
   depois do fim da fala, também em rampa (sem estalo).

O shell só usa esse caminho quando o aplay está tocando de fato: ele conta cada bloco que
mistura (`beat`). Se esse contador não andar em 300 ms, ou se a música parar no meio da frase
(600 ms sem consumir), o resto da fala vai pelo caminho antigo: `/dev/dsp`, com a música solta
durante a fala. O Multimídia e o app Internet continuam no caminho antigo.

## Medido no FOSTON (2026-11-26, música do celular por A2DP)

| | |
|---|---|
| Frase de 3,44 s | tocada inteira em 3,51–3,52 s (3 vezes); +80 ms = a rampa antes da voz |
| CPU do `bluealsa-aplay` | 6,0 % sem voz → 7,4 % durante a fala |

Medido com `bfsay`, um programa de teste que chama o mesmo `btaudio_say` do shell, e com o
aplay novo rodando de `/tmp` (o sistema instalado não foi trocado nesse teste).

## Testes no PC

`make test` em `src/ui` (`tests/test_btvoice.c`). A função de mistura é tirada do próprio
patch, então o teste confere o código que vai para o aparelho. Ele verifica:
- a voz inteira a 44,1 e 48 kHz;
- a música baixa durante a fala e de volta depois;
- nenhum salto fora da entrada e da saída da voz;
- a volta ao `/dev/dsp` sem música;
- a música parando no meio da frase;
- a fala interrompida.

## A testar dirigindo

- Ouvir se -12 dB é o ponto certo (constante `BF_DUCK` no patch).
- Falas seguidas (manobra e radar) com a música.
