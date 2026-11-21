# Estado da implementação — BigFoston OS no FOSTON FS-707DC

Atualizado em 20/11/2026. Cada linha diz o que foi **medido no aparelho** (não no
simulador), com a evidência e onde está o detalhe. Legenda:

- **FUNCIONANDO** — testado no hardware e em uso.
- **PARCIAL** — funciona com uma limitação conhecida, descrita na linha.
- **NÃO IMPLEMENTADO** — sem driver ou sem código; a interface diz isso.

## Resumo

O FOSTON liga no Windows CE, o agente `bfagent` (na memória interna, pelo
`Launch90`) conta 12 s e inicia o Linux gravado na memória interna. A tela de
abertura do kernel mostra o logotipo FOSTONOS; em 6,2 s a interface LVGL está na
tela, com toque, som, cartão SD com gravação, rede USB com SSH e relógio certo.
Ajustes, calibração e livros ficam no cartão SD.

Ficam de fora, por falta de driver: GPS, Bluetooth, TV, transmissor FM e
microfone. Parcial: reiniciar (o aparelho fica desligado).

## Boot e sistema

| Área | Estado | Evidência | Detalhe |
|---|---|---|---|
| Agente no Windows CE (`bfagent` 0.5.1) | **FUNCIONANDO** | abre sozinho no boot (`HKLM\init\Launch90`), arquivos conferidos por SHA-256 | [05-bootloader.md](05-bootloader.md) |
| Boot automático do Linux | **FUNCIONANDO** | `autoboot.conf` `delay=12`; "Ficar no Windows CE" cancela; o PC cancela ao falar com o agente | [05-bootloader.md](05-bootloader.md) |
| Atualizar o Linux pelo próprio Linux (`linux-update.py`, agente 0.6.0) | **FUNCIONANDO** | 21/11/2026: enviado pelo SSH ao cartão, aplicado pelo CE no boot (`update.log`: APLICADA), `dropbear` novo em execução com SHA-256 igual ao compilado | [05-bootloader.md](05-bootloader.md) |
| Linux na memória interna (`install-linux`, `boot-flash`) | **FUNCIONANDO** | zImage, initrd e DTB gravados em `\ResidentFlash\BigFoston\linux` e relidos com SHA-256 | [05-bootloader.md](05-bootloader.md), [17-recuperacao.md](17-recuperacao.md) |
| Tela de abertura do kernel | **FUNCIONANDO** | logotipo sobre degradê calculado pelo kernel, barra 5→100 % | [09-interface-ux-ui.md](09-interface-ux-ui.md) |
| Kernel 6.12 + initramfs XZ (BusyBox, musl estático) | **FUNCIONANDO** | interface em 6,2 s após o kernel; 5 MB de 122 MB usados | [04-linux-port.md](04-linux-port.md) |
| Desligar | **FUNCIONANDO** | watchdog como handler de POWER_OFF; tela "Desligando…" até apagar | [04-linux-port.md](04-linux-port.md) |
| Reiniciar | **PARCIAL** | o reset do watchdog deixa o aparelho **desligado** (o CE faz igual); liga no botão | [04-linux-port.md](04-linux-port.md) |
| Relógio | **PARCIAL** | sem driver de RTC no Linux; vem do relógio do CE (que guarda a hora desligado), do PC, do ajuste manual ou do NTP | [09-interface-ux-ui.md](09-interface-ux-ui.md) |
| Log do kernel após reset (ramoops) | **PARCIAL** | só sobrevive a reset com energia; desligar apaga a RAM (pstore vazio no boot seguinte) | [09-interface-ux-ui.md](09-interface-ux-ui.md) |

## Hardware

| Área | Estado | Evidência | Detalhe |
|---|---|---|---|
| Tela (framebuffer 800×480 RGB565) | **FUNCIONANDO** | `simplefb`, captura de `/dev/fb0` igual à interface | [04-linux-port.md](04-linux-port.md) |
| Brilho | **FUNCIONANDO** | `mt3351-backlight`, palavra da PMU `0x80061308` | [04-linux-port.md](04-linux-port.md) |
| Touchscreen | **FUNCIONANDO** | `mt3351-ts`, calibração de 5 pontos gravada no SD | [touchscreen-driver.md](touchscreen-driver.md) |
| Alto-falante (ALSA + OSS) | **FUNCIONANDO** | MP3 a ~7 % de CPU com espectro; sons de toque | [audio.md](audio.md) |
| Fone de ouvido | **FUNCIONANDO** | detecção no GPIO 12 e troca alto-falante/fone conferidas com um fone | [audio.md](audio.md) |
| Microfone | **NÃO IMPLEMENTADO** | o CE só grava no caminho de voz do DSP | [audio.md](audio.md) |
| Cartão SD | **FUNCIONANDO** | leitura conferida com leitor no PC; gravação liberada só no cartão com a marca do seu número de série; gravação sustentada corrigida (576 MB conferidos por MD5, zero erros); reparo automático com `fsck.fat` se o cartão não foi desmontado | [sd-controller.md](sd-controller.md) |
| Bateria e carregador | **FUNCIONANDO** | carga por software (algoritmo do CE) ligada por padrão; carregador de tomada: 16 min em corrente constante sem falhas (21/11/2026); USB do PC: recusa (VCHG cai abaixo de 4,35 V sob carga), com histerese; ícone com raio e verde na carga completa | [battery-charger.md](battery-charger.md) |
| USB (dispositivo): rede + serial + SSH | **FUNCIONANDO** (High-Speed) | `ssh root@192.168.7.2` (login 0,55 s; antes 5,9 s pela espera de DNS reverso); TCP 3,7 MB/s PC→FOSTON, 6,2 MB/s FOSTON→PC (21/11/2026) | [04-linux-port.md](04-linux-port.md) |
| Reconexão do cabo USB | **FUNCIONANDO** (em observação) | kernel anterior: rede parada após recolocar o cabo (2 de 2); kernel com a verificação da FIFO no reset do barramento: rede de volta em 4 de 4 trocas (ping do PC e log no SD). A verificação não precisou restaurar nada, então o mecanismo exato não foi isolado | [04-linux-port.md](04-linux-port.md) |
| USB ao ligar sem cabo | **FUNCIONANDO** | sem cabo no boot o CE deixa o bloco USB desligado; o glue o liga como o CE (conferido 21/11/2026) | [04-linux-port.md](04-linux-port.md) |
| USB High-Speed | **FUNCIONANDO** | `MUSB+0x70` como o CE e FIFO de 32 bits; `mt3351.hs=0` volta ao Full-Speed | [04-linux-port.md](04-linux-port.md) |
| UARTs | **FUNCIONANDO** (recepção) | `8250_mt3351`, ttyS0..3 | [uart.md](uart.md) |
| GPS (MT3328) | **FUNCIONANDO** | `gps.dll` do aparelho no Linux (`bfgps`), serviço no boot e tela GPS; posição real conferida ao ar livre (21/11/2026) | [gps.md](gps.md) |
| Bluetooth, TV (ISDB-T), transmissor FM | **NÃO IMPLEMENTADO** | sem driver | [01-hardware.md](01-hardware.md) |

## Interface (bigfoston-shell, LVGL 9.2.2)

| App / tela | Estado | Observação |
|---|---|---|
| Início: Grade, Painel, Quiosque | **FUNCIONANDO** | Painel a 2,2 % de CPU parado |
| Aparência: modo escuro/claro/automático, 6 temas, papel de parede, animações, sons, escurecer a tela | **FUNCIONANDO** | papel de parede pré-calculado (redesenhar é cópia) |
| Multimídia (MP3) | **FUNCIONANDO** | capa, espectro, fila, aleatório, repetir |
| Livros (TXT, EPUB, PDF, SVG) | **FUNCIONANDO** | paginação real, sumário, marcadores, busca |
| Arquivos | **FUNCIONANDO** | copiar, recortar, renomear, apagar e nova pasta no SD e na pasta de dados (sistema só leitura); visualizadores de texto, JPEG e SVG |
| Terminal | **FUNCIONANDO** | PTY + VT100, teclado na tela |
| Run, Monitor, Sistema de Informação | **FUNCIONANDO** | |
| Data: ajuste, fusos, horário de verão, NTP | **FUNCIONANDO** | NTP pelo PC (`sntp_server.py`) testado |
| Luz posterior, volume, bateria, língua (pt/en/es), calibração, reset, SSH (leitura) | **FUNCIONANDO** | |
| Big Screen (painel do PC) | **FUNCIONANDO** | FSP por TCP na rede USB, 36 quadros/s | 
| Avisos de dispositivos (fone, carregador, USB, SD) | **FUNCIONANDO** | |
| Galeria (JPEG, PNG, GIF, BMP, SVG) | **FUNCIONANDO** | miniaturas numa thread com cache no cartão; visualizador com zoom, ◀ ▶ e GIF animado | [apps.md](apps.md) |
| Controle de volume (barra superior e Multimídia) | **FUNCIONANDO** (simulador; ícone no aparelho) | barra vertical, mudo | [09-interface-ux-ui.md](09-interface-ux-ui.md) |
| GPS, GpsInfo, TV, Bluetooth, FM, USB | **NÃO IMPLEMENTADO** | telas dizem "Não disponível neste hardware ainda" e o que falta |

## Ferramentas no PC

| Ferramenta | Uso |
|---|---|
| `tools/bigfoston/bfagent.py` | fala com o agente: `install-linux`, `boot-flash`, `boot-mem`, `autoboot`, `install-agent`, `settime`, `ramoops`, arquivos, registro |
| `tools/bigfoston/linux-update.py` | atualiza o Linux da memória interna a partir do Linux em execução (cartão SD + agente ≥ 0.6.0) |
| `tools/bigfoston/sntp_server.py` | servidor de hora para o FOSTON pela rede USB |
| `tools/bigfoston/sd-write-test.sh` | teste de gravação do SD (roda no aparelho) |
| `tools/bigfoston/make_splash.py`, `src/ui/tools/make_logo.py` | imagens do logotipo |
| `tools/pc/bigfoston-ipaq.conf` | corrige o atraso de ~100 s da USB do CE no PC |

## Limitações que o usuário percebe

1. **Não deixar horas no Linux com a bateria baixa:** quem carrega é o CE.
2. **Reiniciar desliga o aparelho**; é preciso ligar no botão (a tela avisa).
3. **A hora do Linux vem do relógio do CE**; se ele perder a hora (bateria
   totalmente descarregada), acerte pelo PC (`bfagent.py settime`) ou em Ajustes > Data.
4. **Liberar a gravação do SD reinicia o cartão** e soma ~1,3 s ao boot.
5. **Atualizar o sistema na memória interna exige o CE:** desligue, ligue com o
   cabo e deixe o PC assumir na contagem (ou toque em "Ficar no Windows CE").

## Próximos passos (ordem recomendada)

| # | Item | Valor | Risco / esforço |
|---|---|---|---|
| 1 | GPS: mapa/navegação offline sobre a posição | navegação | alto |
| 2 | Bluetooth (UART2), TV, FM | — | alto: drivers novos |
