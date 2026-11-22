# Big GPS — roteamento

Código: construtor [tools/big-gps/builder/bgroute-build.cpp](../../tools/big-gps/builder/bgroute-build.cpp)
(PC, RoutingKit), formato [src/ui/biggps/bgroute_format.h](../../src/ui/biggps/bgroute_format.h),
consulta [src/ui/biggps/bgroute.c](../../src/ui/biggps/bgroute.c) (aparelho, C),
ferramenta [src/ui/biggps/bgroute-cli.c](../../src/ui/biggps/bgroute-cli.c).

## Modelo

- **Grafo da via** (RoutingKit): nós de roteamento (cruzamentos e pontas), um arco por
  sentido de circulação, geometria completa entre os nós, nome/ref/classe da via.
- **Perfis:** carro (regras de carro do RoutingKit: acesso, sentido único, velocidades por
  tipo de via e `maxspeed`), moto (as mesmas regras, respeitando `motorcycle=*`), bicicleta
  (regras de bicicleta do RoutingKit; 16 km/h em vias comuns, menos em trilhas, calçadas e
  escadas) e a pé (5 km/h, 2 km/h em escadas, sem sentido único).
- **Grafo expandido por conversões:** cada arco vira um nó; uma aresta a→b existe quando a
  conversão é permitida — proibições do OSM (`restriction=*`, carro e moto), sem retorno no
  meio da via exceto em rua sem saída (a pé, sempre permitido). Peso = custo da conversão +
  tempo de b. Custos (carro/moto): esquerda 8 s (13 s se fechada), direita 3 s (8 s se
  fechada), retorno 30 s; bicicleta: esquerda 3 s, direita 1 s, retorno 10 s.
- **Bicicleta e a pé sem expansão** (modo por nós, `BGR_F_NODE`): sem proibições nem custo
  de conversão, a CH vai direto sobre os nós e arcos da via — no DF bicicleta 47,2 → 30,5 MB
  e a pé 55,1 → 31,3 MB, construção em 20 s em vez de 49 s; 0 diferenças contra Dijkstra.
  A origem entra pela ponta do arco (com o resto dele) e o destino pela origem do arco (mais
  a fração percorrida).
- **Contraction Hierarchies** (RoutingKit) sobre o grafo expandido; no aparelho, Dijkstra
  bidirecional só "para cima", com tabela hash dos nós visitados (a memória acompanha a
  busca, não o tamanho do grafo) e desempacotamento dos atalhos com pilha explícita.
- **Encaixe na via:** grade de ~300 m com os arcos de cada célula (só do maior componente
  fortemente conexo); projeção no segmento mais próximo; o rumo do GPS penaliza a via no
  sentido contrário.
- **Formato 2 (compacto):** cada nó da CH guarda seus arcos num bloco de varints (destino
  em delta, peso, e o id da aresta expandida — ou, num atalho, só o nó do meio, desempacotado
  procurando os dois arcos no bloco dele, como numa CH clássica). Grade de encaixe esparsa
  (só as células com vias, busca binária). DF: carro 58,3 → **37,4 MB**, a pé 92,6 →
  **55,1 MB**; leituras por rota 318 → 219. Moto sem arquivo próprio usa o de carro.
- **Localidade no SD:** nós numerados pela curva de Hilbert e arcos na ordem da origem; ids
  da CH na ordem espacial para a parte baixa da hierarquia e o topo (2 %) junto, em ordem
  de nível. Leitura com `pread` por páginas de 8 KB e cache de 8 MB (funciona com arquivos
  maiores que o espaço de endereçamento de 32 bits).

## Conferência independente

`bgroute-build --tests` grava 300 consultas aleatórias com a distância calculada por um
Dijkstra comum sobre o grafo expandido; `bgroute -c` refaz cada uma pela CH:

| Perfil (DF) | Resultado no PC |
|---|---|
| carro | 300 consultas, **0 diferentes** |
| moto | 300 consultas, **0 diferentes** |
| bicicleta | 300 consultas, **0 diferentes** |
| a pé | 300 consultas, **0 diferentes** |

No aparelho, a mesma conferência do carro: **300 consultas, 0 diferentes** (formatos 1 e 2).

## Medições (DF, 2026-11-21)

| Perfil | Nós | Arcos | Arestas expandidas | CH (ida + volta) | Arquivo | Construção (PC) |
|---|---|---|---|---|---|---|
| carro | 155 622 | 357 074 | 664 356 | 967 299 + 966 961 | 37,4 MB (formato 2) | 30 s |
| moto | 155 643 | 357 100 | 664 393 | 968 061 + 967 235 | 37,5 MB | 32 s |
| bicicleta (modo por nós) | 183 187 | 444 023 | — | — | 30,5 MB | 20 s |
| a pé (modo por nós) | 179 208 | 478 342 | — | — | 31,3 MB | 20 s |

Rota de teste Rodoviária do Plano Piloto → Taguatinga Centro: carro 23,11 km / 21 min 24 s
(N1, S1, EPIG, EPTG…), bicicleta 22,65 km / 89 min, a pé 21,77 km / 262 min.

No aparelho (carro, cache do sistema limpo antes de cada rota — o pior caso, primeira rota):

| Rota | Encaixe | Busca + desempacotamento | Nós visitados | Leituras de 8 KB |
|---|---|---|---|---|
| Rodoviária → Taguatinga (23 km) | 427 ms | 975 ms | 324 | 318 |
| Planaltina → Gama (73 km) | 338 ms | 1109 ms | 498 | 355 |
| Asa Sul → Guará (12,5 km) | 280 ms | 685 ms | 244 | 198 |
| *formato 2:* Rodoviária → Taguatinga | 682 ms | 861 ms | 324 | 219 |
| *formato 2:* Planaltina → Gama | 509 ms | 1120 ms | 498 | 274 |
| *formato 2:* Asa Sul → Guará | 291 ms | 706 ms | 244 | 133 |

Com o cache quente (rota repetida): busca de 9 ms. O tempo frio é quase todo leitura do SD
(~3 ms por leitura aleatória). Tamanho da página medido: 4 KB, 8 KB e 16 KB dão o mesmo
tempo total (~1 s); 8 KB ficou ligeiramente melhor.

Evolução da localidade (leituras por rota, PC, cache frio): numeração do RoutingKit 454;
ordem espacial pura 597; espacial + topo de 2 % em ordem de nível **425** (páginas de 4 KB).

## Semáforos, pedágios e moto (22/11/2026)

- **Semáforos:** o construtor lê os nós do OSM (libosmium; o RoutingKit não entrega tags de
  nós). `highway=traffic_signals` e `crossing=traffic_signals` a menos de 30 m formam um
  cruzamento; a espera (carro 10 s, moto 8 s, bicicleta 6 s, a pé 0; `BGR_SIGNAL_DS`) entra no
  `ARC_TIME` do trecho que entra no cruzamento, uma vez — vias duplas não contam duas vezes.
- **Cabines de pedágio** (`barrier=toll_booth`) marcam a via como pedágio, como `toll=yes`.
- **Evitar pedágios:** `--avoid-toll` gera `NOME-carro-sem-pedagio.bgr` (`BGR_F_TOLLPEN`): cada
  trecho de via com pedágio custa +1 h (`BGR_TOLL_PENALTY_DS`) só na hierarquia; o aparelho
  subtrai a penalidade de cada trecho de pedágio percorrido, então o tempo mostrado é o real.
  A rota informa quantos trechos de pedágio tem (`toll_arcs`).
- **Moto:** perfil próprio (`NOME-moto.bgr`): máximo 110 km/h, 70 % (até 50 km/h) em terra e
  cascalho, trilhas até 25 km/h, conversões esquerda 6 s / direita 2 s / retorno 25 s.

| Conferência (300 consultas CH × Dijkstra, DF) | Resultado |
|---|---|
| carro, carro sem pedágio, moto, bicicleta | 0 diferentes em cada |

## Pendências

- **Tamanho:** formato 2 = 177 MB para os quatro perfis do DF (290 MB no formato 1). O
  Brasil: ver [13-dados-brasil.md](13-dados-brasil.md).
- Arquivos nacionais com semáforos, moto e sem pedágio ainda não gerados no cartão.
- Rotas **não aprovadas para uso no trânsito real**: precisam de testes de campo.
