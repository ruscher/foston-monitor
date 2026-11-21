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
| SD (MSDC1) | **FUNCIONANDO** (leitura e gravação) | driver `mt3351-msdc` (polling/PIO); SDHC de 32 GB: leitura conferida com leitor no PC, ~8 MB/s; gravação liberada só no cartão com a marca do seu número de série (~1,4 MB/s, testes com MD5); ajustes do shell no cartão — [sd-controller.md](sd-controller.md) |
| Brilho da tela | **FUNCIONANDO** | `mt3351-backlight` (`/sys/class/backlight`), palavra da PMU `0x80061308` (3–19), testado pelo slider da interface |
| Áudio (alto-falante) | **FUNCIONANDO** | `mt3351-audio` (ALSA): DMA de áudio `0x8004B000`, DAC interno, amplificador no GPIO 5; `aplay` de WAV com som limpo — [audio.md](audio.md) |
| Reinício (`reboot`) | PARCIAL | `mt3351-reboot` dispara o watchdog como o CE (`0x80003000`); o aparelho **desliga** em vez de reiniciar (o CE faz igual) e volta no botão liga |
| Desligar (`poweroff`) | **FUNCIONANDO** | a mesma sequência do watchdog registrada como POWER_OFF; antes o `poweroff` só parava a CPU com a tela acesa |
| GPS | NÃO IMPLEMENTADO | MT3328 *host-based*: a posição é calculada pela biblioteca proprietária MNL do `gps.dll` (UART1, 921600 baud). Caminho viável: carregar o `gps.dll` do próprio aparelho num processo Linux com uma camada de API do CE (estimativa 4–6 semanas); primeiro passo é o driver de UART |
| Relógio | PARCIAL | sem driver de RTC no Linux; a hora vai no DTB (`bigfoston,boot-epoch`): a do PC (`boot-mem`/`boot-flash`) ou, no boot automático, a do relógio do CE, que o PC acerta com `bfagent.py settime` (agente ≥ 0.5.1; também automático em `install-linux`/`autoboot on`/`boot-*`). Hora antes de 2024 é ignorada e a barra mostra `--:--`. No Linux: ajuste manual, fusos, horário de verão e NTP (`ntpd` do BusyBox; o PC serve a hora com `tools/bigfoston/sntp_server.py`) em Ajustes > Data |
| USB device: rede CDC-ECM + serial CDC-ACM + SSH | **FUNCIONANDO** (High-Speed desde 21/11/2026, abaixo; gravado na memória interna e reconexão do cabo em 6 de 6 trocas em High-Speed; reconexão do cabo em 4 de 4 trocas com a verificação da FIFO no reset, contadores em `/sys/module/mt3351/parameters/stats`) | 20/11/2026: PC vê `0525:a4aa` "CDC Composite Gadget", `ping` 0,6–1,2 ms, `ssh root@192.168.7.2`; 2 MB com md5 igual: 189 KB/s PC→FOSTON, 241 KB/s FOSTON→PC |
| UARTs | **FUNCIONANDO** (recepção testada) | `8250_mt3351`: ttyS0..3, 16550A a 29,25 MHz; para o DMA que o CE deixa ligado (causa da escrita em 0x7E8000); GPS recebido em ttyS0 — [uart.md](uart.md) |
| MP3 | **FUNCIONANDO** | `mpg123` (decodificador `arm_nofpu`) pela emulação OSS: MP3 de 128 kbps a ~11 % da CPU |
| Bateria e carregador | **FUNCIONANDO** (leitura) | `power_supply` no `mt3351-ts` (mesmo ADC do touch, com trava): tensão pelo canal auxiliar `+0x28` (mV = valor × 5600/1023), % pela tabela do CE, carregador em `0x80061420` (bit 3 presente, bit 14 carregando). Medido: 3996 mV, 100 %, Full. **A carga é feita por software no CE; no Linux a bateria pode não carregar** — não deixar horas no Linux com bateria baixa |
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
| `device descriptor read/64, error -71` em High-Speed (até 20/11/2026) | não isolada: o PHY (`0x80100600`–`0x8010061B`) já está como o CE deixa (engenharia reversa do `usbfn.dll`) | High-Speed desde 21/11/2026 gravando `0x0003` em `MUSB+0x70` na inicialização e a cada reset, como o CE (`usbfn.dll`, VA `0x3cd23d0`); `mt3351.hs=0` volta ao Full-Speed |
| `error -32` (STALL) em Full-Speed; só IRQs de reset/SOF; `FADDR` preso em `0x04` | reset do barramento **não zera `FADDR`**; status de IRQ é **W1C** (como `mediatek.c`) | zera `FADDR` no reset; `clearb/clearw` W1C |

**High-Speed (21/11/2026).** O PC registra `new high-speed USB device`; 16 MB com
SHA-256 igual nos dois sentidos. TCP puro (sem a cifra do SSH): **3,7 MB/s
PC→FOSTON e 6,2 MB/s FOSTON→PC**; pelo SSH ~1,1–1,2 MB/s (a cifra no ARM é o
limite). Em Full-Speed eram 189/241 KB/s. Parâmetros em `/sys/module/mt3351/parameters/`:

| Parâmetro | Padrão | Efeito |
|---|---|---|
| `hs` | `Y` | High-Speed; `mt3351.hs=0` na linha de comando volta ao Full-Speed |
| `reg70` | `3` | valor gravado em `MUSB+0x70` na inicialização e a cada reset (o do CE); `-1` não grava. Lê `0x1201` em HS no CE e no Linux. Se a gravação é necessária **não foi isolado** (sem ela, uma reconexão por software ainda negocia HS, mas o registrador pode manter o efeito da gravação do boot) |
| `fifo32` | `Y` | FIFO com acessos de 32/16 bits para buffers alinhados (endpoints 1–4); com `0` (alterável em execução), byte a byte. Com bytes: 2,7 e 3,4 MB/s |

O CE (engenharia reversa do `usbfn.dll`, região XIP 1) liga o PHY com `0x600` b19/b7,
`0x60C` b8 e `0x610`/`0x61A`/`0x61B` (detecção do carregador), não reage ao
`POWER.HSMODE` e não usa o DMA Inventra; o VBUS vem do **GPIO7 / EINT1**
(`0x80002400` bit 7), não do comparador do MUSB.

Limitações atuais: PIO (sem o DMA Inventra); tirar e recolocar o cabo funciona
(6 de 6 em High-Speed, 21/11/2026). **Ligar o aparelho sem o cabo e conectar depois
falhava** (21/11/2026: nenhum evento USB no PC): sem cabo no boot, o CE deixa o bloco USB
desligado (PHY `0x600` b7 / `0x60C` b8, clock e PMU `0x80061358` b0). O glue agora lê
esses bits no probe e, se estiverem desligados, liga o bloco com a sequência do
`usbfn.dll`/`bus.dll` (sem a detecção de carregador); com cabo no boot nada é escrito
— **falta conferir no aparelho**. O MAC visto pelo PC é fixo (`g_cdc.host_addr=02:46:53:37:30:01`)
e a conexão `bigfoston` do NetworkManager o usa, em qualquer porta USB. O log do kernel
fica no cartão em `BigFoston/logs/boot.log` (aos 45 s e aos 3 min). Uso: `ssh root@192.168.7.2` (só chave), serial em
`/dev/ttyACM0`; no PC, `192.168.7.1/24` na interface `cdc_ether`.

O estado de cada linha é atualizado conforme os testes no aparelho
([15-testes.md](15-testes.md)).
