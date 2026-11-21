#!/usr/bin/env python3
"""Extract the FOSTON's own gps.dll (Windows CE ROM module) into a BFX1 image
for the BigFoston GPS loader (src/gps).

The DLL is MediaTek's proprietary MNL driver: this tool reads it from the NAND
backup of the user's own device and the output must stay on that device (SD
card); it is not distributed with BigFoston.

  gps-extract.py OUT.bfx [--nand DSK9-msflash.img]

BFX1 layout (little endian):
  "BFX1" u32 entry_va u32 nseg u32 nimp u32 nexp
  nseg x { u32 va, u32 vsize, u32 filesize, u32 prot (1 r, 2 w, 4 x), data[filesize] }
  nimp x { u32 iat_va, u16 ordinal, char dll[16], char name[46] }
  nexp x { u32 va, char name[28] }
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "cere"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--nand", help="NAND image (default: backup/2026-11-19-nand/DSK9-msflash.img)")
    args = ap.parse_args()
    if args.nand:
        os.environ["BF_NAND_IMG"] = args.nand
    import mod                      # noqa: E402  (reads BF_NAND_IMG at import)
    from exps import exports        # noqa: E402

    img, e32, secs = mod.module("gps.dll")
    vbase = e32[3]
    m = mod.Mem(secs)

    segs = []
    for s in secs:
        f = s["flags"]
        prot = (1 if f & 0x40000000 else 0) | (2 if f & 0x80000000 else 0) | (4 if f & 0x20000000 else 0)
        vsize = max(s["vsize"], 1)
        filesize = min(s["psize"], vsize)
        segs.append((s["realaddr"], vsize, s["blob"][:filesize], prot))

    imps = []
    imp = vbase + e32[13]
    i = 0
    while True:
        ilt, _ts, _fw, nm, iat = [m.u32(imp + 20 * i + 4 * k) for k in range(5)]
        if not nm:
            break
        dll = m.cstr(vbase + nm)
        ex = exports(dll)
        j = 0
        while True:
            v = m.u32(vbase + ilt + 4 * j)
            if not v:
                break
            if not v & 0x80000000:
                raise SystemExit(f"import by name not supported ({dll})")
            o = v & 0xFFFF
            imps.append((vbase + iat + 4 * j, o, dll, ex.get(o, f"ord{o}")))
            j += 1
        i += 1

    # exports: names and addresses
    ed = vbase + e32[11]
    f = [m.u32(ed + 4 * k) for k in range(10)]
    base, nfun, nnames, afun, anames, aords = f[4], f[5], f[6], f[7], f[8], f[9]
    exps = []
    for k in range(nnames):
        name = m.cstr(vbase + m.u32(vbase + anames + 4 * k))
        s_, off = m.find(vbase + aords + 2 * k)
        idx = struct.unpack_from("<H", s_["blob"], off)[0]
        exps.append((vbase + m.u32(vbase + afun + 4 * idx), name))

    out = bytearray(b"BFX1" + struct.pack("<IIII", vbase + e32[2], len(segs), len(imps), len(exps)))
    for va, vsize, data, prot in segs:
        out += struct.pack("<IIII", va, vsize, len(data), prot) + data
    for iat, o, dll, name in imps:
        out += struct.pack("<IH16s46s", iat, o, dll.encode()[:15], name.encode()[:45])
    for va, name in exps:
        out += struct.pack("<I28s", va, name.encode()[:27])
    with open(args.out, "wb") as fo:
        fo.write(out)
    print(f"{args.out}: entry {vbase + e32[2]:#x}, {len(segs)} segmentos, {len(imps)} imports, "
          f"{len(exps)} exports, {len(out)} bytes, sha256 {hashlib.sha256(out).hexdigest()[:16]}")
    for va, vsize, data, prot in segs:
        print(f"  {va:#010x} {vsize:#9x} ({len(data):#x} da ROM) prot {prot}")


if __name__ == "__main__":
    main()
