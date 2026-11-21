"""A stand-in for bfagent.exe on a pseudo-terminal, for host tests only.

It answers BFP requests from canned data, including a synthetic ARMv5 page
table so walk_mmu() can be checked. This is NOT a hardware test: results
obtained against it say nothing about the FOSTON itself.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import pty
import struct
import threading
import tty

import bfp

TTB = 0x30000000


def build_memory():
    """Sparse physical memory: page table at TTB, one coarse table, some RAM."""
    mem = {}
    level1 = bytearray(16 * 1024)

    def l1(va_mb, desc):
        struct.pack_into("<I", level1, va_mb * 4, desc)

    # 0x80000000..0x87FFFFFF -> RAM 0x10000000 cached (sections, C=B=1, AP=1)
    for i in range(128):
        l1(0x800 + i, (0x10000000 + (i << 20)) | (1 << 10) | 0b1110)
    # 0xA0000000 uncached alias of the same RAM
    for i in range(128):
        l1(0xA00 + i, (0x10000000 + (i << 20)) | (1 << 10) | 0b0010)
    # 0xB0000000: coarse table with two 4 KB device pages
    coarse = bytearray(1024)
    struct.pack_into("<I", coarse, 0, 0x80000000 | (1 << 4) | 0b10)
    struct.pack_into("<I", coarse, 4, 0x80001000 | (1 << 4) | 0b10)
    l1(0xB00, 0x30010000 | 0b01)
    mem[TTB] = bytes(level1)
    mem[0x30010000] = bytes(coarse)
    mem[0x10000000] = bytes(range(256)) * 16
    return mem


def phys_read(mem, address, length):
    for base, data in mem.items():
        if base <= address and address + length <= base + len(data):
            return data[address - base:address - base + length]
    return bytes(length)


class FakeAgent:
    def __init__(self):
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.port = os.ttyname(slave)
        self._slave = slave
        self.mem = build_memory()
        self.files = {"\\Windows\\a.txt": b"abc" * 1000}
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def reply(self, msg, payload=b"", status=0):
        os.write(self.master, bfp.message(msg.kind | bfp.RESPONSE, payload, msg.seq, status))

    def run(self):
        parser = bfp.Parser()
        while not self.stop:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            parser.feed(data)
            while (msg := parser.next()) is not None:
                self.handle(msg)

    def handle(self, msg):
        p = msg.payload
        if msg.kind == bfp.INFO:
            self.reply(msg, b"agent: fake\n")
        elif msg.kind == bfp.PING:
            self.reply(msg, p)
        elif msg.kind == bfp.CPU:
            self.reply(msg, f"main_id=0x41069265\ncontrol=0x0005317f\nttb=0x{TTB:08x}\n".encode())
        elif msg.kind == bfp.PHYS:
            address, length, width = struct.unpack_from("<IIB", p)
            self.reply(msg, phys_read(self.mem, address, length))
        elif msg.kind == bfp.READ:
            offset, length = struct.unpack_from("<II", p)
            name = p[8:].split(b"\0")[0].decode()
            if name not in self.files:
                self.reply(msg, struct.pack("<I", 2) + b"open failed", bfp.E_SYSTEM)
                return
            data = self.files[name]
            self.reply(msg, struct.pack("<II", len(data), offset) + data[offset:offset + length])
        elif msg.kind == bfp.WRITE:
            offset, truncate = struct.unpack_from("<IB", p)
            name, _, data = p[5:].partition(b"\0")
            name = name.decode()
            old = b"" if truncate else self.files.get(name, b"")
            self.files[name] = old[:offset] + data
            self.reply(msg, struct.pack("<I", len(data)))
        elif msg.kind == bfp.DISKINFO:
            self.reply(msg, struct.pack("<6I", 100, 512, 0, 0, 0, 0))
        elif msg.kind == bfp.DISKREAD:
            sector, count = struct.unpack_from("<II", p)
            self.reply(msg, b"".join(bytes([s & 0xFF]) * 512 for s in range(sector, sector + count)))
        else:
            self.reply(msg, struct.pack("<I", 0) + b"unknown", bfp.E_UNKNOWN)

    def close(self):
        self.stop = True
        os.close(self._slave)
        os.close(self.master)
