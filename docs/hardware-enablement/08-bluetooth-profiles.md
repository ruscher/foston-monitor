# Bluetooth — BlueZ e áudio

Estado: **FUNCIONANDO NO HARDWARE (firmware de teste)** — 22/11/2026, com um POCO X6 Pro:
pareou (código confirmado no celular), o celular mostra "Conectado" e a música do celular
saiu no alto-falante do FOSTON (A2DP). Firmware de bibliotecas compartilhadas carregado por
kexec, ainda não instalado.

## Pilha

| Peça | Papel |
|---|---|
| `bfbt-attach -b 921600` (S35bluetooth) | liga o MT6611 e o entrega ao kernel ([07](07-bluetooth-driver.md)) |
| D-Bus (`S30dbus-daemon`) | barramento do BlueZ e do BlueALSA |
| BlueZ 5.79 `bluetoothd` (`S40bluetoothd`) | pareamento, SDP, perfis; `/etc/bluetooth/main.conf`: nome "BigFoston", classe áudio/alto-falante |
| BlueALSA (`S41bluealsa`) | A2DP fonte e destino (SBC) |
| `bluealsa-aplay --pcm=hw:0,0` (S41bluealsa) | toca no alto-falante o que o celular manda |
| [`bfbt-agent`](../../src/bt/bfbt-agent.c) (S42bfbt-agent) | agente de pareamento e ponte para o shell (abaixo) |
| app **Bluetooth** do shell ([scr_bluetooth.c](../../src/ui/screens/scr_bluetooth.c), [btstat.c](../../src/ui/core/btstat.c)) | liga/desliga, visível, procurar, aparelhos, música |

O BlueZ e o BlueALSA exigem bibliotecas compartilhadas (`dlopen`): o firmware de teste troca
`BR2_STATIC_LIBS` por `BR2_SHARED_LIBS` (todo o sistema). Medido: initrd 1,7 → 4,8 MB
(XZ), sistema na RAM 5,5 → 16,7 MB; com tudo rodando sobram 88 MB. Shell, GPS, cartão com
gravação e rede USB funcionaram iguais.

Os pareamentos ficam em `/var/lib/bluetooth` (RAM; o BlueZ usa ":" nos nomes, que o FAT32
não aceita) e são guardados em `/data/bigfoston/bluetooth-pareamentos.tar` ao parar o serviço
e restaurados no boot.

## Teste (22/11/2026)

| Passo | Resultado |
|---|---|
| Celular procura aparelhos | vê "BigFoston" |
| Pareamento (agente sem entrada/saída) | código mostrado no celular, aceito; FOSTON: Paired, Bonded, Trusted |
| Conexão iniciada pelo celular | **falhou** (A2DP "connection refused", AVDTP Discover timeout) |
| Conexão iniciada pelo FOSTON (`bluetoothctl connect`) | **conectou**; AVRCP mostra artista/álbum da música do celular |
| Música do celular no alto-falante do FOSTON | **funcionou** (confirmado pelo usuário) |
| Erros na UART | 4 "Frame reassembly failed" + 1 estouro da FIFO no início; depois 1,9 MB sem erro |

## Limitação principal: CPU

Com o áudio tocando: **CPU 92 %** (8 % livre) — BlueALSA (decodificação SBC sem FPU) 26 % e
**43 % no kernel**: a UART interrompe a cada byte (~48 mil interrupções/s a ~48 KB/s).
Assim não dá para navegar e ouvir música por Bluetooth ao mesmo tempo.

**Correção 1 (feita):** controle de fluxo automático da UART, como o `uart.dll` do CE
(LCR = 0xBF, EFR bits 0x40/0x80) e FIFO de recepção com gatilho de 8 bytes quando a porta
usa RTS/CTS (o GPS, sem RTS/CTS, continua com 1 byte). Medido no kernel novo: 1,2 MB
recebidos em 88 mil interrupções (**~14 bytes por interrupção** contra 1), nenhum erro de
remontagem; GPS na ttyS0 normal.

| Música do celular no FOSTON (A2DP, 10 s, `/proc/stat`) | Antes | Com controle de fluxo |
|---|---|---|
| Interrupções da ttyS1 | ~48 mil/s | **3.142/s** (43,5 KB/s, ~14 bytes cada) |
| Erros de remontagem | 4 no início | 0 |
| CPU livre | 8 % | **23 %** |
| `bluealsa` (SBC) | 26 % | 21 % |
| `bluealsa-aplay` | — | 9 % |
| Kernel fora dos processos (UART, tty, HCI) | ~43 % | ~28 % |

Melhorou, mas ainda é pesado para navegar com música ao mesmo tempo.

Se ainda faltar CPU, o passo seguinte é a **recepção por DMA** (o CE usa o canal GDMA 0x11
para esta UART; o driver `8250_mt3351` hoje só o desliga).

Controle da música (AVRCP): o FOSTON comanda o celular pelo D-Bus
(`org.bluez.MediaPlayer1`: `Next`, `Pause`, `Play`, `Previous`; propriedades `Status` e
`Track`). Testado: "próxima" trocou a faixa e "pausa" deixou o celular em `paused`; título e
artista chegam. Com `CONFIG_INPUT_UINPUT` o BlueZ também cria o dispositivo de entrada
"<celular> (AVRCP)", que só recebe teclas quando o outro lado controla o FOSTON (fones,
controles); os botões do próprio celular só param o envio do áudio.

Ligação do chip: a troca de velocidade falhava às vezes logo depois de ligar (o HCI Reset
lia só 7 bytes); o `bfbt-attach` agora espera 100 ms com o chip desligado, lê a resposta
inteira e repete a sequência até 3 vezes: 6/6 de primeira em religações seguidas.

## Agente e app

O shell é estático e não fala D-Bus; o `bfbt-agent` (libdbus, 0,9 MB de RAM) faz a ponte:

- **Agente "DisplayYesNo"**: o pedido de pareamento aparece em qualquer tela do shell, com o
  código em letras grandes para comparar com o celular; Aceitar marca o aparelho como
  confiável. Serviços de aparelhos pareados e confiáveis são autorizados sem perguntar.
  Aparelhos antigos que pedem digitar um PIN são recusados.
- **Estado** em `/run/bigfoston-bt` (`chave=valor`; `dev=endereço|pareado|conectado|confiável|rssi|ícone|nome`,
  `req=`, `player=estado|título|artista|aparelho`, `result=operação|ok/erro/andamento|erro`).
- **Comandos** no FIFO `/run/bigfoston-bt.cmd`: `on`, `off`, `visible 0|1`, `scan 0|1`,
  `pair|connect|disconnect|forget ENDEREÇO`, `accept`, `reject`, `play`, `pause`, `next`, `prev`.
- Chamadas longas (parear, conectar) são assíncronas: o agente continua respondendo durante
  o pareamento. Registra o agente de novo quando o `bluetoothd` reinicia.
- **Reconexão automática** aos pareados e confiáveis 3, 15 e 60 s depois de ligar (antes do
  BlueALSA registrar o áudio a conexão falha com "profile-unavailable"); um comando do
  usuário cancela as tentativas restantes.

Testado no aparelho pelo FIFO: reconexão automática, desconectar/conectar, visível,
procurar, título e artista da música. A tela foi conferida no simulador (capturas 43 e 43b);
o teste no aparelho com o dedo está pendente.

## Falta

- DMA de recepção da UART (o kernel ainda gasta ~28 % só recebendo o áudio).
- Conexão iniciada pelo celular (hoje só a iniciada pelo FOSTON funciona).
- Teste do app no aparelho (pareamento novo pela tela, música pelos botões).
- Saída de áudio do BigFoston para fones e caixas (A2DP fonte): o shell toca por OSS
  (mpg123), precisa de uma rota para o BlueALSA.
- Viva-voz (HFP/SCO) e microfone; HID (teclado, mouse, controle).
- Decidir se o firmware oficial passa para bibliotecas compartilhadas.
