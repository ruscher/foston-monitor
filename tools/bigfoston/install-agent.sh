#!/bin/sh
# Copy the BigFoston diagnostic agent to a FOSTON in USB storage mode (045e:ffff).
# Writes only <volume>/BigFoston/bfagent.exe (+ .sha256). The device caches its
# file system while exported over USB: reset it after copying.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
EXE="$TOP/src/wince/agent/build/bfagent.exe"
[ -f "$EXE" ] || make -C "$TOP/src/wince/agent"

find_block() {
    for dev in /sys/block/sd*; do
        node=$(readlink -f "$dev/device")
        while [ "$node" != / ]; do
            if [ -f "$node/idVendor" ]; then
                [ "$(cat "$node/idVendor"):$(cat "$node/idProduct")" = 045e:ffff ] && basename "$dev"
                break
            fi
            node=$(dirname "$node")
        done
    done
}

BLOCK=$(find_block | head -n1)
if [ -z "$BLOCK" ]; then
    echo "FOSTON em modo pendrive (045e:ffff) nao encontrado." >&2
    exit 1
fi
PART=/dev/${BLOCK}1
[ -b "$PART" ] || PART=/dev/$BLOCK
MNT=$(lsblk -no MOUNTPOINT "$PART" | head -n1)
if [ -z "$MNT" ]; then
    udisksctl mount -b "$PART" >/dev/null
    MNT=$(lsblk -no MOUNTPOINT "$PART" | head -n1)
fi
echo "Volume do FOSTON: $MNT ($PART)"
mkdir -p "$MNT/BigFoston"
cp "$EXE" "$MNT/BigFoston/bfagent.exe.new"
sync
WANT=$(sha256sum < "$EXE" | cut -d' ' -f1)
GOT=$(sha256sum < "$MNT/BigFoston/bfagent.exe.new" | cut -d' ' -f1)
[ "$WANT" = "$GOT" ] || { echo "verificacao falhou" >&2; exit 1; }
mv -f "$MNT/BigFoston/bfagent.exe.new" "$MNT/BigFoston/bfagent.exe"
echo "$WANT  bfagent.exe" > "$MNT/BigFoston/bfagent.exe.sha256"
sync
echo "Copiado: BigFoston\\bfagent.exe (sha256 $WANT)"
echo "Agora: desconecte o cabo, de reset no FOSTON e aponte o Caminho da navegacao para BigFoston\\bfagent.exe."
