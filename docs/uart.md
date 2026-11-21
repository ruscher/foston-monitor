# UARTs no Linux

Estado: **funcionando, testado só recebendo** (2026-11-20). As quatro portas aparecem como
16550A em `/dev/ttyS0..3` (driver `8250_mt3351`, sobre o 8250 padrão).

| Linux | CE | Base | IRQ | Uso |
|---|---|---|---|---|
| ttyS0 | UART1 / COM2 | 0x80026000 | 3 | GPS MT3328 (host-based) |
| ttyS1 | UART2 / COM3 | 0x80027000 | 4 | Bluetooth MediaTek (`mtk_bthuart.dll`), desligado por padrão |
| ttyS2 | UART3 / COM4 | 0x80028000 | 5 | sem uso encontrado |
| ttyS3 | UART4 / COM5 | 0x80029000 | 6 | sem uso encontrado |
| — | UART0 | 0x80025000 | ? | debug do `nk.exe`, não usada |

Os dados vêm do `uart.dll` do WinCE, analisado offline:

- **Registradores:** 16550 com espaçamento de 4 bytes e acesso por byte. Extras da MediaTek: HIGHSPEED 0x24, SAMPLE_COUNT 0x28, SAMPLE_POINT 0x2C, RATE_FIX 0x34, GUARD 0x3C, DMA_EN 0x4C.
- **Clock:** 29,25 MHz (468 MHz/16, a mesma fonte do GPT). Com HIGHSPEED = 0 o divisor é o 16x padrão e 38400/115200/921600 erram −0,8 %.
- **Clock-gates:** os bits 5..9 do PDN em `0x80001300` já estão ligados quando o Linux assume.

## Ajustes depois do CE (driver `8250_mt3351`)

O WinCE deixa a UART1 e a UART2 com HIGHSPEED e DMA de recepção ligados. Os canais GDMA
0x10/0x11 (`0x80024000`/`0x80024100`) continuam rodando e gravam em `0x007E8000` e
`0x007DB000`. **Era essa a escrita misteriosa em 0x7E8000** que corrompia o BusyBox no início
do port; a faixa `0x700000–0x7FFFFF` continua reservada no DT.

Ao registrar cada porta, o driver:

1. zera HIGHSPEED e DMA_EN;
2. para o canal GDMA indicado em `mediatek,ce-rx-dma` (registrador START = 0).

Ele não mexe em clock nem em pinmux.

## Teste de recepção (ttyS0, GPS)

| Baud | Bytes em 4 s |
|---|---|
| 38400 | 50 |
| 115200 | 68 |
| 921600 | 395 |

São dados binários do protocolo *host-based* do MT3328: há sequências que se repetem, e não é
NMEA. O receptor fica ligado pelo CE. A 921600 houve 126 estouros de FIFO (FIFO de 16 bytes,
interrupção por byte): para usar o GPS de verdade é preciso o DMA da UART ou uma latência
menor. A posição em si depende do motor MNL proprietário (ver
[04-linux-port.md](04-linux-port.md), linha GPS).
