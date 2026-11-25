#!/usr/bin/env python3
"""Confere se cada função que o agente importa do COREDLL existe no coredll.dll do FOSTON.

O Windows CE do aparelho não abre (e não diz nada) um .exe que importa uma função ausente.
As exportações vêm do coredll.dll do backup da NAND (seções extraídas em <rva>.bin).

  check_imports.py OBJDUMP build/bfagent.exe backup/.../mod/coredll.dll
Sai com erro se faltar alguma. SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import struct
import subprocess
import sys

# funções que este coredll exporta pelo nome mas que já falharam no aparelho
KNOWN_BAD = {"_vsnwprintf"}


def exports(mod_dir):
    secs = []
    for f in os.listdir(mod_dir):
        if f.endswith(".bin"):
            secs.append((int(f[:-4], 16), open(os.path.join(mod_dir, f), "rb").read()))

    def rd(rva, n):
        for b, blob in secs:
            if b <= rva and rva + n <= b + len(blob):
                return blob[rva - b:rva - b + n]
        return None

    def cstr(rva):
        for b, blob in secs:
            if b <= rva < b + len(blob):
                return blob[rva - b:blob.index(b"\0", rva - b)].decode("latin1")
        return ""

    for _, blob in secs:                   # o IMAGE_EXPORT_DIRECTORY cujo nome é COREDLL
        for off in range(0, len(blob) - 40, 4):
            _, _, _, name, _, nf, nn, _, an, _ = struct.unpack_from("<10I", blob, off)
            if 0 < nn <= nf < 5000 and name and (rd(name, 7) or b"").lower() == b"coredll":
                out = set()
                for i in range(nn):
                    p = rd(an + 4 * i, 4)
                    if p:
                        out.add(cstr(struct.unpack("<I", p)[0]))
                return out
    raise SystemExit("tabela de exportações do coredll não encontrada")


def imports(objdump, exe):
    txt = subprocess.run([objdump, "-p", exe], capture_output=True, text=True, check=True).stdout
    out, on = set(), False
    for line in txt.splitlines():
        if "DLL Name:" in line:
            on = "COREDLL" in line.upper()
            continue
        m = re.match(r"\s+[0-9a-f]+\s+\d+\s+([A-Za-z_]\w*)\s*$", line)
        if on and m:
            out.add(m.group(1))
    return out


def main():
    objdump, exe, mod = sys.argv[1:4]
    have, need = exports(mod), imports(objdump, exe)
    missing = sorted(n for n in need if n not in have or n in KNOWN_BAD)
    print(f"{exe}: {len(need)} funções do COREDLL, {len(have)} exportadas pelo aparelho")
    if missing:
        print("FALTAM no aparelho: " + ", ".join(missing))
        sys.exit(1)
    print("todas existem no coredll do FOSTON")


if __name__ == "__main__":
    main()
