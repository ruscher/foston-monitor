#!/usr/bin/env python3
"""oal_xref.py - find the MT3351 register addresses used by the Windows CE OAL.

Treats an XIP region of the CE image (BINFS partition) as flat memory, finds
the functions that reference a debug string ("+OALTimerInit(...)" etc.) and
disassembles them, annotating every constant loaded from a literal pool with
the physical address it stands for (the OAL uses the uncached kernel
mapping: VA 0xA0000000+x -> PA per the OEMAddressTable read from the device).

  oal_xref.py IMAGE --offset 0x20000 --base 0x8c003000 --size 0x283000 STRING...

Requires capstone (pip install capstone). Read-only analysis of a backup.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import re
import struct
import sys

# OEMAddressTable of the FS-707DC as read from its page tables (bfagent.py mmu):
# cached VA base, physical base, size. Uncached alias = VA + 0x20000000.
OEM_ADDRESS_TABLE = [
    (0x80000000, 0x80000000, 16 << 20),
    (0x81000000, 0xA0000000, 1 << 20),
    (0x82000000, 0xA2000000, 4 << 20),
    (0x8C000000, 0x00000000, 128 << 20),
    (0x96000000, 0x40000000, 1 << 20),
    (0x97000000, 0x48000000, 1 << 20),
    (0x98000000, 0x50000000, 1 << 20),
]


def va_to_pa(va):
    for base, pa, size in OEM_ADDRESS_TABLE:
        for alias in (base, base + 0x20000000):
            if alias <= va < alias + size:
                return pa + va - alias, alias != base
    return None, None


class Region:
    def __init__(self, data, base):
        self.data, self.base = data, base

    def word(self, va):
        off = va - self.base
        if 0 <= off <= len(self.data) - 4:
            return struct.unpack_from("<I", self.data, off)[0]
        return None

    def string_vas(self, text):
        hits = []
        for enc in ("utf-16-le", "ascii"):
            for m in re.finditer(re.escape(text.encode(enc)), self.data):
                hits.append(self.base + m.start())
        return hits

    def literal_refs(self, va):
        needle = struct.pack("<I", va)
        return [self.base + m.start() for m in re.finditer(re.escape(needle), self.data) if m.start() % 4 == 0]


def function_start(region, md, va, limit=0x2000):
    """Walk back to the nearest 'push {...lr}' / 'stmdb sp!, {...lr}'."""
    for addr in range(va & ~3, va - limit, -4):
        word = region.word(addr)
        if word is None:
            break
        if (word & 0xFFFF4000) == 0xE92D4000:          # stmdb sp!, {..., lr}
            return addr
    return None


def describe_constant(value):
    pa, uncached = va_to_pa(value)
    if pa is not None and pa >= 0x40000000:
        return f"  ; -> PA 0x{pa:08x}{' (uncached)' if uncached else ''}"
    return ""


def disassemble_function(region, md, start, max_insns=400):
    out = []
    count = 0
    code = region.data[start - region.base:start - region.base + max_insns * 4]
    for insn in md.disasm(code, start):
        note = ""
        m = re.match(r"(\w+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]", insn.op_str)
        if insn.mnemonic.startswith("ldr") and m:
            target = insn.address + 8 + int(m.group(2), 0)
            value = region.word(target)
            if value is not None:
                note = f"  ; =0x{value:08x}" + describe_constant(value)
                s = try_string(region, value)
                if s:
                    note += f'  "{s}"'
        out.append(f"  {insn.address:08x}: {insn.mnemonic:<8} {insn.op_str}{note}")
        count += 1
        if count > 8 and (insn.mnemonic.startswith("ldm") and "pc" in insn.op_str or
                          insn.mnemonic == "bx" and insn.op_str == "lr"):
            break
    return out


def try_string(region, va):
    off = va - region.base
    if not 0 <= off < len(region.data) - 4:
        return None
    raw = region.data[off:off + 160]
    if raw[1:2] == b"\0" and 32 <= raw[0] < 127:
        text = raw.decode("utf-16-le", "replace").split("\0")[0]
    else:
        text = raw.split(b"\0")[0].decode("ascii", "replace")
    if len(text) >= 4 and all(31 < ord(c) < 127 for c in text):
        return text[:70]
    return None


def peripheral_constants(region):
    """Every word in the region that points into a peripheral mapping, with counts per 4 KB page."""
    pages = {}
    for off in range(0, len(region.data) - 3, 4):
        value = struct.unpack_from("<I", region.data, off)[0]
        pa, _ = va_to_pa(value)
        if pa is not None and pa >= 0x40000000:
            pages.setdefault(pa & ~0xFFF, set()).add(pa & 0xFFF)
    return pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--offset", type=lambda x: int(x, 0), required=True, help="region offset in the image file")
    ap.add_argument("--size", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True, help="region virtual address")
    ap.add_argument("--pages", action="store_true", help="list peripheral pages referenced anywhere")
    ap.add_argument("strings", nargs="*")
    args = ap.parse_args()
    import capstone
    with open(args.image, "rb") as f:
        f.seek(args.offset)
        region = Region(f.read(args.size), args.base)
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    if args.pages:
        for page, offsets in sorted(peripheral_constants(region).items()):
            print(f"0x{page:08x}: {len(offsets):3d} offsets  " +
                  " ".join(f"{o:03x}" for o in sorted(offsets)[:24]))
    seen = set()
    for text in args.strings:
        for sva in region.string_vas(text):
            for ref in region.literal_refs(sva):
                start = function_start(region, md, ref)
                if start is None or start in seen:
                    continue
                seen.add(start)
                print(f"\n=== {text!r}: string 0x{sva:08x}, literal 0x{ref:08x}, function 0x{start:08x}")
                print("\n".join(disassemble_function(region, md, start)))


if __name__ == "__main__":
    sys.exit(main())
