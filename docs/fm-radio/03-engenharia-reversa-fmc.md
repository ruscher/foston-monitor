# Engenharia reversa do transmissor FM (Windows CE)

Análise só de leitura dos arquivos do próprio aparelho: `FMC.dll` (9 728 bytes, PE ARMv4T,
`E:\WINCE500\platform\APOLLO\...\FMC.pdb`), `buses.dll` e `wavedev_drv.dll` (ROM XIP) e
`Gui.dll` (partição de aplicativos). Desmontagem com o `objdump` do toolchain do Buildroot.
Nenhum código ou dado proprietário vai para o repositório.

## FMC.dll

Exporta o driver de fluxo `FMC_*` (`FMC1:` em `HKLM\Drivers\BuiltIn\fmc`). Textos:
`"KT0801A"`, `"I2C1:"`, `"GIO1:"`, `"pwm simulation crystalloid"`,
`"--- powerup FM_KT0801A_ChipStart------- !"`.

| IOCTL | Função | Rotina |
|---|---|---|
| `0x224000` | ler a frequência (MHz×10) | `0x1000219c` |
| `0x224004` | definir a frequência (MHz×10) | `0x1000226c` |
| `0x22401C` | ligar: PWM ligada + `ChipStart` | `0x10001f20(1)`, `0x10002328` |
| `0x224020` | desligar: `ChipStop` + PWM desligada | `0x10002400`, `0x10001f20(0)` |

`FMC_Init`: prepara a PWM (`0x10001d10(1)`) e o I²C/GPIO (`0x10002490`). Não liga o chip.

### Clock de referência: PWM3 a 32 768 Hz

`0x10001d10` (`"pwm simulation crystalloid"`):

1. calcula o divisor (`0x10002794`) para 32 768 Hz a partir de **58 500 000 Hz** (literal
   `0x037CA3A0`): testa prescaler 1, 4, 16, 64 e escolhe o menor erro → prescaler 1, período
   **1786** (32 754 Hz, erro de 14 Hz);
2. mapeia a PWM em `0x8002B000` (0x19C bytes) e o sistema em `0x80001000`;
3. `0x80001700 |= 0x400` (bit 10; multiplexação do pino);
4. pelo `GIO1:` (tabela de funções, ver abaixo), **OCFG `0x80002600` &= ~(1<<23)**
   (`"Open GPIO To Disable Clock Output"`);
5. canal PWM3 (bloco em +0x90, layout dos PWM MT65xx): `CON (+0x90) |= 0x8000` (modo antigo),
   `&= ~8` (clock do bloco), divisor 0; `DATA_WIDTH (+0xBC) = 1785`, `THRESH (+0xC0) = 892`
   (50 %), `WAVE_NUM (+0xB8) = 0` (contínuo).

Ligar/desligar (`0x10001f20`): bit 2 de `PWM+0x00` (enable da PWM3).

No Linux, depois do boot (o CE roda antes): `DATA_WIDTH = 0x6F9 (1785)`, `THRESH = 0x37C (892)`,
`0x80001700` bit 10 = 1, OCFG bit 23 = 0, PWM3 desligada — exatamente o que a DLL deixa.

### Alimentação: GPIO4

`0x10002490` abre `GIO1:` e pede a tabela de funções (IOCTL `0x220400`, 64 bytes). A tabela do
`buses.dll` (`0x3d12b7c`) tem, por deslocamento:

| +off | Função (`buses.dll`) | Registrador |
|---|---|---|
| +8 | escrever saída (pino, valor) | `0x80002300` DOUT |
| +16 | direção (1 = saída) | `0x80002000` DIR |
| +24 | pull enable | `0x80002100` |
| +32 | pull select | `0x80002200` |
| +48 / +52 | escrever / ler OCFG inteiro | `0x80002600` |

Init: GPIO4 saída, sem pull, nível 0. `ChipStart` (`0x10002328`): **GPIO4 = 1**, espera
**500 ms**, lê o registrador `0x0F` do chip e confere o bit 4 (**PW_OK**), até 3 vezes; depois
grava a tabela do chip. `ChipStop` (`0x10002400`): lê de volta os registradores da tabela e põe
GPIO4 = 0.

### I²C: escravo 0x36

Transações pelo `I2C1:` com a estrutura `{tipo, velocidade, endereço 0x36, ...}`: tipo 2 =
escrita de 2 bytes (registrador, valor), tipo 3 = escrita de 1 byte seguida de leitura de 1.
Configuração `{5, 5, 2000}` (a mais lenta: TIMING `0x73F`, o valor que está no aparelho).

O controlador (`I2C_Init`, `0x3d16314`) fica em **`0x8002D000`** (0x6C bytes) e segue o layout
dos I²C MT65xx:

| Reg | Uso no CE |
|---|---|
| `+0x00` DATA | bytes a enviar; leitura do FIFO |
| `+0x04` SLAVE_ADDR | endereço << 1 (`0x6C`) |
| `+0x08` / `+0x0C` | INTR_MASK / INTR_STAT (= 15 para limpar; bit 0 fim, bits 1-2 sem ACK) |
| `+0x10` CONTROL | `0x30` escrita; `0x32` escrita + leitura (repetição de start) |
| `+0x14` TRANSFER_LEN | bytes (escrita + leitura << 8) |
| `+0x18` TRANSAC_LEN | 1 escrita; 2 escrita+leitura |
| `+0x20` TIMING | `sample << 8 | step` |
| `+0x24` START | 1 |
| `+0x30` FIFO_STAT | bits 7:4 = bytes lidos |
| `+0x38` FIFO_ADDR_CLR | 1 |
| `+0x48` HS | bit 0 = alta velocidade (0) |

Clock do I²C: BUS1 id **19** = PDN0 bit 19 (`0x80001300` estado, `0x80001340` liga,
`0x80001320` desliga).

### Frequência

Tabela do chip em `0x10003038`: **`{0x00, 0x20}, {0x01, 0xDB}`** e a frequência guardada
**800** (80,0 MHz). Codificação (CHSEL de 12 bits, unidades de 50 kHz):

- `reg0 = CHSEL[8:1]`
- `reg1[2:0] = CHSEL[11:9]` (bits 7:3 = RFGAIN/PGA: `0xD8` no CE)
- `reg2 bit 7 = CHSEL[0]` (o CE não grava o reg2: passos de 100 kHz)

O valor da IOCTL é MHz × 10 (CHSEL = valor × 2). Leitura do chip no FOSTON, ligado em 88,1 MHz:

```
00=71 01=c3 02=40 03=00 04=04 05=b4 06=5c 07=e8
08=b1 09=0c 0a=00 0b=00 0c=00 0d=00 0e=c2 0f=ff
10=a8 11=00 12=80 13=80 14=00 15=e0 16=00 17=00
18=22 19=00 1a=00 1b=00 1c=00 1d=00 1e=00 1f=20
```

(reg1 = 0xC3 é o padrão do chip; o `bffm` grava 0xD8|CHSEL[11:9], o valor do CE.)

## Gui.dll e o áudio

O app original usa a classe `CFMControl` (`Gui.dll`):

- `Open` (`0x1001946c`): `CreateFile("FMC1:")`, IOCTL `0x22401C`, e
  **`waveOutMessage(0, 0x41C, 0, 0)`** (COREDLL ord. 398); grava o estado em `HKCU`.
- `Close` (`0x10019574`): IOCTL `0x224020` e **`waveOutMessage(0, 0x41C, 1, 0)`**.
- `SetFreqency` / `GetFreqency`: IOCTLs `0x224004` / `0x224000`.

"Aberto"/"Fechado" da tela original (`FM.xml`: `CheckBtn_Open`, `CheckBtn_Close`) são esses
`Open`/`Close`: ligar e desligar o transmissor. No app novo: **Transmitir — Ligado/Desligado**.

### Áudio

`wavedev_drv.dll`, mensagem `0x41C` (tabela de `0x3c080e8`, caso 0x1C → `0x3c08274`): guarda o
parâmetro numa flag (`ctx+0x10C8`) e recalcula a rota (`0x3c0bbdc`); com a flag em 0 a rota dá
"sem alto-falante" e `0x3c0743c` escreve o **GPIO5 = 0** (amplificador do alto-falante; saída,
sem pull). Nada mais muda: o DAC continua tocando. Conclusão: **o KT0801A recebe o sinal
analógico do DAC** (o mesmo que vai ao amplificador), e "FM ligado" no CE = transmissor ligado +
alto-falante mudo.

No Linux o `mt3351-audio` já controla o GPIO5 pelo "Speaker Playback Switch"; o `bffm` usa esse
controle (sem escrever o GPIO5 por fora) e o devolve ao desligar.
