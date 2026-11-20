"""Host tests of the BFP protocol and the PC tool (no device needed).

  python3 -m unittest discover -s tests/agent -v

SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "bigfoston"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bfagent  # noqa: E402
import bfp  # noqa: E402
from fake_agent import FakeAgent  # noqa: E402

AGENT_DIR = os.path.join(ROOT, "src", "wince", "agent")
HOST_BIN = os.path.join(AGENT_DIR, "build", "host_bfp")


def build_host():
    subprocess.run(["make", "-s", "-C", AGENT_DIR, "host-test"], check=True)


class CrcTest(unittest.TestCase):
    def test_check_value(self):
        # CRC-16/CCITT-FALSE check value
        self.assertEqual(bfp.crc16(b"123456789"), 0x29B1)


class CrossLanguageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_host()

    def test_c_encodes_python_decodes(self):
        stream = subprocess.run([HOST_BIN, "encode"], capture_output=True, check=True).stdout
        parser = bfp.Parser()
        parser.feed(b"\x00garbage BF" + stream)
        got = []
        while (m := parser.next()) is not None:
            got.append((m.kind, m.seq, m.status, len(m.payload), m.crc_ok))
        self.assertEqual(got, [(0x81, 1, 0, 5, True), (0x82, 2, 0, 0, True), (0xC0, 65535, 3, 70000, True)])

    def test_python_encodes_c_decodes(self):
        big = bytes((i * 7 + 3) & 0xFF for i in range(70000))
        corrupt = bytearray(bfp.message(0x12, b"xyz" * 10, seq=9))
        corrupt[-1] ^= 0xFF                                  # payload CRC must fail, framing must hold
        stream = (b"BFjunk" + bfp.message(0x01, b"", seq=1) + bytes(corrupt) +
                  bfp.message(0x40, big, seq=500, status=0))
        out = subprocess.run([HOST_BIN, "decode"], input=stream, capture_output=True, check=True).stdout.decode()
        lines = out.split("\n")
        self.assertEqual(lines[0].split()[:5], ["1", "1", "0", "0", "1"])
        self.assertEqual(lines[1].split()[:5], ["18", "9", "0", "30", "0"])
        self.assertEqual(lines[2].split()[:5], ["64", "500", "0", "70000", "1"])
        self.assertTrue(lines[3].startswith("dropped 6"))


class ToolAgainstFakeAgentTest(unittest.TestCase):
    def setUp(self):
        self.agent = FakeAgent()
        self.link = bfagent.Link(self.agent.port, wait=2, timeout=5)

    def tearDown(self):
        self.link.close()
        self.agent.close()

    def test_ping_and_info(self):
        self.assertEqual(self.link.request(bfp.PING, b"\x01" * 3000).payload, b"\x01" * 3000)
        self.assertIn("fake", self.link.info())

    def test_error_is_reported(self):
        with self.assertRaises(bfagent.AgentError):
            self.link.read_file("\\missing")

    def test_files_round_trip_in_chunks(self):
        data = os.urandom(50000)
        self.link.write_file("\\Temp\\x.bin", data, chunk=16 * 1024)
        self.assertEqual(self.link.read_file("\\Temp\\x.bin", chunk=7000), data)

    def test_mmu_walk(self):
        cpu = self.link.cpu()
        runs = bfagent.walk_mmu(self.link, int(cpu["ttb"], 16))
        self.assertIn((0x80000000, 0x10000000, 128 << 20, "section", 3, 1), runs)
        self.assertIn((0xA0000000, 0x10000000, 128 << 20, "section", 0, 1), runs)
        self.assertIn((0xB0000000, 0x80000000, 8192, "small", 0, 1), runs)
        regions = bfagent.physical_regions(runs)
        self.assertEqual([(s, e) for s, e, c in regions], [(0x10000000, 0x18000000), (0x80000000, 0x80002000)])

    def test_disk_dump_resumes_and_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "dsk.img")
            with open(out, "wb") as f:
                f.write(bytes(512) * 10)                 # pretend an earlier run stopped at sector 10
            info, digest = bfagent.disk_dump(self.link, "DSK1:", out, chunk_sectors=7)
            with open(out, "rb") as f:
                data = f.read()
            self.assertEqual(len(data), 100 * 512)
            self.assertEqual(data[50 * 512], 50)
            self.assertEqual(digest, hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    unittest.main()
