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

## Falta

- **Sem o PC, o som ainda corta.** O próximo passo é o mesmo princípio no modo local: o som em
  thread própria, de prioridade mais alta, com o vídeo descendo a escada.
- **Gravações (`-p`) ainda são decodificadas no FOSTON.**
- **O app TV não mostra quando o PC está decodificando.** O dado já está no estado (`relay=1`).
