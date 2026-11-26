#!/usr/bin/env python3
"""Monta o pacote de uma versão do BigFoston OS para o instalador.

  make_release.py [--images firmware/build/out/images/bigfoston] [--agent src/wince/agent/build/bfagent.exe]
                  [--channel stable] [--notes "..."] [--zip] OUT_DIR

Gera OUT_DIR/BigFoston-OS-FS707DC-<versão>/ com manifest.json, SHA256SUMS, bfagent.exe e
linux/{zImage,initrd.img,fs707dc.dtb} (e o .zip, com --zip). A versão do sistema vem de
src/ui/Makefile (VERSION) e a do agente de src/wince/agent/bfagent.c (AGENT_VERSION): as duas
partes saem do mesmo checkout, nunca de builds misturados (o SHA256SUMS do build é conferido).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(TOP, "installer"))
from bigfoston_installer.core.package import open_package, parse_sha256sums  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def read_version(path, pattern):
    with open(path, encoding="utf-8") as f:
        m = re.search(pattern, f.read(), re.M)
    if not m:
        raise SystemExit(f"versão não encontrada em {path}")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--images", default=os.path.join(TOP, "firmware/build/out/images/bigfoston"))
    ap.add_argument("--agent", default=os.path.join(TOP, "src/wince/agent/build/bfagent.exe"))
    ap.add_argument("--channel", default="stable", choices=["stable", "beta", "experimental"])
    ap.add_argument("--notes", default="")
    ap.add_argument("--zip", action="store_true")
    a = ap.parse_args()

    version = read_version(os.path.join(TOP, "src/ui/Makefile"), r"^VERSION\s*:=\s*(\S+)")
    agent_version = read_version(os.path.join(TOP, "src/wince/agent/bfagent.c"),
                                 r'#define AGENT_VERSION "bfagent (\S+)"')
    # o build é um conjunto só: o SHA256SUMS dele precisa bater com os arquivos
    with open(os.path.join(a.images, "SHA256SUMS"), encoding="ascii") as f:
        build_sums = parse_sha256sums(f.read())
    for n in ("zImage", "initrd.img", "fs707dc.dtb"):
        if build_sums.get(n) != sha256(os.path.join(a.images, n)):
            raise SystemExit(f"{n} não bate com o SHA256SUMS do build: build misturado ou incompleto")

    # o shell dentro da imagem precisa ser desta versão (um build antigo com o Makefile novo
    # sairia com o rótulo errado)
    shell = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.images))), "target", "usr", "bin",
                         "bigfoston-shell")
    if os.path.isfile(shell):
        with open(shell, "rb") as f:
            if f"BigFoston OS {version}\0".encode() not in f.read():
                raise SystemExit(f"o build em {a.images} não é da versão {version} (src/ui/Makefile): "
                                 "refaça o firmware (bigfoston-shell-dirclean + make)")
    else:
        print(f"aviso: {shell} não encontrado; versão do build não conferida", file=sys.stderr)

    name = f"BigFoston-OS-FS707DC-{version}"
    dest = os.path.join(a.out, name)
    if os.path.exists(dest):
        raise SystemExit(f"{dest} já existe")
    os.makedirs(os.path.join(dest, "linux"))
    shutil.copyfile(a.agent, os.path.join(dest, "bfagent.exe"))
    for n in ("zImage", "initrd.img", "fs707dc.dtb"):
        shutil.copyfile(os.path.join(a.images, n), os.path.join(dest, "linux", n))
    files = ["bfagent.exe", "linux/zImage", "linux/initrd.img", "linux/fs707dc.dtb"]
    entries = {n: {"file": n, "sha256": sha256(os.path.join(dest, n)), "size": os.path.getsize(os.path.join(dest, n))}
               for n in files}
    manifest = {"format": 1, "product": "BigFoston OS", "version": version,
                "date": time.strftime("%Y-%m-%d"), "channel": a.channel, "device": "foston-fs707dc",
                "agent": dict(entries["bfagent.exe"], version=agent_version),
                "linux": [entries[n] for n in files[1:]], "notes": a.notes, "min_free_mb": 16}
    with open(os.path.join(dest, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(dest, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        for n in files:
            f.write(f"{entries[n]['sha256']}  {n}\n")
    pkg = open_package(dest)                       # o próprio leitor do instalador confere
    pkg.verify()
    print(f"{dest}: BigFoston OS {version}, agente {agent_version}, {pkg.release.total_size} bytes")
    if a.zip:
        zpath = dest + ".zip"
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
            for n in ["manifest.json", "SHA256SUMS"] + files:
                z.write(os.path.join(dest, n), f"{name}/{n}")
        open_package(zpath).verify()
        print(zpath)


if __name__ == "__main__":
    main()
