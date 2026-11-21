#!/usr/bin/env python3
"""Update the BigFoston Linux in the FOSTON's internal flash from the running Linux.

The new zImage, initrd.img and fs707dc.dtb go over the High-Speed USB network
(SSH) to the SD card, in BigFoston/update.tmp; each file is checked there
(SHA-256 read back from the card, not from the cache) and only then the
directory is renamed to BigFoston/update.  At the next power-on the agent
(bfagent >= 0.6.0, from the internal flash) checks the files again, copies
them to \\ResidentFlash\\BigFoston\\linux through Windows CE's own file system,
checks the copies, keeps the previous version as *.old and boots the new one.
Linux never writes the internal flash.

  linux-update.py stage [--dir DIR] [--poweroff]   send and check the update
  linux-update.py status                           update.log and the running kernel

Needs the SD card mounted with write access (bigfoston-sd status = rw).
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bfagent import dtb_with_epoch  # noqa: E402  (same DTB the PC install writes)

HOST = "root@192.168.7.2"
SSH = ["ssh", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=20", "-o", "BatchMode=yes", HOST]
NAMES = ["zImage", "initrd.img", "fs707dc.dtb"]
SD = "/mnt/sd/BigFoston"
DEFAULT_DIR = os.path.join(HERE, "../../firmware/build/out/images/bigfoston")


class UpdateError(Exception):
    pass


def ssh(command, data=None, timeout=120):
    """Run a fixed command on the device (no user data in it); data goes to stdin."""
    r = subprocess.run(SSH + [command], input=data, capture_output=True, timeout=timeout)
    if r.returncode:
        raise UpdateError(f"no aparelho: {command!r} falhou ({r.returncode}): "
                          f"{r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode(errors="replace")


def stage(local_dir, poweroff):
    files = {}
    for n in NAMES:
        path = os.path.join(local_dir, n)
        if n == "fs707dc.dtb":
            files[n] = dtb_with_epoch(path, 0)   # the agent fills the clock at boot
        else:
            with open(path, "rb") as f:
                files[n] = f.read()
    sums = "".join(f"{hashlib.sha256(files[n]).hexdigest()}  {n}\n" for n in NAMES)

    state = ssh("bigfoston-sd status").strip()
    if state != "rw":
        raise UpdateError(f"o cartão SD não está montado com gravação (bigfoston-sd status: {state})")
    ssh(f"rm -rf {SD}/update.tmp && mkdir -p {SD}/update.tmp")
    for n in NAMES:
        print(f"{n}: enviando {len(files[n])} bytes")
        ssh(f"cat > {SD}/update.tmp/{n}", files[n], timeout=300)
    ssh(f"cat > {SD}/update.tmp/SHA256SUMS", sums.encode())
    # check what is on the card, not in the page cache
    out = ssh(f"sync && echo 3 > /proc/sys/vm/drop_caches && cd {SD}/update.tmp && sha256sum -c SHA256SUMS")
    for line in out.splitlines():
        print("  cartão:", line)
    if sum(line.endswith(": OK") for line in out.splitlines()) != len(NAMES):
        raise UpdateError("conferência no cartão falhou")
    ssh(f"rm -rf {SD}/update {SD}/update.ok {SD}/update.bad && mv {SD}/update.tmp {SD}/update && sync")
    print("atualização pronta no cartão (BigFoston/update).")
    print("Desligue e ligue o aparelho: o agente confere, grava na memória interna e inicia a versão nova.")
    if poweroff:
        print("desligando o aparelho...")
        subprocess.run(SSH + ["poweroff"], capture_output=True, timeout=30)


def status():
    print(ssh("uname -v; cat /sys/class/udc/*/current_speed 2>/dev/null").strip())
    log = ssh(f"tail -n 5 {SD}/update.log 2>/dev/null; ls -d {SD}/update {SD}/update.ok {SD}/update.bad 2>/dev/null; true")
    print(log.strip() or "sem update.log no cartão (nenhuma atualização aplicada ainda)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stage", help="send the update to the SD card and check it")
    p.add_argument("--dir", default=DEFAULT_DIR, help="directory with zImage, initrd.img, fs707dc.dtb")
    p.add_argument("--poweroff", action="store_true", help="power the device off afterwards")
    sub.add_parser("status", help="update.log and the running kernel")
    args = ap.parse_args()
    try:
        if args.cmd == "stage":
            stage(args.dir, args.poweroff)
        else:
            status()
    except (UpdateError, OSError, subprocess.TimeoutExpired) as e:
        print("erro:", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
