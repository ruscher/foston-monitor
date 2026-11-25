#!/usr/bin/env python3
"""Testa o motor do app Internet (bfweb) sem o shell: abre páginas, manda
comandos, mede tempo e memória e salva a tela como PNG.

    web-probe.py --bin DIST/bfweb URL [URL...] [--out DIR] [--cmd "SCROLL 0 300"]...
    web-probe.py --bin ... --ssh root@192.168.7.2 --remote-dir /mnt/sd/BigFoston/web URL

Local: cria o arquivo de memória compartilhada e roda o bfweb no PC.
--ssh: roda no FOSTON (o bfweb já no cartão); a tela vem pelo SSH (base64).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import base64
import os
import select
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def png565(path, w, h, raw):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        line = raw[y * w * 2:(y + 1) * w * 2]
        for i in range(0, len(line), 2):
            v = line[i] | line[i + 1] << 8
            rows += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


class Engine:
    def __init__(self, args):
        self.w, self.h = args.width, args.height
        self.args = args
        self.events = []
        if args.ssh:
            rd = args.remote_dir
            shm = "/dev/shm/bfweb-probe"
            cmd = (f"dd if=/dev/zero of={shm} bs={self.w * 2} count={self.h} 2>/dev/null; "
                   f"cd {rd} && exec /usr/bin/time -v ./bfweb --shm {shm} -w {self.w} -h {self.h} "
                   f"--data /tmp/bfweb-probe --res {rd}/res {'--js' if args.js else ''} about:blank")
            # time -v pode não existir na BusyBox: cai para exec simples
            cmd = cmd.replace("exec /usr/bin/time -v ", "exec ")
            self.p = subprocess.Popen(["ssh", "-o", "BatchMode=yes", args.ssh, cmd],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(args.log, "wb"))
            self.shm = shm
        else:
            fd, self.shm = tempfile.mkstemp(prefix="bfweb-")
            os.ftruncate(fd, self.w * self.h * 2)
            os.close(fd)
            d = os.path.dirname(os.path.abspath(args.bin))
            argv = [args.bin, "--shm", self.shm, "-w", str(self.w), "-h", str(self.h), "--data",
                    tempfile.mkdtemp(prefix="bfweb-data-"), "--res", os.path.join(d, "res")]
            if args.js:
                argv.append("--js")
            argv += self.args.extra
            argv.append("about:blank")
            self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=open(args.log, "wb"))
        self.buf = b""
        self.wait_for("READY", 30)

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def lines(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.p.stdout], [], [], max(0, end - time.time()))
            if not r:
                break
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                raise SystemExit("bfweb terminou (veja o log)")
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield line.decode(errors="replace")

    def wait_for(self, prefix, timeout):
        for line in self.lines(timeout):
            self.events.append(line)
            if line.startswith(prefix):
                return line
        return None

    def settle(self, quiet=2.0, timeout=180):
        """espera a página terminar (LOAD 0) e mais quiet segundos sem começar outra.

        Um <meta http-equiv=refresh> recarrega 1 s depois do fim (o NetSurf não aceita
        menos): nesse caso espera também a página seguinte, senão o fim dela seria
        confundido com o da próxima URL."""
        t0 = time.time()
        end = t0 + timeout
        got = None
        if self.wait_for("LOAD 1", timeout):  # o LOAD 0 da página anterior não conta
            while time.time() < end:
                got = self.wait_for("LOAD 0", end - time.time())
                if not got:
                    break
                t_load = time.time() - t0
                again = None
                for line in self.lines(quiet):
                    self.events.append(line)
                    if line == "LOAD 1":
                        again = line
                if not again:
                    return True, t_load
        return False, time.time() - t0

    def screen(self):
        if self.args.ssh:
            r = subprocess.run(["ssh", "-o", "BatchMode=yes", self.args.ssh, f"base64 {self.shm}"],
                               capture_output=True, check=True)
            return base64.b64decode(r.stdout)
        with open(self.shm, "rb") as f:
            return f.read()

    def rss_kb(self):
        if self.args.ssh:
            r = subprocess.run(["ssh", "-o", "BatchMode=yes", self.args.ssh,
                                # o shell pode ter o motor do app aberto: só o desta medição
                                "for p in $(pidof bfweb); do grep -q bfweb-probe /proc/$p/cmdline && "
                                "grep -E 'VmRSS|VmHWM' /proc/$p/status && "
                                "awk '{printf \"CPU: %.1f s\\n\", ($14+$15)/100}' /proc/$p/stat; done; "
                                "grep MemAvailable /proc/meminfo"],
                               capture_output=True, text=True)
            return r.stdout.strip().replace("\n", "  ")
        with open(f"/proc/{self.p.pid}/status") as f:
            return "  ".join(l.strip() for l in f if l.startswith(("VmRSS", "VmHWM")))

    def quit(self):
        try:
            self.send("QUIT")
            self.p.wait(10)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default="firmware/build/web/host/dist/bfweb")
    ap.add_argument("--ssh")
    ap.add_argument("--remote-dir", default="/mnt/sd/BigFoston/web")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--out", default=".")
    ap.add_argument("--log", default="bfweb-probe.log")
    ap.add_argument("--js", action="store_true")
    ap.add_argument("--cmd", action="append", default=[], help="comando depois de cada página")
    ap.add_argument("--extra", action="append", default=[], help="argumento extra para o bfweb")
    ap.add_argument("urls", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    e = Engine(a)
    try:
        for i, url in enumerate(a.urls):
            e.send("GO " + url)
            ok, t = e.settle()
            title = next((l[6:] for l in reversed(e.events) if l.startswith("TITLE ")), "")
            png = os.path.join(a.out, f"web-{i:02d}.png")
            png565(png, a.width, a.height, e.screen())
            print(f"{url}\n  carregou={ok} {t:.2f} s  título={title!r}\n  {e.rss_kb()}\n  tela: {png}")
            for c in a.cmd:
                e.send(c)
                for line in e.lines(1.5):
                    e.events.append(line)
                png = os.path.join(a.out, f"web-{i:02d}-{c.split()[0].lower()}.png")
                png565(png, a.width, a.height, e.screen())
                print(f"  {c}: tela {png}")
        bad = [l for l in e.events if l.startswith(("WARN", "DL ERR"))]
        if bad:
            print("avisos:", *bad, sep="\n  ")
    finally:
        e.quit()


if __name__ == "__main__":
    main()
