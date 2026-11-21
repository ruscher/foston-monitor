# Big GPS — testes

Tudo abaixo foi executado de fato; onde não há resultado, o teste ainda não foi feito.

## Correção

| Teste | Como | Resultado |
|---|---|---|
| Rotas contra referência independente | `bgroute-build --tests` (Dijkstra comum no grafo expandido) × `bgroute -c` (CH), 300 consultas por perfil | carro, moto, bicicleta, a pé: **0 diferenças** (formatos 1 e 2); no aparelho também |
| Mapa em partes = mapa inteiro | blocos `--chunk11`/`--spill` juntados × uma passada; render pixel a pixel em z6–z17 | `--spill`: **0 pixels diferentes** em 6 zooms; `--chunk11`: 0 (z6, z9, z15, z17), 25–111 px (z12–z13, simplificação por pedaço) |
| Mapa dividido em arquivos | `bgmap-merge` com limite de 8 MB (5 arquivos) × arquivo único | 0 pixels diferentes |
| Leitor com índice no arquivo | render antes/depois da mudança | imagens idênticas |
| Rasterizador | linha sintética com contorno; render antes/depois de cada otimização | simétrico; diferenças < 0,5 % dos pixels só onde o arredondamento mudou |

## Robustez

| Teste | Como | Resultado |
|---|---|---|
| Arquivos corrompidos no cartão | 120 cópias de `df.bgm`, `df-carro.bgr`, `df.bgs` com bytes trocados, cabeçalhos alterados, truncadas e com blocos zerados, lidas por `bgview`/`bgroute`/`bgsearch` compilados com AddressSanitizer + UBSan | **0 falhas** (depois de corrigir 2 casos de comportamento indefinido achados assim) |
| Interface inteira sob sanitizadores | simulador com ASan/UBSan, modo de capturas (94 telas, inclui mapa, busca, cálculo de rota e navegação simulada, threads de desenho e de rotas) | **0 relatos** (depois do patch 0004 do LVGL) |
| Fluxos normais sob sanitizadores | render z6–z18 com rotação, 300 consultas, simulação de percurso, buscas, categorias | 0 erros, 0 vazamentos |

## Navegação (simulada)

| Teste | Resultado |
|---|---|
| Percurso simulado com ruído de ±5 m e desvio lateral de 80 m | saída da rota na 3ª posição fora; volta reconhecida; chegada a 19,4 m |
| Demonstração no aparelho (`BIGGPS_DEMO=1`) | rota em 2,4 s (frio); quadros 125–260 ms navegando; voz falando os avisos |

## Ainda não feito

- Teste dirigindo de verdade (GPS real em movimento, recálculo com desvio real).
- Confirmação auditiva da voz no alto-falante e com o Multimídia tocando.
- Navegação longa no aparelho medindo memória ao longo do tempo (em andamento).
- Remoção do cartão durante o uso.
