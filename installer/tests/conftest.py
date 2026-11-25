"""Pacotes de teste feitos na hora (arquivos aleatórios pequenos, manifesto e SHA256SUMS reais).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

FILES = {"bfagent.exe": 50_000, "linux/zImage": 300_000, "linux/initrd.img": 700_000, "linux/fs707dc.dtb": 3_000}


def write_release(root, version="0.5.1", sizes=FILES, seed=1, mutate=None):
    os.makedirs(os.path.join(root, "linux"), exist_ok=True)
    entries = {}
    for i, (name, size) in enumerate(sizes.items()):
        data = hashlib.sha256(f"{seed}-{i}".encode()).digest() * (size // 32 + 1)
        data = data[:size]
        with open(os.path.join(root, name), "wb") as f:
            f.write(data)
        entries[name] = {"file": name, "sha256": hashlib.sha256(data).hexdigest(), "size": size}
    m = {"format": 1, "product": "BigFoston OS", "version": version, "date": "2026-11-25", "channel": "stable",
         "device": "foston-fs707dc", "agent": dict(entries["bfagent.exe"], version="0.8.0"),
         "linux": [entries[n] for n in sizes if n.startswith("linux/")], "notes": "teste", "min_free_mb": 16}
    if mutate:
        mutate(m)
    with open(os.path.join(root, "manifest.json"), "w") as f:
        json.dump(m, f)
    with open(os.path.join(root, "SHA256SUMS"), "w") as f:
        for n in sizes:
            f.write(f"{entries[n]['sha256']}  {n}\n")
    return m


@pytest.fixture
def release_dir(tmp_path):
    root = tmp_path / "rel"
    write_release(str(root))
    return str(root)
