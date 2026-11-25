# Etapa 7 — Controle em tempo real e modo performance por app

## O atraso medido (24/11/2026, DualShock 4 com `hid-generic`)

O DualShock 4 de verdade respondia com atraso. Medi as camadas em separado, com o mesmo
relógio do aparelho: a captura HCI (`bfsnoop`, lida com `btmon -T`) e os eventos do
`/dev/input/event*` ao mesmo tempo.

| Camada | Medido |
|---|---|
| Pacote Bluetooth chega → evento no `/dev/input` (UART, BlueZ, `uhid`, `hid-generic`) | **~1 ms** (11 de 11 eventos entre 0,8 e 1,9 ms) |
| Relatórios que o controle manda | **81 em 14,3 s** (~6/s); intervalo mediano **130 ms**, máximo **951 ms** |
| Ritmo do enlace | intervalos em múltiplos de 1,25 e 5 ms, sem o padrão de sniff: é o controle que manda pouco |

O FOSTON não segurava os eventos: **o controle mandava devagar**. O DualShock 4 só transmite
em ritmo alto quando o computador lhe manda o relatório de saída 0x11 (controle de hardware,
luz e vibração, com CRC32), como faz o driver `hid-playstation`. Sem o driver, ninguém manda,
e o kernel não tem `hidraw` nem módulos para fazer isso de fora.

## Correção: `hid-playstation` no kernel

`firmware/buildroot/board/foston-fs707dc/linux.config`:

| Opção | Para quê |
|---|---|
| `CONFIG_HID_PLAYSTATION=y` | DS4 e DualSense: configura o controle e lê os relatórios |
| `CONFIG_PLAYSTATION_FF=y` (seleciona `INPUT_FF_MEMLESS`) | vibração |
| `CONFIG_NEW_LEDS`, `CONFIG_LEDS_CLASS`, `CONFIG_LEDS_CLASS_MULTICOLOR=y` | luz (dependência do driver) |

O zImage passou de 2 101 952 para **2 114 336 bytes** (+12 KB). A atualização ficou com
6,10 MB; a de 6,08 MB foi aplicada.

O que muda com o driver:
- **ritmo alto de relatórios:** o driver configura o controle;
- **mapa padrão do Linux:** o banco usa a linha `...cc09000000810000` (versão 0x8100, do
  driver da Sony) e, sem o app, o padrão do Linux também acerta;
- **dois dispositivos a mais:** "Motion Sensors" (sem teclas: o shell ignora) e "Touchpad"
  (`BTN_TOUCH`: o shell ignora).

O `find_touch` do `main.c` agora pula aparelhos Bluetooth e USB, para o touchpad do controle
nunca ser tomado pela tela de toque.

A correção do descritor no cache do BlueZ (etapa 3) continua inofensiva: o `hid-playstation`
lê os relatórios crus e não usa os campos do descritor.

**Como conferir depois de ligar com a versão nova**, com o controle conectado e mexendo nele:
```
ls /sys/bus/hid/drivers/          # tem "playstation"
/usr/sbin/bfsnoop /tmp/s.bin 5    # no PC: btmon -r s.bin | grep -c "> ACL"  -> centenas
```

## Depois do driver: o atraso ainda oscilava (24/11/2026, segunda rodada)

Com o `hid-playstation` ativo, o atraso continuou "bom às vezes, ruim às vezes". A captura
HCI mostrou três causas, todas no enlace Bluetooth:

| Causa | Medido | Correção (`bfbt-agent`) |
|---|---|---|
| O controle pede **modo sniff** quando fica parado; com o MT6611, o enlace em sniff entrega ~3 relatórios/s, com buracos de até 2 s. Exit Sniff resolvia só por ~2 s, até o controle pedir de novo | sniff: 3–6/s, intervalo de até 2348 ms; ativo: 33/s | por conexão de teclado/controle, a política do enlace sem sniff (HCI `0x080D` = 0) e Exit Sniff (`0x0804`) |
| Ativo, o **Tpoll** padrão (40 slots, 25 ms) limita os relatórios | ~38/s, um a cada 26 ms | **QoS Setup** (`0x0807`), serviço garantido, latência 1250 us (concedido): **~64/s, 99% em menos de 25 ms, pior caso 28 ms** |
| **Reconexão automática ao celular ausente** (3, 15 e 60 s depois de ligar o Bluetooth): o chip procura (page) até ~5 s e, nesse tempo, o controle fica **mudo** | 0 relatórios/s por 5 s seguidos | sem reconexão automática enquanto houver teclado ou controle conectado; o celular ainda pode conectar sozinho |

Tudo aplicado 1 s depois de cada conexão (`hid_link_tick`), com o aviso "teclado/controle:
enlace sem sniff e com latência baixa" no log. O teclado Bluetooth recebe o mesmo tratamento.
Custo: mais bateria no teclado e no controle.

Depois das três correções, 20 s seguidos sem nenhum buraco:
`62 63 62 64 67 63 77 75 68 67 64 63 65 66 63 64 72 66 63` relatórios por segundo.

Ainda congelam o controle por alguns segundos, porque o MT6611 não consegue manter o enlace
enquanto faz outra coisa:
- **Procurar** aparelhos na tela Bluetooth;
- tocar em **Conectar** num aparelho desligado (o chip tenta por ~5 s).

**Custo de CPU do controle transmitindo** (64 relatórios/s, `/proc/<pid>/stat` em 10 s): `bluetoothd`
8,4% (inclui o `hid-playstation`, que roda no contexto de quem escreve no `uhid`) e a fila do
HCI 1,2%. A UART do Bluetooth recebe ~450 interrupções/s (~13 bytes cada).

## Prioridade (CPU)

Com um controle conectado (`perfui.c`, `prio_update`):
- a **thread da interface** fica com nice −5: o cursor e as teclas do controle passam na frente
  de tarefas comuns;
- o **`bluetoothd`** fica com nice −10, reaplicado a cada 5 s, caso ele reinicie. É ele que lê o
  canal HID do Bluetooth e entrega ao kernel pelo `uhid`, então está no caminho de cada botão.

A fila de trabalho do HCI no kernel já é de alta prioridade (`WQ_HIGHPRI`).

**Memória:** o sistema roda inteiro da RAM (initramfs), sem swap, e sobram ~89 MB; nada do
shell nem do `bluetoothd` sai da memória, então `mlock` não mudaria nada. O risco que sobra
é o kernel matar um processo se a memória acabar: com um controle conectado, a interface e o
`bluetoothd` ficam com `oom_score_adj` −1000 e nunca são escolhidos.

## Modo performance por app

| Antes | Agora |
|---|---|
| liga com vídeo ou à mão | liga também com **um controle de jogo conectado** (ajuste "Ligar sozinho com um controle de jogo", ligado por padrão) |
| pausa tudo o que está marcado até desligar | pausa, mas **o app aberto religa o que usa** e, ao sair, pausa de novo depois de 3 s |
| janela de progresso a cada troca | sem a janela quando o motivo é o controle (ela segurava os toques bem quando a pessoa ia jogar); a etiqueta "Performance" aparece por 6 s e some |

| App aberto | Religa |
|---|---|
| Big GPS, GpsInfo | GPS (`bfgps`) |
| Telefone | telefone (`bfphone`) e Bluetooth |
| Bluetooth | Bluetooth |
| qualquer tela, com teclado ou controle Bluetooth conectado | Bluetooth (nunca pausa) |

As tarefas agendadas (`crond`) não voltam com app nenhum.

**Como funciona** (`core/perfmode.c`):
- **um alvo**: pausar os grupos marcados menos os que o app segura (`perfmode_hold`);
- **uma thread** acerta os serviços com o alvo: primeiro religa, na ordem inversa, o que o app
  está esperando; depois para, na ordem, o que deve parar;
- **mudança no meio**: se o alvo muda durante uma troca, a thread confere de novo ao terminar;
- **sem pausar logo depois de religar**: um serviço religado há menos de 4 s espera antes de
  parar de novo (voltar ao GPS logo depois de sair não faz o serviço ligar e desligar);
- o estado continua `PM_ON` enquanto o app segura algo, e as telas veem o serviço rodando.

## Testado no aparelho

| Passo | Resultado |
|---|---|
| shell novo com o DualShock conectado | modo ligou sozinho: `bfgps`, `crond` e `bfphone` parados; Bluetooth rodando; shell nice −5, `bluetoothd` nice −10 |
| abrir GpsInfo | `bfgps` de volta em menos de 1 s; a tela mostra "Procurando satélites" |
| voltar ao Início | `bfgps` parado de novo ~3–4 s depois; os outros continuam parados |
| abrir Telefone e voltar | `bfphone` volta e pausa de novo |
| Desativar | todos os serviços de volta; o registro `/run/bigfoston-perfmode` some |
| simulador (ASan), 150 capturas | sem erros; os testes de controle passam com o modo ligado pelo controle |

O ritmo do DualShock com o `hid-playstation` só pode ser medido depois de desligar e ligar o
FOSTON com a atualização. O reinício remoto pelo `kexec` não foi permitido nesta sessão.
