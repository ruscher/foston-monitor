#!/bin/sh
# Teste de gravação do cartão SD no FOSTON (BigFoston OS, BusyBox).
# Roda NO APARELHO, como root, com o cartão montado com gravação em /mnt/sd:
#   ssh root@192.168.7.2 sh -s < tools/bigfoston/sd-write-test.sh
# Grava arquivos aleatórios em /mnt/sd/BigFoston/teste, limpa o cache, relê e
# compara o MD5; repete depois de desmontar e montar de novo. Não toca em
# outros arquivos do cartão. Ao final, apaga os arquivos de teste.
# Se tudo passar e PASS_MARK=1, grava BigFoston/linux-rw com o número de série
# do cartão (libera a gravação nos próximos boots, ver /etc/init.d/S30sdcard).
set -u
MNT=/mnt/sd
DIR=$MNT/BigFoston/teste
DEV=/dev/mmcblk0p1
fail() { echo "FALHOU: $*"; exit 1; }

grep -q " $MNT vfat rw" /proc/mounts || fail "$MNT não está montado com gravação"
mkdir -p "$DIR" || fail "mkdir"
: > /tmp/sdtest.md5
i=0
for size_kb in 1 4 64 1024 16384 65536; do
    i=$((i + 1))
    f="$DIR/t$i.bin"
    t0=$(cut -d' ' -f1 /proc/uptime)
    sum=$(dd if=/dev/urandom bs=1024 count=$size_kb 2>/dev/null | tee "$f" | md5sum | cut -d' ' -f1)
    sync
    t1=$(cut -d' ' -f1 /proc/uptime)
    echo "$sum  $f" >> /tmp/sdtest.md5
    echo "gravado t$i.bin ${size_kb} KB em $(echo "$t0 $t1" | awk '{printf "%.2f", $2-$1}') s"
done
# 200 arquivos pequenos (tabela FAT e diretório)
mkdir -p "$DIR/pequenos"
n=0
while [ $n -lt 200 ]; do
    echo "arquivo $n $(cat /proc/sys/kernel/random/uuid)" > "$DIR/pequenos/p$n.txt"
    n=$((n + 1))
done
(cd "$DIR/pequenos" && md5sum p*.txt > /tmp/sdtest-small.md5)
sync

echo 3 > /proc/sys/vm/drop_caches
md5sum -c /tmp/sdtest.md5 >/dev/null || fail "conferência após limpar o cache"
(cd "$DIR/pequenos" && md5sum -c /tmp/sdtest-small.md5 >/dev/null) || fail "arquivos pequenos"
echo "conferência 1 (cache limpo): ok"

umount "$MNT" || fail "umount"
mount -t vfat -o rw,noatime,flush,iocharset=iso8859-1,utf8,errors=remount-ro "$DEV" "$MNT" || fail "mount"
md5sum -c /tmp/sdtest.md5 >/dev/null || fail "conferência após remontar"
(cd "$DIR/pequenos" && md5sum -c /tmp/sdtest-small.md5 >/dev/null) || fail "arquivos pequenos após remontar"
echo "conferência 2 (remontado): ok"

echo 3 > /proc/sys/vm/drop_caches          # mede o cartão, não o cache
t0=$(cut -d' ' -f1 /proc/uptime)
dd if="$DIR/t6.bin" of=/dev/null bs=65536 2>/dev/null
t1=$(cut -d' ' -f1 /proc/uptime)
echo "leitura de 64 MB em $(echo "$t0 $t1" | awk '{printf "%.2f", $2-$1}') s"

rm -r "$DIR" && sync || fail "apagar os arquivos de teste"
dmesg | grep -iE "mmc0|mmcblk0|FAT-fs" | tail -5
if [ "${PASS_MARK:-0}" = 1 ]; then
    serial=$(cat /sys/block/mmcblk0/device/serial)
    printf 'serial=%s\n' "$serial" > "$MNT/BigFoston/linux-rw" && sync
    echo "marca gravada: $MNT/BigFoston/linux-rw (serial=$serial)"
fi
echo "TESTE OK"
