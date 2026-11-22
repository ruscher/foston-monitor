# Motor do mapa (biggps)

O motor desenha numa thread própria, em buffer duplo, com aritmética inteira e um
rasterizador sem antialiasing (quadriláteros convexos para linhas grossas, Bresenham para
finas). O perfil do motor (`bgview -p`, z14) mostra o tempo no preenchimento de pixels e
na geometria das linhas, sem desperdício evidente: o ganho é **desenhar menos**.

## Níveis de detalhe (`bgr_view_t.detail`, Ajustes > Desempenho)

| Nível | O que muda |
|---|---|
| Detalhado | tudo o que o mapa tem a partir do zoom mínimo de cada tipo (o comportamento antigo) |
| Equilibrado | edifícios e caminhos a partir de z16, vias de serviço z15, estacionamentos z15, ruas residenciais z13 |
| Econômico | edifícios, caminhos e estacionamentos só em z17, serviço e trilhas z16, ruas residenciais z14, terciárias z13; contorno só nas vias principais a partir de z15 — as demais viram uma linha única num tom entre o preenchimento e o contorno |
| **Automático** (padrão) | econômico ao navegar ou quando a média dos quadros passa de 300 ms (volta a equilibrado abaixo de 150 ms); equilibrado no resto |

Motor isolado, Brasília (quadro médio em ms, `bgview -b 5`):

| Zoom | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|
| Detalhado | 274 | 157 | 355 | 173 | 100 | 43 |
| Equilibrado | 195 | 151 | 205 | 123 | 120 | 44 |
| Econômico | 118 | 95 | 130 | 95 | 70 | 46 |

Na navegação (z16,5 econômico) os quadros ficam entre 50 e 170 ms. A visão geral da rota em
z12 desenhava 42 mil feições (428 ms); no econômico as ruas residenciais só entram em z14.

## Prioridade

A thread do motor roda com `nice 10` (`BIGFOSTON_MAP_NICE` muda): num núcleo só, a
interface passa na frente; durante o arrasto o motor só termina quando o dedo pausa, e a
imagem anterior é deslocada até lá.

## Ideias não feitas (medidas e motivo em [19](19-known-limitations.md))

Desenho incremental no deslocamento (só a faixa nova), mapa base pré-rasterizado no PC.
