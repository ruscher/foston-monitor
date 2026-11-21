# Big GPS — pesquisa de tecnologias e provas de viabilidade (fase 2)

Objetivo: escolher como desenhar mapas, calcular rotas, buscar endereços e falar instruções
**no FOSTON** (ARM1176 de ~470 MHz **sem FPU**, ~100 MB livres, SD com 2,7 ms por leitura
aleatória — ver [02-analise-hardware.md](02-analise-hardware.md)), com dados OSM de todo o Brasil.

Toda medição aqui foi feita no aparelho em 2026-11-21. O que não foi medido está marcado como
"não medido" e a avaliação vem do código e da documentação do projeto.

## Dados de teste

| Item | Valor |
|---|---|
| Fonte | Geofabrik, `south-america/brazil/centro-oeste-latest.osm.pbf` (208 614 230 bytes, MD5 conferido) |
| Recorte | caixa do DF `-48.30,-16.06,-47.30,-15.49` com [tools/big-gps/osm-clip.py](../../tools/big-gps/osm-clip.py) (pyosmium 4.3.2) |
| Resultado | 10 788 375 nós, 1 092 080 vias, 31 854 relações; `df.osm.pbf` de 61 MB; 6 min no PC |

O DF tem uma importação extensa de edificações, daí o número alto de nós.

## libosmscout (primeiro candidato)

C++17, LGPL-3.0, commit `d476c2cd` (2026-11-21). Importação no PC (`Import` com
`stylesheets/map.ost`): **102 s, pico de 1,6 GB**, banco final de **56–59 MB** (sem
temporários). No aparelho: núcleo + demos compilados estaticamente com o toolchain do
Buildroot com C++ (diretório separado `firmware/build/out-cxx`; `-mno-unaligned-access`
conferido no wrapper), banco copiado para o SD.

### Roteamento (demo `Routing`, perfil carro)

| Rota | Distância da rota | Tempo total | Tempo do cálculo | Pico de RAM |
|---|---|---|---|---|
| Rodoviária do Plano Piloto → Taguatinga, cache frio | 25,2 km | 28,39 s | — | 45 652 kB |
| a mesma, cache quente | 25,2 km | 24,05 s | 19,43 s (+1,88 s pós-processamento) | 45 560 kB |
| Planaltina → Gama, cache frio | 73,5 km | 42,47 s | 34,67 s (+3,39 s) | 59 820 kB |

### Desenho do mapa (demo `DrawMapSVG`, 800×480, estilo `standard.oss`)

O backend SVG executa todo o pipeline do libosmscout (leitura, projeção, estilos, layout de
rótulos) e só não rasteriza; serve de **limite inferior** para o custo de um quadro.

| Caso | Tempo total | Pico de RAM |
|---|---|---|
| Área vazia (só abrir banco + estilo) | 5,61–5,76 s | 17,4 MB |
| Rodoviária, magnificação 16384 (cidade), frio / quente | 19,04 s / 16,29 s | 33,3 MB |
| Rodoviária, magnificação 65536 (rua), frio / quente | 16,73 s / 14,50 s | 32,2 MB |

Descontada a abertura: **~9 a 11 s por quadro** sem rasterização.

### Conclusão sobre o libosmscout

Inviável neste processador para navegação em tempo real: um quadro leva ~10 s (meta < 1 s)
e uma rota de 25 km no DF leva ~20 s com A* sem hierarquia (para rotas de centenas de km no
Brasil inteiro, e para recálculo durante a navegação, seria pior). A causa principal é a
aritmética em `double` emulada (318 ns por operação, ver `fpbench`) somada a uma arquitetura
pensada para CPUs com FPU. Ele continua útil como **referência** (formatos, regras de
navegação, descrição de manobras).

## Outros candidatos

| Projeto | Licença | Linguagem / requisitos | Situação para o FOSTON |
|---|---|---|---|
| Navit | GPL-2.0 (maior parte) | C, coordenadas inteiras, mapas `binfile` do `maptool` | Bom histórico em PDAs sem FPU, mas é um aplicativo completo com GUI própria (não uma biblioteca para LVGL); roteamento sem hierarquia (lento para rotas longas). GPL-2.0-only não pode ser ligado ao código GPL-3.0 do BigFoston no mesmo binário. **Não medido.** |
| Organic Maps | Apache-2.0 | C++, OpenGL ES 2/3 ou Vulkan | Exige GPU; o MT3351 não tem. Referência de UX e de busca. Inviável. |
| Valhalla | MIT | C++, tiles grandes, muita RAM | Servidor. Útil só no PC para conferir rotas. |
| GraphHopper | Apache-2.0 | Java | Sem JVM no aparelho. Referência no PC. |
| BRouter | MIT | Java | Sem JVM. Referência de perfis de bicicleta. |
| OSRM | BSD-2 | C++, CH/MLD, vários GB de RAM para o Brasil | Servidor. A técnica (Contraction Hierarchies) é a que interessa. |
| Mapsforge | LGPL-3.0 | Java; formato de mapa vetorial aberto | Sem JVM; o formato é uma boa referência (tiles por zoom, coordenadas delta). |
| RoutingKit | BSD-2 | C++, CH com consulta simples sobre arrays | **Escolhido para o PC** (pré-processamento CH). A consulta é reescrita em C no aparelho. |
| libosmium / protozero | BSL-1.0 / BSD-2 | C++ só cabeçalhos | **Escolhido para o PC** (leitura do PBF, montagem de multipolígonos). |
| eSpeak NG | GPL-3.0 | C, síntese por formantes, pt-BR incluído | Candidato para voz (nomes de ruas dinâmicos). **A medir** (fase de voz). |
| Piper | MIT (motor); vozes com licenças próprias | rede neural ONNX | Pesado demais sem FPU para tempo real; possível só para **frases fixas pré-gravadas no PC**, se a licença da voz permitir. **A verificar.** |

## Decisão de arquitetura

Motor próprio, pequeno, em C e com aritmética inteira no aparelho; trabalho pesado no PC:

1. **Construtor de mapas no PC** (C++17 com libosmium): projeta para Web Mercator inteiro,
   simplifica a geometria por nível de zoom, recorta em tiles e grava um pacote binário
   indexado por região. O aparelho não faz trigonometria por ponto.
2. **Rotas**: grafo por perfil (carro, moto, bicicleta, a pé) com Contraction Hierarchies
   calculadas no PC (RoutingKit); no aparelho, Dijkstra bidirecional só sobre arestas "para
   cima" (poucos milhares de nós mesmo para rotas longas), lido do SD por blocos.
3. **Desenho**: rasterizador inteiro próprio (preenchimento por linhas de varredura, linhas
   grossas) direto num buffer RGB565 do LVGL; rótulos com as fontes do shell.
4. **Busca**: índice de nomes normalizados (sem acento, minúsculas) com busca por prefixo.
5. **Voz**: a decidir com medição (eSpeak NG no aparelho; frases fixas pré-geradas).

Cada parte será medida no aparelho antes de seguir para a próxima.
