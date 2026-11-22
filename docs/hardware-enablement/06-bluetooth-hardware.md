# Bluetooth — hardware

Estado: **chip identificado, ligado e respondendo a comandos HCI no Linux** (22/11/2026).
Ainda não há pilha Bluetooth (BlueZ) nem perfis: ver [00-overview.md](00-overview.md).

## O que é

| Item | Valor | Como foi conferido |
|---|---|---|
| Chip | **MediaTek MT6611** | o próprio chip responde "MTK MT6611 #1" ao HCI Read Local Name |
| Fabricante / versão HCI | `0x0046` (MediaTek) / 4 (Bluetooth 2.1 + EDR) | HCI Read Local Version |
| Barramento | UART2 = `/dev/ttyS1` (`0x80027000`, IRQ 4 do CE) | registro do CE: `COM3:` |
| Velocidade | 115200 8N1 com RTS/CTS antes da inicialização; o CE passa a 3.200.000 baud com um comando da MediaTek | registro: `baud` `0x1c200`, `BaudAfterInit` `0x30d400`, `HWFlowControl` 1 |
| Alimentação | `LDO_EN` = **GPIO 104** | `OALIoCtlHalGetBTHWPin` da OAL (desmontada) |
| Reset | `RST` = **GPIO 3** | idem |
| Interrupção | `EINT` 11 | idem |
| Buffers do controlador | ACL 1021 bytes × 8, SCO 28 bytes × 2 | HCI Read Buffer Size |
| Recursos | `ff ff 8d fe 9b ff 79 83`: EDR 2/3 Mbps, eSCO, SCO, modos de economia | HCI Read Local Supported Features |

A anotação antiga "CSR BlueCore" vinha do `bthcsr.dll`, que está na ROM mas não é usado: o
registro aponta o transporte `mtk_bthuart.dll`. Os pinos do GPS lidos na mesma OAL (reset 64,
standby 63) batem com os que o `bfgps` já usa, o que confirma a leitura.

## Como o Windows CE liga o chip (`mtk_bthuart.dll`)

Desmontado com [tools/bigfoston/cedll.py](../../tools/bigfoston/cedll.py) (a DLL fica na
primeira região XIP do backup da NAND, `--offset 0x20000 --base 0x8c003000`):

1. **`Radio_HWTurnOn`** (pinos pela interface `GIO1:`, a mesma que o `bfgps` emula): os dois
   pinos como saída, sem pull, em 0; `LDO_EN` = 1; espera 3 ms; `RST` = 1; espera 1 s; limpa a
   UART; **HCI Reset** (`01 03 0C 00`) e exige "comando concluído" com status 0. Até 3
   tentativas. Desligar: `LDO_EN` = 0.
2. `Radio_CmdMTKAdvanceSetUart` e `Radio_ChangeControllerUART`: comando da MediaTek que leva o
   chip a 3,2 Mbaud com controle de fluxo; o lado do CE muda a UART e acorda o chip.
3. `Radio_CmdMTKGetGormVersion` ("Gorm" = família MT6611): recusa versões desconhecidas.
4. `Radio_CmdMTKSetE2BaseAddr` e **`Radio_WritePatch`**: grava um patch do firmware embutido
   na DLL (`patch\sys_patch_bin.c`).
5. HCI Reset, `Radio_CmdMTKSetBDAddr` (endereço do aparelho), `DisableEsco`, `SetSleep`, nome,
   classe do aparelho, filtro de eventos e visibilidade.
6. Depois, um gerenciador de sono troca comandos de dormir/acordar com o chip.

## Conferido no aparelho

Com [src/bt/btprobe.c](../../src/bt/btprobe.c) (lê e escreve só os registradores de GPIO
acima, os mesmos do `bfgps`):

| Passo | Resultado |
|---|---|
| Leitura antes (CE deixa desligado, `BootUpTurnOn` 0) | `LDO_EN` entrada em 0; `RST` e `EINT` entrada em 1 |
| Sequência do `Radio_HWTurnOn` + HCI Reset a 115200 com RTS/CTS | `04 0E 04 01 03 0C 00` (concluído, status 0) |
| Read Local Name / Version / BD_ADDR / Features / Buffer Size | respostas acima |
| Desligar | `LDO_EN` = 0, `RST` = 0 |

Sem o patch, a ROM do chip já responde aos comandos HCI básicos. O endereço Bluetooth sai
genérico (`00:00:46:66:11:01`) até o comando da MediaTek que grava o endereço; o CE gerou um
endereço próprio para este aparelho e o guardou no registro (ele não vai para o git nem para
esta documentação: é um identificador do aparelho; a ferramenta de instalação deve lê-lo do
backup do próprio aparelho).

## Próximos passos

1. Kernel: `CONFIG_BT`, `CONFIG_BT_HCIUART` (H4) e `CONFIG_BT_HCIUART_H4`.
2. Um programa de inicialização em C (sem daemon) que faça os passos 1–5 e entregue a UART ao
   kernel (`ioctl(TIOCSETD, N_HCI)` + `HCIUARTSETPROTO`, como o `btattach`). O patch e o
   endereço: extraídos do backup do próprio aparelho para o cartão, como o `gps.dll` — firmware
   proprietário não entra no repositório.
3. BlueZ (`bluetoothd`, exige D-Bus) ou uma pilha menor; medir RAM e CPU antes de escolher.
4. Áudio: BlueALSA (A2DP); viva-voz depende do caminho SCO/PCM do chip até o codec e do
   microfone (ainda não implementado).
