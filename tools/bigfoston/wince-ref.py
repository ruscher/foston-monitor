#!/usr/bin/env python3
"""wince-ref.py - build a browsable reference of the FOSTON's Windows CE firmware from the
NAND backup, for reverse engineering (docs/18-wince-referencia.md).

  wince-ref.py [--backup backup] [--out backup/wince-referencia]

Read-only on the backup. Writes, under --out (inside backup/, which git ignores: the
firmware has proprietary parts and must not be published):

  MANIFEST.md          sources with SHA-256 (checked now), NAND layout, XIP regions, counts
  SHA256SUMS           every backup file, to check the copy later (sha256sum -c)
  rom/<region>/modules.tsv   module name, size, vbase, vsize, sections (rva, sizes, flags)
  rom/<region>/files.tsv     ROM files: name, real/compressed size, attributes, offset
  rom/<region>/mod/<name>/<rva>.bin   uncompressed module sections (as in the ROM)
  rom/<region>/files/<name>           uncompressed ROM files
  partitions/<n>/      symlinks to the FAT partitions already extracted in the backup

Compressed ROM data (CE "CECompress") is listed but not extracted.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cerom import E32, O32, ROMHDR, TOC, Image  # noqa: E402

FILES = struct.Struct("<I8sIIII")   # attributes, filetime, real size, compressed size, name, offset
SECTOR = 2048                       # DSK9 uses 2048-byte sectors (MBR LBAs count them)
PART_TYPES = {0x21: "BINFS (ROM XIP do CE)", 0x04: "FAT16", 0x0B: "FAT32", 0x0C: "FAT32 LBA"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def safe(name):
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in name) or "_"


def regions(data):
    """(file offset, VA base, ROMHDR VA) of each XIP region ('ECEC' at region + 0x40)."""
    out, off = [], 0
    while (off := data.find(b"ECEC", off)) >= 0:
        if off % 0x1000 == 0x40:
            romhdr = struct.unpack_from("<I", data, off + 4)[0]
            start = off - 0x40
            # physfirst (ROMHDR word 2) is the VA of the region start
            for base_guess in range(0x8C000000, 0x8E000000, 0x1000):
                rel = romhdr - base_guess
                if 0 <= rel < len(data) - start - ROMHDR.size:
                    h = ROMHDR.unpack_from(data, start + rel)
                    if h[2] == base_guess:
                        out.append((start, base_guess, romhdr))
                        break
        off += 4
    return out


def index_region(data, start, base, romhdr, outdir, log):
    size = len(data) - start
    img = Image(data[start:start + size], base)
    h = ROMHDR.unpack(img.at(romhdr, ROMHDR.size))
    nummods, numfiles = h[4], h[12]
    os.makedirs(outdir, exist_ok=True)
    mods_x = files_x = 0
    with open(os.path.join(outdir, "modules.tsv"), "w") as mt:
        mt.write("nome\ttamanho\tvbase\tvsize\tseções (rva:vsize/psize:flags, c = comprimida)\n")
        for i in range(nummods):
            attrs, _, msize, name_va, e32_va, o32_va, _load = TOC.unpack(
                img.at(romhdr + ROMHDR.size + i * TOC.size, TOC.size))
            name = img.cstr(name_va)
            e32 = E32.unpack(img.at(e32_va, E32.size))
            objcnt, vbase, vsize = e32[0], e32[3], e32[7]
            secs = []
            for k in range(objcnt):
                vsz, rva, psize, dataptr, _real, flags = O32.unpack(img.at(o32_va + k * O32.size, O32.size))
                comp = bool(flags & 0x2000)
                secs.append("0x%x:%x/%x:%08x%s" % (rva, vsz, psize, flags, "c" if comp else ""))
                if not comp and psize:
                    try:
                        blob = img.at(dataptr, min(psize, vsz or psize))
                    except ValueError:
                        continue
                    d = os.path.join(outdir, "mod", safe(name))
                    os.makedirs(d, exist_ok=True)
                    with open(os.path.join(d, "%08x.bin" % rva), "wb") as f:
                        f.write(blob)
                    mods_x += 1
            mt.write("%s\t%d\t0x%08x\t0x%x\t%s\n" % (name, msize, vbase, vsize, " ".join(secs)))
    ftoc = romhdr + ROMHDR.size + nummods * TOC.size
    with open(os.path.join(outdir, "files.tsv"), "w") as ft:
        ft.write("nome\ttamanho real\ttamanho na ROM\tatributos\tVA\n")
        for i in range(numfiles):
            attrs, _t, real, comp, name_va, load = FILES.unpack(img.at(ftoc + i * FILES.size, FILES.size))
            name = img.cstr(name_va)
            ft.write("%s\t%d\t%d\t0x%x\t0x%08x\n" % (name, real, comp, attrs, load))
            if real == comp and real:
                try:
                    blob = img.at(load, real)
                except ValueError:
                    continue
                d = os.path.join(outdir, "files")
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, safe(name)), "wb") as f:
                    f.write(blob)
                files_x += 1
    log.append("| 0x%x | 0x%08x | 0x%08x | %d módulos (%d seções extraídas) | %d arquivos (%d extraídos) |"
               % (start, base, romhdr, nummods, mods_x, numfiles, files_x))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../..")
    ap.add_argument("--backup", default=os.path.join(root, "backup"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.join(a.backup, "wince-referencia")
    nand = os.path.join(a.backup, "2026-11-19-nand", "DSK9-msflash.img")
    data = open(nand, "rb").read()
    os.makedirs(out, exist_ok=True)

    # every backup file, hashed now
    sums = []
    for dirpath, dirnames, filenames in os.walk(a.backup):
        dirnames[:] = sorted(d for d in dirnames if os.path.join(dirpath, d) != out)
        for fn in sorted(filenames):
            if fn.endswith(".sha256"):
                continue
            p = os.path.join(dirpath, fn)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            sums.append((sha256(p), os.path.relpath(p, a.backup)))
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        for h, p in sums:
            f.write("%s  %s\n" % (h, p))

    lines = ["# Firmware Windows CE do FOSTON FS-707DC: referência local", "",
             "Gerado por `tools/bigfoston/wince-ref.py` a partir de `backup/` (fora do git: tem partes "
             "proprietárias — Windows CE, DLLs da MediaTek, iGO). Não publicar.", "",
             "Conferir a cópia: `cd backup && sha256sum -c wince-referencia/SHA256SUMS` "
             "(%d arquivos)." % len(sums), "",
             "## NAND (DSK9, %d bytes, SHA-256 %s)" % (len(data), sha256(nand)), "",
             "| Partição | Tipo | Início | Tamanho | Extraída em |", "|---|---|---|---|---|"]
    for i in range(4):
        e = data[446 + 16 * i:462 + 16 * i]
        t = e[4]
        lba, n = struct.unpack_from("<II", e, 8)
        if not t:
            continue
        ext = "partitions/%d (backup: 2026-11-19-nand/part%d/)" % (i + 1, i) if i else "rom/ (este índice)"
        lines.append("| %d | %s (0x%02x) | 0x%x | %.1f MB | %s |"
                     % (i + 1, PART_TYPES.get(t, "?"), t, lba * SECTOR, n * SECTOR / 1048576, ext))
        if i:
            src = os.path.join(a.backup, "2026-11-19-nand", "part%d" % i)
            dst = os.path.join(out, "partitions", "%d" % (i + 1))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isdir(src) and not os.path.lexists(dst):
                os.symlink(os.path.relpath(src, os.path.dirname(dst)), dst)
    lines += ["", "A 4ª partição (12 MB) é a `\\ResidentFlash` do CE: arquivos do GPS e, desde 21/11/2026, "
              "o agente e o Linux instalados (`BigFoston\\linux`). O backup é de antes disso.", "",
              "## Regiões XIP da ROM", "",
              "| Início no arquivo | VA base | ROMHDR | Módulos | Arquivos |", "|---|---|---|---|---|"]
    log = []
    for n, (start, base, romhdr) in enumerate(regions(data)):
        index_region(data, start, base, romhdr, os.path.join(out, "rom", "regiao%d-%08x" % (n + 1, base)), log)
    lines += log
    lines += ["", "Seções e arquivos comprimidos (CECompress) estão listados nos `.tsv` mas não extraídos. "
              "Para desmontar uma DLL pelo endereço virtual: `tools/bigfoston/cedll.py`.", "",
              "## O que não está nesta cópia", "",
              "- O bootloader EBoot (\"JWD B.13D...101025\"): fica fora da DSK9 e não foi lido.",
              "- O estado atual da `\\ResidentFlash` (o backup é de 19/11/2026)."]
    with open(os.path.join(out, "MANIFEST.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("referência em", out)
    print("\n".join(log))


if __name__ == "__main__":
    main()
