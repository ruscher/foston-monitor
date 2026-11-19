#!/usr/bin/env python3
"""mkboot.py - prepare the device tree for a BigFoston boot.

Writes /chosen/linux,initrd-start and linux,initrd-end for the initrd that
the bfagent loader copies to INITRD_PA, and optionally the kernel command
line. Needs fdtput (dtc package; Buildroot: host-dtc).

Load layout shared with src/wince/agent/bfagent.c (cmd_boot):
  zImage  0x00800000  (max 8 MB; decompresses itself to 0x00008000)
  initrd  0x01000000  (max 12 MB)
  DTB     0x01F00000  (max 64 KB)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import shutil
import subprocess
import sys
import zlib

KERNEL_PA = 0x00800000
INITRD_PA = 0x01000000
DTB_PA = 0x01F00000
LIMITS = {"zImage": 8 << 20, "initrd": 12 << 20, "dtb": 64 << 10}


def fdtput(dtb, *args):
    subprocess.run(["fdtput", dtb, *args], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dtb", required=True, help="device tree built by the kernel")
    ap.add_argument("--initrd", help="initrd image (sets the range in /chosen)")
    ap.add_argument("--bootargs", help="kernel command line (default: keep the one in the DTB)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if not shutil.which("fdtput"):
        sys.exit("mkboot: fdtput not found (install dtc)")
    shutil.copyfile(args.dtb, args.out)
    if args.initrd:
        size = os.path.getsize(args.initrd)
        if size > LIMITS["initrd"]:
            sys.exit(f"mkboot: initrd of {size} bytes exceeds {LIMITS['initrd']}")
        fdtput(args.out, "-t", "x", "/chosen", "linux,initrd-start", f"{INITRD_PA:x}")
        fdtput(args.out, "-t", "x", "/chosen", "linux,initrd-end", f"{INITRD_PA + size:x}")
        with open(args.initrd, "rb") as f:
            crc = zlib.crc32(f.read()) & 0xFFFFFFFF
        # checked by the kernel (mt3351-bootcon) to locate any corruption of the hand-over
        fdtput(args.out, "-t", "x", "/chosen", "bigfoston,initrd-crc32", f"{crc:x}")
        print(f"initrd crc32 {crc:08x}")
    # The board has no hardware RNG and few entropy sources: the kernel's
    # CRNG only became ready after 255 s on the device (2026-11-19), stalling
    # anything that needs random numbers. Seed it from the PC at each build.
    fdtput(args.out, "-t", "bx", "/chosen", "rng-seed", *[f"{b:02x}" for b in os.urandom(64)])
    if args.bootargs is not None:
        fdtput(args.out, "-t", "s", "/chosen", "bootargs", args.bootargs)
    if os.path.getsize(args.out) > LIMITS["dtb"]:
        sys.exit("mkboot: DTB too large")
    print(f"{args.out}: initrd {'0x%08x+%d' % (INITRD_PA, size) if args.initrd else 'none'}")


if __name__ == "__main__":
    main()
