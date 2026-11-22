# Hardware medido e linha de base

Conferido no aparelho em 21/11/2026 (`/proc/cpuinfo`, `/proc/meminfo`, `/proc/config.gz`):

| Item | Medido |
|---|---|
| CPU | ARMv6 (ARM1176, `CPU part 0xb76`), 1 núcleo, BogoMIPS 466,9, sem VFP em uso (soft-float) |
| RAM | MemTotal 121 960 kB (128 MB menos a reserva do WinCE/framebuffer) |
| Kernel | 6.12.117, `CONFIG_HZ=100`, `PREEMPT_VOLUNTARY`, sem PSI e sem zram (antes) |
| Banda de memória | `memcpy` RAM→RAM **58 MB/s** (13,2 ms por tela de 768 KB), `memset` 118 MB/s, cópia para o framebuffer 72 MB/s (`membench`) |
| Cartão SD | leitura sequencial 7,0 MB/s pelo driver `mt3351-msdc` (PIO) |
| Interrupções em repouso | ~400/s (GPT 100/s, UART do GPS ~96/s, USB) |

A banda de memória baixa define o piso de qualquer redesenho de tela inteira: copiar
uma imagem 800×480 RGB565 custa ~13 ms.

## Linha de base (antes das mudanças)

Repouso na tela inicial (`bf-perf-monitor`, 15 s): CPU 16 % (com o monitor), **iowait
~20 % sem nenhuma leitura ou escrita no cartão**; o shell acordava 57 vezes/s.

Mapa (roteiro `BIGFOSTON_BENCH`, mapa nacional, Brasília z15): ver
[14](14-benchmarks-before-after.md). No arrasto a interface ficava **~260 ms parada em
cada volta do laço** (3,6 quadros/s).
