#!/bin/sh
# build-data.sh IN.osm.pbf SAIDA NOME [BBOX] — dados do Big GPS para o cartão, a partir de um
# extrato do OpenStreetMap (Geofabrik). BBOX = MINLON,MINLAT,MAXLON,MAXLAT (recorte; para o
# Brasil: -74.1,-34.0,-28.5,5.4). Gera em SAIDA/big-gps:
#   mapas/NOME.bgm (+ NOME-2.bgm… se passar de 3,9 GB)   rotas/NOME-carro.bgr, -bicicleta, -ape
#   busca/NOME.bgs   voz/espeak-ng-data   SHA256SUMS
# O oceano vem de firmware/build/osm/agua/water-polygons-split-4326.zip se existir
# (osmdata.openstreetmap.de, ODbL). Moto usa o arquivo de carro (mesmas regras hoje).
# Para regiões grandes o mapa é feito em blocos (uma leitura do extrato).
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
IN=${1:?uso: build-data.sh IN.osm.pbf SAIDA NOME [BBOX]}
OUT=${2:?saída}
NAME=${3:?nome}
BBOX=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/builder"
VENV="$HERE/../../firmware/build/venv-osm/bin/python"
WATERZIP="$HERE/../../firmware/build/osm/agua/water-polygons-split-4326.zip"
D="$OUT/big-gps"
mkdir -p "$D/mapas" "$D/rotas" "$D/busca" "$D/voz" "$OUT/tmp"
make -C "$B" >/dev/null
BB=""; [ -n "$BBOX" ] && BB="--bbox=$BBOX"
WATER=""
if [ -n "$BBOX" ] && [ -f "$WATERZIP" ]; then
    "$VENV" -I "$HERE/water-extract.py" "$WATERZIP" "$OUT/tmp/agua.bin" --bbox="$BBOX"
    WATER="--water=$OUT/tmp/agua.bin"
fi
echo "== mapa"
rm -rf "$OUT/tmp/spill" && mkdir -p "$OUT/tmp/spill"
"$B/bgmap-build" "$IN" "$OUT/tmp/x.bgm" $BB $WATER --spill="$OUT/tmp/spill,16"
rm -f "$D/mapas/$NAME"*.bgm
"$B/bgmap-merge" "$D/mapas/$NAME.bgm" "$OUT/tmp/spill"/part-*.bgm
rm -rf "$OUT/tmp/spill"
echo "== busca"
"$B/bgsearch-build" "$IN" "$D/busca/$NAME.bgs" $BB
for p in car:carro bike:bicicleta foot:ape; do
    echo "== rotas ${p#*:}"
    "$B/bgroute-build" "$IN" "$D/rotas/$NAME-${p#*:}.bgr" --profile "${p%%:*}"
done
echo "== voz"
"$HERE/make-voice-data.sh" "$D/voz" >/dev/null
rm -rf "$OUT/tmp"
(cd "$D" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
du -sh "$D"/*
