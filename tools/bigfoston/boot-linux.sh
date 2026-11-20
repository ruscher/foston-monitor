#!/bin/sh
# Send a BigFoston OS build to the FOSTON and start it (Windows CE stops; reset brings it back).
#   tools/bigfoston/boot-linux.sh [BUILD_DIR] [--dry-run]
# BUILD_DIR defaults to firmware/build/out/images/bigfoston (zImage, initrd.img, fs707dc.dtb).
# Each file is read back from the SD card and compared by SHA-256 before booting.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
AGENT="python3 $TOP/tools/bigfoston/bfagent.py"
DIR="$TOP/firmware/build/out/images/bigfoston"
DRY=""
DELAY=0
for arg in "$@"; do
    case "$arg" in
    --dry-run) DRY=--dry-run ;;
    --delay=*) DELAY=${arg#--delay=} ;;
    *) DIR=$arg ;;
    esac
done
DEV='\SDMMC\BigFoston\linux'
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

$AGENT --wait 60 info | head -n1
$AGENT mkdir "$DEV" 2>/dev/null || true
for f in zImage fs707dc.dtb initrd.img; do
    $AGENT put "$DIR/$f" "$DEV\\$f"
    $AGENT get "$DEV\\$f" "$TMP/$f" 2>/dev/null
    if [ "$(sha256sum < "$DIR/$f")" != "$(sha256sum < "$TMP/$f")" ]; then
        echo "verificacao falhou: $f" >&2
        exit 1
    fi
    echo "ok $f $(sha256sum < "$DIR/$f" | cut -c1-16)"
done
$AGENT boot "$DEV\\zImage" --dtb "$DEV\\fs707dc.dtb" --initrd "$DEV\\initrd.img" --dry-run
[ -n "$DRY" ] && exit 0
$AGENT boot "$DEV\\zImage" --dtb "$DEV\\fs707dc.dtb" --initrd "$DEV\\initrd.img" --delay "$DELAY"
echo "Linux iniciado no FOSTON. Para voltar ao Windows CE: reset."
