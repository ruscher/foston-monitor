#!/usr/bin/env python3
"""cedll.py - disassemble functions of a DLL kept in a Windows CE 5 XIP region, found by a
debug string they print (read-only analysis of the NAND backup).

  cedll.py IMAGE --offset 0x20000 --base 0x8c003000 --module mtk_bthuart.dll "string" ...
  cedll.py IMAGE ... --module X --at 0x01f21234 [--count 200]    (disassemble at a VA)
  cedll.py IMAGE ... --module X --dump 0x01f40000 0x100          (hex dump at a VA)

An XIP DLL runs in place but refers to its own data by the virtual address of its slot
(e32 vbase + section rva), not by the ROM address: this tool maps those addresses to the
ROM bytes through the o32 section table, finds the literal-pool words that point to the
string, walks back to the function start and disassembles it (Capstone, ARM mode),
showing the strings and the peripheral-looking constants each literal holds.

Requires capstone (pip install capstone). SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import re
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from cerom import Image, modules  # noqa: E402


class Dll:
    def __init__(self, img, mod):
        self.img, self.mod, self.secs = img, mod, []
        for s in mod["sections"]:
            if s["flags"] & 0x2000:          # compressed: not readable here
                continue
            n = min(s["psize"], s["vsize"] or s["psize"])
            try:
                blob = img.at(s["dataptr"], n)
            except ValueError:
                continue
            self.secs.append((mod["vbase"] + s["rva"], blob, s["flags"]))

    def read(self, va, n):
        for base, blob, _ in self.secs:
            if base <= va and va + n <= base + len(blob):
                return blob[va - base:va - base + n]
        return None

    def word(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if b else None

    def string(self, va):
        b = self.read(va, 2)
        if b is None:
            return None
        for base, blob, _ in self.secs:
            if base <= va < base + len(blob):
                raw = blob[va - base:va - base + 400]
                if len(raw) > 3 and raw[1] == 0 and 0x20 <= raw[0] < 0x7f:
                    t = raw.decode("utf-16-le", "replace").split("\0")[0]
                else:
                    t = raw.split(b"\0")[0].decode("latin-1")
                return t if len(t) >= 3 and all(0x20 <= ord(c) < 0x7f or c in "\r\n\t" for c in t) else None
        return None

    def code(self):
        return [(b, blob) for b, blob, f in self.secs if f & 0x20]

    def find_string_vas(self, text):
        out = []
        for enc in ("utf-16-le", "ascii"):
            pat = re.escape(text.encode(enc))
            for base, blob, _ in self.secs:
                for m in re.finditer(pat, blob):
                    out.append(base + m.start())
        return out

    def literal_refs(self, value):
        out = []
        for base, blob in self.code():
            for off in range(0, len(blob) - 3, 4):
                if struct.unpack_from("<I", blob, off)[0] == value:
                    out.append(base + off)
        return out


def func_start(dll, md, va, limit=0x3000):
    """Nearest earlier instruction 'push {..., lr}' (stmdb sp!) — ARM mode."""
    a = va & ~3
    while a > va - limit:
        w = dll.word(a)
        if w is not None and (w & 0xFFFF4000) == 0xE92D4000:
            return a
        a -= 4
    return None


def annotate(dll, value):
    s = dll.string(value)
    if s is not None:
        return f'"{s[:70]}"'
    if 0x80000000 <= value < 0x81000000 or 0x40000000 <= value < 0x40100000:
        return "periférico?"
    return ""


def disasm(dll, md, start, count):
    code = dll.read(start, count * 4)
    if not code:
        print(f"  (sem código em 0x{start:08x})")
        return
    for ins in md.disasm(code, start):
        note = ""
        m = re.search(r"\[pc, #(-?0x[0-9a-f]+|-?\d+)\]", ins.op_str)
        if ins.mnemonic.startswith("ldr") and m:
            lit = ins.address + 8 + int(m.group(1), 0)
            v = dll.word(lit)
            if v is not None:
                note = f"  ; =0x{v:08x} {annotate(dll, v)}"
        print(f"  {ins.address:08x}: {ins.mnemonic:<8} {ins.op_str}{note}")
        if ins.mnemonic in ("bx", "ldmia", "pop", "ldm") and ("lr" in ins.op_str or "pc" in ins.op_str) \
                and ins.address > start + 8 and count > 0 and not args_all:
            break


args_all = False


def main():
    global args_all
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--offset", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x5b5000)
    ap.add_argument("--module", required=True)
    ap.add_argument("--at", type=lambda x: int(x, 0))
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--all", action="store_true", help="do not stop at the first return")
    ap.add_argument("--dump", nargs=2, type=lambda x: int(x, 0))
    ap.add_argument("strings", nargs="*")
    a = ap.parse_args()
    args_all = a.all
    with open(a.image, "rb") as f:
        f.seek(a.offset)
        img = Image(f.read(a.size), a.base)
    romhdr = struct.unpack_from("<I", img.data, 0x44)[0]
    mod = next((m for m in modules(img, romhdr) if m["name"].lower() == a.module.lower()), None)
    if not mod:
        sys.exit(f"{a.module}: módulo não encontrado")
    dll = Dll(img, mod)
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    if a.dump:
        b = dll.read(a.dump[0], a.dump[1])
        for i in range(0, len(b or b""), 16):
            print(f"  {a.dump[0] + i:08x}: " + " ".join(f"{x:02x}" for x in b[i:i + 16]))
        return
    if a.at is not None:
        disasm(dll, md, a.at, a.count)
        return
    for text in a.strings:
        for sva in dll.find_string_vas(text):
            for ref in dll.literal_refs(sva):
                st = func_start(dll, md, ref)
                print(f"=== {text!r}: texto 0x{sva:08x}, literal 0x{ref:08x}, função 0x{st or 0:08x}")
                if st:
                    disasm(dll, md, st, a.count)


if __name__ == "__main__":
    main()
