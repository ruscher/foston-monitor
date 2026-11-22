# Big GPS — preparar e instalar os dados

## 1. Gerar no PC

Requisitos: as bibliotecas do construtor ([05-construtor-mapas.md](05-construtor-mapas.md)),
o `venv-osm` (pyosmium e pyshp) e espaço em disco (~30 GB para o Brasil durante a
construção). Baixe **uma vez** o extrato da Geofabrik e, para o litoral, os polígonos de
oceano:

```sh
cd firmware/build/osm
curl -LO https://download.geofabrik.de/south-america/brazil-latest.osm.pbf
curl -LO https://download.geofabrik.de/south-america/brazil-latest.osm.pbf.md5 && md5sum -c brazil-latest.osm.pbf.md5
mkdir -p agua && curl -L -o agua/water-polygons-split-4326.zip \
    https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip
```

Depois, um comando gera tudo:

```sh
tools/big-gps/build-data.sh firmware/build/osm/brazil-latest.osm.pbf /tmp/dados brasil -74.1,-34.0,-28.5,5.4
```

Saída em `/tmp/dados/big-gps`: `mapas/brasil.bgm`, `rotas/brasil-carro.bgr`,
`rotas/brasil-bicicleta.bgr`, `rotas/brasil-ape.bgr`, `busca/brasil.bgs`,
`voz/espeak-ng-data/` e `SHA256SUMS`. (Moto usa o arquivo de carro.)

Para uma região menor, use um extrato regional e a caixa dela (o DF de teste:
`osm-clip.py` com `-48.30,-16.06,-47.30,-15.49`).

## 2. Instalar no cartão

**Recomendado — cartão no leitor do PC** (dezenas de MB/s):

```sh
tools/big-gps/install-sd.py local /tmp/dados/big-gps /run/media/$USER/CARTAO
```

**Pelo cabo USB, com o aparelho ligado no Linux** (só ~1,3 MB/s — o controlador USB do
aparelho não tem DMA; serve para atualizar um arquivo):

```sh
make -C tools/big-gps/bgrecv
tools/big-gps/install-sd.py rede /tmp/dados/big-gps --so rotas/brasil-carro.bgr
```

Nos dois modos cada arquivo é gravado com nome temporário, conferido por SHA-256 (no modo
local lendo de volta do cartão, sem o cache do PC; no modo rede calculado pelo `bgrecv`
enquanto recebe) e só então renomeado; um `MANIFEST.sha256` no cartão evita copiar de novo
o que já está lá.

## Estrutura no cartão

```
BigFoston/big-gps/
  mapas/*.bgm           todos os .bgm da pasta formam um mapa só (partes sem sobreposição)
  rotas/*-carro.bgr     *-moto.bgr (opcional), *-bicicleta.bgr, *-ape.bgr
  busca/*.bgs
  voz/espeak-ng-data/   voz em português (sem ela, o botão de som some)
  MANIFEST.sha256
```
