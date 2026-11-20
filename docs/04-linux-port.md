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
| Shell do BusyBox | **FUNCIONANDO** com alignment trap | `bftest` passo 9: `echo`, `func`, `subst`, `grep`, `sleep`, `init-like` → exit 0 |
| `/init` + init do BusyBox | **NÃO TESTADO** com `-mno-unaligned-access` | com o alignment trap, kernel panic em `strncpy_from_user` (ver abaixo) |
| Touchscreen | NÃO IMPLEMENTADO | ADC/registradores ainda não identificados |
| SD (MSDC) | NÃO IMPLEMENTADO | driver CE `sdhc_drv` (família MSDC) |
| USB device (rede/serial → SSH) | NÃO IMPLEMENTADO | base `0x80100000`, IRQ 8; IP ainda não identificado |
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

O estado de cada linha é atualizado conforme os testes no aparelho
([15-testes.md](15-testes.md)).
