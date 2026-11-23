#!/usr/bin/env python3
"""Resume um perfil do bfweb (BFWEB_PROF=arquivo): amostras por função.

    web-prof.py PERFIL BINARIO_NAO_STRIPPADO [--libdir DIR] [--top 40]

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import bisect
import collections
import os
import subprocess


def symbols(path):
    out = subprocess.run(["nm", "-n", "--defined-only", path], capture_output=True, text=True).stdout
    addrs, names = [], []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[1].lower() in "tw" and not p[2].startswith("$"):
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("binary")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--lib", action="append", default=[], help="biblioteca não strippada (ex.: libc.so do sysroot)")
    a = ap.parse_args()
    maps, pcs = [], []
    for l in open(a.profile):
        if l.startswith("MAP "):
            f = l.split()
            lo, hi = (int(x, 16) for x in f[1].split("-"))
            off = int(f[3], 16)
            maps.append((lo, hi, off, f[-1] if len(f) > 6 else "?"))
        else:
            pcs.append(int(l, 16))
    exe = os.path.basename(a.binary)
    addrs, names = symbols(a.binary)
    libs = {os.path.basename(l): symbols(l) for l in a.lib}
    by_func, by_obj = collections.Counter(), collections.Counter()
    for pc in pcs:
        m = next((m for m in maps if m[0] <= pc < m[1]), None)
        if not m:
            by_obj["?"] += 1
            continue
        obj = os.path.basename(m[3])
        by_obj[obj] += 1
        if obj == exe:
            # PIE: endereço no arquivo = pc - início do mapa + offset (segmento de texto)
            fa = pc - m[0] + m[2]
            i = bisect.bisect_right(addrs, fa) - 1
            by_func[names[i] if i >= 0 else "?"] += 1
        elif obj in libs:
            la, ln = libs[obj]
            i = bisect.bisect_right(la, pc - m[0] + m[2]) - 1
            by_func[f"{ln[i] if i >= 0 else '?'} [{obj}]"] += 1
        else:
            by_func[f"[{obj}]"] += 1
    n = len(pcs) or 1
    print(f"{len(pcs)} amostras ({len(pcs) * 2 / 1000:.1f} s de CPU)")
    for o, c in by_obj.most_common(8):
        print(f"  {100 * c / n:5.1f}%  {o}")
    print()
    for f, c in by_func.most_common(a.top):
        print(f"  {100 * c / n:5.1f}%  {f}")


if __name__ == "__main__":
    main()
