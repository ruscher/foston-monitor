#!/usr/bin/env python3
"""Install Big GPS data (tools/big-gps/build-data.sh output) on the FOSTON's SD card.

  install-sd.py local DADOS/big-gps /caminho/do/cartao     card in a PC card reader (fast)
  install-sd.py rede  DADOS/big-gps                        over the USB network (slow, ~1.3 MB/s)
  install-sd.py rede  DADOS/big-gps --so rotas/df-carro.bgr   only some files

The card gets BigFoston/big-gps/<same layout>.  Every file is written under a temporary
name, checked (SHA-256 read back from the card in "local" mode; computed while receiving
by bgrecv in "rede" mode) and only then renamed.  Files already on the card with the right
size and hash (MANIFEST.sha256 of a previous install) are skipped.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = "root@192.168.7.2"
ADDR = "192.168.7.2"
SSH = ["ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=20", "-o", "BatchMode=yes", HOST]
REMOTE = "/mnt/sd/BigFoston/big-gps"
BGRECV = os.path.join(HERE, "bgrecv", "bgrecv")


class InstallError(Exception):
    pass


def sums(path):
    out = {}
    with open(os.path.join(path, "SHA256SUMS")) as f:
        for line in f:
            h, name = line.rstrip("\n").split("  ", 1)
            out[os.path.normpath(name)] = h
    return out


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def evict(path):
    """Drop the file from the PC's page cache, so the check reads the card (no root needed)."""
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)


def install_local(src, card, only):
    expected = sums(src)
    dst_root = os.path.join(card, "BigFoston", "big-gps")
    manifest = os.path.join(dst_root, "MANIFEST.sha256")
    old = {}
    if os.path.exists(manifest):
        old = sums_file(manifest)
    for name, h in sorted(expected.items()):
        if only and name not in only:
            continue
        s = os.path.join(src, name)
        d = os.path.join(dst_root, name)
        if old.get(name) == h and os.path.exists(d) and os.path.getsize(d) == os.path.getsize(s):
            print(f"{name}: já está no cartão")
            continue
        os.makedirs(os.path.dirname(d), exist_ok=True)
        tmp = d + ".tmp"
        t0 = time.time()
        with open(s, "rb") as fi, open(tmp, "wb") as fo:
            for b in iter(lambda: fi.read(4 << 20), b""):
                fo.write(b)
            fo.flush()
            os.fsync(fo.fileno())
        evict(tmp)
        if sha256_file(tmp) != h:
            os.unlink(tmp)
            raise InstallError(f"{name}: conferência falhou no cartão")
        os.replace(tmp, d)
        old[name] = h
        write_manifest(manifest, old)
        mb = os.path.getsize(d) / 1e6
        print(f"{name}: {mb:.1f} MB em {time.time() - t0:.0f} s, conferido")


def sums_file(p):
    out = {}
    with open(p) as f:
        for line in f:
            h, name = line.rstrip("\n").split("  ", 1)
            out[name] = h
    return out


def write_manifest(p, entries):
    with open(p + ".tmp", "w") as f:
        for n, h in sorted(entries.items()):
            f.write(f"{h}  {n}\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(p + ".tmp", p)


def ssh(cmd, data=None, timeout=120):
    r = subprocess.run(SSH + [cmd], input=data, capture_output=True, timeout=timeout)
    if r.returncode:
        raise InstallError(f"{cmd!r}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode(errors="replace")


def send_one(local, remote_tmp, port):
    size = os.path.getsize(local)
    p = subprocess.Popen(SSH + [f"/tmp/bgrecv {ADDR} {port} {size} {remote_tmp}"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = p.stdout.readline().decode().strip()
    if line != "pronto":
        p.kill()
        raise InstallError(f"bgrecv não iniciou: {line} {p.stderr.read().decode(errors='replace')}")
    s = socket.create_connection((ADDR, port), timeout=30)
    s.settimeout(600)
    with open(local, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            s.sendall(b)
    s.shutdown(socket.SHUT_WR)
    out, err = p.communicate(timeout=600)
    s.close()
    if p.returncode:
        raise InstallError(f"bgrecv: {err.decode(errors='replace').strip()}")
    return out.decode().strip().split()[-1]


def install_net(src, only):
    expected = sums(src)
    if not os.path.exists(BGRECV):
        raise InstallError(f"falta {BGRECV}: make -C tools/big-gps/bgrecv")
    if ssh("bigfoston-sd status").strip() != "rw":
        raise InstallError("o cartão não está montado com gravação no aparelho")
    with open(BGRECV, "rb") as f:
        ssh("cat > /tmp/bgrecv && chmod 755 /tmp/bgrecv", f.read())
    old = {}
    try:
        for line in ssh(f"cat {REMOTE}/MANIFEST.sha256 2>/dev/null; true").splitlines():
            h, n = line.split("  ", 1)
            old[n] = h
    except ValueError:
        old = {}
    port = 5741
    for name, h in sorted(expected.items()):
        if only and name not in only:
            continue
        if old.get(name) == h:
            print(f"{name}: já está no cartão")
            continue
        if "'" in name or ".." in name.split("/"):
            raise InstallError(f"nome inválido: {name}")
        d = f"{REMOTE}/{name}"
        ssh(f"mkdir -p '{os.path.dirname(d)}'")
        t0 = time.time()
        got = send_one(os.path.join(src, name), d + ".tmp", port)
        port += 1
        if got != h:
            ssh(f"rm -f '{d}.tmp'")
            raise InstallError(f"{name}: SHA-256 recebido não confere")
        ssh(f"mv '{d}.tmp' '{d}' && sync")
        old[name] = h
        man = "".join(f"{hh}  {nn}\n" for nn, hh in sorted(old.items())).encode()
        ssh(f"cat > {REMOTE}/MANIFEST.sha256.tmp && mv {REMOTE}/MANIFEST.sha256.tmp {REMOTE}/MANIFEST.sha256 && sync", man)
        mb = os.path.getsize(os.path.join(src, name)) / 1e6
        print(f"{name}: {mb:.1f} MB em {time.time() - t0:.0f} s ({mb / max(time.time() - t0, 0.1):.2f} MB/s), conferido")
    ssh("rm -f /tmp/bgrecv")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("local")
    a.add_argument("dados")
    a.add_argument("cartao")
    a.add_argument("--so", nargs="*", default=[])
    b = sub.add_parser("rede")
    b.add_argument("dados")
    b.add_argument("--so", nargs="*", default=[])
    args = ap.parse_args()
    only = {os.path.normpath(x) for x in args.so}
    try:
        if args.cmd == "local":
            install_local(args.dados, args.cartao, only)
        else:
            install_net(args.dados, only)
    except (InstallError, OSError, subprocess.SubprocessError, socket.error) as e:
        print("erro:", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
