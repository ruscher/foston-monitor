#!/bin/sh
# make-voice-data.sh OUTDIR — dados de voz do eSpeak NG só com o português, para o cartão
# (BigFoston/big-gps/voz/espeak-ng-data). Compila o eSpeak NG 1.52.0 no PC (a mesma versão
# do pacote bigfoston-espeak-ng do sistema) e copia fonemas, entonações, definições de
# língua e o dicionário pt. Licença dos dados: GPL-3.0-or-later (eSpeak NG).
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
OUT=${1:?uso: make-voice-data.sh PASTA_DE_SAIDA}
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD="$HERE/../../firmware/build"
SRC="$BUILD/src-espeak-ng"
HOST="$BUILD/espeak-host"
if [ ! -d "$SRC" ]; then
    git -c advice.detachedHead=false clone --depth 1 --branch 1.52.0 https://github.com/espeak-ng/espeak-ng.git "$SRC"
fi
[ "$(git -C "$SRC" describe --tags)" = "1.52.0" ] || { echo "src-espeak-ng não está na tag 1.52.0" >&2; exit 1; }
if [ ! -f "$HOST/espeak-ng-data/phondata" ]; then
    cmake -S "$SRC" -B "$HOST" -DCMAKE_BUILD_TYPE=Release -DUSE_MBROLA=OFF -DUSE_LIBSONIC=OFF \
        -DUSE_LIBPCAUDIO=OFF -DUSE_ASYNC=OFF -DESPEAK_BUILD_MANPAGES=OFF >/dev/null
    cmake --build "$HOST" -j"$(nproc)" >/dev/null
fi
D="$HOST/espeak-ng-data"
rm -rf "$OUT/espeak-ng-data"
mkdir -p "$OUT/espeak-ng-data"
cp "$D/phondata" "$D/phonindex" "$D/phontab" "$D/intonations" "$D/pt_dict" "$OUT/espeak-ng-data/"
cp -r "$D/lang" "$D/voices" "$OUT/espeak-ng-data/"
# teste rápido no PC
"$HOST/src/espeak-ng" --path="$OUT" -v pt-br -q "Em 300 metros, vire à direita" && echo "voz pt-br ok: $OUT/espeak-ng-data"
du -sh "$OUT/espeak-ng-data"
