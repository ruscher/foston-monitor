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

Ficam de fora, por falta de driver: GPS, Bluetooth, TV, transmissor FM, fone e
microfone, carga da bateria pelo Linux, USB High-Speed e reiniciar sem desligar.

## Boot e sistema

| Área | Estado | Evidência | Detalhe |
|---|---|---|---|
| Agente no Windows CE (`bfagent` 0.5.1) | **FUNCIONANDO** | abre sozinho no boot (`HKLM\init\Launch90`), arquivos conferidos por SHA-256 | [05-bootloader.md](05-bootloader.md) |
| Boot automático do Linux | **FUNCIONANDO** | `autoboot.conf` `delay=12`; "Ficar no Windows CE" cancela; o PC cancela ao falar com o agente | [05-bootloader.md](05-bootloader.md) |
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
| Fone de ouvido, microfone | **NÃO IMPLEMENTADO** | roteamento analógico do MT3351 não mapeado | [audio.md](audio.md) |
| Cartão SD | **FUNCIONANDO** | leitura conferida com leitor no PC; gravação liberada só no cartão com a marca do seu número de série, duas rodadas de teste com MD5 | [sd-controller.md](sd-controller.md) |
| Bateria e carregador | **PARCIAL** | leitura de tensão, %, carregador presente/carregando; **a carga é feita por software no CE** | [04-linux-port.md](04-linux-port.md) |
| USB (dispositivo): rede + serial + SSH | **FUNCIONANDO** (Full-Speed) | `ssh root@192.168.7.2`, ~190–240 KB/s | [04-linux-port.md](04-linux-port.md) |
| USB High-Speed, reconexão do cabo | **NÃO IMPLEMENTADO** | — | [04-linux-port.md](04-linux-port.md) |
| UARTs | **FUNCIONANDO** (recepção) | `8250_mt3351`, ttyS0..3 | [uart.md](uart.md) |
| GPS (MT3328) | **NÃO IMPLEMENTADO** | protocolo binário *host-based*; a posição sai da biblioteca proprietária do CE | [uart.md](uart.md) |
| Bluetooth, TV (ISDB-T), transmissor FM | **NÃO IMPLEMENTADO** | sem driver | [01-hardware.md](01-hardware.md) |

## Interface (bigfoston-shell, LVGL 9.2.2)

| App / tela | Estado | Observação |
|---|---|---|
| Início: Grade, Painel, Quiosque | **FUNCIONANDO** | Painel a 2,2 % de CPU parado |
| Aparência: modo escuro/claro/automático, 6 temas, papel de parede, animações, sons, escurecer a tela | **FUNCIONANDO** | papel de parede pré-calculado (redesenhar é cópia) |
| Multimídia (MP3) | **FUNCIONANDO** | capa, espectro, fila, aleatório, repetir |
| Livros (TXT, EPUB, PDF, SVG) | **FUNCIONANDO** | paginação real, sumário, marcadores, busca |
| Arquivos | **FUNCIONANDO** (só leitura) | visualizadores de texto, JPEG e SVG |
| Terminal | **FUNCIONANDO** | PTY + VT100, teclado na tela |
| Run, Monitor, Sistema de Informação | **FUNCIONANDO** | |
| Data: ajuste, fusos, horário de verão, NTP | **FUNCIONANDO** | NTP pelo PC (`sntp_server.py`) testado |
| Luz posterior, volume, bateria, língua (pt/en/es), calibração, reset, SSH (leitura) | **FUNCIONANDO** | |
| GPS, GpsInfo, TV, Bluetooth, FM, USB, Big Screen | **NÃO IMPLEMENTADO** | telas dizem "Não disponível neste hardware ainda" e o que falta |

## Ferramentas no PC

| Ferramenta | Uso |
|---|---|
| `tools/bigfoston/bfagent.py` | fala com o agente: `install-linux`, `boot-flash`, `boot-mem`, `autoboot`, `install-agent`, `settime`, `ramoops`, arquivos, registro |
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
| 1 | Carga da bateria no Linux | usar o Linux por horas | alto: controle do carregador na PMU, precisa mapear o que o CE faz antes de escrever qualquer registrador |
| 2 | Fone de ouvido e detecção do conector | áudio no carro | médio: rotas analógicas em `0x80061200…`, a partir do driver do CE |
| 3 | Companion / Big Screen pela rede USB | integração com o PC | baixo: só software (protocolo do Big Screen) |
| 4 | Arquivos com gravação, montar/ejetar o SD pela interface | uso do SD pelo usuário | baixo |
| 5 | USB High-Speed e reconexão | atualizações mais rápidas | médio |
| 6 | GPS via biblioteca do CE | navegação | alto: 4–6 semanas, camada de API do CE |
| 7 | Bluetooth (UART2), TV, FM | — | alto: drivers novos |
