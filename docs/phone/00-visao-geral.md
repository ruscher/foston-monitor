# Telefone: ligações, contatos e mensagens pelo celular

O FOSTON controla as ligações do celular conectado por Bluetooth. O app **Telefone** fica no
dock do Foston Glass e na grade Apps (página 2). Pelo FOSTON dá para discar, atender, recusar,
desligar e mandar teclas (DTMF), e ver quem está ligando com o nome da agenda. A agenda, o
histórico e as mensagens podem ser importados do celular.

| Discador | Contatos | Recentes |
|---|---|---|
| ![Discador](../images/ui/45-telefone-discador.png) | ![Contatos](../images/ui/45b-telefone-contatos.png) | ![Recentes](../images/ui/45c-telefone-recentes.png) |

| Mensagens | Em ligação | Chamada recebida (em qualquer tela) |
|---|---|---|
| ![Mensagens](../images/ui/45d-telefone-mensagens.png) | ![Ligação](../images/ui/45e-telefone-ligacao.png) | ![Recebida](../images/ui/45f-telefone-chamada-recebida.png) |

| Volume da ligação | Teclado (DTMF) |
|---|---|
| ![Volume](../images/ui/45h-telefone-volume.png) | ![Teclado](../images/ui/45i-telefone-teclado.png) |

### Botões da ligação

| Botão | O que faz |
|---|---|
| Silenciar | desliga o microfone do FOSTON (ADC do microfone, `0x80061108`); com a voz no FOSTON |
| Teclado | teclas (DTMF, `AT+VTS`) |
| Volume | −/+ de 1 a 10 (ganho da voz no FOSTON: 100 % + 30 % por nível; 5 = 250 %); com a voz no celular manda `AT+VGS` |
| Nova chamada | abre o discador por cima da ligação ("Voltar à ligação" no canto); a nova chamada põe a atual em espera |
| Em espera | `AT+CHLD=2`: põe em espera / retoma |
| Gravar | grava as duas vozes (8 kHz, WAV) em `BigFoston/telefone/gravacoes/ligacao-AAAAMMDD-HHMMSS.wav`; aviso "Gravando no cartão" na tela; com a voz no FOSTON |
| Falar pelo Celular / FOSTON | onde fica a voz |
| Desligar / Atender | — |


(Capturas do simulador, com nomes e números inventados.)

## Estado (22/11/2026, POCO X4 Pro 5G)

| Função | Estado |
|---|---|
| Canal de chamadas (HFP 1.6, o FOSTON é o Hands-Free) | **funcionando**: conecta sozinho com o celular |
| Sinal, bateria, operadora do celular | **funcionando** (`+CIND`, `+COPS`) |
| Chamadas: estado, número, nome | **funcionando** com uma ligação real (`AT+CLCC`; nome pela agenda importada) |
| Discar, rediscar, atender, recusar/desligar, DTMF | implementado (`ATD`, `AT+BLDN`, `ATA`, `AT+CHUP`, `AT+VTS`) |
| Voz mantida no celular | **funcionando**: ao reconectar no meio da ligação o celular abriu o canal de voz duas vezes e o FOSTON desfez as duas |
| Escolha "Falar pelo: Celular / FOSTON" | no painel da ligação; FOSTON explica que falta o caminho do DSP (`voice_foston=0`) |
| Aviso de chamada recebida em qualquer tela | implementado (simulador); falta o teste real |
| Agenda (PBAP) | **funcionando**: 5363 contatos em ~1 min |
| Histórico de chamadas (PBAP) | **funcionando** |
| Mensagens SMS (MAP) | implementado; o POCO responde "serviço indisponível" (0xD3) até que o acesso às mensagens seja permitido no celular |
| **Voz da ligação no FOSTON** (alto-falante e microfone) | **funcionando** com os arquivos do aparelho no cartão ([02-audio-da-chamada.md](02-audio-da-chamada.md)) |

## Arquitetura

```
celular (AG) ──HFP/RFCOMM── BlueALSA (-p hfp-hf) ──RFCOMM1.Open (D-Bus)──┐
            ──PBAP/MAP (OBEX/RFCOMM)── bfpbap (filho do bfphone) ─────────┤
            ──SCO (voz)── HCI ── bfphone desfaz o canal de voz ───────────┤
                                                                     bfphone
                                       /run/bigfoston-phone (estado) ─────┤
                                       /run/bigfoston-phone.cmd (FIFO) ───┘
                                                                         │
                                         shell: core/phonestat.c, screens/scr_phone.c
```

| Peça | Papel |
|---|---|
| `src/bt/bfphone.c` | serviço (S43bfphone): canal AT, estado, comandos, sincronização, voz |
| `src/bt/bfpbap.c` | cliente OBEX próprio (sem o obexd, que exige C++ no Buildroot): PBAP (agenda, histórico) e MAP (lista de mensagens); acha o canal pelo SDP |
| `rootfs-overlay/etc/init.d/S41bluealsa` | BlueALSA com `-p hfp-hf` (antes só A2DP: o celular não abria o HFP) |
| `src/ui/core/phonestat.c` | leitura do estado, comandos e o aviso global de chamada recebida |
| `src/ui/screens/scr_phone.c` | o app: Discador, Contatos (busca), Recentes, Mensagens e o painel da ligação |

O BlueALSA repassa as respostas finais do celular com `:` na frente (`:OK`, `:ERROR`); o
`bfphone` as aceita assim (sem isso a lista de chamadas nunca era confirmada).

O `bfphone` consulta `AT+CLCC` a cada 5 s parado e a cada 1 s com chamada, e logo depois de
`RING`, `+CIEV` ou `+CLIP`. Indicadores e operadora são relidos a cada 30 s. Se o celular
conectar só o A2DP, o `bfphone` pede o HFP uma vez (`Device1.ConnectProfile`).

Comandos (`echo ... > /run/bigfoston-phone.cmd`): `dial NÚMERO`, `redial`, `answer`, `hangup`,
`dtmf C`, `voice phone|foston`, `sync`, `erase`. `voice foston` só vale quando o firmware tiver
a voz do DSP (`voice_foston=1`); `voice foston-teste` deixa o canal de voz aberto para
diagnóstico (a ligação fica muda).

## Privacidade

- A importação só acontece quando o usuário toca em **Importar do celular** e confirma. A
  primeira vez, o Android pede a autorização no próprio celular.
- Os dados ficam no cartão SD, em `BigFoston/telefone/<aparelho>/` (`contatos.tsv`,
  `historico.tsv`, `mensagens.tsv`, `enviadas.tsv`, `sincronizado`), criados só para o root
  (o cartão FAT, lido num PC, não guarda permissões). O botão da lixeira apaga tudo (nada muda no celular).
- Números, nomes e mensagens não vão para o syslog. O estado das chamadas fica só na RAM
  (`/run`).

## Como testar

1. Conecte o celular em Bluetooth (o celular precisa permitir "ligações" para o BigFoston).
2. Abra o Telefone: no cartão à direita aparecem o celular, o sinal e a bateria.
3. Ligue para o celular de outro aparelho: o aviso aparece em qualquer tela. Atender no FOSTON
   atende no celular; a conversa é pelo celular.
4. Discador: digite um número e toque em Ligar. O celular disca e a voz fica nele.
5. Contatos: Importar do celular, confirme no celular se ele pedir, e busque pelo nome ou
   número.
6. Mensagens: no celular, em Bluetooth > BigFoston, permita o acesso às mensagens, e importe
   de novo.

Diagnóstico (no terminal do FOSTON): `cat /run/bigfoston-phone` (tem números: não publicar),
`logread | grep bfphone`.
