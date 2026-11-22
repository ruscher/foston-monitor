#!/usr/bin/env python3
"""Extract the code section of the FOSTON's own wavedev_drv.dll (Windows CE audio driver)
for bfvoice, the BigFoston call-voice loader (src/bt/bfvoice.c, docs/phone/02).

The section holds MediaTek's proprietary DSP program images (~192 KB) and speech
coefficient tables. This tool reads it from the NAND backup of the user's own device and
the output must stay on that device (SD card: BigFoston/voz/wavedev-text.bin); it is not
distributed with BigFoston.

  voice-extract.py OUT.bin [--patch PATCH.bin] [--nand DSK9-msflash.img]

OUT.bin = the raw .text section, loaded at VA 0x03BD1000 (bfvoice reads it by VA).
PATCH.bin = the MT6611 ROM patch that mtk_bthuart.dll (Radio_WritePatch) sends to this
FOSTON's chip (version 3, revision 0x8B: 0x1CDC bytes at VA 0x01F2CFE0); bfbt-attach
sends it like Windows CE (SD card: BigFoston/bt/mt6611-patch.bin).
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
    ap.add_argument("--patch", help="also write the MT6611 patch here")
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
    if args.patch:
        _i, _e, bsecs = mod.module("mtk_bthuart.dll")
        t = [x for x in bsecs if x["realaddr"] == 0x01F21000][0]
        tb = bytes(t["blob"][:min(t["psize"], t["vsize"])])
        patch = tb[0x01F2CFE0 - 0x01F21000:0x01F2CFE0 - 0x01F21000 + 0x1CDC]
        with open(args.patch, "wb") as f:
            f.write(patch)
        print("%s: %d bytes, sha256 %s" % (args.patch, len(patch), hashlib.sha256(patch).hexdigest()))


if __name__ == "__main__":
    main()
