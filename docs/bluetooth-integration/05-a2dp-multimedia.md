# Música do celular na Multimídia (A2DP)

Não há um segundo player: a **Multimídia** ganhou a fonte **Bluetooth**.

- O `bluealsa-aplay` decodifica o SBC e toca em `hw:0,0` (44,1 kHz, a taxa do celular e do driver:
  sem conversão). O patch `firmware/buildroot/patches/bluez-alsa/0001-...` faz ele copiar as
  amostras (mono) para `/run/bluealsa-ring`, que o espectro da Multimídia lê (`core/btaudio.c`): sem
  segunda decodificação.
- Quando o áudio do celular começa (o anel anda), a fonte troca sozinha; a música do cartão pausa e
  solta o `/dev/dsp`. Tocar uma faixa do cartão pede pausa ao celular.
- A voz do Big GPS manda SIGUSR1 ao `bluealsa-aplay` (solta o PCM e descarta o áudio enquanto fala)
  e SIGUSR2 no fim: a música fica muda durante a frase, sem pausar o celular.
- O Painel da tela inicial mostra e controla a música do celular.

Medido com música (`bf-audio-profiler`, POCO X4, 22/11/2026): 41,6 KB/s pela UART, 0 buracos, 0
XRUN, PCM sem reaberturas.
