"""BFP - BigFoston Probe protocol v1 (host side). Byte layout: src/wince/agent/bfp.h.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import struct

MAGIC = b"BF"
VERSION = 1
HEADER = 16
MAX_PAYLOAD = 1024 * 1024
MAX_REQUEST = 32 * 1024
RESPONSE = 0x80

INFO, PING = 0x01, 0x02
LIST, READ, WRITE, MKDIR, DELETE = 0x10, 0x11, 0x12, 0x13, 0x14
REG, REGSET = 0x20, 0x21
EXEC, PS, KILL = 0x30, 0x31, 0x32
BOOT = 0x38
STAGE = 0x39
PHYS, CPU, KIOCTL = 0x40, 0x41, 0x42
DISKINFO, DISKREAD = 0x50, 0x51
SHOT = 0x60
QUIT, REBOOT = 0x70, 0x71

OK, E_UNKNOWN, E_ARGS, E_SYSTEM, E_DENIED, E_TOOBIG, E_CRC = range(7)
STATUS_NAMES = {OK: "ok", E_UNKNOWN: "unknown request", E_ARGS: "bad arguments", E_SYSTEM: "system error",
                E_DENIED: "denied", E_TOOBIG: "response truncated", E_CRC: "CRC error"}


def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


try:  # binascii.crc_hqx is CRC-16/CCITT with a caller-chosen initial value: same algorithm, C speed
    import binascii

    def crc16(data, crc=0xFFFF):  # noqa: F811
        return binascii.crc_hqx(bytes(data), crc)
except ImportError:  # pragma: no cover
    pass


def message(kind, payload=b"", seq=0, status=0):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    head = struct.pack("<2sBBHHIH", MAGIC, VERSION, kind, seq & 0xFFFF, status, len(payload),
                       crc16(payload) if payload else 0xFFFF)
    return head + struct.pack("<H", crc16(head)) + payload


class Message:
    __slots__ = ("kind", "seq", "status", "payload", "crc_ok")

    def __init__(self, kind, seq, status, payload, crc_ok):
        self.kind, self.seq, self.status, self.payload, self.crc_ok = kind, seq, status, payload, crc_ok

    def error_text(self):
        if len(self.payload) >= 4:
            code = struct.unpack_from("<I", self.payload)[0]
            text = self.payload[4:].decode("utf-8", "replace")
            return f"{STATUS_NAMES.get(self.status, self.status)}: {text} (Win32 error {code})"
        return STATUS_NAMES.get(self.status, str(self.status))


class Parser:
    """Incremental decoder; resynchronises on the next header whose CRC checks out."""

    def __init__(self):
        self.buf = bytearray()
        self.dropped = 0

    def feed(self, data):
        self.buf += data

    def next(self):
        while True:
            start = self.buf.find(MAGIC[:1])
            if start < 0:
                self.dropped += len(self.buf)
                self.buf.clear()
                return None
            if start:
                self.dropped += start
                del self.buf[:start]
            if len(self.buf) < HEADER:
                return None
            head = bytes(self.buf[:HEADER])
            magic, version, kind, seq, status, length, pcrc, hcrc = struct.unpack("<2sBBHHIHH", head)
            if magic != MAGIC or version != VERSION or hcrc != crc16(head[:14]) or length > MAX_PAYLOAD:
                self.dropped += 1
                del self.buf[:1]
                continue
            if len(self.buf) < HEADER + length:
                return None
            payload = bytes(self.buf[HEADER:HEADER + length])
            del self.buf[:HEADER + length]
            return Message(kind, seq, status, payload, pcrc == (crc16(payload) if payload else 0xFFFF))
