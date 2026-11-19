#!/bin/sh
# Make a counterfeit ("fake capacity") SD card usable at its REAL size, for the
# FOSTON / BigFoston OS. Uses f3 (Fight Flash Fraud: sudo pacman -S f3).
#
#   sudo tools/bigfoston/fix-fake-sd.sh /dev/sdX
#
# 1. checks the target is a removable USB card-reader disk (never the FOSTON
#    itself, 045e:*, and never an internal SATA/NVMe disk) and asks for SIM;
# 2. f3probe --destructive: finds the real capacity (ERASES the card);
# 3. f3fix: one partition covering only the real capacity;
# 4. FAT32 (label BIGFOSTON) with a BigFoston/ folder.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[ "$(id -u)" = 0 ] || { echo "rode com sudo" >&2; exit 1; }
DEV=${1:-}
[ -b "$DEV" ] || { echo "uso: sudo $0 /dev/sdX" >&2; exit 1; }
for t in f3probe f3fix mkfs.fat partprobe; do
    command -v "$t" >/dev/null || { echo "falta $t (sudo pacman -S f3 dosfstools parted)" >&2; exit 1; }
done
NAME=$(basename "$DEV")
SYS=/sys/block/$NAME
[ -d "$SYS" ] || { echo "$DEV nao e um disco inteiro (use /dev/sdX, nao /dev/sdX1)" >&2; exit 1; }
case "$(readlink -f "$SYS")" in *usb*) ;; *) echo "$DEV nao e USB: recusado" >&2; exit 1 ;; esac
node=$(readlink -f "$SYS/device")
while [ "$node" != / ] && [ ! -f "$node/idVendor" ]; do node=$(dirname "$node"); done
[ "$(cat "$node/idVendor")" != 045e ] || { echo "$DEV e o proprio FOSTON: recusado (use um leitor de cartao)" >&2; exit 1; }
if grep -q "^$DEV" /proc/mounts; then
    for p in $(grep "^$DEV" /proc/mounts | cut -d' ' -f1); do umount "$p"; done
fi
echo "Alvo: $DEV  $(cat "$node/manufacturer" 2>/dev/null) $(cat "$node/product" 2>/dev/null)  anuncia $(( $(cat "$SYS/size") * 512 / 1000000000 )) GB"
lsblk -o NAME,SIZE,FSTYPE,LABEL "$DEV"
printf 'TUDO no cartao sera apagado. Digite SIM para continuar: '
read -r answer
[ "$answer" = SIM ] || { echo "cancelado"; exit 0; }

echo "1/3 f3probe: medindo a capacidade real (alguns minutos)..."
LOG=$(mktemp)
f3probe --destructive --time-ops "$DEV" | tee "$LOG"
LAST=$(sed -n 's/.*--last-sec=\([0-9]*\).*/\1/p' "$LOG" | head -n1)
if [ -z "$LAST" ]; then
    if grep -qi "is the real thing" "$LOG"; then
        echo "f3probe: o cartao NAO e falso; formatando com o tamanho inteiro"
        LAST=$(( $(cat "$SYS/size") - 1 ))
    else
        echo "f3probe nao informou a capacidade real; nada foi corrigido (veja a saida acima)" >&2
        exit 1
    fi
fi
REAL_MB=$(( (LAST + 1) * 512 / 1048576 ))
echo "Capacidade real: $REAL_MB MiB (ultimo setor bom $LAST)"

echo "2/3 f3fix: particao com o tamanho real..."
f3fix --last-sec="$LAST" --fs-type=fat32 "$DEV"
sync
partprobe "$DEV" 2>/dev/null || true
sleep 2
PART=${DEV}1
[ -b "$PART" ] || { echo "particao $PART nao apareceu; reconecte o leitor e rode mkfs.fat -F 32 -n BIGFOSTON $PART" >&2; exit 1; }

echo "3/3 FAT32 (rotulo BIGFOSTON)..."
mkfs.fat -F 32 -n BIGFOSTON "$PART"
MNT=$(mktemp -d)
mount "$PART" "$MNT"
mkdir -p "$MNT/BigFoston/linux"
sync
umount "$MNT"
rmdir "$MNT"
echo "Pronto: $PART tem $REAL_MB MiB reais, FAT32, com a pasta BigFoston/."
echo "Agora (sem sudo): tools/bigfoston/sd-install.sh"
