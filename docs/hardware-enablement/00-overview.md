# BigFoston OS — recuperação do hardware e multimídia

Continuação da versão 0.2.0 (branch `feat/hardware-enablement`, a partir da `main`).
Estados: **FUNCIONANDO NO HARDWARE**, FUNCIONANDO NO SIMULADOR, PARCIAL, NÃO IMPLEMENTADO,
INVIÁVEL NESTE HARDWARE, BLOQUEADO POR DEPENDÊNCIA.

## Auditoria (22/11/2026)

- `main` (66c8d81) contém tudo de `bigfoston-linux` e `perf/bigfoston-extreme-optimization`.
  O único commit da `big-foston-os` fora da `main` é o merge da PR #1 (395650c), cujos dois
  pais estão na `main` e cuja árvore é igual à do segundo pai: nada se perde.
- Aparelho: firmware 0.2.0 aplicado (`update.log` 21/11 22:34 APLICADA, shell "BigFoston OS
  0.2.0"). Sem alterações locais não enviadas.

## Estado

| Recurso | Estado | Documento |
|---|---|---|
| Vigia do cartão por aviso do kernel | **FUNCIONANDO NO HARDWARE** | [03](03-sd-watchdog.md) |
| `reboot` do terminal com kexec | **FUNCIONANDO NO HARDWARE** | [04](04-reboot-kexec.md) |
| Busca com abreviações, índice v3, busca em thread | **FUNCIONANDO NO HARDWARE** | [05](05-big-gps-improvements.md) |
| Semáforos, pedágios, moto, evitar pedágios | PARCIAL: arquivos nacionais gerados e conferidos no PC; falta copiar para o cartão | [05](05-big-gps-improvements.md) |
| Zoom 15 em cidades densas | NÃO IMPLEMENTADO | [05](05-big-gps-improvements.md) |
| Navegação com GPS real em movimento | NÃO TESTADO | [05](05-big-gps-improvements.md) |
| Bluetooth: kernel (hci0), 921600 baud, endereço do aparelho | **FUNCIONANDO NO HARDWARE** | [07](07-bluetooth-driver.md) |
| Bluetooth: BlueZ, pareamento, música do celular no FOSTON (A2DP) | **FUNCIONANDO NO HARDWARE** (firmware de teste; com música: 23 % de CPU livre, falta o DMA da UART) | [08](08-bluetooth-profiles.md) |
| Bluetooth: app (parear, conectar, música do celular) | PARCIAL: agente testado no aparelho, tela no simulador | [08](08-bluetooth-profiles.md) |
| Bluetooth: fones/caixas, telefone, HID | NÃO IMPLEMENTADO | [08](08-bluetooth-profiles.md) |
| Microfone | NÃO IMPLEMENTADO (o CE grava pelo caminho de voz do DSP, ver [audio.md](../audio.md)) | — |
| TV digital (Siano SMS1xxx, ISDB-T, SPI) | NÃO IMPLEMENTADO | — |
| TV analógica | INVIÁVEL: o Siano Nova é só digital, e a transmissão analógica terrestre terminou no Brasil | — |
| IPTV / vídeos / gravação | NÃO IMPLEMENTADO (depende de benchmarks de decodificação) | — |
| AV-IN | NÃO IMPLEMENTADO (`Avin.exe` no backup) | — |
| Transmissor FM | NÃO IMPLEMENTADO (chip não identificado, `FMC.dll`) | — |
| Rádio FM/AM (recepção) | NÃO IDENTIFICADO: nenhum receptor conhecido; não haverá tela de recepção sem hardware | — |

## Ferramentas novas

- [tools/bigfoston/cedll.py](../../tools/bigfoston/cedll.py): desmonta funções de DLLs da ROM
  do CE achadas pelos textos de depuração (serve para TV, FM e microfone).
- [src/bt/btprobe.c](../../src/bt/btprobe.c): liga o chip Bluetooth como o CE e manda comandos
  HCI.
