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
| `bluealsa-aplay --pcm=hw:0,0` | toca no alto-falante o que o celular manda |

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
Assim não dá para navegar e ouvir música por Bluetooth ao mesmo tempo. Próximo passo:
**recepção da UART por DMA** (o CE usa o canal GDMA 0x11 para esta UART; o driver
`8250_mt3351` hoje só o desliga), o que também deve resolver a conexão iniciada pelo celular
e os erros do início.

## Falta

- DMA da UART; depois medir CPU de novo.
- Agente de pareamento na interface (hoje o teste usa `bluetoothctl`), app Bluetooth
  (ligar/desligar, parear, conectar, esquecer).
- Saída de áudio do BigFoston para fones e caixas (A2DP fonte): o shell toca por OSS
  (mpg123), precisa de uma rota para o BlueALSA.
- Viva-voz (HFP/SCO) e microfone; HID (teclado, mouse, controle).
- Decidir se o firmware oficial passa para bibliotecas compartilhadas.
