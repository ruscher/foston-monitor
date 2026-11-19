#!/bin/sh
# Collect the files the BigFoston loader needs into $BINARIES_DIR/bigfoston/
# with a manifest and SHA-256 sums (tools/bigfoston/mkboot.py writes the
# initrd range into the DTB).
set -eu
OUT="$BINARIES_DIR/bigfoston"
TOP="$BR2_EXTERNAL_BIGFOSTON_PATH/../.."
mkdir -p "$OUT"
cp "$BINARIES_DIR/zImage" "$OUT/zImage"
cp "$BINARIES_DIR/rootfs.cpio.gz" "$OUT/initrd.gz"
python3 "$TOP/tools/bigfoston/mkboot.py" \
    --dtb "$BINARIES_DIR/mt3351-foston-fs707dc.dtb" \
    --initrd "$OUT/initrd.gz" --out "$OUT/fs707dc.dtb"
( cd "$OUT" && sha256sum zImage initrd.gz fs707dc.dtb > SHA256SUMS )
echo "BigFoston boot files in $OUT"
