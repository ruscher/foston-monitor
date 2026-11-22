# Voz da ligação: por que ainda fica no celular

Resumo: no FOSTON, a voz de uma ligação Bluetooth **não passa pelo Linux**. O chip Bluetooth
(MT6611) entrega o canal de voz (SCO) pelos seus pinos PCM ao DSP do MT3351, como no Windows
CE. Enquanto o Linux não programar esse caminho, o `bfphone` desfaz cada canal de voz assim que
ele abre, e o celular volta a voz para o próprio fone. Assim a ligação nunca fica muda.

## O que foi medido (22/11/2026)

Com uma ligação ativa, HFP conectado e o canal de voz aberto pelo celular:

| Medida (btsnoop pelo `bfsnoop`) | Resultado |
|---|---|
| Enlace SCO | aberto (Synchronous Connection Complete, status 0) |
| Pacotes de voz do FOSTON para o celular (silêncio pelo BlueALSA) | 3568 enviados, 3566 eventos "Data Buffer Overflow" |
| Pacotes de voz do celular para o FOSTON pelo HCI | **zero** |
| `AT+BCC` (pedir o canal de voz) | OK, sem novo SCO |

Sem pacote de voz chegando pelo HCI, o BlueALSA não tem o que tocar: a ligação fica muda dos
dois lados. Esse era o problema relatado ("ligação aberta e não sai som no FOSTON").

## O que o Windows CE faz (análise estática)

Tudo na cópia local da ROM (`backup/wince-referencia`, fora do git):

1. **Nenhum comando HCI de roteamento de voz.** O `mtk_bthuart.dll` só manda os comandos de
   fábrica da MediaTek (endereço FC1A, patch FCC4, UART FCD0/FC77/FCC0, sono FC7A, máscara de
   recursos FC1E). O `hciext.dll` só tem modos de teste de rádio (DUT, CW, TxRfTest, SingleHop).
2. **O app Bluetooth do CE** (`part2/app/BT`, RoyalTek/ANW "PhoneLink"): o `BTService.dll`, ao
   ver o SCO abrir ("Switch sound to Bluetooth"), abre o `WAV1:` e chama
   `DeviceIoControl(IOCTL_WAV_MESSAGE = 0x1D000C)` com a mensagem **0x66** e o parâmetro 1
   (0 ao terminar). Ele também lê a chave `MapAudioToPcmMode`.
3. **O `wavedev_drv.dll`** atende essa mensagem com `MM_GSMPCM_2WAY_START` (rotina em
   `0x03C0C5A8`): liga os clocks 42, 43 e 37 do áudio, marca o modo, e chama o motor de voz
   (`0x03C1A100`). O motor escreve palavras de 16 bits em duas janelas de memória
   compartilhada do DSP, por exemplo os endereços DSP `0x37` e `0x7F2C`. Os parâmetros do
   registro (`Drivers\BuiltIn\WavformDriver`: volume de voz e de microfone, supressão de eco,
   ruído de conforto, modo alto-falante, `PCM4WAY_DEBUG_INDEX`) vão para o mesmo DSP. Os textos
   `speech_via_BT`, `Input_Data_from_BT` e `Speech_Via_loudspeaker` confirmam o caminho
   MT6611 → PCM → DSP → alto-falante, e microfone → DSP → PCM → MT6611.

## Mapa de memória do DSP (do `wavedev_drv`, rotinas em `0x03C17444` e `0x03C166DC`)

| Físico | Tamanho mapeado | Uso provável |
|---|---|---|
| `0x8004F000` | 0x50 | controle do DSP |
| `0xA0000000` | 0x1A0 | caixa postal ARM↔DSP: o driver espera `0x5555`, escreve `0x6666`, passa 22 blocos de parâmetros (`0xA5xx`/`0x5Axx`), espera `0x1111` e responde `0x2222` (rotina `0x03C17F18`) |
| `0x80051000` | 4 KB | janela de dados |
| `0xA2000000`, `0xA2010000` | 64 KB cada | memória do DSP (bancos 0 e 1) |
| `0xA2100000`, `0xA2200000` | 32 KB cada | memória do DSP |
| `0x8004D000` | 0x15C | front-end de voz (ADC do microfone, DAC de voz); o mesmo bloco da rotina de teste de fábrica do microfone |
| `0x80001000` | 0x400 | clocks e energia (ids 37, 42, 43, 45 ligados para a voz) |

A caixa postal mostra que o DSP roda um programa próprio e conversa com o ARM: o Linux terá
de iniciá-lo (ou encontrá-lo já rodando depois do CE) e repetir esse diálogo.

## O que falta para a voz no FOSTON

1. Ler, só dentro dessas janelas e com os clocks do CE ligados, o estado do DSP depois do boot
   pelo CE (sem ligação aberta: um acesso errado já travou o aparelho uma vez).
2. Descobrir se o DSP precisa de código carregado pelo ARM (o `.text` do `wavedev_drv` tem
   313 KB, grande o bastante para conter imagens do DSP) e como ele é iniciado.
3. Reproduzir no driver ALSA `mt3351-audio` a sequência do `MM_GSMPCM_2WAY_START` (só
   registradores que o CE comprovadamente acessa), com a volta segura ao modo normal.
4. Testar com `voice foston` no `bfphone` (o SCO fica aberto) e uma ligação real.

Isso dá alto-falante, microfone e cancelamento de eco feitos pelo próprio DSP. É um trabalho
de engenharia reversa separado, com escrita em registradores do DSP: cada passo novo no
hardware será confirmado antes.

Alternativa estudada, mas não suficiente: SCO pelo HCI (o MT6611 da MediaTek não tem, no
firmware deste aparelho, o comando que troca a voz de PCM para HCI; nenhum dos DLLs o manda).

## Testes no aparelho (22/11/2026, ligação real com o POCO X4)

| Passo | Resultado |
|---|---|
| Estado do DSP no CE parado e no Linux (só leitura, `bfagent phys` e `bfvoice status`) | iguais: caixa `+0x00 = 0x5555`, controle `+0x00 = 5`, `+0x28 = 1`, `+0x48 = 0xBFFF`. O CE deixa o DSP carregado e ele continua assim no Linux, também depois do kexec. O PDN1 é igual nos dois (`0xFFFC78C6`): o bit 21 **não** é o clock do DSP |
| `bfvoice start`/`stop` sem ligação | o DSP confirma as trocas de modo (`+0xAA/+0xE0/+0xF6` vão a 2 e voltam a 0 no stop) |
| Canal de voz pedido pelo Linux | o kernel pedia **eSCO** (2-EV3): o chip aceita o comando e nunca completa. O CE manda `0xFC1E` (máscara LMP sem eSCO); o `bfbt-attach` agora também manda, e o canal SCO abre (o celular o abre sozinho ao reconectar) |
| Voz do DSP com SCO aberto | o DSP pede um bloco a cada 20 ms (caixa `+0x0A = 0x14`, bit `0x20` em `+0x10`); a ponte do `bfvoice run` (CE IST `0x3C0CEA4`: copia `Y[+0x9A*2]` para `+0x280` e responde `+0x0A = 0`) atende 50 blocos/s de 160 amostras, sem erro |
| Patch da ROM do MT6611 | o CE lê a versão (`0xFCD1 {04 00 00 80}` → `03 8B`) e manda 7388 bytes pelo `0xFCC4`; o `bfbt-attach` faz o mesmo (patch do backup do aparelho no cartão, `BigFoston/bt/mt6611-patch.bin`); aceito |
| **Som** | **não sai**: os blocos do DSP ficam em ruído de fundo (pico ~60 de 32767) com a outra pessoa falando e com o usuário falando; nenhum pacote SCO pelo HCI. A voz não chega ao DSP pelos pinos PCM |

| Tom de 1 kHz escrito pelo ARM no bloco de destino da ponte (`bfvoice tone`, sem ligação) | **o usuário ouviu no alto-falante**: DSP → DAC de voz → amplificador funciona. Logo o destino é o alto-falante e a origem (que fica em ruído) é a voz que deveria chegar do MT6611 pelos pinos PCM |
| Clocks (`bus.dll`, `0x3D231A0`) | ids 0–23: PDN0 bit id; 24–44: PDN1 bit id−24 (42 = bit 18, conferido); id 45 (DSP) = bit 2 de `0x80001204` (0 = ligado, está ligado no Linux) |

**Bloqueio atual:** a voz do celular não chega do MT6611 ao MT3351 pela interface PCM (DAI).
O app Bluetooth do CE não manda nenhum comando de fabricante para isso (só a API da pilha da
Microsoft); a OAL (`nk.exe`) e o `uart.dll` têm a tabela de nomes dos grupos de pinos
(CONB, CONA, JTAG, SPI, UT0–4, **DAI_CTL**, PWM, MC0/1, CAM0/1, NLD, EP, DPI), sem ligação
direta no código: falta achar onde a OAL liga os pinos do DAI ou se o MT6611 precisa de
outra configuração de PCM.

Por segurança, "Falar pelo FOSTON" só aparece com a marca `/data/bigfoston/voz-experimental`
(sem ela a ligação ficaria muda).

## Próximo passo

Medir no próprio Windows CE, com uma ligação real pelo app Bluetooth original (PhoneLink),
os registradores enquanto a voz funciona, e comparar com o Linux: pinos (função do DAI/PCM),
`0x8004D000`, `0x80061xxx`, a caixa postal e os blocos do DSP. O que diferir é o que falta.
