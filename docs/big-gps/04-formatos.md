# Big GPS — formatos dos dados

Todos binários, little endian, pensados para leitura com `pread` no aparelho (sem carregar
índices inteiros na RAM) e conferidos na abertura (magia, versão, limites das seções); um
arquivo corrompido vira "sem dado ali", nunca leitura fora do buffer
([15-testes.md](15-testes.md)).

| Arquivo | Versão | Definição | Conteúdo |
|---|---|---|---|
| `mapas/*.bgm` | BGM1 v1 | [bgmap_format.h](../../src/ui/biggps/bgmap_format.h) | tiles nos zooms 5/8/11/14, Web Mercator de 32 bits, deltas zigzag-varint, nomes por tile, índice ordenado por nível, atribuição ODbL; vários arquivos sem sobreposição formam um mapa só |
| `rotas/*-{carro,moto,bicicleta,ape}.bgr` | BGR1 v2 | [bgroute_format.h](../../src/ui/biggps/bgroute_format.h) | grafo da via, geometria, nomes, CH em blocos de varints (grafo expandido por conversões no carro/moto; por nós na bicicleta/a pé, `BGR_F_NODE`), grade esparsa de encaixe |
| `busca/*.bgs` | BGS1 v2 | [bgsearch_format.h](../../src/ui/biggps/bgsearch_format.h) | entradas (nome, contexto, tipo, categoria, posição), palavras normalizadas com listas de entradas, índice por categoria e tile z10 |
| `voz/espeak-ng-data/` | eSpeak NG 1.52.0 | — | fonemas, entonação e dicionário pt |

Arquivos na pasta de dados do shell (`$BIGFOSTON_DATA`): `biggps-last` ("lat lon zoom
tinha_posição"), `biggps-favoritos.txt` e `biggps-recentes.txt` ("lat lon nome" por linha).
