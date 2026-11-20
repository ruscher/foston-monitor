# 04 · Port do Linux para o MT3351

Não existe suporte público ao MT3351 no Linux (nem upstream, nem BSP do fabricante
encontrado). O port é feito a partir do zero, sobre o Linux **6.12 LTS**, com drivers
escritos para os registradores medidos no aparelho ([01-hardware.md](01-hardware.md)).

## Plataforma

| Item | Escolha | Motivo |
|---|---|---|
| Kernel | Linux 6.12.117 (LTS) | suporte longo; ARMv6 multiplataforma com device tree |
| Arquitetura | `ARCH_MULTI_V6`, `CPU_V6K`, sem SMP, sem VFP | ARM1176JZ-S medido; VFP não habilitado pelo CE |
| Errata | `ARM_ERRATA_411920` | ARM1176 r0p4 |
| Boot | zImage + DTB + initrd pelo `bfagent` ([05-bootloader.md](05-bootloader.md)) | sem gravar a flash |
| Userland | Buildroot 2026.02.3 (LTS), musl, soft-float, BusyBox | pequeno e reproduzível |

## Código do port

Fonte: [`firmware/kernel/overlay/`](../firmware/kernel/overlay/) (arquivos novos) +
[`firmware/kernel/apply-overlay.sh`](../firmware/kernel/apply-overlay.sh) (Kconfig/Makefile).
O patch do Buildroot é gerado por `firmware/kernel/mkpatch.sh`.

| Arquivo | Função | Base factual |
|---|---|---|
| `arch/arm/mach-mt3351/Kconfig` | plataforma `ARCH_MT3351` | — |
| `drivers/irqchip/irq-mt3351-cirq.c` | controlador de interrupções CIRQ, 64 linhas, `handle_fasteoi` | OAL desmontada: `OALInterruptInit`, `SetIRQMask`, `ClearIRQMask`, tratador de IRQ |
| `drivers/clocksource/timer-mt3351-gpt.c` | clocksource, sched_clock e clockevent one-shot no GPT1 | medição: contador a 14,625 MHz, compare em `+0x1C`, IRQ 39 |
| `drivers/video/fbdev/mt3351-bootcon.c` | console de texto no framebuffer do CE (antes do timer) | framebuffer medido em `0x03E00030` |
| `drivers/usb/musb/mt3351.c` | cola do controlador USB (Mentor MUSB, traços MediaTek) | registradores lidos no aparelho (abaixo) |
| `drivers/input/touchscreen/mt3351-ts.c` | touchscreen resistivo (polling, evdev) | `touch.dll` do CE + `devmem` |
| `drivers/mmc/host/mt3351-msdc.c` | slot SD MSDC1 (polling, PIO, ro por padrão) | `sdhc_drv.dll` do CE + `devmem` |
| `arch/arm/boot/dts/mediatek/mt3351-foston-fs707dc.dts` | device tree | tudo medido |

## Etapas e estado

| Etapa | Estado | Evidência |
|---|---|---|
| Tomar a CPU a partir do CE (payload bare-metal) | **FUNCIONANDO** | `fbtest` no aparelho, 19/11/2026 |
| Kernel compila | **FUNCIONANDO** | Buildroot 2026.02.3: zImage 1,4 MB, initrd 1,1 MB |
| Kernel inicia, mensagens na tela | **FUNCIONANDO** | log do kernel no framebuffer — [foto](images/foto-linux-primeiro-boot.jpg) |
| Timer e interrupções | **FUNCIONANDO** | `selfcheck`: jiffies e ticks do GPT avançam com `mdelay`/`msleep`; `bfwatch` a cada segundo |
| initramfs descompactado | **FUNCIONANDO** | `bfcheck` confere o CRC32 do initrd antes de descompactar |
| Memória (103 MB vigiados) | **FUNCIONANDO** | `memwatch: no change` durante todos os testes |
| Processos, fork, sinais, COW, exec | **FUNCIONANDO** | `bftest` passos 1–13, 19/11/2026 |
| Shell do BusyBox | **FUNCIONANDO** | `bftest` passo 9: `echo`, `func`, `subst`, `grep`, `sleep`, `init-like` → exit 0 |
| `/init` + init do BusyBox + `rcS` | **FUNCIONANDO** | build com `-mno-unaligned-access`, 19/11/2026: `bigfoston: alive 630s`, timer irqs 17254→19708, `memwatch: no change (103 MB)` |
| Touchscreen | **FUNCIONANDO** | driver `mt3351-ts` (evdev, filtro de pressão como o CE); calibração de 5 pontos: erro 3,6 px no centro, 10–12 px nos cantos — [touchscreen-driver.md](touchscreen-driver.md) |
| Framebuffer `/dev/fb0` | **FUNCIONANDO** | `simplefb` 800×480 RGB565 em `0x03E00030` (não alinhado à página: mmap + 0x30) |
| Interface LVGL no aparelho | **FUNCIONANDO** | `bigfoston-shell` sobe no boot (`S90bigfoston-shell`), toque calibrado, 1,7 MB de RAM |
| SD (MSDC1) | **LEITURA FUNCIONANDO**, escrita bloqueada | driver `mt3351-msdc` (polling/PIO): cartão de 1,91 GiB, FAT32 montado ro, SHA-256 confere, ~6 MB/s — [sd-controller.md](sd-controller.md) |
| Relógio | PARCIAL | sem driver de RTC; a hora do PC vai no DTB (`bigfoston,boot-epoch`) e o `S05clock` acerta no boot |
| USB device: rede CDC-ECM + serial CDC-ACM + SSH | **FUNCIONANDO** (Full-Speed) | 20/11/2026: PC vê `0525:a4aa` "CDC Composite Gadget", `ping` 0,6–1,2 ms, `ssh root@192.168.7.2`; 2 MB com md5 igual: 189 KB/s PC→FOSTON, 241 KB/s FOSTON→PC |
| UARTs | NÃO IMPLEMENTADO | 16550 MTK em `0x80026000…0x80029000`, IRQ 3–6; clock desconhecido |
| Luz de fundo (PWM) | NÃO IMPLEMENTADO | PWM em `0x8002B000` |

## Problemas encontrados e corrigidos

| Sintoma | Causa | Correção |
|---|---|---|
| Nada na tela após o salto | CE deixa o **FCSE PID** (CP15 c13) ligado | o stub do loader zera o PID |
| initramfs "malformed archive" | DMA do USB do CE continua escrevendo na RAM | boot com contagem e **cabo USB desconectado** |
| páginas do BusyBox corrompidas em `0x007e8000` | buffers DMA do CE | `reserved-memory` de 1 MB em `0x00700000` |
| espera de 255 s no CRNG | sem fonte de entropia | `rng-seed` no DTB |
| `cachepolicy=writethrough` sem efeito | ARMv6 só aceita writeback | — (teste inválido descartado) |
| **ash trava em PC 0x5333c** | **defeito do MT3351**, abaixo | `-mno-unaligned-access` |

### Defeito do MT3351: STRH em endereço ímpar após copy-on-write

Medido com `bftest=bytes` (19/11/2026). Uma escrita de meia-palavra em endereço
**ímpar**, logo depois de uma falha de copy-on-write na mesma página, **desfaz a
escrita de byte feita imediatamente antes na mesma palavra**: o byte volta ao valor
antigo ou ao conteúdo da página antes da cópia.

| Teste (página recém-copiada por COW, 400 repetições) | trap OFF | trap ON |
|---|---|---|
| `memset(p,0,8)` da musl (`strb b0; strb b7; strh b1-2; strh b5-6; …`) | 388 erros | **0** |
| `strb b0; strh p+1` | **400 erros** | 0 |
| `strb b0; strb b1; strb b2` | 0 | 0 |

Não é cache de dados (com o D-cache desligado o erro persiste), nem TLB, nem
aliasing. O GCC gera esses `strh` desalinhados porque o ARMv6 os permite
(`-munaligned-access` é o padrão). O BusyBox `ash` morria pelo `memset` da musl
zerando estruturas recém-copiadas após `fork`.

- **Ligar `SCTLR.A`** (alignment trap) elimina o erro no espaço do usuário, mas o
  kernel passa a gerar exceções em cópias palavra-a-palavra de dados do usuário
  (`strncpy_from_user`) e entra em pânico ao abrir arquivos. Fica só como chave de
  teste: `timer_mt3351_gpt.align_trap=1`.
- **Correção adotada:** kernel e todo o userland compilados com
  `-mno-unaligned-access` (`apply-overlay.sh` e `BR2_TARGET_OPTIMIZATION`), para
  que o compilador nunca emita escritas desalinhadas.

### USB: Mentor MUSB com traços MediaTek

Identificado lendo, pelo `bfagent` (só leitura), os registradores de configuração em
`0x80100000`: `EPINFO 0x44` (4 TX + 4 RX), `RAMINFO 0x4a` (FIFO 4 KB, 4 canais DMA),
`LINKINFO 0x5c` e `VPLEN 0x3c` (valores de reset do MUSBHDRC). O que foi preciso:

| Sintoma no PC | Causa | Correção na cola |
|---|---|---|
| `device descriptor read/64, error -71` em High-Speed | PHY não ajustado para HS após o CE | `maximum_speed = USB_SPEED_FULL` |
| `error -32` (STALL) em Full-Speed; só IRQs de reset/SOF; `FADDR` preso em `0x04` | reset do barramento **não zera `FADDR`**; status de IRQ é **W1C** (como `mediatek.c`) | zera `FADDR` no reset; `clearb/clearw` W1C |

Limitações atuais: só **com o cabo conectado durante o boot** (ao desconectar, o PHY
não volta sozinho: VBUS deixa de ser visto, `DEVCTL 0x89`; o CE usa um EINT para isso),
Full-Speed, PIO byte a byte. Uso: `ssh root@192.168.7.2` (só chave), serial em
`/dev/ttyACM0`; no PC, `192.168.7.1/24` na interface `cdc_ether`.

O estado de cada linha é atualizado conforme os testes no aparelho
([15-testes.md](15-testes.md)).
