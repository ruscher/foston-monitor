"""End-to-end host test of bigfoston-bigscreen (src/bigscreen) against the real
FSP encoder of Big Screen Monitor Display (protocol.py), over a pseudo-terminal.

Checks: HELLO -> HELLO_ACK fields, FILL + raw/RLE/deflate rectangles land
pixel-exact in the framebuffer only after PRESENT, PRESENTED and CREDIT come
back, PING -> PONG, BYE ends the client. Host test only: no device involved.

  python3 -m unittest discover -s tests/bigscreen -v

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import pty
import select
import subprocess
import sys
import tempfile
import time
import tty
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BSMD = os.environ.get("BSMD_SRC", os.path.join(os.path.dirname(ROOT), "big-screen-monitor-display",
                                               "usr", "share", "big-screen-monitor-display"))
sys.path.insert(0, os.path.join(BSMD, "bsmd", "displays", "foston"))
import protocol as fsp  # noqa: E402

CLIENT_DIR = os.path.join(ROOT, "src", "bigscreen")
CLIENT = os.path.join(CLIENT_DIR, "build", "bigfoston-bigscreen")
W, H = 800, 480


class FspLinuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", CLIENT_DIR], check=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fb_path = os.path.join(self.tmp.name, "fb.raw")
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.proc = subprocess.Popen([CLIENT, "--tty", os.ttyname(slave), "--fb-file", f"{self.fb_path}:{W}x{H}",
                                      "--once"], stderr=subprocess.PIPE)
        os.close(slave)
        self.parser = fsp.Parser()
        self.pending = []
        self.seq = 0
        time.sleep(0.2)

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.proc.stderr.close()
        os.close(self.master)
        self.tmp.cleanup()

    def send(self, kind, payload=b""):
        self.seq += 1
        os.write(self.master, fsp.message(kind, payload, self.seq))

    def wait_for(self, kind, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for i, msg in enumerate(self.pending):
                if msg.kind == kind:
                    return self.pending.pop(i)
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if ready:
                self.pending.extend(self.parser.feed(os.read(self.master, 65536)))
        self.fail(f"no message 0x{kind:02x}")

    def framebuffer(self):
        with open(self.fb_path, "rb") as f:
            return np.frombuffer(f.read(), dtype="<u2").reshape(H, W)

    def test_session(self):
        self.send(fsp.HELLO, fsp.hello("test-host"))
        ack = fsp.HelloAck.parse(self.wait_for(fsp.HELLO_ACK).payload)
        self.assertEqual((ack.width, ack.height), (W, H))
        self.assertTrue(ack.strings["client"].startswith("bigfoston-bigscreen"))
        self.assertEqual(ack.strings["platform"], "BigFoston OS")

        rng = np.random.default_rng(7)
        noise = rng.integers(0, 65536, size=(40, 64), dtype=np.uint16)
        stripes = np.tile(np.array([0x343C] * 8 + [0x0000] * 8, dtype=np.uint16), (32, 4))
        flat = np.full((30, 50), 0xF800, dtype=np.uint16)
        for encoded in (fsp.encode_rect(noise, 10, 20, "raw"),
                        fsp.encode_rect(stripes, 100, 200, "rle"),
                        fsp.encode_rect(stripes, 300, 100, "deflate"),
                        fsp.encode_rect(flat, 700, 400, "fill")):
            self.send(encoded.kind, encoded.payload)
        time.sleep(0.2)
        self.assertFalse(self.framebuffer().any(), "pixels must appear only on PRESENT")
        self.send(fsp.PRESENT, fsp.present(42))
        presented = self.wait_for(fsp.PRESENTED)
        self.assertEqual(int.from_bytes(presented.payload[:4], "little"), 42)
        self.wait_for(fsp.CREDIT)
        fb = self.framebuffer()
        np.testing.assert_array_equal(fb[20:60, 10:74], noise)
        np.testing.assert_array_equal(fb[200:232, 100:164], stripes)
        np.testing.assert_array_equal(fb[100:132, 300:364], stripes)
        np.testing.assert_array_equal(fb[400:430, 700:750], flat)
        self.assertEqual(int(fb[0, 0]), 0)

        self.send(fsp.PING, (1234).to_bytes(4, "little"))
        self.assertEqual(int.from_bytes(self.wait_for(fsp.PONG).payload[:4], "little"), 1234)
        self.send(fsp.BYE, bytes([fsp.BYE_QUIT]))
        self.assertEqual(self.proc.wait(timeout=3), 0)


if __name__ == "__main__":
    unittest.main()
