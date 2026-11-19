#!/usr/bin/env python3
"""bfagent.py - talk to the BigFoston diagnostic agent on the FOSTON (Windows CE).

The agent (src/wince/agent/bfagent.exe) runs on the device and exposes, read
only, what a Linux port needs: CP15/MMU state, physical memory, raw disk
sectors, registry, modules, files and screenshots. This tool is the PC side.

  bfagent.py info                       system summary
  bfagent.py cpu                        CP15 registers (main id, control, TTB...)
  bfagent.py mmu [--all]                decode the live page tables (VA -> PA map)
  bfagent.py survey OUTDIR              everything above + registry, modules, disks, screenshot
  bfagent.py ls '\\Windows'             list a directory
  bfagent.py get '\\path\\file' OUT     copy a file from the device
  bfagent.py put LOCAL '\\path\\file'   copy a file to the device (mkdir / rm also exist)
  bfagent.py reg HKLM 'Drivers' -d 9    dump a registry tree
  bfagent.py ps [-m]                    processes (and their modules)
  bfagent.py phys ADDR LEN [-w 4] [-o F] read physical memory (hexdump or file)
  bfagent.py disk-info DSK1:            block device geometry
  bfagent.py disk-dump DSK1: OUT        raw image of a block device (resumable, sha256)
  bfagent.py shot OUT.png [--under]     screenshot (--under: hide the agent window first)
  bfagent.py exec '\\path\\app.exe' [ARGS]
  bfagent.py boot '\\SDMMC\\BigFoston\\zImage' [--dtb F] [--initrd F] [--dry-run]
                                        start Linux / a bare-metal payload (Windows CE stops;
                                        reset to get it back - nothing is written to flash)
  bfagent.py quit [--restart]           stop the agent (USB storage comes back) or restart it

The Big Screen Monitor service also opens 045e:00ce ports (exclusively). It
does not understand the agent and backs off; this tool retries until it gets
the port (up to --wait seconds).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import glob
import hashlib
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfp  # noqa: E402

DEVICE_IDS = {("045e", "00ce")}           # Windows CE Serial_Class (FOSTON agent / bsmd-ce)
ROOTS = {"HKLM": 0, "HKCU": 1, "HKCR": 2, "HKU": 3}


class AgentError(RuntimeError):
    pass


def find_ports():
    ports = []
    for tty in sorted(glob.glob("/sys/bus/usb-serial/devices/ttyUSB*")):
        node = os.path.realpath(tty)
        while node != "/" and not os.path.exists(os.path.join(node, "idVendor")):
            node = os.path.dirname(node)
        try:
            ids = tuple(open(os.path.join(node, name)).read().strip() for name in ("idVendor", "idProduct"))
        except OSError:
            continue
        if ids in DEVICE_IDS:
            ports.append("/dev/" + os.path.basename(tty))
    return ports


class Link:
    SLICE = 1024

    def __init__(self, port=None, wait=60.0, timeout=20.0, verbose=False):
        import serial
        self.timeout = timeout
        self.verbose = verbose
        self.seq = 0
        self.parser = bfp.Parser()
        deadline = time.monotonic() + wait
        last_error = None
        while True:
            candidates = [port] if port else find_ports()
            for name in candidates:
                try:
                    self.ser = serial.Serial(name, 115200, timeout=0.2, exclusive=True)
                    self.port = name
                    break
                except (OSError, serial.SerialException) as error:
                    last_error = error
            else:
                if time.monotonic() > deadline:
                    raise AgentError(f"no agent port ({'busy: ' + str(last_error) if last_error else 'not found'}); "
                                     "is bfagent.exe running on the FOSTON and the cable connected?")
                time.sleep(0.25)
                continue
            break
        self.ser.reset_input_buffer()
        if verbose:
            print(f"# port {self.port}", file=sys.stderr)

    def close(self):
        self.ser.close()

    def request(self, kind, payload=b"", timeout=None, check=True):
        if len(payload) > bfp.MAX_REQUEST:
            raise ValueError("request too large")
        self.seq = (self.seq + 1) & 0xFFFF
        frame = bfp.message(kind, payload, self.seq)
        # The Windows CE Serial_Class driver keeps only a few KB while the agent
        # is not reading: pace large requests so it can drain between slices.
        for start in range(0, len(frame), self.SLICE):
            self.ser.write(frame[start:start + self.SLICE])
            self.ser.flush()
            if len(frame) > self.SLICE:
                time.sleep(0.003)
        deadline = time.monotonic() + (timeout or self.timeout)
        while time.monotonic() < deadline:
            data = self.ser.read(self.ser.in_waiting or 1)
            if data:
                self.parser.feed(data)
            while (msg := self.parser.next()) is not None:
                if msg.seq != self.seq or msg.kind != (kind | bfp.RESPONSE):
                    continue                     # stale or foreign (e.g. a late reply)
                if not msg.crc_ok:
                    raise AgentError("response failed its CRC (data lost on the link)")
                if check and msg.status not in (bfp.OK,):
                    raise AgentError(msg.error_text())
                return msg
        raise AgentError(f"timeout waiting for the reply to request 0x{kind:02x}")

    # ------------------------------------------------------------ commands
    def text(self, kind, payload=b"", **kw):
        msg = self.request(kind, payload, **kw)
        return msg.payload.decode("utf-8", "replace"), msg.status

    def info(self):
        return self.text(bfp.INFO)[0]

    def cpu(self):
        return dict(line.split("=", 1) for line in self.text(bfp.CPU)[0].split())

    def listdir(self, path):
        entries = []
        for line in self.text(bfp.LIST, path.encode() + b"\0")[0].splitlines():
            kind, size, attrs, name = line.split(" ", 3)
            entries.append({"dir": kind == "D", "size": int(size), "attrs": int(attrs, 16), "name": name})
        return entries

    def read_file(self, path, chunk=256 * 1024, progress=None):
        out, offset, size = bytearray(), 0, None
        while size is None or offset < size:
            msg = self.request(bfp.READ, struct.pack("<II", offset, chunk) + path.encode() + b"\0", timeout=60)
            size, _ = struct.unpack_from("<II", msg.payload)
            data = msg.payload[8:]
            if not data:
                break
            out += data
            offset += len(data)
            if progress:
                progress(offset, size)
        return bytes(out)

    def write_file(self, path, data, chunk=6 * 1024, progress=None):
        try:
            self._write_file(path, data, chunk, progress)
        except AgentError as error:
            # Windows CE refuses to overwrite an executable it launched recently (error 32,
            # seen with the agent's own .exe on the SD card) but lets it be deleted.
            if "error 32)" not in str(error):
                raise
            self.request(bfp.DELETE, path.encode() + b"\0")
            self._write_file(path, data, chunk, progress)

    def _write_file(self, path, data, chunk, progress):
        offset = 0
        name = path.encode() + b"\0"
        while True:
            piece = data[offset:offset + chunk]
            msg = self.request(bfp.WRITE, struct.pack("<IB", offset, 1 if offset == 0 else 0) + name + piece,
                               timeout=30)
            written = struct.unpack_from("<I", msg.payload)[0]
            if written != len(piece):
                raise AgentError(f"short write at {offset}: {written}/{len(piece)}")
            offset += len(piece)
            if progress:
                progress(offset, len(data))
            if offset >= len(data):
                return

    def reg(self, root, path="", depth=0):
        payload = bytes([ROOTS[root], depth]) + path.encode() + b"\0"
        return self.text(bfp.REG, payload, timeout=60)

    def phys(self, address, length, width=1):
        out = bytearray()
        while length:
            n = min(length, 256 * 1024)
            out += self.request(bfp.PHYS, struct.pack("<IIB", address, n, width), timeout=30).payload
            address += n
            length -= n
        return bytes(out)

    def disk_info(self, device):
        names = ("total_sectors", "bytes_per_sector", "cylinders", "heads", "sectors", "flags")
        return dict(zip(names, struct.unpack("<6I", self.request(bfp.DISKINFO, device.encode() + b"\0").payload)))

    def disk_read(self, device, sector, count):
        return self.request(bfp.DISKREAD, struct.pack("<II", sector, count) + device.encode() + b"\0",
                            timeout=60).payload

    def boot(self, kernel, dtb="", initrd="", kernel_pa=0x00800000, dtb_pa=0x01F00000, initrd_pa=0x01000000,
             machine=0xFFFFFFFF, dry_run=False):
        payload = struct.pack("<IIIIB", kernel_pa, dtb_pa, initrd_pa, machine, 1 if dry_run else 0)
        payload += kernel.encode() + b"\0" + dtb.encode() + b"\0" + initrd.encode() + b"\0"
        msg = self.request(bfp.BOOT, payload, timeout=120, check=False)
        text = msg.payload[4:].decode("utf-8", "replace") if dry_run or msg.status else msg.payload.decode()
        if msg.status:
            raise AgentError(msg.error_text())
        return text

    def shot(self, under=False):
        payload = self.request(bfp.SHOT, bytes([1 if under else 0]), timeout=30).payload
        width, height = struct.unpack_from("<HH", payload)
        return width, height, payload[4:]


# ------------------------------------------------------------- page tables

def _ap(ap):
    return {0: "--", 1: "rw/--", 2: "rw/ro", 3: "rw/rw"}[ap]


def walk_mmu(link, ttb, kernel_only=True):
    """Decode ARMv5 (ARM926EJ-S) page tables. Returns runs of contiguous mappings:
    (va, pa, size, kind, cb, ap)."""
    base = ttb & ~0x3FFF
    level1 = link.phys(base, 16 * 1024, 4)
    pages = []
    for index in range(4096):
        desc = struct.unpack_from("<I", level1, index * 4)[0]
        va = index << 20
        kind = desc & 3
        if kind == 0:
            continue
        if kind == 2:
            pages.append((va, desc & 0xFFF00000, 1 << 20, "section", (desc >> 2) & 3, (desc >> 10) & 3))
            continue
        if kernel_only and va < 0x80000000:
            continue
        if kind == 1:                       # coarse: 256 entries, 4 KB granularity
            table = link.phys(desc & 0xFFFFFC00, 1024, 4)
            entries, step = 256, 4096
        else:                               # fine: 1024 entries, 1 KB granularity
            table = link.phys(desc & 0xFFFFF000, 4096, 4)
            entries, step = 1024, 1024
        for sub in range(entries):
            d2 = struct.unpack_from("<I", table, sub * 4)[0]
            t2 = d2 & 3
            sva = va + sub * step
            if t2 == 1:                      # large page 64 KB (repeated 16x)
                pages.append((sva, (d2 & 0xFFFF0000) | (sva & 0xF000), step, "large", (d2 >> 2) & 3, (d2 >> 4) & 3))
            elif t2 == 2:
                pages.append((sva, d2 & 0xFFFFF000, step, "small", (d2 >> 2) & 3, (d2 >> 4) & 3))
            elif t2 == 3:
                pages.append((sva, d2 & 0xFFFFFC00, step, "tiny", (d2 >> 2) & 3, (d2 >> 4) & 3))
    runs = []
    for va, pa, size, kind, cb, ap in pages:
        if runs:
            rva, rpa, rsize, rkind, rcb, rap = runs[-1]
            if rva + rsize == va and rpa + rsize == pa and rcb == cb and rap == ap and (rkind == "section") == (kind == "section"):
                runs[-1] = (rva, rpa, rsize + size, rkind, rcb, rap)
                continue
        runs.append((va, pa, size, kind, cb, ap))
    return runs


def format_runs(runs):
    lines = ["virtual                 physical                size        type     C B  AP"]
    for va, pa, size, kind, cb, ap in runs:
        lines.append(f"0x{va:08x}-0x{va + size - 1:08x}  0x{pa:08x}-0x{pa + size - 1:08x}  "
                     f"{size // 1024:>8} KB  {kind:<8} {cb >> 1} {cb & 1}  {_ap(ap)}")
    return "\n".join(lines)


def physical_regions(runs):
    """Distinct physical ranges reachable through the kernel mapping (merged)."""
    ranges = sorted((pa, pa + size, cb) for va, pa, size, kind, cb, ap in runs if va >= 0x80000000)
    merged = []
    for start, end, cb in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
            merged[-1][2] |= cb
        else:
            merged.append([start, end, cb])
    return merged


# ------------------------------------------------------------------- utils

def hexdump(data, address=0, width=16):
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{address + off:08x}  {chunk.hex(' '):<{width * 3}} {text}")
    return "\n".join(lines)


def save_png(path, width, height, rgb565):
    from PIL import Image
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        v = rgb565[2 * i] | (rgb565[2 * i + 1] << 8)
        pixels[3 * i] = ((v >> 11) & 31) * 255 // 31
        pixels[3 * i + 1] = ((v >> 5) & 63) * 255 // 63
        pixels[3 * i + 2] = (v & 31) * 255 // 31
    Image.frombytes("RGB", (width, height), bytes(pixels)).save(path)


def progress_bar(label):
    start = time.monotonic()

    def show(done, total):
        rate = done / max(time.monotonic() - start, 1e-3) / 1024
        pct = 100 * done / total if total else 100
        print(f"\r{label}: {done // 1024} / {total // 1024} KB ({pct:5.1f}%) {rate:7.1f} KB/s", end="",
              file=sys.stderr, flush=True)
        if done >= total:
            print(file=sys.stderr)
    return show


def disk_dump(link, device, out, chunk_sectors=None):
    info = link.disk_info(device)
    bps, total = info["bytes_per_sector"], info["total_sectors"]
    chunk_sectors = chunk_sectors or max(1, (256 * 1024) // bps)
    done = os.path.getsize(out) // bps if os.path.exists(out) else 0
    show = progress_bar(f"{device} -> {out}")
    with open(out, "ab") as f:
        f.truncate(done * bps)
        sector = done
        while sector < total:
            n = min(chunk_sectors, total - sector)
            f.write(link.disk_read(device, sector, n))
            sector += n
            show(sector * bps, total * bps)
    digest = hashlib.sha256()
    with open(out, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    with open(out + ".sha256", "w") as f:
        f.write(f"{digest.hexdigest()}  {os.path.basename(out)}\n")
    return info, digest.hexdigest()


# ------------------------------------------------------------------ survey

def survey(link, outdir, shots=True):
    """Collect what the Linux port needs into OUTDIR (keep it private: it holds the device registry)."""
    os.makedirs(outdir, exist_ok=True)
    log = []

    def save(name, text):
        with open(os.path.join(outdir, name), "w") as f:
            f.write(text if text.endswith("\n") else text + "\n")
        log.append(name)
        print(f"  {name}", file=sys.stderr)

    def attempt(label, fn):
        try:
            return fn()
        except (AgentError, ValueError, struct.error) as error:
            save(f"{label}.error.txt", str(error))
            return None

    save("info.txt", link.info())
    cpu = attempt("cpu", link.cpu)
    if cpu:
        save("cpu.txt", "\n".join(f"{k}={v}" for k, v in cpu.items()))
    for code, name in ((0x01010064, "processor"), (0x01010054, "deviceid"), (0x0101004C, "uuid")):
        msg = attempt(f"kioctl-{name}", lambda c=code: link.request(bfp.KIOCTL, struct.pack("<I", c)))
        if msg:
            with open(os.path.join(outdir, f"kioctl-{name}.bin"), "wb") as f:
                f.write(msg.payload)
            save(f"kioctl-{name}.txt", hexdump(msg.payload))
    ps = attempt("ps", lambda: link.text(bfp.PS, b"\1", timeout=60)[0])
    if ps:
        save("ps-modules.txt", ps)
    for root in ("HKLM", "HKCU"):
        top, _ = link.reg(root, "", 1)
        parts = [top]
        names = [line[1:-1] for line in top.splitlines() if line.startswith("[") and line.endswith("]")
                 and line[1:-1]]
        for name in names:
            text, status = link.reg(root, name, 30)
            parts.append(text + ("\n# TRUNCATED\n" if status == bfp.E_TOOBIG else ""))
        save(f"registry-{root}.txt", "\n".join(parts))
    for path in ("\\", "\\Windows", "\\Temp"):
        entries = attempt("ls", lambda p=path: link.listdir(p))
        if entries is not None:
            save(f"ls-{path.strip(chr(92)).replace(chr(92), '_') or 'root'}.txt",
                 "\n".join(f"{'D' if e['dir'] else 'F'} {e['size']:>10} 0x{e['attrs']:05x} {e['name']}"
                           for e in entries))
    disks = []
    for n in range(10):
        dev = f"DSK{n}:"
        try:
            disks.append((dev, link.disk_info(dev)))
        except AgentError as error:
            disks.append((dev, str(error)))
    save("disks.txt", "\n".join(f"{dev} {json.dumps(info)}" for dev, info in disks))
    if cpu and "ttb" in cpu:
        runs = attempt("mmu", lambda: walk_mmu(link, int(cpu["ttb"], 16), kernel_only=True))
        if runs:
            save("mmu.txt", format_runs(runs))
            save("physical-regions.txt", "\n".join(
                f"0x{s:08x}-0x{e - 1:08x} {(e - s) // 1024:>8} KB {'cached' if c else 'uncached'}"
                for s, e, c in physical_regions(runs)))
    if shots:
        shot = attempt("shot", lambda: link.shot(under=True))
        if shot:
            save_png(os.path.join(outdir, "screen-under-agent.png"), *shot)
            log.append("screen-under-agent.png")
    save("survey-index.txt", "\n".join(log))


# -------------------------------------------------------------------- main

def parse_int(text):
    return int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--wait", type=float, default=60.0, help="seconds to wait for the port")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("cpu")
    p = sub.add_parser("mmu")
    p.add_argument("--all", action="store_true", help="also walk user-space coarse tables")
    p = sub.add_parser("survey")
    p.add_argument("outdir")
    p.add_argument("--no-shot", action="store_true")
    p = sub.add_parser("ls")
    p.add_argument("path")
    p = sub.add_parser("get")
    p.add_argument("path")
    p.add_argument("out")
    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("path")
    p = sub.add_parser("mkdir")
    p.add_argument("path")
    p = sub.add_parser("rm")
    p.add_argument("path")
    p = sub.add_parser("reg")
    p.add_argument("root", choices=sorted(ROOTS))
    p.add_argument("path", nargs="?", default="")
    p.add_argument("-d", "--depth", type=int, default=0)
    p = sub.add_parser("ps")
    p.add_argument("-m", "--modules", action="store_true")
    p = sub.add_parser("phys")
    p.add_argument("address", type=parse_int)
    p.add_argument("length", type=parse_int)
    p.add_argument("-w", "--width", type=int, choices=(1, 4), default=1)
    p.add_argument("-o", "--out")
    p = sub.add_parser("disk-info")
    p.add_argument("device")
    p = sub.add_parser("disk-dump")
    p.add_argument("device")
    p.add_argument("out")
    p = sub.add_parser("shot")
    p.add_argument("out")
    p.add_argument("--under", action="store_true")
    p = sub.add_parser("exec")
    p.add_argument("path")
    p.add_argument("args", nargs="?", default="")
    p = sub.add_parser("kill")
    p.add_argument("pid", type=parse_int)
    p = sub.add_parser("ping")
    p.add_argument("-n", type=int, default=10)
    p.add_argument("-s", "--size", type=int, default=4096)
    p = sub.add_parser("boot")
    p.add_argument("kernel", help="device path of the kernel / payload")
    p.add_argument("--dtb", default="")
    p.add_argument("--initrd", default="")
    p.add_argument("--kernel-pa", type=parse_int, default=0x00800000, help="0x8000 for bare-metal payloads")
    p.add_argument("--dtb-pa", type=parse_int, default=0x01F00000)
    p.add_argument("--initrd-pa", type=parse_int, default=0x01000000)
    p.add_argument("--machine", type=parse_int, default=0xFFFFFFFF)
    p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("quit")
    p.add_argument("--restart", action="store_true", help="start the agent again from the card (after an update)")
    args = ap.parse_args(argv)

    try:
        link = Link(args.port, wait=args.wait, verbose=args.verbose)
    except AgentError as error:
        print(f"bfagent: {error}", file=sys.stderr)
        return 2
    try:
        if args.cmd == "info":
            print(link.info(), end="")
        elif args.cmd == "cpu":
            for key, value in link.cpu().items():
                print(f"{key}={value}")
        elif args.cmd == "mmu":
            cpu = link.cpu()
            runs = walk_mmu(link, int(cpu["ttb"], 16), kernel_only=not args.all)
            print(format_runs(runs))
        elif args.cmd == "survey":
            survey(link, args.outdir, shots=not args.no_shot)
        elif args.cmd == "ls":
            for e in link.listdir(args.path):
                print(f"{'D' if e['dir'] else 'F'} {e['size']:>10} 0x{e['attrs']:05x} {e['name']}")
        elif args.cmd == "get":
            data = link.read_file(args.path, progress=progress_bar(args.path))
            with open(args.out, "wb") as f:
                f.write(data)
        elif args.cmd == "put":
            with open(args.local, "rb") as f:
                link.write_file(args.path, f.read(), progress=progress_bar(args.path))
        elif args.cmd in ("mkdir", "rm"):
            link.request(bfp.MKDIR if args.cmd == "mkdir" else bfp.DELETE, args.path.encode() + b"\0")
        elif args.cmd == "reg":
            text, status = link.reg(args.root, args.path, args.depth)
            print(text, end="")
            if status == bfp.E_TOOBIG:
                print("# TRUNCATED: ask for a deeper path", file=sys.stderr)
        elif args.cmd == "ps":
            print(link.text(bfp.PS, bytes([1 if args.modules else 0]), timeout=60)[0], end="")
        elif args.cmd == "phys":
            data = link.phys(args.address, args.length, args.width)
            if args.out:
                with open(args.out, "wb") as f:
                    f.write(data)
            else:
                print(hexdump(data, args.address))
        elif args.cmd == "disk-info":
            print(json.dumps(link.disk_info(args.device), indent=1))
        elif args.cmd == "disk-dump":
            info, digest = disk_dump(link, args.device, args.out)
            print(f"{args.out}: {info['total_sectors']} x {info['bytes_per_sector']} bytes, sha256 {digest}")
        elif args.cmd == "shot":
            save_png(args.out, *link.shot(args.under))
        elif args.cmd == "exec":
            pid = struct.unpack("<I", link.request(bfp.EXEC, args.path.encode() + b"\0" + args.args.encode() + b"\0")
                                .payload)[0]
            print(f"pid 0x{pid:08x}")
        elif args.cmd == "kill":
            link.request(bfp.KILL, struct.pack("<I", args.pid))
        elif args.cmd == "ping":
            payload = os.urandom(args.size)
            start = time.monotonic()
            for _ in range(args.n):
                if link.request(bfp.PING, payload).payload != payload:
                    raise AgentError("ping payload corrupted")
            elapsed = time.monotonic() - start
            print(f"{args.n} x {args.size} B round trips in {elapsed:.3f} s "
                  f"({2 * args.n * args.size / elapsed / 1024:.0f} KB/s both ways)")
        elif args.cmd == "boot":
            print(link.boot(args.kernel, args.dtb, args.initrd, args.kernel_pa, args.dtb_pa, args.initrd_pa,
                            args.machine, args.dry_run))
        elif args.cmd == "quit":
            link.request(bfp.QUIT, b"\1" if args.restart else b"")
    except AgentError as error:
        print(f"bfagent: {error}", file=sys.stderr)
        return 1
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
