# Big GPS — renderização do mapa

Código: [src/biggps/raster.c](../../src/biggps/raster.c) (rasterizador),
[src/biggps/bgrender.c](../../src/biggps/bgrender.c) (estilos, camadas, rótulos),
[src/biggps/bgview.c](../../src/biggps/bgview.c) (ferramenta de teste e medição).

## Como funciona

1. Escolhe o nível de dados do pacote para o zoom (z5, z8, z11 ou z14) e os tiles que
   cobrem a tela (considerando rotação).
2. Por quadro, só algumas contas em `double`: escala, seno e cosseno da rotação, origem
   de cada tile na tela, retângulo visível de cada tile em unidades do tile.
3. Por feição: descarte pela caixa envolvente (`int16`, calculada na decodificação);
   por vértice, só multiplicação de 64 bits e deslocamento (transformação afim em ponto
   fixo, 1/16 de pixel).
4. Áreas: preenchimento por linhas de varredura (par-ímpar, com buracos), polígono
   recortado à tela antes para que toda a conta por linha caiba em 32 bits.
5. Vias: cada segmento vira um quadrilátero convexo e cada vértice (a partir de 4 px)
   um octógono, preenchidos por duas cadeias de arestas sem ordenação; linhas finas com
   Bresenham; tracejado em ponto fixo.
6. Camadas: áreas → rios e córregos → divisas → trilhos → contornos das vias → vias →
   balsas. Rótulos (lugares e nomes de vias) voltam para a interface desenhar com as fontes
   do shell.
7. Divisões: o ARM1176 não tem instrução de divisão; as inclinações usam uma tabela de
   recíprocos (8192 entradas) em vez de `__aeabi_idiv`.

Paletas dia e noite estão na mesma tabela de estilos (cor de preenchimento, cor de
contorno, largura por zoom de 5 a 19, tracejado).

## Medições no aparelho (2026-11-21)

Pacote do DF ([05-construtor-mapas.md](05-construtor-mapas.md)), tela 800×480, centro
de Brasília (Rodoviária) e Taguatinga; `bgview -b 60` simula 60 km/h com um quadro por
segundo de deslocamento (16,7 m por quadro), cache de tiles quente.

Evolução, para registro (cada linha é uma medição real após a mudança indicada; nas quatro
primeiras, o segundo quadro parado no centro; na última, a média do deslocamento):

| Versão | z13 | z15 | z17 |
|---|---|---|---|
| Primeira (duas passadas, sem descarte por feição) | 536 ms (quente) | 508 ms | 122 ms |
| Caixa envolvente por feição, decodificação em uma passada | 404 ms | 383 ms | 115 ms |
| Polilinha grossa num só preenchimento, tracejado sem divisão de 64 bits | 344 ms | 348 ms | 85 ms |
| Sem recorte de segmentos já dentro da tela; arestas ativas ordenadas | 227 ms | 278 ms | 70 ms |
| Preenchimento convexo para vias + tabela de recíprocos | **171 ms** (média, pior 297) | **163 ms** (média, pior 325) | **55 ms** (média, pior 152) |

Cache frio (primeiro quadro, leitura do SD + decodificação), versão final de decodificação:
z15 = 263 ms de busca/decodificação para 616 KB (9 tiles), z17 = 61 ms para 281 KB.

Perfil por amostragem no aparelho (`bgview -p`, SIGPROF) na versão final, z15:
escrita de pixels 26 %, preenchimento convexo 13 %, polígonos 11 %, avanço de arestas
10 %, transformação 9 %. A escrita de pixels é limitada pela memória do aparelho
(memset de 4 MB a **82 MB/s**, leitura a 110 MB/s, medidos com `membench`).

Comparação: o libosmscout leva ~9–11 s por quadro sem rasterizar (ver
[01-pesquisa-tecnologias.md](01-pesquisa-tecnologias.md)).

## Ideias ainda não aplicadas (medir antes)

- Renderizar por faixas horizontais que cabem no cache L1 (16 KB) para que a sobreposição
  de camadas não vá para a memória.
- Deslocar o quadro anterior e desenhar só a faixa nova durante a navegação.
- Rótulos de vias ao longo da linha.
