# Big GPS — dados de todo o Brasil

Extrato: Geofabrik `south-america/brazil-latest.osm.pbf` de 20/11/2026, 2 107 062 627 bytes
(MD5 conferido). Recorte: `-74.1,-34.0,-28.5,5.4` (inclui Fernando de Noronha e Trindade).
Oceano: `water-polygons-split-4326.zip` de 20/11/2026 (osmdata.openstreetmap.de, ODbL):
848 polígonos, 797 256 pontos na caixa do Brasil. PC: 16 núcleos, 46 GB de RAM.

## Construção (medida)

| Etapa | Tempo | Pico de RAM | Resultado |
|---|---|---|---|
| Mapa (`bgmap-build --spill=…,16`, uma leitura do extrato, 139–221 blocos de 16×16 tiles z11) | 232 s (sem oceano) | 8,4 GB | partes em disco |
| Junção (`bgmap-merge`) | 2,9 s | 121 MB | **`brasil.bgm` = 1 683 900 849 bytes** (com oceano): z5 19 tiles, z8 484, z11 26 770, z14 2 828 705 |
| Busca (`bgsearch-build`) | 72 s | 8,4 GB | **`brasil.bgs` = 204 MB**: 3 795 075 entradas (148 723 lugares), 239 725 palavras |
| Rotas carro (`bgroute-build`) | em andamento | ~11,6 GB | grafo: 11 231 866 nós, 27 980 668 arcos, 6 823 216 vias, 146 803 proibições de conversão; 55 541 794 arestas expandidas |

O formato do mapa guarda o índice no próprio arquivo (busca binária com `pread`): o
aparelho não carrega o índice de 2,8 milhões de tiles na RAM. Um arquivo passa de 3,9 GB?
`bgmap-merge` divide em partes que o aparelho abre juntas.

## Consultas de busca no PC (índice nacional)

"avenida paulista" 7,2 ms; "mercado municipal" 13,6 ms; "hospital" 12,9 ms (perto da Sé,
São Paulo). O contexto "Bairro, Cidade" (necessário para "rua xv de novembro curitiba")
entra na próxima reconstrução do índice.

## Cópia para o cartão

Pelo cabo USB a cópia fica em ~1 MB/s (o aparelho fica sem CPU livre: recepção, interrupções
da USB e o GPS); o mapa nacional leva ~25–30 min. Para o conjunto completo use o cartão no
leitor do PC ([17-instalacao-sd.md](17-instalacao-sd.md)).
