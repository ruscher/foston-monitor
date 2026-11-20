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
