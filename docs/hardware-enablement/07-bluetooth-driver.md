# Bluetooth — driver e inicialização no Linux

Estado: **FUNCIONANDO NO HARDWARE até o kernel** (hci0 ligado, visível e conectável a
921600 baud, com o endereço do aparelho; 22/11/2026, firmware carregado por kexec). A pilha
BlueZ está em [08-bluetooth-profiles.md](08-bluetooth-profiles.md).

## Peças

| Peça | O que faz |
|---|---|
| `linux.config` | `CONFIG_BT`, BR/EDR, RFCOMM, BNEP, HIDP, `CONFIG_BT_HCIUART` + H4, `CONFIG_UHID` (+180 KB no zImage) |
| patch `0002-bluetooth-ext-features-pages` | o MT6611 anuncia a página 2 de "Read Local Extended Features" e a recusa com 0x30; o kernel abortava a inicialização. Páginas acima de 1 que falham agora são ignoradas (só têm recursos de versões mais novas do Bluetooth) |
| [src/bt/bfbt-attach.c](../../src/bt/bfbt-attach.c) | liga o chip (pinos do CE), HCI Reset, grava o endereço (0xFC1A), troca a velocidade (0xFC77 + byte 0xFF, espera 0xFCC0), anexa `/dev/ttyS1` ao kernel (`N_HCI`, H4) e fica segurando a porta; SIGTERM desanexa e corta a alimentação |
| [src/bt/bfhci.c](../../src/bt/bfhci.c) | estado do hci0, `up`/`down`, `scan on/off`, sem o BlueZ |
| [src/bt/btprobe.c](../../src/bt/btprobe.c) | comandos HCI crus para testes (`probe`, `cmd`, `scan`, `visible`) |
| `S35bluetooth` | liga no boot só com a marca `/data/bigfoston/bluetooth-ligado` (o CE também deixa desligado); guarda e restaura os pareamentos |

## Velocidade

O CE usa 3,2 Mbaud (modo de alta velocidade da UART da MediaTek, com DMA). O driver
`8250_mt3351` usa o modo 16550 (clock 29,25 MHz / 16 → até ~1,8 Mbaud, FIFO de 16 bytes,
interrupção por byte). Testado: **460800 e 921600** — o chip troca, o kernel inicializa e o
hci0 liga sem erros nem estouros da FIFO. 921600 (92 KB/s) cobre o A2DP (SBC ~40 KB/s).

Com RTS/CTS o driver liga o controle de fluxo automático do chip da UART (LCR = 0xBF, EFR
+0x08 bits 0x40 RTS e 0x80 CTS, como o `uart.dll`) e usa gatilho de 8 bytes na FIFO de
recepção: ~14 bytes por interrupção em vez de 1, sem erros de remontagem
([08](08-bluetooth-profiles.md)). Próximo passo, se precisar: DMA de recepção.

Códigos do comando 0xFC77 (desmontados do `Radio_ChangeControllerUART`): 9600 1, 19200 3,
38400 6, 57600 7, 115200 8, 230400 9, 460800 0xA, 921600 0xB, 3200000 0xC; segundo byte 0x40 =
controle de fluxo RTS/CTS. O chip responde "Command Status" na velocidade antiga; depois da
troca, o byte 0xFF o acorda e ele responde "Command Complete" do opcode 0xFCC0.

## Endereço Bluetooth

De fábrica todo MT6611 sai com `00:00:46:66:11:01`. O endereço que o Windows CE gerou para o
aparelho fica no registro (`Transports\BDADDR`, bytes `BD_ADDR_0..5`); ele foi copiado do
backup do próprio aparelho para `BigFoston/bt/endereco` no cartão (não vai para o git nem
para o firmware). Sem o arquivo, o `bfbt-attach` gera um endereço administrado localmente
(`02:…`) e o salva. Conferido: o chip passa a usar o endereço do CE, então os celulares que
já parearam com o FOSTON no CE o reconhecem.

## Ainda não usado

- O patch de firmware do `mtk_bthuart.dll` (`Radio_WritePatch`): o rádio já funciona sem ele
  (um celular viu o aparelho e pediu conexão). Fica como opção se aparecer instabilidade.
- O gerenciador de sono do chip (comandos de dormir/acordar): sem ele o chip fica sempre
  acordado enquanto ligado — consumo a medir.
- Viva-voz (SCO): o caminho PCM do chip até o codec ainda não foi estudado.
