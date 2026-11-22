# TV do FOSTON: visão geral e estado

Recuperar a **TV digital (ISDB-T 1seg)** e a **entrada AV-IN** reais do FS-707DC no BigFoston OS.

## Estado (22/11/2026)

| Parte | Estado | Evidência |
|---|---|---|
| Sintonizador Siano (SPI) ligado pelo Linux | **FUNCIONANDO** | `bftv boot`: firmware carregado em 2,1 s; versão: chip 0x2130, ROM 7.1.1, "ISDBT 02.02.17 PELE" |
| Sintonia ISDB-T 1seg | **FUNCIONANDO** | canal 15 UHF (479,143 MHz): demodulador travado, TS chegando (mensagens 607 com múltiplos de 188 bytes) |
| Fluxo de transporte (TS) | **FUNCIONANDO** | 15 s = 682 KB (~364 kbit/s); `ffprobe`: TV Brasil 1seg, H.264 Constrained Baseline 320×180 15 qps, HE-AAC LATM 48 kHz estéreo (2 faixas); um quadro decodificado no PC |
| Decodificação e exibição no FOSTON | a fazer | ffmpeg (h264 + aac_latm + mpegts) no ARMv6, sem NEON |
| Busca de canais, EPG, interface | a fazer | |
| AV-IN (TVP5150) | mapeado (análise estática), a testar | [07-av-in.md](07-av-in.md) |
| TV analógica por antena | não existe | o Siano é só digital; a analógica terrestre acabou no Brasil |

Os arquivos proprietários (firmware `.inp` da Siano) ficam só no cartão do usuário:
`tools/bigfoston/voice-extract.py OUT --tvfw ISDBT.inp`, copiado para `BigFoston/tv/`.

## Documentos

- [02-hardware-sintonizador.md](02-hardware-sintonizador.md): SPI, pinos, energia e protocolo.
- [07-av-in.md](07-av-in.md): entrada AV (decodificador TVP5150, captura, escalador).

## Player (bftvd) — primeira medição no aparelho (22/11/2026)

| Parte | Escolha | CPU (15 s de TV) |
|---|---|---|
| Vídeo H.264 320×180 | ffmpeg 6.1.5 mínimo (ARMv6, sem ARMv6T2/VFP/NEON), sem filtro de desblocagem | 7,4 s (49 %); com filtro: 10,3 s (69 %) |
| Áudio HE-AAC LOAS/LATM | fdk-aac 2.0.3 (ponto fixo) | 2,1 s (14 %); decodificador do ffmpeg (ponto flutuante): 8,8 s (59 %) |
| Imagem | YUV420 → RGB565 ampliado 2× (640×360) com tabelas, direto no framebuffer | — (`swscale` para 800×450: 59 s, inviável) |
| Som | 48 → 32 kHz mono, /dev/dsp; o vídeo segue o relógio do áudio | — |

`bftvd 479.142857` no FOSTON: 13,9 qps mostrados, 2 quadros atrasados, sem perda de TS, ~70 % de CPU.

O ffmpeg e a fdk-aac são compilados à parte (firmware/build/ffmpeg-tv, toolchain `out-cxx` com C++)
e o player fica no cartão: a memória interna não tem espaço para eles.
