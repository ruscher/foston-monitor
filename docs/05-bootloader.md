# 05 · Caminho de boot: Linux a partir do Windows CE, sem gravar a flash

## Decisão

O EBoot do fabricante (`JWD B.13D…101025`) e a NAND continuam **intocados**. O Linux
é carregado pelo agente `bfagent` a partir do cartão SD, à maneira do HaRET (que
nunca suportou o MT3351). Um **reset** devolve o Windows CE original.

Esta é a Estratégia B do projeto (mídia externa, sem alterar a flash). Gravar um
bootloader próprio na NAND só será considerado depois de existir backup do bootloader e
uma rota de recuperação comprovada.

## Como funciona (`src/wince/agent/boot.c`)

1. **Páginas com endereço físico conhecido.** Reserva > 2 MB (`MEM_RESERVE`), efetiva,
   trava; em modo kernel (`SetKMode`) traduz cada página percorrendo a tabela de páginas
   viva (TTB do CP15, lida pela janela sem cache `0xAC000000 + PA`; endereços abaixo de
   32 MB passam pelo FCSE: `MVA = VA | PID`). Só usa páginas com **PA ≥ 32 MB**, fora do vídeo.
2. **Arquivos** (kernel, DTB, initrd) lidos do cartão para essas páginas.
3. **Lista de cópia** `{destino, origem, tamanho}` encadeada entre páginas.
4. **Stub** (128 bytes, independente de posição) numa página própria, mapeada **VA = PA**
   com uma seção de 1 MB inserida na tabela de primeiro nível.
5. IRQ/FIQ desligados, D-cache limpo e invalidado, I-cache e TLB invalidados, salto para o stub.
6. O stub zera o FCSE PID, desliga MMU, caches e predição de desvio, copia as páginas e entra no kernel
   com `r0 = 0`, `r1 = 0xFFFFFFFF` (boot por device tree), `r2 = PA do DTB`.

### Layout de carga

| Região | PA | Máximo |
|---|---|---|
| kernel descomprimido | `0x00008000` | (o zImage se descomprime para cá) |
| zImage | `0x00800000` | 8 MB |
| initrd | `0x01000000` | 12 MB |
| DTB | `0x01F00000` | 64 KB |
| páginas do carregador | ≥ `0x02000000` | — |
| vídeo (não usar) | `0x03E00000–0x03FFFFFF` | framebuffer em `0x03E00030` |

## Validação no aparelho (19/11/2026)

| Passo | Resultado | Evidência |
|---|---|---|
| tradução VA→PA | **ok** | ensaio (`--dry-run`) e leitura física: o stub está no PA calculado byte a byte, parâmetros e lista corretos |
| salto e cópia | **ok** | payload `fbtest` (108 bytes, `src/baremetal/fbtest`) carregado em `0x8000` |
| execução sem Windows CE | **FUNCIONANDO** | faixas coloridas se movendo na tela (MMU e caches desligados, IRQs desligadas); confirmado visualmente pelo usuário |
| volta ao Windows CE | **ok** | reset → Windows CE normal, agente reabre |
| **Linux 6.12** (zImage + DTB + initrd) | **FUNCIONANDO** até `Run /init` (agente 0.2.4) | [foto](images/foto-linux-primeiro-boot.jpg); userland ainda não roda ([04-linux-port.md](04-linux-port.md)) |

### A armadilha do FCSE

O 1º boot do Linux (agente 0.2.3) não mostrou nada. Causa: o Windows CE deixa no CP15 c13
o **FCSE PID** do processo chamador (`0x1A000000`). O FCSE soma esse valor a todo endereço
abaixo de 32 MB, **inclusive com a MMU desligada**; a cópia para `0x8000`/`0x800000`
foi parar em outro lugar. O `fbtest` funcionou por acaso: escrita e execução foram
deslocadas igualmente e ele nunca liga a MMU. O kernel, ao ligar a MMU, calcula o
próprio endereço físico a partir do PC e salta para lixo. Correção: o stub zera c13
(PID e context ID) antes da cópia, enquanto executa acima de 32 MB.

Primeira versão do carregador (0.2.0) falhou no ensaio: o CE5 pôs o pool de páginas
dentro do slot do processo (janela do FCSE). Correção: reservar antes de efetivar e
traduzir pelo FCSE.

## Usando

```sh
# no PC, com o agente aberto no aparelho e o cabo conectado
tools/bigfoston/bfagent.py put zImage '\SDMMC\BigFoston\zImage'
tools/bigfoston/bfagent.py put fs707dc.dtb '\SDMMC\BigFoston\fs707dc.dtb'
tools/bigfoston/bfagent.py put initrd.img '\SDMMC\BigFoston\initrd.img'
tools/bigfoston/bfagent.py boot '\SDMMC\BigFoston\zImage' \
    --dtb '\SDMMC\BigFoston\fs707dc.dtb' --initrd '\SDMMC\BigFoston\initrd.img' --dry-run
tools/bigfoston/bfagent.py boot '\SDMMC\BigFoston\zImage' \
    --dtb '\SDMMC\BigFoston\fs707dc.dtb' --initrd '\SDMMC\BigFoston\initrd.img'
```

Payload bare-metal: `bfagent.py boot '\SDMMC\BigFoston\fbtest.bin' --kernel-pa 0x8000`.

## Riscos conhecidos

- Periféricos do CE com DMA continuam ativos depois do salto (LCD: desejável; USB, GPS,
  áudio: podem escrever na RAM). Se o kernel mostrar corrupção aleatória, os
  controladores precisam ser parados antes do salto (registradores em [01-hardware.md](01-hardware.md)).
- O watchdog (`ANWDOG.exe` é de software; existência de watchdog de hardware não verificada)
  pode reiniciar o aparelho durante o Linux.


## Boot a partir da memória interna (sem enviar o Linux pela USB)

`bfagent.py install-linux` grava `zImage` e `initrd.img` em `\ResidentFlash\BigFoston\linux\`.
Ele fecha o menu do CE durante a cópia, lê cada arquivo de volta para conferir o SHA-256
e grava um `SHA256SUMS`. Se a versão gravada já for a mesma, não envia nada.
Depois, `bfagent.py boot-flash` faz o boot com kernel e initrd lidos da memória interna;
só o DTB (3 KB) vai pela USB, carimbado com a hora do PC.

| Caminho | Medido em 2026-11-20 |
|---|---|
| `boot-mem` (2,9 MB pela USB, 25–60 KB/s) | ~2 min só de envio |
| `install-linux` (uma vez por versão) | 125 s |
| `boot-flash` | o comando volta em 1 s; o Linux começa ~9 s depois; a interface sobe 4,7 s após o início do kernel |

### PC: driver `ipaq` e o atraso de ~100 s na rede USB

Depois do salto, o PC às vezes leva ~100 s para enxergar o gadget do Linux. O FOSTON fica
"usb not attached" enquanto isso. A causa está no PC: o driver `ipaq` tenta abrir a porta
serial do CE até `connect_retries` vezes (padrão 100, 1 s cada) se algum programa abrir
`/dev/ttyUSB0` no instante do salto. Correção no PC (root):

```sh
sudo cp tools/pc/bigfoston-ipaq.conf /etc/modprobe.d/
echo 3 | sudo tee /sys/module/ipaq/parameters/connect_retries   # vale já, sem reiniciar
```
