# Cartão SD (MSDC1) no Linux

Estado: **leitura funcionando** no aparelho (driver `mt3351-msdc`, 2026-11-20).
**Escrita bloqueada de propósito** (parâmetro `readonly`, padrão 1). Dois motivos:
o cartão atual falha em gravações (veja [01-hardware.md](01-hardware.md)) e guarda o lado
WinCE do BigFoston (`\SDMMC\BigFoston`).

## Origem

Mapa de registradores tirado do `sdhc_drv.dll` do WinCE do aparelho (análise estática
da cópia da NAND, sem tocar no hardware) e conferido com leituras `devmem` no Linux.
É a geração de MSDC do MT6516, não a do `mtk-sd.c` do mainline (offsets diferentes).

## Recursos

| Item | Valor | Origem |
|---|---|---|
| Base | `0x8002E000 + 0x1000 × MsdcIndex`; o FS-707DC usa **MSDC1 = 0x8002F000** | fórmula no driver CE + registro `MsdcIndex=1` |
| IRQ | 15 (14 + índice), usada pelo CE só para detectar o cartão | driver CE |
| Clock | UPLL 48 MHz; f = 48 MHz / (SCLKF ? 4·SCLKF : 2) | driver CE |
| Energia | VSDIO (`0x80061374` [10:9]=3, bit0=1), clock-gate MSDC1 (bit15 de `0x80001300`; liga em `0x80001340`), GPIO35 em nível baixo | `bus.dll`, `buses.dll` |

O WinCE entrega o slot já ligado. O Linux **não mexe** em PMU, clock-gate nem GPIO.
Registradores medidos no salto para o Linux:

| Registrador | Valor | Leitura |
|---|---|---|
| `MSDC_CFG` | `0x45600061` | SCLKF = 0 (24 MHz), sem DMA |
| `MSDC_PS` | `0x01078007` | bit3 = 0, cartão presente |
| `SDC_CFG` | `0x28038200` | 4 bits, BLKLEN 512 |
| `PDN 0x80001300` | `0xFF6D5808` | bit15 = 0, clock do MSDC1 ligado |
| `PMU_CON1D` | `0x00000601` | VSDIO ligado |

## Registradores usados pelo driver

| Offset | Nome | Uso |
|---|---|---|
| 0x00 | MSDC_CFG | SCLKF[15:8], DMA bit17 (desligado), [31:28]=1 para PIO |
| 0x04 | MSDC_STA | bit2 = FIFO tem dado/espaço; bit14 limpa a FIFO antes de transferir |
| 0x0C | MSDC_DAT | porta da FIFO, 32 bits |
| 0x10 | MSDC_PS | bit3 = 0 quando há cartão |
| 0x20 | SDC_CFG | BLKLEN[11:0], bit16 liga a interface serial, bit17 = barramento de 4 bits |
| 0x24 / 0x28 | SDC_CMD / SDC_ARG | escrever o ARG antes do CMD |
| 0x2C | SDC_STA | bit0 controlador ocupado, bit1 comando ocupado, bit4 cartão ocupado (DAT0) |
| 0x30–0x3C | RESP0–3 | R2: RESP3 é a palavra mais significativa (confirmado com o CID) |
| 0x40 | SDC_CMDSTA | bit0 ok, bit1 timeout, bit2 erro de CRC |
| 0x44 | SDC_DATSTA | bit0 bloco ok, bit1 timeout, bit2 erro de CRC |

Palavra de comando: `índice[5:0] | resposta[9:7] | dados[12:11] | escrita bit13 | stop bit14`.
Tipos de resposta: 0 nenhuma, 1 R1, 2 R2, 3 R3, 6 R6, 7 R1b. Dados: 1 = um bloco, 2 = vários.
**Não existe contador de blocos**: uma transferência de vários blocos termina com CMD12.
Por isso o driver não anuncia CMD23.

## Driver Linux

`firmware/kernel/overlay/drivers/mmc/host/mt3351-msdc.c`. Funciona por polling, como o
driver do CE: comando, dados e detecção do cartão (`MMC_CAP_NEEDS_POLL`). A transferência
é PIO, feita em uma workqueue. Suporta SD de 4 bits; SDIO e MMC estão desligados.

Parâmetros: `readonly` (padrão 1) e `max_hz` (padrão 24 MHz, o mesmo do CE).

## Testes no aparelho

- **Detecção:** `mmc0: new SD card at address b368` e `mmcblk0: 1.91 GiB (ro)`, com a partição `p1` lida.
- **CID** `000000534420202000000061ed00b903`: nome "SD", série `0x61ed`, fabricado em 09/2011. Os campos saem coerentes, o que confirma a ordem dos bytes da resposta R2.
- **FAT32** montado com `-o ro`: `BigFoston/` listado, e o SHA-256 do `bfagent.exe` bate com o `bfagent.exe.sha256` do cartão.
- **Leitura:** 8 MB em 1,34 s (cerca de 6 MB/s). Duas leituras de 64 MB deram o mesmo MD5.

## Pendente

- **Escrita.** Testar só com um cartão novo, e com `readonly=0` explícito.
- **Interrupção de detecção** (IRQ 15). Não se sabe como reconhecê-la, então o driver consulta o slot periodicamente.
- **DMA.** O canal periférico com master = 1 está documentado no `dmadrv.dll`.
- **Montagem automática** e **ejeção segura** pela interface.
- **Persistência:** a calibração e os ajustes do shell no SD, quando a escrita estiver liberada.
