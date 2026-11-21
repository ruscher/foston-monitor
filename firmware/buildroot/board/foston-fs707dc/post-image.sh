#!/bin/sh
# Collect the files the BigFoston loader needs into $BINARIES_DIR/bigfoston/
# with SHA-256 sums. fs707dc.dtb gets the initrd range and the normal
# command line (with the splash) from tools/bigfoston/mkboot.py.
set -eu
OUT="$BINARIES_DIR/bigfoston"
TOP="$BR2_EXTERNAL_BIGFOSTON_PATH/../.."
# g_cdc.iSerialNumber: names the USB gadget so the PC's Big Screen Monitor
# recognises the FOSTON with BigFoston OS (profile foston_bigfoston); IDs unchanged.
# g_cdc.host_addr/dev_addr: fixed, locally administered MACs (they were random
# at every boot), so the PC's network connection follows the device, not the
# USB port it is plugged into (NetworkManager: 802-3-ethernet.mac-address).
BOOTARGS=${BIGFOSTON_BOOTARGS:-"loglevel=7 rdinit=/init user_debug=31 print_fatal_signals=1 splash g_cdc.iSerialNumber=BIGFOSTON-FS707DC g_cdc.host_addr=02:46:53:37:30:01 g_cdc.dev_addr=02:46:53:37:30:02"}
mkdir -p "$OUT"
rm -f "$OUT/initrd.gz" "$OUT/normal.dtb"
cp "$BINARIES_DIR/zImage" "$OUT/zImage"
cp "$BINARIES_DIR/rootfs.cpio.xz" "$OUT/initrd.img"
python3 "$TOP/tools/bigfoston/mkboot.py" \
    --dtb "$BINARIES_DIR/mt3351-foston-fs707dc.dtb" \
    --initrd "$OUT/initrd.img" --bootargs "$BOOTARGS" --out "$OUT/fs707dc.dtb"
( cd "$OUT" && sha256sum zImage initrd.img fs707dc.dtb > SHA256SUMS )
echo "BigFoston boot files in $OUT"
