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

Na prática, a música **some** enquanto a voz fala (~1–3 s): o celular continua mandando, e o
`bluealsa-aplay` descarta esse trecho (patch `firmware/buildroot/patches/bluez-alsa/0001-aplay-bigfoston-ring-and-hold.patch`).
Depois a música volta. Ela não é abaixada.

## O que falta para abaixar em vez de parar

O MT3351 tem uma saída de som só, e o OSS do `/dev/dsp` é exclusivo. Para tocar música baixa e
voz ao mesmo tempo é preciso **mixar**:

1. Usar o `dmix` do ALSA na saída (`/etc/asound.conf`): o `bluealsa-aplay --pcm=dmix` e a voz
   abrindo o mesmo `dmix`. A voz passaria do OSS para a libasound.
2. Durante a fala, baixar o volume da música no `bluealsa` (propriedade `Volume` do PCM pelo
   D-Bus, `org.bluealsa.PCM1`) e devolvê-lo depois.
3. Medir no aparelho o custo de CPU do `dmix` (reamostragem e mistura em ponto fixo) com o mapa
   navegando. O ARM1176 não tem FPU nem NEON.

Isso mexe na cadeia de áudio do sistema inteiro: Multimídia, Telefone (HFP) e TV também usam a
saída. Por isso fica para uma etapa própria, testada com um celular tocando música de verdade.
Nada disso foi feito ainda.

## A testar no aparelho (ainda não feito)

- Navegação com o celular tocando música: a voz se ouve? A música volta sozinha? A ligação
  A2DP se mantém?
- Falas seguidas (manobra e radar) com a música: sem cortes nem travamento.
- CPU durante a fala com a música e o mapa.
