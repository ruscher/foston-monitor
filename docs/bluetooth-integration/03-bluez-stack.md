# Pilha Bluetooth e ajustes do MT6611

`bfbt-attach` (liga o chip, 921600 baud, H4) → kernel `hci_uart` → `bluetoothd` (BlueZ 5.79) →
`bluealsa` (A2DP) e `bfbt-agent` (agente de pareamento e ponte para o shell). Detalhes do chip e
da UART: [hardware-enablement/06](../hardware-enablement/06-bluetooth-hardware.md) e
[07](../hardware-enablement/07-bluetooth-driver.md).

## Ajustes gravados a cada vez que o adaptador liga (`bfbt-agent`, HCI cru)

| Comando | Por quê | Medido |
|---|---|---|
| Write Inquiry Scan Activity (0x0C1E): janela 11,25 ms a cada 2,56 s | o chip sai do reset varrendo a visibilidade 2,40 s a cada 2,56 s | visível, A2DP 5,2 → 41,6 KB/s, 6 → 0 cortes em 15 s |
| Write Default Link Policy (0x080F) = 0x0004 (só sniff, **sem troca de papel**) | o celular pedia para virar mestre no meio da abertura do áudio e o chip perdia a resposta do AVDTP Discover | POCO X4: 0/5 → 5/5 conexões; reconexão no boot sem erro |

Diagnóstico da troca de papel (22/11/2026, `bfsnoop` + `btmon`): Create Connection, autenticação e
criptografia normais, canal PSM 25 aceito, `AVDTP Discover` enviado e confirmado pelo chip (Number
of Completed Packets), em seguida `Encryption Key Refresh` e `Role Change` (FOSTON → periférico) e
nenhuma resposta do celular; 6 s depois o BlueZ desiste ("Discover: Operation timed out"). Sem a
troca de papel o Discover é respondido em ~5 ms.

## Papel no enlace: celular mestre depois de conectar

Com o FOSTON mestre (efeito da política acima), o áudio chegava a 18,7 KB/s (59 % do tempo real:
música "acelerada" e com pausas). Captura: o celular mandava pacotes DH5 sem EDR (cada pacote de
áudio de 612 bytes em dois) e a qualidade do enlace caía de 255 para 8 com sinal forte (-50 dBm):
interferência, que o mestre evita pelo AFH; o MT6611 nem aceita o comando de leitura do mapa AFH.

O `bfbt-agent`, 2 s depois de os perfis estarem conectados (`ServicesResolved`), libera a troca de
papel nesse enlace (Write Link Policy 0x0005) e pede Switch Role para periférico, se ainda for
mestre (HCIGETCONNINFO). POCO X4: 5/5 conexões a 41,5-41,6 KB/s, 0 XRUN; 60 s seguidos sem buraco
nem corte; qualidade 255. A troca com a música já tocando não causou falha.

## UART

Recepção em lotes no `8250_mt3351` (256 bytes, silêncio na linha ou 2 ms) e controle de fluxo
automático (EFR): com música, CPU livre 23 % → 49 %, threads do kernel 33 % → 13 %
([../hardware-enablement/08](../hardware-enablement/08-bluetooth-profiles.md)).

## Desligar

`S42bfbt-agent stop` pede `Powered=false` ao BlueZ antes de tudo: cada aparelho é desconectado com
o motivo "desligado" e o celular fica sabendo na hora (o `S40bluetoothd` e o `S35bluetooth` param
depois).

## Tamanho

O firmware oficial (bibliotecas compartilhadas) tem zImage 2,10 MB + initrd 3,71 MB: sem
`bluetoothctl`/`readline`/`ncurses`, `hcitool`/`hciconfig`/`hcidump`, ferramentas do GLib, e com o
initrd em XZ com o filtro ARM. A `\ResidentFlash` (12 MB) não comporta duas versões inteiras: o
agente do CE 0.6.2 instala arquivo por arquivo ([24](24-recovery.md)).
