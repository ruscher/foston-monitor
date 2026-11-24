# Jogos, grupos de apps e pacotes .bfapp

Estado em 24/11/2026, testado no simulador e no FOSTON FS-707DC.

O BigFoston passou a instalar apps de fora do sistema. Cada app vem num pacote
`.bfapp`, cai sozinho no grupo certo da grade de Apps pelo `Categories=` do `.desktop` e
sai com um toque longo. O primeiro app é o **SC Controller**, no grupo **Jogos**. Ele testa,
mapeia e registra controles de jogo usando o banco de controles do
[sc-controller](https://github.com/ruscher/sc-controller).

| Etapa | Documento | O que entrega |
|---|---|---|
| 1 | [Grupos (categorias)](01-categorias.md) | pastas na grade de Apps; Jogos sempre presente |
| 2 | [Pacotes .bfapp](02-pacotes-bfapp.md) | formato, instalar/atualizar/desinstalar, segurança, `bfapp.py` |
| 3 | [Mapa de controles](03-mapa-de-controles.md) | SDL GameControllerDB no shell, ordem de busca dos mapas |
| 4 | [SC Controller](04-sc-controller.md) | o app: controles, testador ao vivo, registro de controle |
| 5 | [Criar um app](05-criar-um-app.md) | guia para os próximos apps (`.desktop`, ícone, cor, tela) |
| 6 | [Testes e medidas](06-testes.md) | testes de unidade, capturas, roteiro no aparelho |

## Em uma frase por peça

- `src/ui/core/desktop.c`: lê os `.desktop` (também `Categories`, `Icon`,
  `X-BigFoston-Screen`, `X-BigFoston-Color`) e diz o grupo de cada app.
- `src/ui/core/pkg.c`: instala e desinstala pacotes, sem LVGL, com testes de unidade.
- `src/ui/core/apps.c`: lista os apps instalados, desenha os ícones, abre os apps e mostra os
  diálogos de instalar e desinstalar.
- `src/ui/widgets/tile.c` (`tile_create_folder`): a pasta do grupo na grade.
- `src/ui/screens/scr_appgroup.c`: a tela do grupo.
- `src/ui/core/padmap.c`: o mapa de controles no formato do SDL, sem LVGL, com testes.
- `src/ui/core/gamepad.c`: aplica o mapa e transforma o controle em cursor e teclas.
- `src/ui/screens/scr_scc.c`: o app SC Controller.
- `apps/sc-controller/`: a pasta-fonte do pacote.
- `tools/bigfoston/bfapp.py`: monta, copia e instala pacotes pelo PC.

## Por que um SC Controller nativo

O pedido era o [sc-controller](https://github.com/ruscher/sc-controller), um driver e
mapeador em Python com interface GTK 4. No FS-707DC:

| O sc-controller precisa | O FOSTON tem |
|---|---|
| GTK 4 com Wayland ou X11 (a interface) | só framebuffer, sem servidor gráfico |
| Python ≥ 3.12, `evdev`, `libusb1`, `libudev` | nenhum Python; initrd já no limite da flash (~6 MB) |
| USB host para o receptor do Steam Controller, Steam Deck e DS4/DS5 por cabo | a porta USB só funciona como aparelho (gadget MUSB), sem `/sys/bus/usb` |
| Bluetooth LE para o Steam Controller sem fio | MT6611: Bluetooth 2.1 + EDR, sem LE |
| `hidraw` para DS4/DS5 sem fio | kernel sem `CONFIG_HIDRAW` |
| ponto flutuante para o mapeador | ARM1176 sem VFP (contas com vírgula emuladas) |

Dos drivers do sc-controller, só o genérico (`evdev`) funcionaria. Ele faria em Python, numa
CPU de 466 MHz sem FPU, o que o shell já faz em C. Por isso o app foi refeito em C/LVGL
dentro do shell, aproveitando o que o projeto tem de mais valioso para compatibilidade:
- **o banco de controles** (`gamecontrollerdb.txt`, SDL GameControllerDB): 746 modelos para
  Linux, entre eles DualShock 4, DualSense, Xbox, 8BitDo e Switch Pro;
- **o registro de controle** (o "Device registration"): apertar cada botão para ensinar o mapa
  de um controle desconhecido.

O pacote `sc-controller.bfapp` leva o banco e o `.desktop`. A tela vem no shell
(`X-BigFoston-Screen=sc-controller`).
