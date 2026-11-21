# Cartão SD (MSDC1) no Linux

Estado: **leitura e escrita funcionando** no aparelho (driver `mt3351-msdc`,
2026-11-20). A escrita é liberada **por cartão**: o kernel sobe com `readonly=1`, e o
`/etc/init.d/S30sdcard` só libera se o cartão traz `BigFoston/linux-rw` com o número de
série dele (`serial=0x...`), arquivo gravado depois que o cartão passa no teste
`tools/bigfoston/sd-write-test.sh`. O cartão antigo (gravação falhando, veja
[01-hardware.md](01-hardware.md)) nunca é gravado.

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

Parâmetros: `readonly` (padrão 1; gravável em tempo de execução, vale quando o cartão
é reiniciado: o S30sdcard desliga e religa o host por `unbind`/`bind`) e `max_hz`
(padrão 24 MHz, o mesmo do CE).

## Montagem e persistência (S30sdcard)

1. Monta `/dev/mmcblk0p1` em `/mnt/sd` só leitura (`iocharset=iso8859-1,utf8`: nomes em
   UTF-8 e sem distinção de maiúsculas nos nomes ASCII, como no CE).
2. Se `BigFoston/linux-rw` tem `serial=<este cartão>`: `readonly=0`, `unbind`/`bind`,
   monta com gravação (`flush,errors=remount-ro`) e liga `BigFoston/data` em
   `/data/bigfoston` (`mount --bind`): ajustes, calibração do toque e livros do shell
   ficam no cartão. O shell grava os ajustes com `fsync` antes de trocar o nome.
3. No desligamento: `sync` e desmontagem.

## Testes no aparelho

- **Detecção:** `mmc0: new SD card at address b368` e `mmcblk0: 1.91 GiB (ro)`, com a partição `p1` lida.
- **CID** `000000534420202000000061ed00b903`: nome "SD", série `0x61ed`, fabricado em 09/2011. Os campos saem coerentes, o que confirma a ordem dos bytes da resposta R2.
- **FAT32** montado com `-o ro`: `BigFoston/` listado, e o SHA-256 do `bfagent.exe` bate com o `bfagent.exe.sha256` do cartão.
- **Leitura:** 8 MB em 1,34 s (cerca de 6 MB/s). Duas leituras de 64 MB deram o mesmo MD5.
- **SDHC (cartão novo SanDisk SS32G, 32 GB, CID `035344535333324780253e4a4a00e46b`):**
  o setor 0 lido pelo FOSTON e por um leitor no PC tem o mesmo MD5 (`52dd4a54…`);
  formatado no PC (MBR, FAT32 a partir de 4 MiB).
- **Escrita** (`sd-write-test.sh`, duas rodadas): arquivos aleatórios de 1 KB a 64 MB e
  200 arquivos pequenos conferidos por MD5 após limpar o cache e após remontar, sem
  erros de `mmc0` no log. Gravação ≈ 1,4 MB/s (inclui gerar os dados com
  `/dev/urandom`), leitura de 64 MB com o cache limpo em 7,97 s (≈ 8 MB/s).

## Gravação sustentada: defeito encontrado e corrigido (21/11/2026)

Um teste pesado (3 arquivos de 96 MB a ~3 MB/s, conferidos por MD5) **corrompeu 2 dos
3 arquivos** com o driver anterior. O log novo mostrou 14 falhas `CMD25 ... data -110`:
numa gravação longa o cartão para de aceitar dados entre blocos enquanto programa a
flash, e o driver desistia depois de 500 ms; pior, contava como transferido o que a
FIFO já tinha aceitado, e o núcleo MMC regravava só "o que faltava". Antes disso, num
teste de 80 MB, uma falha dessas levou o núcleo a dar o cartão como **removido**.

Correções em `mt3351-msdc.c`:

- espera de até 2 s por trecho (medido: até **670 ms**), parâmetro `max_stall_ms`;
- em qualquer erro, `bytes_xfered = 0`: o núcleo regrava a requisição inteira;
- CMD12/CMD13 só esperam a linha de comando (o cartão responde a eles ocupado); os
  demais esperam o controlador até 1 s;
- PIO sem `SG_MITER_ATOMIC` e com `cond_resched()` enquanto o cartão está ocupado
  (antes a espera congelava o sistema de uma CPU);
- toda falha registrada no log com `SDC_STA`, `CMDSTA`, `DATSTA` e `PS`.

Depois: 2 rodadas do mesmo teste, **6 de 6 arquivos conferidos**, zero erros; e as 22
verificações do motor de arquivos do shell direto no cartão.

**Reparo automático:** o `S30sdcard` lê o bit "dirty" do FAT32 (setor de boot `0x41`,
bit 0) e, se o cartão não foi desmontado (energia cortada no carro), roda
`fsck.fat -a` antes de liberar a gravação; o relatório fica em
`BigFoston/fsck-ultimo.log`. Testado: corrigiu o cartão em 6 s após a falha acima.

## Pendente

- **Interrupção de detecção** (IRQ 15). Não se sabe como reconhecê-la, então o driver consulta o slot periodicamente.
- **DMA.** O canal periférico com master = 1 está documentado no `dmadrv.dll`.
- **Montagem automática** e **ejeção segura** pela interface.
- **Queda de energia** no meio de uma gravação: o FAT pode ficar inconsistente
  (`errors=remount-ro` protege o resto da sessão); conferir com `fsck.vfat` no PC.
