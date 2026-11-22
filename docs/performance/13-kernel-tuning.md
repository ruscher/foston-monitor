# Kernel

Acrescentado em `firmware/buildroot/board/foston-fs707dc/linux.config`:

- `CONFIG_ZRAM` (+ `ZSMALLOC`, compressores LZ4, LZO, ZSTD; padrão LZ4): para o teste de
  [05](05-zram.md). **Não é ativado no boot.**
- `CONFIG_PSI`: `/proc/pressure/{cpu,memory,io}` para medir pressão.
- `CONFIG_KEXEC` (Reiniciar sem desligar, entrega anterior).

Não mudou: `HZ=100`, `PREEMPT_VOLUNTARY`, `-mno-unaligned-access` (defeito do MT3351
documentado em docs/04-linux-port.md), drivers de SD/toque/áudio/UART.
