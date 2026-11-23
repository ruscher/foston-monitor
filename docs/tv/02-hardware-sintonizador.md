# Sintonizador Siano "Nova B0" (ISDB-T) no SPI do MT3351

Mapa obtido por análise estática dos drivers do Windows CE (`SianoSPI.dll`, `SmsGenDrv.dll`,
`buses.dll`, `nk.exe`, `SmsCtrlLibCE.dll`) e confirmado no aparelho com o `bftv` (src/tv/bftv.c).

## Hardware

| Item | Valor |
|---|---|
| Controlador SPI | `0x8004E000` (bloco "mt65xx" da MediaTek: CFG0 +0x00, CFG1 +0x04, TX/RX DMA +0x08/+0x0C, FIFO TX/RX +0x10/+0x14, CMD +0x18, STATUS0 +0x1C, STATUS1 +0x20); IRQ 42 |
| Clock | id 36 = PDN1 bit 12 (`0x80001344` liga, `0x80001324` desliga) |
| Pinos | `0x80001700` bits[3:2] = 2 (SPI); força 13 nos GPIO 14–17 |
| Alimentação | GPIO0 (1 = ligado) |
| Reset | GPIO1 (ativo em 0): 1 → 0 (60 ms) → 1 (100 ms) |
| Interrupção do chip | GPIO2 = EINT2 (IRQ 66); o Linux não trata EINT: o `bftv` lê por consulta |
| Cristal do chip | 12 MHz |

SPI: modo 0, MSB primeiro, CS ativo a transferência inteira; relógio ClkLow/High = 15 no download do
firmware e 7 depois. O CE usa DMA; o `bftv` usa a FIFO em pedaços de 32 bytes com o modo pausa
(CMD bit4) mantendo o CS.

## Protocolo

- Cada mensagem: `A5 5A E7 7E` + cabeçalho `{u16 tipo, u8 origem, u8 destino, u16 tamanho, u16 flags}`
  + dados, completada com zeros até múltiplo de 256 bytes; toda transferência é full duplex e a
  parte recebida passa pelo analisador (procura o preâmbulo).
- Partida: 8 bytes `00 00 DE C1 A5 51 F1 ED`; msg 710 `{0, 19, 20}` (linha de interrupção do chip);
  firmware em blocos de 240 bytes (msg 660, resposta 661); msg 664 `{0x40260, 3, 0x200, 0, 1}`
  (resposta 665); 150 ms; relógio do SPI mais rápido.
- Modo ISDB-T: msg 578 `{5}` (resposta 579). Sintonia: msg 776 `{Hz, 4 = 1seg, 12000000, 1}`
  (resposta 777). Estatísticas: msg 653 (resposta 654). Dados: msg 607 (TS, múltiplos de 188 bytes).

## Medido (22/11/2026)

- `bftv boot`: firmware (51 580 bytes) em 2,1 s; versão "ISDBT 02.02.17 PELE", chip 0x2130, ROM 7.1.1.
- `bftv dump 479.142857 15`: 195 mensagens 607, 682 440 bytes; TV Brasil 1seg (H.264 320×180 15 qps,
  HE-AAC 48 kHz).
