#!/usr/bin/env python3
"""Pacotes de aplicativo do BigFoston (.bfapp): montar, copiar para o cartão e instalar.

Um .bfapp é um tar (ustar) com gzip e uma pasta só, a do app, com o .desktop dentro
(src/ui/core/pkg.h). A pasta-fonte de um app fica em apps/<id>/; arquivos listados no
.bfappignore dela (padrões glob) ficam de fora.

  bfapp.py build apps/sc-controller [-o pkg/apps]       monta pkg/apps/sc-controller.bfapp
  bfapp.py copy PACOTE                                   copia para o cartão (BigFoston/Apps)
  bfapp.py install PACOTE [--ask]                        copia e instala (--ask: o aparelho pergunta)
  bfapp.py uninstall ID                                  desinstala
  bfapp.py list                                          apps instalados no aparelho

O pacote é reprodutível: nomes em ordem, data zero, dono root, modo 0644/0755.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import fnmatch
import gzip
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import time

HOST = "root@192.168.7.2"
SSH = ["ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=20", "-o", "BatchMode=yes", HOST]
CARD_DIR = "/mnt/sd/BigFoston/Apps"
APPS_DIR = "/data/bigfoston/apps"
CTL = "/run/bigfoston/ctl"
ID_RE = re.compile(r"^[a-z0-9_][a-z0-9._-]{0,39}$")


class BfappError(Exception):
    pass


def ssh(command, data=None, timeout=60):
    """Roda um comando fixo no aparelho (sem dados do usuário nele); dados vão pelo stdin."""
    r = subprocess.run(SSH + [command], input=data, capture_output=True, timeout=timeout)
    if r.returncode:
        raise BfappError(f"no aparelho: {command!r} falhou ({r.returncode}): "
                         f"{r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode(errors="replace")


def read_desktop(path):
    """As chaves de [Desktop Entry]."""
    keys, inside = {}, False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("["):
                inside = line == "[Desktop Entry]"
            elif inside and "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                keys[k] = v
    return keys


def build(src, out_dir):
    src = os.path.abspath(src)
    app_id = os.path.basename(src.rstrip("/"))
    if not ID_RE.match(app_id):
        raise BfappError(f"id inválido: {app_id!r} (use a-z, 0-9, ., _ e -)")
    desktops = [n for n in os.listdir(src) if n.endswith(".desktop")]
    if len(desktops) != 1:
        raise BfappError(f"{src}: precisa de exatamente um .desktop na raiz (achei {len(desktops)})")
    d = read_desktop(os.path.join(src, desktops[0]))
    for k in ("Name", "Categories"):
        if not d.get(k):
            raise BfappError(f"{desktops[0]}: falta {k}=")
    if not d.get("Exec") and not d.get("X-BigFoston-Screen"):
        raise BfappError(f"{desktops[0]}: falta Exec= ou X-BigFoston-Screen=")
    icon = d.get("Icon", "")
    if icon and not os.path.isfile(os.path.join(src, icon)):
        raise BfappError(f"{desktops[0]}: Icon={icon} não existe na pasta")
    ignore = [".bfappignore"]
    ig = os.path.join(src, ".bfappignore")
    if os.path.isfile(ig):
        with open(ig, encoding="utf-8") as f:
            ignore += [l.strip() for l in f if l.strip() and not l.startswith("#")]

    files = []
    for root, dirs, names in os.walk(src):
        dirs[:] = sorted(x for x in dirs if not x.startswith("."))
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src)
            if n.startswith(".") or any(fnmatch.fnmatch(rel, p) or fnmatch.fnmatch(n, p) for p in ignore):
                continue
            if os.path.islink(full) or not os.path.isfile(full):
                raise BfappError(f"{rel}: só arquivos comuns (sem links)")
            files.append(rel)
    if len(files) > 512:
        raise BfappError("mais de 512 arquivos")

    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        dirs_done = set()

        def add_dir(rel):
            if rel in dirs_done:
                return
            dirs_done.add(rel)
            ti = tarfile.TarInfo(rel + "/")
            ti.type, ti.mode, ti.mtime, ti.uname, ti.gname = tarfile.DIRTYPE, 0o755, 0, "root", "root"
            tar.addfile(ti)

        add_dir(app_id)
        for rel in files:
            parts = rel.split(os.sep)
            for i in range(1, len(parts)):
                add_dir("/".join([app_id] + parts[:i]))
            full = os.path.join(src, rel)
            with open(full, "rb") as f:
                data = f.read()
            ti = tarfile.TarInfo("/".join([app_id] + parts))
            ti.size, ti.mtime, ti.uname, ti.gname = len(data), 0, "root", "root"
            ti.mode = 0o755 if os.access(full, os.X_OK) else 0o644
            tar.addfile(ti, io.BytesIO(data))
    total = raw.tell()
    if total > 24 * 1024 * 1024:
        raise BfappError("maior que 24 MB")

    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, app_id + ".bfapp")
    with open(out, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0, compresslevel=9) as gz:
            gz.write(raw.getvalue())
    print(f"{out}: {len(files)} arquivos, {total} bytes no tar, {os.path.getsize(out)} bytes")
    print(f"  {d['Name']} — grupo pelo Categories={d['Categories']}")
    return out


def pkg_id(pkg):
    with tarfile.open(pkg, "r:*") as tar:
        first = tar.getmembers()[0].name.split("/")[0]
    if not ID_RE.match(first):
        raise BfappError(f"{pkg}: id inválido {first!r}")
    return first


def copy(pkg):
    app_id = pkg_id(pkg)
    with open(pkg, "rb") as f:
        data = f.read()
    h = hashlib.sha256(data).hexdigest()
    dest = f"{CARD_DIR}/{app_id}.bfapp"
    ssh(f"mkdir -p {CARD_DIR} && cat > {dest}.tmp && echo '{h}  {dest}.tmp' | sha256sum -c - >/dev/null "
        f"&& mv {dest}.tmp {dest} && sync", data)
    print(f"no cartão: {dest} ({len(data)} bytes, SHA-256 conferido)")
    return app_id, dest


def install(pkg, ask):
    app_id, dest = copy(pkg)
    ssh(f"echo '{'install-ask' if ask else 'install'} {dest}' > {CTL}")
    if ask:
        print("o aparelho mostra o \"Instalar?\": toque em Instalar")
        return
    for _ in range(40):
        out = ssh(f"ls {APPS_DIR}/{app_id}/ 2>/dev/null; true")
        if ".desktop" in out:
            print(f"instalado em {APPS_DIR}/{app_id}: {' '.join(out.split())}")
            return
        time.sleep(0.25)
    raise BfappError("o aparelho não instalou: veja /var/log/bigfoston-shell.log")


def uninstall(app_id):
    if not ID_RE.match(app_id):
        raise BfappError(f"id inválido: {app_id!r}")
    ssh(f"echo 'uninstall {app_id}' > {CTL}")
    for _ in range(40):
        if "sim" not in ssh(f"test -d {APPS_DIR}/{app_id} && echo sim; true"):
            print(f"{app_id} desinstalado")
            return
        time.sleep(0.25)
    raise BfappError("o aparelho não desinstalou: veja /var/log/bigfoston-shell.log")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("src")
    b.add_argument("-o", "--out", default=os.path.join(os.path.dirname(__file__), "../../pkg/apps"))
    c = sub.add_parser("copy")
    c.add_argument("pkg")
    i = sub.add_parser("install")
    i.add_argument("pkg")
    i.add_argument("--ask", action="store_true")
    u = sub.add_parser("uninstall")
    u.add_argument("id")
    sub.add_parser("list")
    a = ap.parse_args()
    try:
        if a.cmd == "build":
            build(a.src, os.path.normpath(a.out))
        elif a.cmd == "copy":
            copy(a.pkg)
        elif a.cmd == "install":
            install(a.pkg, a.ask)
        elif a.cmd == "uninstall":
            uninstall(a.id)
        else:
            print(ssh(f"ls -1 {APPS_DIR} 2>/dev/null | grep -v '^\\.'; true").strip() or "(nenhum)")
    except (BfappError, OSError, tarfile.TarError, subprocess.TimeoutExpired) as e:
        print(f"erro: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
