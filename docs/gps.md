# GPS (MT3328) no BigFoston

Estado: **não implementado — estudo de viabilidade concluído (21/11/2026)**, plano abaixo.

## Como o GPS funciona no aparelho

O MT3328 é *host-based* e **não tem modo autônomo**: não guarda firmware. A cada vez que
o chip liga, o `gps.dll` do Windows CE carrega o firmware do DSP pela UART (embutido no
DLL, 32 774 bytes) e, daí em diante, o chip só fala um protocolo binário de medidas. O
motor de navegação da MediaTek (MNL, `AXN_1.33`) roda na CPU principal, dentro do
`gps.dll`, e é ele que monta o NMEA (`$GPGGA`, `$GPRMC`, `$GPGSA`, `$GPGSV`, `$GPVTG`).
Não existe NMEA fora do `gps.dll`.

| Item | Valor | Fonte |
|---|---|---|
| `gps.dll` | módulo da ROM (XIP região 1), base `0x01DC0000`, 1,35 MB, **sem relocações** (só carrega nesse endereço) | NAND, `cerom.py` |
| Interface | stream driver `COM_*` (é o próprio `COM1:` dos aplicativos); entrega o NMEA por `VCOM_Driver.dll` / `VCOM_Driver2.dll` (buffer em memória compartilhada); `GPS2.dll` = `COM9:` | exportações, registro `Drivers\BuiltIn\GPS` |
| UART | `COM2:` do CE = UART1 `0x80026000` = **ttyS0** no Linux; início a 38 400, depois **921 600** baud, sem RTS/CTS, TCXO 26 MHz | estrutura de configuração em `0x01ECC0A0` |
| Reset | **GPIO 64** (DIR `0x80002020` b0, OUT `0x80002320` b0): pulso 1→0→1 (10 ms), espera 100 ms, até 3 tentativas | `IOCTL_HAL_GET_GPS_HW_PIN` do OAL; `0x1DF8D3C` |
| Standby | **GPIO 63** (DIR `0x80002010` b31, OUT `0x80002310` b31) | idem |
| Alimentação | PMU **`0x8006136C` bit 0** (`BusIoControl` recurso 3) | `bus.dll` |
| Arquivos | `\ResidentFlash\mtkgps.dat` (NV), `BEE.BIN`, `ARC.BIN` (assistência), `\Program Files\BEE_CE\*` | strings; partição 3 da NAND |
| Código | 1 160 funções, todas ARM 32 bits; sem chamadas de sistema do CE, sem SEH, **sem VFP** (ponto flutuante em software: `__muld`, `__addd`… pela tabela de imports) | pdata, desmontagem |
| Imports | 142 (coredll por ordinal + 2 do CEDDK): threads/eventos/filas de mensagens, arquivos, serial, registro, tempo (inclui `SetSystemTime`), `KernelIoControl`, `DeviceIoControl` (`GIO1:`) | tabela de imports |

GPIO/PMU lidos no Linux logo após o CE (21/11/2026, só leitura): GPIO 64 e 63 como saída,
ambos em 1; PMU `0x8006136C` = `0x1`. O CE deixa o chip ligado, com o DSP carregado.

## Caminho escolhido

Carregar o `gps.dll` **do próprio aparelho** num processo Linux, com uma camada que imita
as funções do CE que ele importa (um "Wine" mínimo para este DLL):

- mapear o código em `0x01DC1000` e os dados em `0x01EC9000` (`MAP_FIXED`), copiar os
  dados iniciais e preencher a tabela de imports com as funções da camada;
- `COM2:` → `/dev/ttyS0`; `GIO1:` → GPIO 63/64; recurso 3 → PMU `0x8006136C`;
  `\ResidentFlash\` → pasta de dados no cartão SD;
- a saída NMEA (`VCOM_Driver`) vai para um pseudo-terminal lido pela interface (e por
  `gpsd`, se um dia for preciso).

O DLL não é redistribuído: é extraído da memória interna do próprio aparelho na
instalação. Descartados: modo NMEA do chip (não existe) e reimplementar o motor (meses).

Cuidados de ABI: `printf` do CE recebe doubles sem alinhamento de 8 bytes (precisa de um
`printf` próprio); pilha alinhada em 4 bytes; código ARMv4 (`mov pc`), camada com `-marm`;
`__rt_sdiv(divisor, dividendo)`; `wchar_t` de 16 bits.

## Marcos (cada um conferível no aparelho)

| # | Marco | Teste | Estimativa |
|---|---|---|---|
| M0 | UART sem perdas (hoje 126 estouros em 4 s a 921 600: FIFO de 16 bytes) | zero estouros em 10 min; alternativa: baixar a velocidade do link (o motor aceita 115 200…921 600) | 1–2 semanas |
| M1 | carregador: mapear, imports que registram cada chamada, `COM_Init` | log até `GPS_HWInit: GPS_Reset=64; GPS_Standby=63` | 3–5 dias |
| M2 | núcleo da camada: threads, eventos, filas, registro, arquivos, tempo, ponto flutuante, `printf` | o motor inicia sem chamadas pendentes | 1–2 semanas |
| M3 | hardware: ttyS0, GPIO, PMU | `MT3328 Init Success` com o DSP carregado pelo Linux | ~1 semana |
| M4 | saída NMEA num pseudo-terminal; tela GPS da interface | `$GPRMC`/`$GPGSV` com satélites; primeira posição ao ar livre | 3–5 dias |
| M5 | robustez: guardar NV/BEE, religar o chip, consumo de CPU, `SetSystemTime` | horas de uso sem falha | ~1 semana |

Ferramentas e saídas da análise (desmontagem, imports, strings) ficaram em
`scratchpad/re-gps/` da sessão de 21/11/2026; os endereços acima permitem refazer.
