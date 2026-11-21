# Big GPS — arquitetura

```
PC (pré-processamento)                         FOSTON (BigFoston OS)
──────────────────────                         ─────────────────────
Geofabrik .osm.pbf ──► osm-clip.py (região)    bfgps ──► /run/bigfoston-gps (1 Hz)
          │                                           │
          ├──► bgmap-build ──► mapa.bgm ──SD──► bgmap.c (tiles, cache) ──► bgrender.c ──► LVGL
          ├──► (roteamento: grafo CH por perfil)  ──► consulta CH em C       │
          └──► (busca: índice de nomes)           ──► busca por prefixo       ▼
                                                                   tela do Big GPS no shell
```

## Componentes

| Componente | Onde | Linguagem | Estado |
|---|---|---|---|
| Recorte de regiões | `tools/big-gps/osm-clip.py` | Python (pyosmium) | feito |
| Construtor de mapas | `tools/big-gps/builder/` | C++17 (libosmium) | feito (v1) |
| Leitor do pacote + cache | `src/ui/biggps/bgmap.c` | C | feito |
| Rasterizador inteiro | `src/ui/biggps/raster.c` | C | feito |
| Renderizador (estilos, camadas, rótulos) | `src/ui/biggps/bgrender.c` | C | feito |
| Ferramenta de teste/medição | `src/ui/biggps/bgview.c` | C | feito |
| Construtor de rotas (CH) | `tools/big-gps/builder/bgroute-build.cpp` | C++ (RoutingKit) | feito |
| Consulta de rotas | `src/ui/biggps/bgroute.c` | C | feito |
| Navegação (manobras, acompanhamento, recálculo) | `src/ui/biggps/bgnav.c`, `bgrouter.c` | C | feito |
| Voz, busca | `src/ui/biggps/`, `src/ui/` | C | a fazer |
| Tela no shell | `src/ui/screens/scr_map.c` | C (LVGL 9) | mapa, destino, rota e navegação |

O código do aparelho (`src/ui/biggps/`) não depende do LVGL: devolve um buffer RGB565 e uma
lista de rótulos; a mesma biblioteca compila no PC (simulador e `bgview`) e no aparelho.

## Cartão SD

```
/mnt/sd/BigFoston/big-gps/
  mapas/      pacotes .bgm (um por região ou o Brasil inteiro)
  rotas/      grafos de roteamento por perfil: <região>-carro.bgr, -moto, -bicicleta, -ape
  busca/      índices de nomes e endereços (a fazer)
  voz/        frases e/ou vozes (a fazer)
  dados/      favoritos, histórico, configurações do usuário
```

## Formato do mapa (`.bgm`, versão 1)

Descrito em [src/ui/biggps/bgmap_format.h](../../src/ui/biggps/bgmap_format.h): coordenadas Web
Mercator de 32 bits; tiles na grade padrão nos zooms 5, 8, 11 e 14, 8192 unidades por tile
com margem de 256; índice ordenado por nível (busca binária); nomes por tile; feições
ordenadas pela ordem de desenho; geometria em deltas zigzag-varint; atribuição ODbL dentro
do arquivo. O leitor confere magia, versão, limites do índice e cada tile durante a
decodificação (dados corrompidos viram tile vazio, nunca leitura fora do buffer).
