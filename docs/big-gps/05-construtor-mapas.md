# Big GPS — construtor de mapas (PC)

Código: [tools/big-gps/builder/bgmap-build.cpp](../../tools/big-gps/builder/bgmap-build.cpp).
Formato: [src/biggps/bgmap_format.h](../../src/biggps/bgmap_format.h). Tabela de zoom mínimo
por tipo, comum ao construtor e ao aparelho: [src/biggps/bgkinds.h](../../src/biggps/bgkinds.h).

## Dependências (só no PC)

| Biblioteca | Versão | Licença | Uso |
|---|---|---|---|
| libosmium | v2.20.0 | BSL-1.0 | leitura do PBF, índice de nós, montagem de multipolígonos |
| protozero | v1.7.1 | BSD-2-Clause | decodificação protobuf (usada pela libosmium) |
| zlib | do sistema | Zlib | blocos do PBF |
| pyosmium | 4.3.2 | BSD-2-Clause | recorte de regiões (`tools/big-gps/osm-clip.py`) |

As bibliotecas ficam em `firmware/build/src-libosmium` e `firmware/build/src-protozero`
(clonadas das tags acima; não vão para o aparelho).

```sh
git clone --depth 1 --branch v2.20.0 https://github.com/osmcode/libosmium.git firmware/build/src-libosmium
git clone --depth 1 --branch v1.7.1 https://github.com/mapbox/protozero.git firmware/build/src-protozero
make -C tools/big-gps/builder
tools/big-gps/builder/bgmap-build df.osm.pbf df.bgm
```

## O que faz

1. **Passada 1 (relações):** multipolígonos (água, uso do solo, parques, edificações…) e
   as vias que formam divisas estaduais/nacionais (`boundary=administrative`,
   `admin_level` 2 ou 4).
2. **Passada 2:** índice de posições dos nós (`FlexMem`), lugares (`place=*` com nome),
   vias classificadas (rodovias, ruas, caminhos, trilhos, rios), áreas montadas.
3. Projeção para Web Mercator de 32 bits (inteiro).
4. Para cada nível (z5, z8, z11, z14): simplificação Douglas-Peucker com tolerância de
   0,35 px no zoom de detalhe do nível (z6, z9, z12, z17), descarte de áreas menores que
   2 px², recorte em tiles com margem de 256 unidades (Liang-Barsky nas linhas,
   Sutherland-Hodgman nos polígonos).
5. Codificação: nomes por tile, feições ordenadas pela ordem de desenho, coordenadas em
   deltas zigzag-varint, totais no cabeçalho do tile (decodificação em uma passada).

## Medições (PC, DF)

| Etapa | Resultado |
|---|---|
| Recorte do Centro-Oeste para o DF (pyosmium) | 6 min, `df.osm.pbf` 61 MB |
| `bgmap-build` no DF | 5 s, 738 MB de pico, 972 678 feições coletadas |
| Pacote `df.bgm` | 30 465 292 bytes |
| Nível z5 / z8 / z11 / z14 | 6 / 74 / 952 / 10 404 tiles |

O Brasil inteiro será medido na fase de dados nacionais ([13-dados-brasil.md](13-dados-brasil.md)).
