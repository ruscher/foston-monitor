# Som

`src/remoteplay/audio.c`. O Sunshine manda o som em Opus (48 kHz, estéreo) pelo mesmo canal da
sessão. O FOSTON:

| Etapa | Como |
|---|---|
| Decodificar | libopus 1.6.1 em **ponto fixo** (o ARM11 não tem FPU), estática, só com a otimização em assembly ARMv6 (EDSP/Media); sem intrínsecos NEON, que o ARM11 não tem |
| Mais barato | com um fluxo estéreo (o caso do Sunshine), o próprio Opus mistura em **mono** e decodifica a **24 kHz** (banda até 12 kHz) |
| Tocar | `/dev/dsp` mono 32 kHz, o mesmo formato da TV; 24 → 32 kHz por interpolação linear (3 → 4) |
| Buffer | ~190 ms (6 × 2 KB) |
| Pacote perdido | o PLC do Opus |
| Pacotes maiores | `CAPABILITY_SLOW_OPUS_DECODER` |
| Alto-falante ocupado (música do celular, TV) | a sessão segue sem som (`AUDIO ocupado`) |

O motor manda `ASTATS pacotes perdidos secou alto-falante` a cada 2 s.

## Medido no aparelho (24/11/2026)

| Medida | Valor |
|---|---|
| CPU da decodificação, estéreo a 48 kHz | 19,3 % |
| CPU da decodificação, mono a 24 kHz | **15,2 %** |
| `/dev/dsp` durante a sessão | aberto pelo `bfrp`, placa em `RUNNING` |

A biblioteca é compilada à parte (`firmware/build/opus-arm`), conferida pelo SHA-256 do pacote
`opus` do Buildroot:

```
./configure --host=arm-buildroot-linux-musleabi --enable-static --disable-shared \
    --enable-fixed-point --disable-intrinsics --disable-doc --disable-extra-programs
```

Pendente: microfone (a captura de voz ainda não funciona no BigFoston).
