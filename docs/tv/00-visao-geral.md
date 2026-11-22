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
