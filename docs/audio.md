# Áudio (alto-falante) no Linux

Estado: **reprodução funcionando** (driver ALSA `mt3351-audio`, 2026-11-20). Testado com
um tom de 1 kHz e com `aplay` (WAV 22 050 Hz estéreo, duração correta, som limpo).
Fone de ouvido: **funcionando** (detecção e troca, 2026-11-20). Pendente: gravação
(microfone), interrupção de período (IRQ 55; hoje usa um timer) e 48 kHz.

## Fone de ouvido

Do `wavedev_drv.dll` (análise estática) e conferido no aparelho:

| Item | Valor |
|---|---|
| Detecção | GPIO 12, nível em `0x80002400` bit 12: **1 = fone plugado** (confirmado com o fone) |
| No CE | EINT 6 = IRQ 70 (linha 7 do CIRQ, com debounce de hardware); no Linux, leitura a cada 200 ms e duas leituras iguais |
| Com o fone | amplificador do alto-falante (GPIO 5) desligado e `0x8004B204` = `0x01F0` (bit 12 zerado) |
| Sem o fone | amplificador ligado durante a reprodução e `0x8004B204` = `0x11F0` |
| Analógico | nada muda nas palavras `0x800612xx` (o fone sempre recebe o DAC) |

Teste: tom de 440 Hz com o fone plugado saiu só no fone (alto-falante mudo); ao tirar,
o amplificador religou com `0x11F0`. ALSA: "Headphone Jack" (com dispositivo de entrada
`SW_HEADPHONE_INSERT`) e "Speaker Playback Switch". O shell mostra um aviso na tela.

**Microfone:** o wave-in do CE não lê nenhum ADC (parece cópia da saída); a captura
real só aparece no caminho de voz do DSP (Bluetooth viva-voz) e numa rotina de teste de
fábrica (`0x8004D000`, IRQ 54, 16 kHz mono por PIO). Não implementado.

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
| `0x80002400` bit 12 | entrada do GPIO 12: fone plugado |

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

## Tocador do Multimídia (shell)

`src/ui/core/player.c` + `src/ui/screens/scr_music.c`:

- **Motor:** a `libmpg123` (decodificador `arm_nofpu` em ponto fixo, linkada estaticamente no shell) decodifica numa thread e escreve em `/dev/dsp` (emulação OSS do kernel). Não usa `alsa-lib`, que num rootfs estático custaria ~600 KB por programa.
- **Fontes:** `*.mp3` em `/mnt/sd` (cartão SD montado só leitura pelo `S30sdcard`) e em `<dados>/music`.
- **Tela:**
  - capa do álbum (JPEG da tag ID3 APIC, decodificado com o TJpgDec do LVGL e reduzido para 176×176 RGB565);
  - título, artista, álbum e ano;
  - formato real (MPEG, kbps — média do arquivo em VBR —, kHz, canais);
  - espectro de 24 faixas calculado do som tocado (FFT inteira de 256 pontos);
  - barra de progresso com avanço por toque;
  - aleatório, repetir (desligado/todas/uma), lista de reprodução, informações e estatísticas da sessão;
  - tela cheia.
- A música continua ao sair da tela.

Medido no FS-707DC com MP3 de 320 kbps e a tela do tocador aberta: decodificação 11 % da CPU, interface 7 %, sistema 19 %. O espectro é desenhado por **um único objeto** com desenho próprio: com 24 objetos-barra eram ~1000 áreas invalidadas por segundo e a interface gastava ~33 %.
