# Voz: Piper pt_BR-faber-medium, a única voz do Big GPS

## Decisão

O Big GPS fala **só** com a voz neural Piper **pt_BR-faber-medium**. As frases são **geradas no
PC** com o modelo Faber e tocadas no FOSTON. Se o pacote de voz não estiver no cartão, não há
voz (o botão de som some). Nunca entra outra voz no lugar, nem para completar uma frase.

## Por que a síntese não roda no aparelho (medido em 25/11/2026)

| Medida | Valor | Como |
|---|---|---|
| Custo da Faber | **2,8 bilhões de multiplicações-somas por segundo de fala** (92 % em `Conv`) | ONNX Runtime 1.32 no PC com profiling; as formas reais de cada nó dão as contas |
| FOSTON, float32 | **3,0 milhões/s** | `macbench` (laço de produto escalar, `-O2 -march=armv6`) no aparelho |
| FOSTON, int16 | 36,3 milhões/s | o mesmo laço em inteiros |
| FPU | ausente: `CONFIG_VFP` desligado no kernel, sem `vfp` no `/proc/cpuinfo` | |
| Swap | desligada (decisão em [docs/performance/06-swap-sd.md](../performance/06-swap-sd.md)) | `free`, `/proc/swaps` |

Contas:
- **Modelo como é (float32):** 2.800 / 3,0 ≈ **930 s de CPU por segundo de fala**. Uma frase de
  2,5 s ("Em 300 metros, vire à direita") levaria **~39 minutos** com o processador a 100 %.
- **Se o modelo fosse convertido para inteiros de 16 bits:** isso exigiria um motor de inferência
  próprio e perderia qualidade. Seriam 2.800 / 36 ≈ 77 s por segundo de fala, ~3 minutos por
  frase. Mesmo dobrando com instruções DSP (`SMLAD`), seria ~1,5 minuto por frase, com o mapa
  parado.
- **Swap ou RAM** não mudam essas contas: o limite é o cálculo, não a memória. Com swap no
  cartão ficaria ainda mais lento, porque os 63 MB de pesos seriam relidos do SD a cada frase.
- Se o MT3351 tiver VFP desligada no kernel (não confirmado), o ganho seria de algumas dezenas
  de vezes. Ainda assim seriam dezenas de segundos por segundo de fala.

Por isso, **inferência local: inviável**, agora por medição e não por estimativa. A voz é a
mesma Faber, gerada pelo mesmo modelo no PC.

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
| DF com nomes (14.148 vias × 2 formas) | 28.342 | 53.978 s (15 h) | 596 MB | 41 min (PC de 16 threads) |

Para o DF:
- a lista de nomes sai de `bgvoice-names centro-oeste-latest.osm.pbf nomes.txt --bbox=-48.29,-16.06,-47.30,-15.49`,
  que leva 2,6 s;
- no cartão: `BigFoston/big-gps/voz/faber-df.bgv` (SHA-256 conferido no PC e no aparelho);
- no FOSTON: `voz: Faber, 28342 frases`, e a navegação simulada falou "Em 200 metros, na
  rotatória, pegue a 1ª saída **para a DF-440**" inteira, sem trecho faltando.

O Brasil inteiro não cabe: seriam ~40 GB. A geração é por região.

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
