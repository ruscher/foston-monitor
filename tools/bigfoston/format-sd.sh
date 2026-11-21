#!/bin/sh
# Re-partition and format the FOSTON FS-707DC SD card (seen by the PC as USB
# storage 045e:ffff while Windows CE is at its main menu).
#
#   sudo tools/bigfoston/format-sd.sh [--skip-raw]
#
# Preferred: the card in a PC card reader (found by its filesystem UUID).
# Through the FOSTON's own USB storage mode the writes were lost (Windows CE
# keeps the card mounted on its side).
#
# --skip-raw: no raw image (use when the card has unreadable sectors and a
#             verified file backup exists, e.g. backup/2026-11-19-sd-files).
# The new filesystem is created with a bad-block scan (mkfs.fat -c): sectors
# that fail to read are marked and never used. (On this unit one long dd of
# the card failed once with "Hardware Error" near sector 923840; the full
# read check afterwards found no bad sector: a transient USB/CE failure.)
#
# Safety: refuses any device that is not the FOSTON (USB ids 045e:ffff,
# 1-4 GB), saves a raw image of the whole card first (backup/<date>-sd-raw/,
# with SHA-256) and only then writes: a new MBR with one FAT32 partition
# aligned at 1 MiB, label BIGFOSTON.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[ "$(id -u)" = 0 ] || { echo "rode com sudo" >&2; exit 1; }
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OWNER=${SUDO_USER:-root}
SKIP_RAW=0
[ "${1:-}" = --skip-raw ] && SKIP_RAW=1

DEV=""
for b in /sys/block/sd*; do
    node=$(readlink -f "$b/device")
    while [ "$node" != / ]; do
        if [ -f "$node/idVendor" ]; then
            [ "$(cat "$node/idVendor"):$(cat "$node/idProduct")" = 045e:ffff ] && DEV=/dev/$(basename "$b")
            break
        fi
        node=$(dirname "$node")
    done
done
# Card taken out of the FOSTON and put in a PC card reader (the safe way: in
# USB storage mode Windows CE keeps the card mounted and loses large writes,
# seen 2026-11-19). Identified by its filesystem: UUID 0897-EDDF (this unit's
# original card) or label BIGFOSTON, on a removable disk.
if [ -z "$DEV" ]; then
    for b in /sys/block/sd*; do
        [ "$(cat "$b/removable" 2>/dev/null)" = 1 ] || [ "$(readlink -f "$b" | grep -c usb)" = 1 ] || continue
        for p in /dev/$(basename "$b")?*; do
            [ -b "$p" ] || continue
            id=$(blkid -o value -s UUID "$p" 2>/dev/null || true)
            label=$(blkid -o value -s LABEL "$p" 2>/dev/null || true)
            if [ "$id" = 0897-EDDF ] || [ "$label" = BIGFOSTON ]; then
                DEV=/dev/$(basename "$b")
                echo "Cartao do FOSTON no leitor: $p (UUID $id, rotulo ${label:--})"
            fi
        done
    done
fi
[ -n "$DEV" ] || { echo "Cartao do FOSTON nao encontrado (nem o aparelho 045e:ffff, nem um cartao com UUID 0897-EDDF / rotulo BIGFOSTON)" >&2; exit 1; }
SECTORS=$(cat "/sys/block/$(basename "$DEV")/size")
BYTES=$((SECTORS * 512))
if [ "$BYTES" -lt 1000000000 ] || [ "$BYTES" -gt 4300000000 ]; then
    echo "$DEV tem $BYTES bytes: nao parece o cartao do FOSTON, abortando" >&2
    exit 1
fi
echo "Alvo: $DEV ($BYTES bytes)"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS "$DEV"

for p in "$DEV"?*; do
    [ -b "$p" ] && umount "$p" 2>/dev/null || true
done

if [ "$SKIP_RAW" = 0 ]; then
    OUT="$TOP/backup/$(date +%Y-%m-%d)-sd-raw"
    mkdir -p "$OUT"
    echo "1/3 Imagem bruta do cartao em $OUT/sdcard.img ..."
    dd if="$DEV" of="$OUT/sdcard.img" bs=4M status=progress conv=fsync
    ( cd "$OUT" && sha256sum sdcard.img > sdcard.img.sha256 )
    [ "$(stat -c %s "$OUT/sdcard.img")" = "$BYTES" ] || { echo "imagem incompleta, abortando sem formatar" >&2; exit 1; }
    chown -R "$OWNER" "$OUT"
else
    echo "1/3 Imagem bruta pulada (--skip-raw): use o backup por arquivos verificado"
fi

printf 'Formatar %s agora? Digite SIM: ' "$DEV"
read -r answer
[ "$answer" = SIM ] || { echo "cancelado; nada foi escrito no cartao"; exit 0; }

echo "2/3 Nova tabela de particoes (MBR, FAT32 0x0b, inicio em 1 MiB)..."
printf 'label: dos\nstart=2048, type=b\n' | sfdisk --wipe always "$DEV"
sync
sleep 1
PART="${DEV}1"
[ -b "$PART" ] || partprobe "$DEV" 2>/dev/null || sleep 2
echo "3/3 FAT32 com verificacao de setores ruins (le o cartao inteiro, ~20 min)..."
mkfs.fat -F 32 -s 8 -c -n BIGFOSTON "$PART"
sync
echo "Pronto."
[ "$SKIP_RAW" = 0 ] && echo "Restaurar o cartao antigo: sudo dd if=$OUT/sdcard.img of=$DEV bs=4M conv=fsync"
true
