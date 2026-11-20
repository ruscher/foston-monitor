#!/bin/sh
# Collect the files the BigFoston loader needs into $BINARIES_DIR/bigfoston/
# with SHA-256 sums. fs707dc.dtb gets the initrd range and the normal
# command line (with the splash) from tools/bigfoston/mkboot.py.
set -eu
OUT="$BINARIES_DIR/bigfoston"
TOP="$BR2_EXTERNAL_BIGFOSTON_PATH/../.."
BOOTARGS=${BIGFOSTON_BOOTARGS:-"loglevel=7 rdinit=/init user_debug=31 print_fatal_signals=1 splash"}
mkdir -p "$OUT"
rm -f "$OUT/initrd.gz" "$OUT/normal.dtb"
cp "$BINARIES_DIR/zImage" "$OUT/zImage"
cp "$BINARIES_DIR/rootfs.cpio.xz" "$OUT/initrd.img"
python3 "$TOP/tools/bigfoston/mkboot.py" \
    --dtb "$BINARIES_DIR/mt3351-foston-fs707dc.dtb" \
    --initrd "$OUT/initrd.img" --bootargs "$BOOTARGS" --out "$OUT/fs707dc.dtb"
( cd "$OUT" && sha256sum zImage initrd.img fs707dc.dtb > SHA256SUMS )
echo "BigFoston boot files in $OUT"
