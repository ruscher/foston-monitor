# TV decodificada no PC (relay)

A mesma ideia do Big Remote Play: com o FOSTON ligado ao PC pelo USB, **o PC decodifica a TV** e o
FOSTON só desenha e toca. Sem o PC (no carro), o FOSTON decodifica sozinho, como antes.

## Por que

Medido no FOSTON em 26/11/2026, na Globo 1SEG (canal 21, SNR 17–19, **0 pacotes de TS perdidos**),
com o FOSTON decodificando:

- **10 a 12 quadros por segundo**, de 30;
- **95 cortes de som em 36 s**;
- CPU cheia.

A antena estava boa. O problema era o processador: o ARM11 sem FPU não decodifica o H.264 a 30
quadros por segundo junto com o HE-AAC. Além disso, o som e o vídeo saíam da mesma thread, e o som
esperava cada quadro.

> As medições anteriores ("~100 quadros em 5 s") estavam erradas. O script de teste roda com
> prioridade normal e o `bftvd` com -10, então o `sleep 5` acordava 8–9 s depois. Agora o script
> mede com o `/proc/uptime`.

## Como funciona

```
antena → FOSTON (bftvd) ──TS 340 kbit/s──► PC (bftv-relay)
                                             H.264 inteiro, 30 qps, com desblocagem
                                             HE-AAC → mono 32 kHz (swresample)
                                             Lanczos para a metade do retângulo do vídeo
FOSTON ◄──quadros yuvz + PCM com horário───┘
  amplia 2x, desenha na hora do relógio do som, toca
```

**Conexão**
- O `bftvd` escuta na porta 48122 da rede USB.
- O relay do PC tenta conectar a cada 1 s, como no Big Remote Play. Nenhuma porta é aberta no PC.
- O protocolo está em `src/tv/tvrelay.h`.

**Formato dos quadros** (`src/tv/yuvz.c`): é o do relay do Big Remote Play.
- YUV 4:2:0 com o brilho em 6 bits e a cor em 5, com histerese.
- Delta por pares de linhas e LZ4 HC.
- Com conteúdo de TV, 97 % das linhas mudam a cada quadro, então quase todo quadro vai inteiro:
  ~19–23 KB, 300–700 KB/s.

**Tamanho do quadro**
- Na janela (640×360), o PC manda 320×180.
- Na tela cheia (800×450), manda 400×225, que o FOSTON amplia 2×.
- O PC troca de tamanho quando o FOSTON avisa (`S`).

**Queda do PC**
- Se o PC sai no meio, o `bftvd` passa a decodificar no próprio FOSTON sem parar a TV.
- Testado: o relay foi morto no meio e a TV seguiu a ~17 qps.

## O que precisou de cuidado (cada item medido no aparelho)

| Problema | Medido | Correção |
|---|---|---|
| A recepção esperava vaga para o vídeo, e o som (que vem atrás, na mesma conexão) atrasava | 12 cortes numa troca para a tela cheia | A recepção nunca espera: com a fila cheia, o quadro novo é descartado. A fila cresceu para 128 quadros (4 s) |
| A thread que manda o TS ao PC ficou com a menor prioridade | 21 cortes | Ela é o começo da cadeia: nice -14 (gasta 2–3 %) |
| `malloc`/`free` por bloco de som e por quadro (135 KB, `mmap`/`munmap`): a thread do som esperava a trava do alocador atrás de threads de prioridade menor | O som secava com 0,3–0,5 s **na fila**, em pares a cada ~2 s | Anel fixo de amostras para o som. Cada vaga da fila de vídeo guarda o próprio buffer. Resultado: **0 cortes em 3 rodadas** e tela cheia estável a 30 qps |
| Pacotes de TS perdidos por interferência tiram trechos de áudio | Record (SNR 11–14): 130 pacotes perdidos e 21 cortes em 15 s | O PC preenche o buraco: repete o último trecho, sumindo em 60 ms, e depois põe silêncio |
| Desenho em 32 bits | janela 22–26 ms, tela cheia 35–45 ms por quadro | 4 pontos por escrita de 64 bits: janela 11–17 ms, tela cheia 22–29 ms |

**Prioridades** (nice): som e antena -15, envio do TS -14, recepção -13, desenho -12. No aperto,
quem cede é o vídeo: um quadro atrasado é descartado.

Na fila de som do PC há 300 ms de reserva antes de começar. O buffer do `/dev/dsp` tem 12 × 2 KB
(384 ms; conferido com `GETOSPACE`).

**Tentado e descartado**
- **Espaçar o envio dos quadros no PC:** piorou, com 9 e 20 cortes contra 2. As rajadas eram de
  só 2–7 quadros.
- **Redesenhar só as linhas que mudaram:** 97 % mudam.
- **Clamp aritmético (`USAT`) no lugar das tabelas:** ganho de 0,3 ms.
- **Disputa do barramento de memória entre o desenho e o DMA do som:** descartada; o teste
  isolado deu 1 corte em 20 s, com ou sem desenho.

## Resultados (FOSTON, 26/11/2026)

Medido pelo app TV, com o Modo Performance como no uso real, ou pelo `bftvd` direto com o GPS
pausado:

| Canal | Modo | Quadros/s | Cortes de som |
|---|---|---|---|
| Globo (SNR 17–19, 0 perdas) | FOSTON decodifica | 10–12 | 95 em 36 s |
| Globo | PC, janela | **30** | **0** |
| Globo | PC, tela cheia | **30** (3 rodadas) | **0** (3 rodadas) |
| Record (SNR 11–14, ~9 pacotes perdidos/s) | PC, app TV | 23–24 (o ffmpeg descarta quadros danificados) | **0** em 31 s; 1–2 s de som recomposto a cada 10 s |

O que a interferência levou não volta. Nos trechos perdidos o som baixa suavemente em vez de
picotar, e a imagem pode mostrar blocos onde faltou dado.

## Instalar

No PC, com ffmpeg e lz4 (com cabeçalhos):

```
tools/bigfoston/tv-relay-install.sh          # compila e liga o serviço do usuário (systemd)
tools/bigfoston/tv-relay-install.sh remove
journalctl --user -u bigfoston-tv-relay -f   # a cada 10 s: qps, KB/s, som recomposto
```

No cartão: `BigFoston/tv/bftvd` (`tools/bigfoston/build-tv.sh`). `bftvd.anterior` é a versão
anterior.

`BFTV_RELAY=0` desliga o relay no `bftvd` (para comparar). O estado em `/run/bigfoston-tv` ganhou
`relay=1` quando o PC está decodificando.

## Sem o PC: o som em thread própria (26/11/2026)

O modo local (no carro) também foi refeito. Antes uma thread só lia o TS, decodificava o vídeo e
o som; o som esperava cada quadro e secava. Agora são três:

- **Leitura**: só distribui os pacotes (nice -14) e nunca espera o vídeo.
- **Som**: decodifica o AAC e toca, com nice -15. Um buraco nos horários do áudio (pacotes
  perdidos no ar ou na gravação) vira silêncio pelo tempo que falta, para o relógio seguir o
  tempo real.
- **Vídeo**: decodifica com o que sobra. Se um pacote já está 1 s atrás do relógio do som, pula
  até o próximo quadro-chave (decodificar P sem as referências daria lixo).

A escada de decodificação mede o atraso pela fila de pacotes de vídeo.

Medido na Globo sem o PC (mesmo canal, GPS pausado como no app):

| | Quadros/s | Cortes de som |
|---|---|---|
| Antes (4 rodadas no total) | 11–14 | 0, 102, 62, 95 (depende de quanto o vídeo pesa no trecho) |
| Agora (5 rodadas) | 11–13 (o que o processador decodifica) | **0** em todas |

Gravação da Record com interferência (38 s, com 15 s de áudio perdidos no ar):

| | Duração | Cortes |
|---|---|---|
| Antes | 37 s | 167 |
| Agora, aqui | 41 s (tempo real) | **0** |
| Agora, pelo PC | 39 s (tempo real) | **0**, 730 quadros contra 327 |

## Gravações pelo PC

Com o PC conectado, a gravação (`-p`) também vai para ele. O envio tem duas fases:

- **Começo:** até 256 KB de uma vez, sem esperar. É o que o PC analisa antes de devolver o
  primeiro som (~200 KB).
- **Depois:** no ritmo do relógio, a no máximo 2× a taxa do arquivo. Enquanto há mais de 2 s de
  som devolvido na fila, o envio espera.
- **Sem som de volta** (gravação só com vídeo): o envio segue na taxa do arquivo, freado pela fila
  de quadros. Antes ele parava nos 256 KB e a gravação nunca terminava (o binário antigo ficou
  parado com 162 quadros; o novo vai até o fim).
- Sem o limite de taxa, o laço mandava o arquivo inteiro antes da primeira resposta: 16 s de som
  na fila e amostras perdidas, e a gravação tocava em 23 s em vez de 38.
- No fim, o som que sobra abaixo da reserva de 300 ms toca quando o arquivo já foi todo e o PC
  parou de mandar som há 300 ms. Antes ele ficava preso e a gravação não chegava ao fim.
- Se o PC sai no meio, a gravação recomeça aqui.

## Segurança e falhas do PC

- O `bftvd` só escuta no endereço da rede USB (192.168.7.2) e só aceita conexão vinda do PC
  (192.168.7.1). O FOSTON também entra em rede pelo Bluetooth (PAN do celular), e quem conecta
  recebe a TV e manda imagem para a tela. Testado: do endereço de rede local do PC, a conexão é
  fechada ("conexão recusada"); de 192.168.7.1, entra.
- **PC travado com a conexão aberta:**
  - a escrita do TS tem limite de 3 s;
  - se o PC recebeu mais de 256 KB de TS e não devolveu imagem nem som por 8 s, o FOSTON
    decodifica sozinho. Sem sinal na antena não chega TS, e isso não conta;
  - testado: relay congelado com `SIGSTOP`, o FOSTON assumiu em menos de 8 s; um PC falso que só
    lê, depois de 260 KB ("o PC parou de responder").
- Ao voltar para cá, o FOSTON segue a partir do ponto atual da antena. O TS que o PC não leu fica
  para trás; senão tocaria acelerado e o som ficaria segundos atrás. O demux local é aberto de
  novo a cada volta, sem os dados velhos de antes do PC.
- Sem sinal na antena, um PC que conecta também é atendido (antes a leitura esperava o próximo
  pacote da antena).

## Troca no meio

- O `bftvd` continua escutando enquanto decodifica aqui. Se o PC conecta depois, ele assume na
  hora.
- Isso resolve a troca de canal: o `bftvd` novo esperava só 1,5 s, e às vezes o relay ainda não
  tinha reconectado. Aquele canal ficava inteiro decodificado no FOSTON.
- Testado:
  - o PC chegando depois: passou a 28 qps, 0 cortes;
  - 4 trocas de canal seguidas: 4 pelo PC;
  - ida e volta na mesma TV (PC, PC sai, aqui, PC volta, PC): 0 cortes em todas as etapas.

## Na tela

O rodapé do app TV mostra "Decodificado no PC" quando o PC está decodificando (BigFoston OS
0.6.1). Captura do simulador: `46i-tv-pc`.

## Falta

- Testar a recuperação do som com interferência em outros canais e lugares (só a Record foi
  medida).
- Sem o PC, o vídeo continua limitado a ~12 qps: é o que o ARM11 decodifica.
- Gravação só com vídeo (sem som): sem o relógio do som, o vídeo anda no ritmo livre de 15 qps e
  a barra de posição fica parada. Toca até o fim (37 s de vídeo em ~50 s).

## Revisão do PR #11 (26/11/2026)

Os 19 apontamentos do CodeRabbit foram conferidos no código e corrigidos (branch
`fix/tv-revisao`). Além do que está acima:

- **Tela cheia com altura ímpar** (800×450 → 400×225 do PC): os laços 2× liam uma linha além do
  quadro e pintavam duas linhas fora do retângulo do vídeo. A última linha sai sozinha agora.
- **Horário do som** (`audio_pts`): um `double` são duas palavras no ARM1176; escrito pela thread
  do som e lido pelas outras, agora pela trava.
- **Primeira mensagem ao PC:** se a TV já estava em tela cheia, pedia o tamanho da janela.
- **Threads que não sobem:** a sessão para e decodifica aqui, em vez de travar.
- **Pacote de vídeo descartado** (fila cheia, modo local): do próximo quadro-chave em diante,
  com o decodificador limpo; antes, lixo na imagem até ele.
- **Fim de gravação local:** espera o mostrador esvaziar a fila (até 3 s).
- **yuvz:** sem memória para o delta ou o inteiro, a base fica inválida (pede outro inteiro); o
  codificador não perde os buffers num `realloc` que falha; e, se a mensagem não sai, o próximo
  é inteiro. O teste agora estraga a soma de um inteiro e exige a recusa.
- **PC (`bftv-relay`):** o som sem horário não é mais puxado para trás a cada bloco; o pipe é
  fechado antes de esperar a thread de leitura (o relay não reconectava se o FOSTON caía com o
  pipe cheio); e as alocações do demux são conferidas.

Medido no FOSTON, Globo, alternando o binário antigo e o novo (20 s cada):

| | Janela | Tela cheia | Cortes de som |
|---|---|---|---|
| Antigo | 29,7 e 26,8 qps | 17,7 e 28,1 qps | 3 |
| Novo | 31,1 e 29,9 qps | 27,1 e 27,7 qps | 0 |

O `shown` só conta quadros que mudaram (cena parada não conta), então uma janela sozinha varia
muito; só a alternância compara.
