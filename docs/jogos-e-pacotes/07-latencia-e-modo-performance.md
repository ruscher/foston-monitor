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

## Prioridade (CPU)

Com um controle conectado (`perfui.c`, `prio_update`):
- a **thread da interface** fica com nice −5: o cursor e as teclas do controle passam na frente
  de tarefas comuns;
- o **`bluetoothd`** fica com nice −10, reaplicado a cada 5 s, caso ele reinicie. É ele que lê o
  canal HID do Bluetooth e entrega ao kernel pelo `uhid`, então está no caminho de cada botão.

A fila de trabalho do HCI no kernel já é de alta prioridade (`WQ_HIGHPRI`). Memória: o
FOSTON não tem swap e sobram ~89 MB, então nada do shell sai da RAM. Travar páginas com
`mlock` não mudaria nada.

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
