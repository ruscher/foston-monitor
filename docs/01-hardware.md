# 01 · Hardware do FOSTON FS-707DC

Tudo nesta página foi **lido do próprio aparelho** em 19/11/2026, com o agente de
diagnóstico `bfagent` (só leitura, ver [03-engenharia-reversa.md](03-engenharia-reversa.md)),
salvo quando marcado como *anúncio* ou *inferência*.

Unidade analisada: ID `Nav-61`, plataforma `APOLLO`, OEM `MTK`.

## Resumo

| Componente | O que é | Evidência |
|---|---|---|
| SoC | **MediaTek MT3351** | `IOCTL_PROCESSOR_INFORMATION` da OAL: "MT3351", "MTK" |
| CPU | **ARM1176JZ(F)-S r0p4** (ARMv6), 468 MHz *(anúncio)* | CP15 `main_id=0x410fb764`; OAL: "ARM1176JZ-S" |
| Caches | I/D separados, `cache_type=0x1d152152` | CP15 c0 |
| TCM | ITCM 16 KB **ligada em PA `0x50004000`**; DTCM desligada | CP15 c0 `0x00020002`, c9 `itcm=0x50004011`, `dtcm=0x00000010` |
| VFP | não habilitado pelo CE (`CPACR=0`); userland soft-float | CP15 c1 |
| RAM | **128 MB** em PA `0x00000000–0x07FFFFFF` | tabela de páginas do CE (OEMAddressTable) |
| NAND interna | ~128 MB, setores de 2048 B, `DSK9:` 64 640 setores | `IOCTL_DISK_GETINFO` |
| Cartão SD | 2 GB, `DSK3:` (é o "pendrive" visto no PC); era FAT16 desalinhado (setor 245), reformatado em FAT32 a partir de 1 MiB em 19/11/2026; leitura completa (`mkfs.fat -c`) sem erros, mas **gravações falham** de forma intermitente tanto pelo FOSTON quanto num leitor USB do PC (`I/O error … WRITE`, resets do leitor): **cartão em fim de vida, trocar** | `Drivers\Active`; `mkfs.fat -c`; log do kernel do PC |
| Tela | 7", 800×480, RGB565 | GDI |
| Framebuffer | **PA `0x03E00030`**, stride 1600 B | comparação pixel a pixel tela × RAM (383 929/384 000 iguais) |
| Toque | resistivo, 1 ponto, driver `touch.dll` | registro `HARDWARE\DEVICEMAP\TOUCH` |
| TV digital | **Siano SMS1xxx "Nova B0" (ISDB-T) via SPI** | `SianoSPI.dll`, `SianoTunerCe5_r.dll`, firmware `isdbt_nova_12mhz_b0.inp` |
| Bluetooth | chip Bluetooth na UART2 (`mtk_bthuart.dll`, MediaTek; antes anotado como CSR BlueCore por causa do `bthcsr.dll`) (pilha ANW, A2DP, viva-voz) | `bthcsr.dll`, `mtk_bthuart.dll`, `hciext.dll`, `BtA2dpSnd` |
| GPS | GPS MediaTek do MT3351, biblioteca `MNL` (AXN_1.33) | `Platform\GpsVersion`, `gps.dll` em COM1, `BEE.BIN`/`mtkgps.dat` |
| Transmissor FM | **KT Micro KT0801A**, I²C1 (`0x8002D000`) escravo 0x36, referência de 32 kHz pela PWM3, alimentação GPIO4 ([fm-radio](fm-radio/00-visao-geral.md)) | `Drivers\BuiltIn\fmc` (FMC1:) |
| Áudio | `wavedev_drv.dll` (WAV1:) | `Drivers\Active` |
| Entrada AV | `avin.exe` | processo em execução |
| USB | só função (device): Serial_Class e Mass_Storage_Class | `Drivers\USB\FunctionDrivers` |
| Bootloader | EBoot "JWD B.13D.000000.101025.V2.0.6" | argumentos de boot em PA `0x200` |
| Sistema | Windows CE 5.0 build 1400, kernel de 04/08/2008, BSP MT3351 de 16/11/2011 | strings da imagem `nk` |

## Mapa de memória física

Lido das tabelas de página vivas (CP15 `TTB=0x00710000`) — é a `OEMAddressTable` do BSP,
confirmada byte a byte na imagem do kernel (região XIP em VA `0x8C003000`).

| Físico | Tamanho | VA cache / sem cache no CE | Uso |
|---|---|---|---|
| `0x00000000` | 128 MB | `0x8C000000` / `0xAC000000` | DRAM |
| `0x40000000` | 1 MB | `0x96000000` / `0xB6000000` | SRAM interna / boot ROM *(inferência)* |
| `0x48000000` | 1 MB | `0x97000000` / `0xB7000000` | *(a identificar)* |
| `0x50000000` | 1 MB | `0x98000000` / `0xB8000000` | contém a **ITCM** (16 KB em `0x50004000`, habilitada pelo CE; CP15 c9 `0x50004011`) |
| `0x80000000` | 16 MB | `0x80000000` / `0xA0000000` | periféricos APB |
| `0xA0000000` | 1 MB | `0x81000000` / `0xA1000000` | *(a identificar)* |
| `0xA2000000` | 4 MB | `0x82000000` / `0xA2000000` | *(a identificar)* |

### Uso da RAM pelo Windows CE

| Físico | Uso |
|---|---|
| `0x00000200` | argumentos de boot do EBoot (versão, OEM, plataforma) |
| `0x00003000–0x005BA0D8` | região XIP 1: kernel `nk.exe` + OAL + 42 módulos |
| `0x00705000–…` | região XIP 2 (142 módulos) e RAM do sistema |
| `0x00710000` | tabela de páginas de 1º nível (16 KB) |
| até `0x03E00000` | fim da RAM do CE (`ROMHDR.ulRAMEnd = 0x8FE00000`) |
| `0x03E00030` | **framebuffer** 800×480×2 |
| `0x04000000–0x07FFFFFF` | RAM estendida do CE *(inferência: total de 118 MB relatado)* |

## Registradores identificados (OAL desmontada)

Fonte: funções da OAL localizadas pelas strings de depuração do BSP e desmontadas
(`tools/bigfoston/oal_xref.py`).

| PA | Bloco | Como foi achado |
|---|---|---|
| `0x80001000` | configuração do sistema | `OEMInit` faz ler-modificar-escrever |
| `0x80022000` | **controlador de interrupções (CIRQ)** | `OALInterruptInit`; layout MediaTek MT62xx (abaixo) |
| `0x8002A000` | **GPT – timer do sistema** | `OALTimerInit`; `OEMinterruptHandles` ("GPT_IRQ_STA") |
| `0x8002C000` | 3º bloco mapeado junto (RTC? GPT2?) | `OALInterruptInit` |

### Periféricos declarados no registro do Windows CE (`HKLM\Drivers\BuiltIn`)

| Bloco | PA (`MemBase`) | IRQ | Driver CE | Observação |
|---|---|---|---|---|
| GPIO | `0x80002000` | | `buses.dll` | |
| DMA | `0x80023000` | | `dmadrv.dll` | portas VFIFO em `0x80110000…0x8011A000` |
| UART1 (COM2) | `0x80026000` | 3 | `uart.dll` | DMA rx 0x10 / tx 0x13 |
| UART2 (COM3) | `0x80027000` | 4 | `uart.dll` | DMA rx 0x11 / tx 0x14 |
| UART3 (COM4) | `0x80028000` | 5 | `uart.dll` | |
| UART4 (COM5) | `0x80029000` | 6 | `uart.dll` | |
| PWM | `0x8002B000` | | `buses.dll` | luz de fundo (`BKL`) usa o PWM |
| I2C | `0x8002D000` | | `buses.dll` | |
| SPI | *(não declarado)* | | `buses.dll` | TV Siano usa SPI |
| USB função | `0x80100000` | 8 | `usbfn.dll` | **Mentor MUSBHDRC** (EPINFO 0x44, RAMINFO 0x4a), IRQ W1C, FADDR não zera no reset — rede USB + SSH no Linux funcionando |
| SD (MSDC 1) | *(no código)* | DMA irq 1 | `sdhc_drv.dll` | |

### CIRQ em `0x80022000` (compatível com MT6516/MT62xx)

| Offset | Registrador | Uso observado na OAL |
|---|---|---|
| `+0x00…+0x30` | `IRQ_SEL0..12` | 5 campos de 6 bits por palavra; o CE grava a identidade (0,1,2,3,4 → `0x040c2040`) |
| `+0x48` / `+0x4C` | `IRQ_MASK_SET` L/H | `0xFFFFFFFF`: tudo mascarado no início |
| `+0x60` / `+0x64` | `IRQ_SENS` L/H | sensibilidade por fonte |
| `+0x78` | `IRQ_STA2` | código da IRQ ativa; bit `0x100` = nenhuma |
| `+0x7C` | `IRQ_EOI2` | fim de interrupção |
| `+0x10C` | — | `0xFFFF` gravado na inicialização |
| `+0x120…` | 16 palavras zeradas | |

### GPT em `0x8002A000`

`OALTimerInit` grava `+0x14 = 0x22`, `+0x1C = 0xFFFFFFFF`, `+0x10 = 0x23`.
O tratador lê `+0x04` (status) e grava `+0x08` (ack). Código de IRQ do GPT: `0x27`
(tradução por tabela em `0x8C005CA8`).

## Partições da NAND (`DSK9:`)

Imagem: `backup/2026-11-19-nand/DSK9-msflash.img` (não versionada; sha256
`a1d7ee88…62d83`). MBR com 4 partições:

| # | Tipo | Setor inicial | Tamanho | Conteúdo |
|---|---|---|---|---|
| 0 | `0x21` BINFS | 64 | 36,1 MB | imagem do Windows CE (2 regiões XIP) |
| 1 | `0x04` FAT16 | 18 560 | 3,0 MB | registro (`system.hv`, `user.hv`) |
| 2 | `0x0B` FAT32 | 20 096 | 75,0 MB | aplicativos: `app\` (multimídia, TV Cidana ISDB-T, BT ANW, jogos, Setting, USB) |
| 3 | `0x0B` FAT32 | 58 496 | 12,0 MB | assistência GPS (`BEE.BIN`, `ARC.BIN`, `mtkgps.dat`) |

O **bootloader** (EBoot) e o que vem antes do MBR **não** aparecem em `DSK9:`:
o backup dele exige outra rota (pendente).

## Drivers ativos (Windows CE)

`GPIO`, `DMA`, `MSFLASH (DSK9)`, `PWM`, `BKL` (luz de fundo), `Battery`, `NDIS`, `WAV1`
(áudio), `SIP`, `Uart1..4 (COM2..COM5)`, `I2C`, `SPI`, `fmc (FMC1)`, `SDBus`, `SDHC`,
`SPC`, `GPS (COM1)`, `GPS2 (COM9)`, `UsbFn (UFN1)`, `ViatualCom01/02 (COM0/COM7)`,
`HciExt`, `BtA2dpSnd`, `DdeDrv`, `MSDK`.

## Problema encontrado no cliente bsmd-ce

`IOCTL_UFN_CHANGE_CURRENT_CLIENT` **também grava o cliente como padrão de boot**
(`Drivers\USB\FunctionDrivers\DefaultClientDriver`). Depois de usar o bsmd-ce, o aparelho
passou a ligar como porta serial (`045e:00ce`) e não mais como pendrive; foi preciso
voltar pelo menu **Ajustes → USB**. O `bfagent` restaura o valor original logo após a troca;
o bsmd-ce precisa da mesma correção.

## Pendências desta etapa

- Identificar o layout interno de UART (16550?), SPI, I2C, NAND, SD, LCD e USB (endereços-base já conhecidos acima).
- ~~Identificar o chip do transmissor FM~~: KT0801A no I²C 0x36 ([fm-radio](fm-radio/03-engenharia-reversa-fmc.md)).
- Confirmar se o GPS é processado em software (MNL) na CPU principal.
- Backup do bootloader (blocos NAND antes do MBR).
- Verificar VFP (`FPSID`) e a frequência real da CPU.
