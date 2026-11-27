# Limite de velocidade das vias

O Big GPS mostra o limite da via em que você está, quando o OpenStreetMap diz qual é.

## De onde vem

- Vem só da etiqueta `maxspeed` da própria via (e de `maxspeed:forward` / `maxspeed:backward`,
  quando o limite muda com o sentido).
- Só valores numéricos: "60", "60 km/h" e "40 mph" (convertido).
- O que não é número fica de fora, sem chute, e a via fica sem placa: "BR:urban", "signals",
  "none".
- O limite de um sentido vale quando a etiqueta do sentido existe, mesmo não numérica:
  `maxspeed=60` com `maxspeed:forward=signals` fica sem placa na ida (antes herdava o 60). No
  extrato de 21/11/2026 nenhum trecho mudou com isso.
- Nenhum limite é deduzido do tipo da via.

**Brasil (extrato de 21/11/2026):**
- 522.659 vias com limite;
- 5.913.511 trechos;
- 72 valores não numéricos ignorados;
- 118 MB;
- 30 s no PC.

## Arquivo

- `big-gps/rotas/NOME.bgk`, gerado por `tools/big-gps/builder/bgspeed-build` (no
  `build-data.sh`).
- Formato em `src/ui/biggps/bgspeed_format.h`: trechos de até 200 m, ordenados por x, com o limite
  em cada sentido da via.
- O arquivo é mapeado, não lido, como os alertas e as faixas. As rotas (`.bgr`) não mudaram.

## Como o limite é achado

`bgk_at` (`src/ui/biggps/bgspeed.c`):
- procura o trecho mais perto a até 8 m;
- o rumo tem de bater: até 35° vale o limite no sentido da via, a 145° ou mais vale o do sentido
  contrário;
- uma via que cruza (rumo diferente) não conta, e uma rua paralela a 15 m também não;
- o trecho mais perto no rumo vale mesmo quando não tem limite naquele sentido: a busca não
  pega o 60 de uma paralela a 6 m.
- A busca olha distância e rumo, não qual via é. Uma via **sem** `maxspeed` não entra no arquivo,
  então se ela corre a menos de 8 m de outra no mesmo rumo, com limite, a placa pode ser a da
  outra (veja Limitações).

**Navegando:** quando a rota fica pronta, cada trecho dela recebe o limite da via que percorre,
calculado no meio do trecho com o rumo do trecho.
- Isso roda em segundo plano (nice 10). No FOSTON, com o arquivo frio no cartão, foram 3,2 s; na
  thread da interface teriam sido 972 ms de tela parada.
- A placa aparece quando o resultado chega.

**Seguindo sem rota (GPS):** a cada posição, o limite da via no ponto, com o rumo do GPS. Também
em segundo plano, porque a busca lê do cartão.

## Na tela

- **Navegando:** placa redonda (borda vermelha, 56 px) na coluna da esquerda, abaixo da
  velocidade. A velocidade fica vermelha acima do limite. Captura `42p-biggps-limite-da-via`.
- **Seguindo:** a mesma placa no canto de baixo, acima da barra de status. O "95 km/h" da barra
  fica vermelho acima do limite.
- **Sem limite no mapa:** sem placa.
- A chave "Avisar radar e lombada" também desliga a placa.
- Não há bipe pelo limite da via. O bipe é só perto de radar ([15](15-alertas-radares.md)).

## Verificado

- `tests/test_bgspeed.c`:
  - sentido de ida e de volta com limites diferentes;
  - a via sem limite num sentido não pega o 60 da paralela a 6 m;
  - a via que cruza e a paralela a 15 m não contam;
  - um trecho sem limite;
  - o GPS 3 m ao lado da via;
  - arquivo cortado é recusado.
- Simulador, rota Brasília–Taguatinga: 417 de 422 trechos com limite (outra rota: 471 de 500). O
  começo dela, numa alça, não tem `maxspeed` no OSM e fica sem placa.
- No FOSTON (26/11/2026):
  - rota do bench com 371 de 389 trechos com limite, em segundo plano;
  - no modo seguir, com GPS falso a 95 km/h no Eixo Rodoviário Sul (trecho de 80 km/h, rumo 53°),
    a placa "80" apareceu.
- Ainda não testado dirigindo.

## Limitações

- O dado do OSM pode estar desatualizado ou faltar. A placa é informação do mapa, não a placa
  oficial da via.
- Sem `maxspeed` na via (alças e ruas pequenas, muitas vezes), em geral não há placa. A exceção:
  se outra via no mesmo rumo, com limite, passa a menos de 8 m (uma marginal colada na pista
  principal, por exemplo), a placa pode ser a dela. O arquivo não guarda as vias sem limite, e a
  busca não sabe em qual das duas o carro está.
- Seguindo sem rota, o resultado da busca só vale perto de onde foi feito (mesmo rumo, até ~3 s
  de caminho). Depois de virar, a placa some até a busca nova chegar, em vez de mostrar o limite
  da via anterior.

## Revisão do PR #12 (26/11/2026)

Os 10 apontamentos do CodeRabbit, conferidos e corrigidos:

- **Fechar o mapa com a busca lendo o cartão:** antes, a espera era de até 3 s e o arquivo era
  desmapeado mesmo com a thread lendo. Agora, se ela está lendo, é ela que fecha ao terminar. A
  tela não espera o cartão.
- **Rota nova sem memória:** a rota anterior é invalidada antes, e o resultado guarda quantos
  trechos tem. O resultado de uma rota não vira o de outra, nem é lido além do fim.
- **Fim da navegação:** a placa e a velocidade vermelha saem junto. Antes ficavam na tela sem GPS.
- **Seguindo:** o resultado da busca vale só perto de onde foi feito (acima).
- **Arquivo:** o tamanho é conferido em 64 bits. No FOSTON (32 bits), uma contagem enorme dava a
  volta e passava.
- **Construtor:** o limite de sentido não numérico não herda o geral, e um erro de gravação no
  meio não sai como sucesso.

