#!/bin/sh
# Copy the BigFoston agent and the Linux boot files to the FOSTON SD card
# placed in a PC card reader, then verify every file by SHA-256 after a
# remount. (Through the FOSTON's own USB the large writes are unreliable.)
#   tools/bigfoston/sd-install.sh [IMAGES_DIR]
# The card is recognised as a removable FAT volume that already has a
# BigFoston/ folder, and never the FOSTON itself (USB 045e:*).
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${1:-$TOP/firmware/build/out/images/bigfoston}
AGENT=$TOP/src/wince/agent/build/bfagent.exe

PART=""
for b in /sys/block/sd*; do
    [ "$(cat "$b/size")" -gt 0 ] || continue
    node=$(readlink -f "$b/device")
    while [ "$node" != / ] && [ ! -f "$node/idVendor" ]; do node=$(dirname "$node"); done
    [ -f "$node/idVendor" ] || continue
    [ "$(cat "$node/idVendor")" = 045e ] && continue
    for p in /dev/$(basename "$b")?*; do
        [ "$(lsblk -no FSTYPE "$p" 2>/dev/null)" = vfat ] || continue
        m=$(lsblk -no MOUNTPOINT "$p" | head -n1)
        [ -n "$m" ] || { udisksctl mount -b "$p" >/dev/null; m=$(lsblk -no MOUNTPOINT "$p" | head -n1); }
        if [ -d "$m/BigFoston" ]; then PART=$p; break 2; fi
        udisksctl unmount -b "$p" >/dev/null || true
    done
done
[ -n "$PART" ] || { echo "cartao do BigFoston nao encontrado no leitor (FAT com pasta BigFoston/)" >&2; exit 1; }
M=$(lsblk -no MOUNTPOINT "$PART" | head -n1)
echo "Cartao: $PART em $M"
mkdir -p "$M/BigFoston/linux"
cp "$AGENT" "$M/BigFoston/bfagent.exe"
for f in zImage initrd.gz "$IMG"/*.dtb; do f=$(basename "$f"); cp "$IMG/$f" "$M/BigFoston/linux/$f"; done
sync
udisksctl unmount -b "$PART" >/dev/null
udisksctl mount -b "$PART" >/dev/null
M=$(lsblk -no MOUNTPOINT "$PART" | head -n1)
ok=1
check() { if [ "$(sha256sum < "$1")" = "$(sha256sum < "$2")" ]; then echo "ok $(basename "$2") $(sha256sum < "$2" | cut -c1-16)"; else echo "FALHOU $2" >&2; ok=0; fi; }
check "$AGENT" "$M/BigFoston/bfagent.exe"
for f in zImage initrd.gz "$IMG"/*.dtb; do f=$(basename "$f"); check "$IMG/$f" "$M/BigFoston/linux/$f"; done
udisksctl unmount -b "$PART" >/dev/null
[ "$ok" = 1 ] && echo "Pronto: pode tirar o cartao e devolve-lo ao FOSTON."
