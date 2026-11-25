# Big GPS — voz em português

> **Substituído no Big GPS 2.0:** a voz agora é só a Piper pt_BR-faber-medium, pré-gerada no PC
> ([docs/big-gps-2/22-piper-faber.md](../big-gps-2/22-piper-faber.md)). O eSpeak NG descrito abaixo saiu
> do firmware e não é mais lido do cartão.

Código: [src/ui/core/voice.c](../../src/ui/core/voice.c) (fala), agendamento em
[src/ui/screens/scr_map.c](../../src/ui/screens/scr_map.c) (`announce`), textos em
[src/ui/biggps/bgnav.c](../../src/ui/biggps/bgnav.c) (`bgn_text`), pacote
[firmware/buildroot/package/bigfoston-espeak-ng](../../firmware/buildroot/package/bigfoston-espeak-ng/),
dados [tools/big-gps/make-voice-data.sh](../../tools/big-gps/make-voice-data.sh).

## Escolha (medida no aparelho)

| Motor | Licença | Situação |
|---|---|---|
| **eSpeak NG 1.52.0** | GPL-3.0-or-later (código e dados) | **Escolhido.** C puro, síntese por formantes, pt-BR incluso. No aparelho: frase de 4,97 s ("Em 300 metros, vire à direita na Estrada Parque Indústrias Gráficas") sintetizada em **0,51 s** com cache quente (1,92 s com o cartão frio), **1,3 MB** de RAM; frase de 8,2 s em 0,75 s. |
| Piper (vozes neurais) | MIT (motor); cada voz com licença própria | Rede neural ONNX: sem FPU, inviável em tempo real. Não usado. |
| eSpeak (antigo, pacote do Buildroot) | GPL-3.0 | C++ e só biblioteca dinâmica: não serve ao rootfs estático. |
| Flite | BSD-like | Sem voz em português. |

A voz do eSpeak NG é robótica, mas inteligível e fala nomes de ruas arbitrários — o que
frases pré-gravadas não fazem. Uma voz melhor (frases fixas gravadas com licença adequada,
nomes pelo eSpeak) fica como melhoria futura.

## Como funciona

- O programa `espeak-ng` vai no sistema (`/usr/bin`, pacote `bigfoston-espeak-ng`: só o
  binário estático, ~0,5 MB no initrd); os dados do português ficam no cartão em
  `BigFoston/big-gps/voz/espeak-ng-data` (1,7 MB, gerados por `make-voice-data.sh`). Sem os
  dados, a voz fica desligada e o botão de som some do mapa.
- Um processo por aviso (`fork` + `execv`, o texto vai como argumento — nada passa por
  shell); o WAV sai pelo pipe e é tocado no `/dev/dsp` enquanto é sintetizado.
- Um aviso novo substitui o que ainda esperava a vez; sair do mapa interrompe a fala.
- Com o Multimídia tocando, o player solta o `/dev/dsp` durante a fala
  (`player_suspend`) e continua do mesmo ponto depois.
- Botão de som na coluna do mapa liga/desliga a voz (ajuste `nav_voice`, salvo).

## Quando fala

Por manobra, no máximo dois avisos:

| Aviso | Quando | Exemplo |
|---|---|---|
| Antecipado | a 300 m (cidade), 1 km (> 50 km/h), 2 km (> 80 km/h) | "Em 300 metros, vire à direita na Avenida Central" |
| Imediato | ~6 s antes (mínimo 30 m) | "Vire à direita na Avenida Central, e depois vire à esquerda na Rua X" |
| Início | ao iniciar | "Siga pelo Eixo Rodoviário" |
| Recálculo | nova rota após sair dela | "Recalculando rota…" |
| Chegada | a menos de ~30 m | "Você chegou ao destino" |

Conferido no aparelho com a rota simulada (`BIGGPS_DEMO=1`): "Siga pelo Eixo Rodoviário"
ao iniciar e "Em 2 quilômetros, pegue o acesso à direita" no Eixão (acima de 80 km/h), com
o `espeak-ng` rodando ~2 s por aviso. **Falta a confirmação auditiva no aparelho** e o teste
com o Multimídia tocando junto.

## Instalação dos dados

```sh
tools/big-gps/make-voice-data.sh /tmp/voz        # cria /tmp/voz/espeak-ng-data
# copiar a pasta espeak-ng-data para BigFoston/big-gps/voz/ no cartão
```
