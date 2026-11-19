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
