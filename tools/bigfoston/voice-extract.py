#!/usr/bin/env python3
"""Extract the code section of the FOSTON's own wavedev_drv.dll (Windows CE audio driver)
for bfvoice, the BigFoston call-voice loader (src/bt/bfvoice.c, docs/phone/02).

The section holds MediaTek's proprietary DSP program images (~192 KB) and speech
coefficient tables. This tool reads it from the NAND backup of the user's own device and
the output must stay on that device (SD card: BigFoston/voz/wavedev-text.bin); it is not
distributed with BigFoston.

  voice-extract.py OUT.bin [--nand DSK9-msflash.img]

OUT.bin = the raw .text section, loaded at VA 0x03BD1000 (bfvoice reads it by VA).
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "cere"))

TEXT_VA = 0x03BD1000
# the FS-707DC firmware this was analysed on (bfvoice refuses other images)
SHA256 = "3f2eeb466764bbdfc4849397c4fbfd21458d10acfb73fc9876cfda1e40b030d7"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--nand", help="NAND image (default: backup/2026-11-19-nand/DSK9-msflash.img)")
    args = ap.parse_args()
    if args.nand:
        os.environ["BF_NAND_IMG"] = args.nand
    import mod                      # noqa: E402  (reads BF_NAND_IMG at import)

    _img, _e32, secs = mod.module("wavedev_drv.dll")
    text = [s for s in secs if s["realaddr"] == TEXT_VA]
    if not text:
        raise SystemExit("wavedev_drv.dll: code section not found at 0x%08x" % TEXT_VA)
    s = text[0]
    blob = bytes(s["blob"][:min(s["psize"], s["vsize"])])
    digest = hashlib.sha256(blob).hexdigest()
    if digest != SHA256:
        raise SystemExit("wavedev_drv.dll differs from the analysed firmware (sha256 %s): not supported" % digest)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("%s: %d bytes, sha256 %s" % (args.out, len(blob), digest))


if __name__ == "__main__":
    main()
