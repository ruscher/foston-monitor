#!/bin/sh
# Bring-up reference for bftest step 6: CRC32 of each 4 KB page of /bin/busybox.
set -eu
python3 - "$TARGET_DIR" <<'PY'
import sys, zlib
t = sys.argv[1]
data = open(f"{t}/bin/busybox", "rb").read()
with open(f"{t}/etc/busybox.pagecrc", "w") as f:
    for i in range(0, len(data), 4096):
        f.write("%08x\n" % (zlib.crc32(data[i:i + 4096]) & 0xFFFFFFFF))
PY

# SSH: authorise the builder's public key (never committed). Override with
# BIGFOSTON_SSH_PUBKEY=/path/key.pub; without a key, SSH stays closed.
KEY=${BIGFOSTON_SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}
if [ -f "$KEY" ]; then
    install -d -m 700 "$TARGET_DIR/root/.ssh"
    install -m 600 "$KEY" "$TARGET_DIR/root/.ssh/authorized_keys"
    echo "post-build: SSH key $(cut -d' ' -f3 "$KEY") authorised for root"
else
    rm -f "$TARGET_DIR/root/.ssh/authorized_keys"
    echo "post-build: no SSH public key ($KEY), SSH login disabled"
fi

# SSH host key: the rootfs lives in RAM, so dropbear would make a new key at
# every boot and the PC would warn of a changed host. A key kept outside git
# (firmware/build/, generated once on the device) is installed if present.
HOSTKEY=${BIGFOSTON_SSH_HOSTKEY:-$(cd "$(dirname "$0")/../../.." && pwd)/build/dropbear_ed25519_host_key}
if [ -f "$HOSTKEY" ]; then
    [ -L "$TARGET_DIR/etc/dropbear" ] && rm -f "$TARGET_DIR/etc/dropbear"
    install -d -m 700 "$TARGET_DIR/etc/dropbear"
    install -m 600 "$HOSTKEY" "$TARGET_DIR/etc/dropbear/dropbear_ed25519_host_key"
    echo "post-build: fixed SSH host key installed"
fi

# mpg123 programs: the shell links libmpg123 itself (~1.4 MB of the initrd)
rm -f "$TARGET_DIR/usr/bin/mpg123" "$TARGET_DIR/usr/bin/mpg123-id3dump" "$TARGET_DIR/usr/bin/mpg123-strip" "$TARGET_DIR/usr/bin/out123"
