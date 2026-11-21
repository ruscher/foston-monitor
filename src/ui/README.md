# bigfoston-shell — interface gráfica do BigFoston OS

Shell gráfico em C (C11, sem C++) sobre **LVGL 9.2.2** para o FOSTON FS-707DC
(MT3351, ARM1176JZ-S ARMv6 **sem VFP**, 128 MB, tela 800×480 RGB565, toque
resistivo de ponto único). Design system, telas e regras de honestidade:
[docs/09-interface-ux-ui.md](../../docs/09-interface-ux-ui.md).

## Compilar

```sh
cd src/ui
make sim                    # simulador de PC (SDL2): build/sim/bigfoston-shell
make run                    # abre a janela 800x480 (mouse = toque, teclado físico funciona no Run)
make screenshots            # gera docs/images/ui/*.png (headless, sem janela)
make target CROSS_COMPILE=arm-buildroot-linux-musleabi-   # aparelho: build/target/
make target-hostcheck       # compila o caminho do alvo (fbdev+evdev) com o gcc do PC
make headless               # simulador sem SDL2 (só --screenshot)
make clean
```

| Variante | Vídeo | Entrada | Flags |
|---|---|---|---|
| `sim` | SDL2 (`lv_sdl_window`) | mouse/teclado SDL | `-O2 -g -DBIGFOSTON_SIM_SDL` |
| `headless` | display LVGL em memória | — | só `--screenshot` |
| `target` | `/dev/fb0` (`lv_linux_fbdev`) | evdev (`lv_evdev`) | `-Os -march=armv6 -mfloat-abi=soft -marm -DBIGFOSTON_TARGET`, `--gc-sections` |
| `target-host` | igual ao `target` | igual | sem flags ARM (validação no PC) |

Sem toolchain cruzada ainda, `make target-hostcheck` garante que o código do alvo
(fbdev, evdev, autodetecção do toque, leitura de sysfs) compila limpo.
`ARCH_FLAGS` pode ser sobrescrito (`make target ARCH_FLAGS="-march=armv6 -mfloat-abi=soft"`).
`SCREENSHOT=1` inclui o modo `--screenshot` (e o lodepng) também no alvo.

Requisitos do PC: gcc, make, `pkg-config sdl2` (só para `sim`). Python 3 + Pillow
e Node (npx) só para **regenerar** ícones/fontes — os `.c` gerados já estão no repositório.

## Executar

```
bigfoston-shell [--screenshot DIR] [--real-status|--sim-status]
                [--lang pt|en|es] [--theme dark|light] [--data DIR]
```

- `--screenshot DIR`: renderiza 36 telas (inclusive tema claro, inglês e espanhol,
  diálogos, teclado, saída de comando) em PNG 800×480 RGB565 e sai. Não precisa de display.
- `--real-status`: no simulador, troca os valores simulados da barra superior pela
  leitura real de sysfs/proc do PC. No alvo, o status é sempre real.

### Variáveis de ambiente (integração com o firmware)

| Variável | Padrão | Uso |
|---|---|---|
| `BIGFOSTON_DATA` | `/data/bigfoston` (alvo), `./data` (sim) | `settings.conf`, `history.txt`, `favorites.txt`, `pointercal`, `lang/<código>.txt` |
| `BIGFOSTON_FBDEV` | `/dev/fb0` | framebuffer |
| `BIGFOSTON_TOUCH` | autodetecção | evdev do toque; sem ela, o 1º `/dev/input/event*` com `ABS_X`+`ABS_Y`+`BTN_TOUCH` |
| `BIGFOSTON_APPS` | `$DATA/apps:/usr/share/bigfoston/apps` | pastas de `.desktop` do Run |
| `BIGFOSTON_RUN_USER` | `bigfoston` | usuário sem privilégios usado pelo Run quando o shell roda como root |
| `BIGFOSTON_ALLOW_ROOT` | (vazio) | `1` permite o Run executar como root (explicitamente) |
| `BIGFOSTON_POWEROFF_CMD` / `BIGFOSTON_REBOOT_CMD` | `/sbin/poweroff` / `/sbin/reboot` | botão Desligar/Reiniciar |

### Arquivos lidos no alvo

`/sys/class/power_supply/*` (bateria, alimentação), `/sys/class/backlight/*`
(brilho; escrita em `brightness`), `/proc/asound/cards` + `amixer` (volume),
`/sys/class/udc/*/state` (USB), `/sys/class/rfkill`, `/sys/class/bluetooth`,
`getifaddrs` (rede, IP, MAC), `/proc/{stat,meminfo,loadavg,uptime,cpuinfo}`,
`/proc/net/tcp{,6}` (SSH na porta 22), `/etc/os-release`, `statvfs($DATA)`.
O que não existir aparece como "—" ou "Não disponível neste hardware ainda".

## Estrutura

```
src/ui/
  main.c                 drivers (SDL / fbdev+evdev), argumentos, laço principal
  lv_conf.h              GERADO por tools/make_lv_conf.py (RGB565, malloc da libc, buffers parciais)
  core/                  app (montagem), nav (pilha), status (+ _linux/_sim), settings,
                         i18n (+ i18n_strings.def), theme, sysinfo, runner (fork/exec),
                         desktop (.desktop, histórico/favoritos), touch_calib, screenshot
  widgets/               topbar, bottombar, tile, dialog, page (blocos de página)
  screens/               uma tela por arquivo (scr_*.c)
  assets/fonts/          Montserrat 14/20/28 + DejaVu Sans Mono 16 com Latin-1 (gerado)
  assets/icons/          ícones A8 (gerado por tools/make_icons.py)
  data/apps/             exemplos de .desktop para o simulador
  tools/                 make_fonts.sh, fix_fonts_v9.py, make_icons.py, make_lv_conf.py, fonts/
  third_party/lvgl/      LVGL v9.2.2 (src + LICENCE.txt; ver VERSION)
  third_party/lodepng/   lodepng (só no modo --screenshot)
```

## Regenerar assets

```sh
tools/make_fonts.sh          # precisa de node/npx (lv_font_conv 1.5.2)
python3 tools/make_icons.py  # precisa de Pillow
python3 tools/make_lv_conf.py
```

## Medições (PC x86-64, 19/11/2026)

- Binário do alvo (`target-hostcheck`, `-Os`, gc-sections, stripped): **≈ 583 KB**
  (fontes ≈ 145 KB, ícones ≈ 47 KB). Em ARM deve ficar na mesma ordem.
- RSS de pico do processo headless renderizando as 36 telas: **≈ 7,1 MB**
  (inclui um quadro 800×480 inteiro + buffers do PNG).
- Simulador SDL (driver offscreen) parado: RSS ≈ 71 MB (quase tudo SDL/GL), CPU ≈ 0,5 %
  (o driver SDL do LVGL consulta eventos a cada 5 ms; no alvo o evdev é lido a cada 30 ms).
- Em repouso só existem: o timer de 1 s da barra superior, o de leitura do toque e o de
  refresh do LVGL (que não desenha nada se nada mudou). O Monitor cria um timer de 2 s
  só enquanto está aberto; o Run, um de 100 ms só enquanto há processo rodando.
