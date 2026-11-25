# Voz: Piper pt_BR-faber-medium, a única voz do Big GPS

## Decisão

O Big GPS fala **só** com a voz neural Piper **pt_BR-faber-medium**. As frases são **geradas no
PC** com o modelo Faber e tocadas no FOSTON. Se o pacote de voz não estiver no cartão, não há
voz (o botão de som some). Nunca entra outra voz no lugar, nem para completar uma frase.

## Por que a síntese não roda no aparelho

| | |
|---|---|
| Modelo | `pt_BR-faber-medium.onnx`, 63.201.294 bytes (VITS "medium", float32) |
| No PC (BigLinux) | 3,6 s de fala em 0,43 s de relógio e 1,5 s de CPU (`piper-tts`, AVX, vários núcleos) |
| FOSTON | ARM1176 a ~400 MHz, **sem FPU usada** (soft-float) e sem NEON; 118 MB de RAM, ~91 MB livres com o shell |

O modelo é todo em ponto flutuante de 32 bits. Sem FPU, cada multiplicação-soma vira chamadas
de software de dezenas de ciclos. Em ordem de grandeza, a mesma frase que leva 1,5 s de CPU num
núcleo x86 com AVX levaria **horas** no ARM1176. Além disso, só o modelo (63 MB) mais o
ONNX Runtime ocupariam quase toda a RAM livre ao lado do mapa. Não há ONNX Runtime
compilado para ARMv6 soft-float neste projeto. Compilar um só para confirmar o óbvio
custaria dias.

Por isso, **inferência local: não viável**. Isso não é uma medição no aparelho, e sim uma
estimativa pelos números acima. Uma quantização para int8 reduziria o tamanho, mas não
resolveria o tempo. A geração no PC continua usando o modelo original, sem perda.

## Como funciona

1. **No PC:** `tools/big-gps/make-faber-voice.py SAIDA.bgv [--nomes NOMES.txt]`
   - Gera com `piper-tts --json-input` (um processo para todas as frases):
     - as **frases fixas**: manobras, rotatórias da 1ª à 8ª saída, distâncias de 50 m a 30 km,
       "e depois", "Recalculando a rota", alertas, limites de 20 a 120 km/h;
     - os **nomes de vias**, opcionais: cada nome em duas formas, "na/no NOME" e "para a/o NOME".
       O gênero segue a mesma regra de `prep()` em `bgnav.c`.
   - `builder/bgvoice-pack` corta os silêncios (deixa 30 ms), normaliza o pico em −1 dBFS (ganho
     máximo 4×), aplica fades de 10 ms e codifica em **IMA ADPCM 4 bits, 22.050 Hz, mono**
     (10,8 KB por segundo de fala).
   - `builder/bgvoice-names IN.osm.pbf NOMES.txt --bbox ...` lista os nomes de vias de uma região
     (`name`, ou `ref` quando não há nome: a mesma regra de `bgr_way_name`).
2. **No cartão:** `BigFoston/big-gps/voz/*.bgv`. Se houver mais de um, vale o que tem mais frases.
3. **No aparelho** (`core/voice.c`):
   - A navegação pede a fala em trechos: `"Em 300 metros|Vire à direita|na Rua X"`. A tela
     continua mostrando a frase inteira.
   - Cada trecho é procurado no pacote, que é mapeado (`mmap`), com busca binária pela frase
     normalizada (`bgv_normalize`: a mesma regra do script).
   - Os trechos encontrados são decodificados e emendados com 70 ms de pausa. O trecho que
     falta (um nome de rua fora do pacote) é pulado, e a fala fica "Em 300 metros, vire à
     direita".
   - A fala começa na hora: não há síntese esperando.

## Formato de áudio (escolha)

| Formato | Tamanho por s | CPU no aparelho | Observação |
|---|---|---|---|
| PCM 16 bits 22,05 kHz | 43 KB | nenhuma | 4× maior |
| **IMA ADPCM 4 bits** | **10,8 KB** | desprezível (somas e deslocamentos) | SNR 44,8 dB num sinal sintético parecido com fala (`test_bgvoice`) |
| MP3/Opus | 2–4 KB | decodificador pesado no ARM1176, disputando CPU com o mapa | não compensa |

## Tamanhos medidos

| Pacote | Frases | Fala | Arquivo | Tempo no PC |
|---|---|---|---|---|
| Só frases fixas | 128 | 174 s | 1,9 MB | 9 s |
| DF com nomes (14.148 vias × 2 formas) | ver [30-benchmarks](30-benchmarks.md) | | | |

## O que saiu

- O eSpeak NG deixou de ser usado: era só o Big GPS que falava. Saíram o pacote do firmware
  (`BR2_PACKAGE_BIGFOSTON_ESPEAK_NG`, binário de 415 KB em `/usr/bin`) e
  `tools/big-gps/make-voice-data.sh`. Os dados antigos no cartão
  (`big-gps/voz/espeak-ng-data`, 5,1 MB) não são mais lidos. O `build-data.sh` novo apaga
  essa pasta quando gera os dados.
- **RHVoice nunca fez parte do projeto**: não havia o que remover.

## Licenças

- Modelo pt_BR-faber-medium: ver o `MODEL_CARD` em `/usr/share/piper-voices/pt/pt_BR/faber/medium`
  e [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices/tree/main/pt/pt_BR/faber/medium).
  O áudio gerado é distribuído no pacote de dados do cartão, não no firmware. Antes de
  publicar um pacote `.bgv`, confira os termos do dataset da voz Faber.
- Piper (programa): MIT.
- Os nomes das vias vêm do OpenStreetMap (ODbL).

## Verificado

- `tests/test_bgvoice.c` (ASan/UBSan):
  - ADPCM codifica e decodifica;
  - normalização de maiúsculas, acentos, espaços e pontuação;
  - o pacote real tem todas as frases que a navegação pede, com durações entre 0,8 e 1,2 s nas
    conferidas.
- No FOSTON (25/11/2026, navegação simulada):
  - `voz: Faber, 128 frases`;
  - as falas saíram em trechos;
  - "para a DF-440" foi pulado, porque o pacote só tinha as frases fixas;
  - CPU do shell em 23 % no instante medido.
- **Ainda não verificado:** se a voz se ouve bem no alto-falante, e a fala junto com a música
  do Bluetooth.
