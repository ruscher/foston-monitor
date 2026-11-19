#!/usr/bin/env python3
"""cerom.py - list the modules of a Windows CE 5 XIP region and the peripheral
addresses each one references (read-only analysis of the NAND backup).

  cerom.py IMAGE --offset 0x722000 --base 0x8c705000 [--romhdr 0x8dcb89c0] [--periph]

--offset  file offset of the region (BINFS partition start + region start)
--base    virtual address of the region (ROMHDR.physfirst)
--romhdr  virtual address of the ROMHDR (word at region offset 0x44, after "ECEC")

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import struct
import sys
from collections import defaultdict

ROMHDR = struct.Struct("<17I2H3I")
TOC = struct.Struct("<I8sIIIII")
O32 = struct.Struct("<6I")
# e32_rom (CE 5): objcnt, imageflags, entryrva, vbase, subsys major/minor, stackmax, vsize,
# sect14rva, sect14size, timestamp, 9 x (rva, size), subsys
E32 = struct.Struct("<HHIIHHIIIII18IH")


class Image:
    def __init__(self, data, base):
        self.data, self.base = data, base

    def at(self, va, n):
        off = va - self.base
        if not 0 <= off <= len(self.data) - n:
            raise ValueError(f"VA 0x{va:08x} outside the region")
        return self.data[off:off + n]

    def cstr(self, va):
        off = va - self.base
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")


def modules(img, romhdr_va):
    h = ROMHDR.unpack(img.at(romhdr_va, ROMHDR.size))
    nummods = h[4]
    out = []
    for i in range(nummods):
        attrs, _, size, name_va, e32_va, o32_va, load_va = TOC.unpack(
            img.at(romhdr_va + ROMHDR.size + i * TOC.size, TOC.size))
        e32 = E32.unpack(img.at(e32_va, E32.size))
        objcnt, vbase, vsize = e32[0], e32[3], e32[7]
        sections = []
        for k in range(objcnt):
            vsz, rva, psize, dataptr, realaddr, flags = O32.unpack(img.at(o32_va + k * O32.size, O32.size))
            sections.append({"rva": rva, "vsize": vsz, "psize": psize, "dataptr": dataptr,
                             "realaddr": realaddr, "flags": flags})
        out.append({"name": img.cstr(name_va), "size": size, "vbase": vbase, "vsize": vsize,
                    "sections": sections})
    return out


def periph_refs(img, mod, lo=0x40000000, hi=0xC0000000):
    """Word constants in the module's uncompressed sections that look like peripheral PAs."""
    found = defaultdict(set)
    for sec in mod["sections"]:
        if sec["flags"] & 0x2000:            # IMAGE_SCN_COMPRESSED
            continue
        try:
            blob = img.at(sec["dataptr"], min(sec["psize"], sec["vsize"] or sec["psize"]))
        except ValueError:
            continue
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, off)[0]
            if 0x80000000 <= v < 0x81000000 or 0x40000000 <= v < 0x40100000 or \
               0x48000000 <= v < 0x48100000 or 0x50000000 <= v < 0x50100000 or \
               0xA0000000 <= v < 0xA0100000 or 0xA2000000 <= v < 0xA2400000:
                if v & 0xFFF and v & 3:
                    continue
                found[v & ~0xFFF].add(v & 0xFFF)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--offset", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x1800000)
    ap.add_argument("--romhdr", type=lambda x: int(x, 0))
    ap.add_argument("--periph", action="store_true")
    ap.add_argument("--only", nargs="*", help="module names to show")
    args = ap.parse_args()
    with open(args.image, "rb") as f:
        f.seek(args.offset)
        img = Image(f.read(args.size), args.base)
    romhdr = args.romhdr or struct.unpack_from("<I", img.data, 0x44)[0]
    for mod in modules(img, romhdr):
        if args.only and mod["name"].lower() not in [n.lower() for n in args.only]:
            continue
        flags = ",".join(f"{s['flags']:x}" for s in mod["sections"])
        print(f"{mod['name']:<28} vbase=0x{mod['vbase']:08x} vsize={mod['vsize']:>8} sections={flags}")
        if args.periph:
            for page, offs in sorted(periph_refs(img, mod).items()):
                print(f"    0x{page:08x}: " + " ".join(f"{o:03x}" for o in sorted(offs)[:20]))


if __name__ == "__main__":
    sys.exit(main())
