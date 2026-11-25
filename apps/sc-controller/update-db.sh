#!/bin/sh
# Atualiza gamecontrollerdb.txt a partir do sc-controller (só as linhas de Linux).
# uso: apps/sc-controller/update-db.sh [pasta do clone do sc-controller]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-}
if [ -z "$SRC" ]; then
    SRC=$(mktemp -d)
    trap 'rm -rf "${SRC:?}"' EXIT
    git clone -q --depth 1 https://github.com/ruscher/sc-controller.git "$SRC"
fi
REV=$(git -C "$SRC" rev-parse --short HEAD)
{
    echo "# Game Controller DB for SDL (2.0.16 format), Linux entries only"
    echo "# Source: https://github.com/mdqinc/SDL_GameControllerDB via https://github.com/ruscher/sc-controller ($REV)"
    echo "# License: zlib (see LICENSE-gamecontrollerdb.txt). Filtered for BigFoston by apps/sc-controller/update-db.sh"
    echo
    grep "platform:Linux" "$SRC/gamecontrollerdb.txt"
} > "$HERE/gamecontrollerdb.txt"
echo "$(grep -c 'platform:Linux' "$HERE/gamecontrollerdb.txt") controles ($REV)"
