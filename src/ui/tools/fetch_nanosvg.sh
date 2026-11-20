#!/bin/sh
# Download nanosvg (SVG parser + rasterizer, zlib licence) into third_party/nanosvg
# (not versioned), pinned to one commit and verified by SHA-256.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST="$HERE/third_party/nanosvg"
REV=93ce879dc4c04a3ef1758428ec80083c38610b1f
URL=https://github.com/memononen/nanosvg/archive/$REV.tar.gz
SHA=259f5236843a2245fdfd92fddffcaf292ac1f46d24f94f38fa3a9b690501e7d2
[ -f "$DEST/nanosvg.h" ] && { echo "nanosvg already in $DEST"; exit 0; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sSfL -o "$TMP/nanosvg.tar.gz" "$URL"
echo "$SHA  $TMP/nanosvg.tar.gz" | sha256sum -c -
tar -xzf "$TMP/nanosvg.tar.gz" -C "$TMP"
mkdir -p "$DEST"
cp "$TMP/nanosvg-$REV/src/nanosvg.h" "$TMP/nanosvg-$REV/src/nanosvgrast.h" "$TMP/nanosvg-$REV/LICENSE.txt" "$DEST/"
echo "nanosvg $REV from $URL (sha256 $SHA)" > "$DEST/VERSION"
echo "nanosvg in $DEST"
