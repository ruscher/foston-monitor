# Áudio (alto-falante) no Linux

Estado: **reprodução funcionando** (driver ALSA `mt3351-audio`, 2026-11-20). Testado com
um tom de 1 kHz e com `aplay` (WAV 22 050 Hz estéreo, duração correta, som limpo).
Pendente: fone (detecção no GPIO 12 / IRQ 70), gravação (microfone), interrupção de período
(IRQ 55; hoje usa um timer) e 48 kHz (código de taxa não confirmado).

## Como funciona

Os dados vêm do `wavedev_drv.dll` do WinCE (análise offline) e foram conferidos no aparelho.
O DSP do chip **não** participa da reprodução: ele é usado pelo CE para voz (PCM4WAY) e o
Linux não toca nele. O caminho é:

ARM → buffer em anel na RAM → **DMA de áudio em `0x8004B000`** → DAC interno → amplificador
(GPIO 5) → alto-falante. Não há codec externo.

| Registrador | Uso |
|---|---|
| `0x8004B000 + n*0x20` | canal n: +0x00 início (PA), +0x04 fim inclusivo, +0x08 posição de leitura, +0x10 formato\|taxa, +0x14 ganho (0x1000), +0x18 período (taxa/40) |
| `+0x10` | formato 0x40 = 16 bits estéreo, 0x60 = 16 bits mono; taxa 1 = 44,1k, 2 = 32k, 3 = 24k, 4 = 22,05k, 5 = 16k, 6 = 12k, 7 = 11,025k, 8 = 8k |
| `0x8004B100` | bit0 = ligado, bit(4+n) = canal n tocando |
| `0x8004B200/204/208` | 1 / 0x11F0 / 0x5B ao iniciar (valores do CE) |
| `0x80061200` | [7:0] volume (dois nibbles iguais, 0..15), [9:8] mudo |
| `0x80061208` | rampa do driver analógico: 0x1C, 200 µs, 0x1F |
| `0x80001344` / `0x80001324` | liga / desliga os clocks do áudio (ids 37 e 43: `0x00082000`) |
| GPIO 5 (`0x80002000` +0x000 dir, +0x300 saída) | amplificador do alto-falante, 1 = ligado |

O WinCE deixa o áudio parado e com os clocks desligados ao passar para o Linux.

## Driver

`firmware/kernel/overlay/sound/arm/mt3351-audio.c` (ALSA simples, uma saída):

- S16_LE, mono ou estéreo, 8 000 a 44 100 Hz;
- buffer de até 64 KB alocado pelo Linux;
- controle de mixer `Master` (0..15, padrão 5), que a interface usa via `amixer sset Master`;
- liga clocks, rampa analógica e amplificador no `prepare` e desliga tudo ao fechar.

Só escreve nos registradores da tabela acima. As páginas compartilhadas (PMU, clocks, GPIO)
são mapeadas sem serem reservadas, e as palavras analógicas são alteradas por
leitura-modificação-escrita.

`aplay` e `amixer` (alsa-utils, sem alsactl/alsamixer) entram no rootfs; a alsa-lib dobrou o
initrd (1,2 → 2,5 MB).

## Nota de teste

`dd` para `/dev/mem` numa faixa `no-map` causa *oops* (o `write()` usa o mapeamento linear,
que não existe ali). Para testes com buffer fora da RAM do Linux, use `devmem` (mmap).
