#!/bin/sh
# Download LVGL v9.2.2 into third_party/lvgl (not versioned), verified by SHA-256.
# Keeps only what the build needs: src/, lvgl.h, lv_version.h, licence, lvgl.mk.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/third_party/lvgl"
URL=https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz
SHA=129b4e00e06639fa79d7e8a6cab3c1ecce2445b1a246652ccd34f22e7b17ad6f
[ -f "$DEST/lvgl.h" ] && { echo "LVGL already in $DEST"; exit 0; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sSfL -o "$TMP/lvgl.tar.gz" "$URL"
echo "$SHA  $TMP/lvgl.tar.gz" | sha256sum -c -
tar -xzf "$TMP/lvgl.tar.gz" -C "$TMP"
mkdir -p "$DEST"
cd "$TMP/lvgl-9.2.2"
cp -r src lvgl.h lv_version.h LICENCE.txt README.md lv_conf_template.h lvgl.mk "$DEST/"
echo "LVGL v9.2.2 from $URL (sha256 $SHA)" > "$DEST/VERSION"
echo "LVGL v9.2.2 in $DEST"
