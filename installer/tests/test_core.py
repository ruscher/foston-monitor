"""Núcleo do instalador: manifesto, pacote, regras do cartão e a transação de preparo.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import os
import shutil
import stat
import zipfile

import pytest
from conftest import write_release

from bigfoston_installer.core import install as inst
from bigfoston_installer.core.manifest import ManifestError, identify, load_devices, parse_manifest
from bigfoston_installer.core.package import open_package
from bigfoston_installer.core.storage import (BLOCK_BIG, BLOCK_FIXED, BLOCK_FS, BLOCK_RO, BLOCK_SYSTEM, Card,
                                              Storage, StorageError, blockers)

# ------------------------------------------------------------------ manifesto


def test_manifest_ok(release_dir):
    pkg = open_package(release_dir)
    assert pkg.release.version == "0.5.1" and pkg.release.agent_version == "0.8.0"
    assert [f.name for f in pkg.release.files] == ["bfagent.exe", "linux/fs707dc.dtb", "linux/initrd.img",
                                                   "linux/zImage"]
    pkg.verify()


@pytest.mark.parametrize("mutate,msg", [
    (lambda m: m.update(format=2), "formato"),
    (lambda m: m.update(product="Outro"), "não é do BigFoston"),
    (lambda m: m.update(version="1.0"), "versão"),
    (lambda m: m.update(channel="nightly"), "canal"),
    (lambda m: m["linux"].pop(), "faltam"),
    (lambda m: m["linux"][0].update(file="../etc/passwd"), "não permitido"),
    (lambda m: m["linux"][0].update(sha256="xyz"), "SHA-256"),
    (lambda m: m["agent"].update(size=50 << 20), "tamanho"),
    (lambda m: m.update(min_free_mb=True), "espaço"),
])
def test_manifest_rejects(tmp_path, mutate, msg):
    root = str(tmp_path / "r")
    write_release(root, mutate=mutate)
    with pytest.raises(ManifestError, match=msg):
        open_package(root)


def test_manifest_json_broken():
    with pytest.raises(ManifestError, match="ilegível"):
        parse_manifest("{")


def test_sums_must_match_manifest(release_dir):
    with open(os.path.join(release_dir, "SHA256SUMS"), "a") as f:
        f.write("0" * 64 + "  extra\n")
    with pytest.raises(ManifestError, match="não dizem o mesmo"):
        open_package(release_dir)


def test_tampered_file_fails_verify(release_dir):
    with open(os.path.join(release_dir, "linux", "initrd.img"), "r+b") as f:
        f.seek(1000)
        f.write(b"X")
    pkg = open_package(release_dir)
    with pytest.raises(ManifestError, match="initrd.img: SHA-256"):
        pkg.verify()


def test_symlink_in_dir_rejected(release_dir, tmp_path):
    target = os.path.join(release_dir, "linux", "zImage")
    shutil.move(target, str(tmp_path / "z"))
    os.symlink(str(tmp_path / "z"), target)
    with pytest.raises(ManifestError, match="arquivo comum"):
        open_package(release_dir).verify()


# ------------------------------------------------------------------ zip

def _zip(src, dst, extra=None, prefix="BigFoston-OS-FS707DC-0.5.1/"):
    with zipfile.ZipFile(dst, "w") as z:
        for n in ["manifest.json", "SHA256SUMS", "bfagent.exe", "linux/zImage", "linux/initrd.img",
                  "linux/fs707dc.dtb"]:
            z.write(os.path.join(src, n), prefix + n)
        if extra:
            extra(z)


def test_zip_ok(release_dir, tmp_path):
    zp = str(tmp_path / "r.zip")
    _zip(release_dir, zp)
    pkg = open_package(zp)
    pkg.verify()
    root = pkg.root
    pkg.close()
    assert not os.path.exists(root)


@pytest.mark.parametrize("extra,msg", [
    (lambda z: z.writestr("../../evil.sh", "x"), "inesperado"),
    (lambda z: z.writestr("BigFoston-OS-FS707DC-0.5.1/linux/../../x", "x"), "inesperado"),
    (lambda z: z.writestr("BigFoston-OS-FS707DC-0.5.1/autorun.inf", "x"), "inesperado"),
])
def test_zip_rejects(release_dir, tmp_path, extra, msg):
    zp = str(tmp_path / "r.zip")
    _zip(release_dir, zp, extra)
    with pytest.raises(ManifestError, match=msg):
        open_package(zp)


def test_zip_symlink_rejected(release_dir, tmp_path):
    zp = str(tmp_path / "r.zip")

    def link(z):
        info = zipfile.ZipInfo("x/linux/zImage")
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        z.writestr(info, "/etc/passwd")
    with zipfile.ZipFile(zp, "w") as z:
        z.write(os.path.join(release_dir, "manifest.json"), "x/manifest.json")
        link(z)
    with pytest.raises(ManifestError, match="link simbólico"):
        open_package(zp)


def test_not_a_package(tmp_path):
    p = tmp_path / "x.txt"
    p.write_text("oi")
    with pytest.raises(ManifestError, match="escolha"):
        open_package(str(p))


# ------------------------------------------------------------------ compatibilidade

def test_devices_and_identify():
    devs = load_devices()
    assert devs[0].id == "foston-fs707dc" and devs[0].installable
    info = {"os": "Windows CE 5.0 build 1400 ", "oem": "MTK", "platform": "APOLLO", "screen": "800x480 16 bpp"}
    assert identify(info).id == "foston-fs707dc"
    assert identify(dict(info, platform="OUTRA")) is None


# ------------------------------------------------------------------ regras do cartão

def card(**kw):
    base = dict(key="k", device="/dev/sdx1", label="SD", fs_type="vfat", fs_uuid="1234-ABCD", size=2 * 10 ** 9,
                connection="usb", removable=True)
    base.update(kw)
    return Card(**base)


def test_blockers():
    assert blockers(card()) == []
    assert BLOCK_SYSTEM in blockers(card(system_disk=True))
    assert BLOCK_FIXED in blockers(card(removable=False, connection="ata"))
    assert BLOCK_FS in blockers(card(fs_type="exfat"))
    assert BLOCK_BIG in blockers(card(size=64 * 10 ** 9))
    assert BLOCK_RO in blockers(card(read_only=True))
    assert blockers(card(size=31_900_000_000)) == []                    # SDHC de 32 GB testado
    assert card(usb_vendor="045e").is_foston_usb


# ------------------------------------------------------------------ transação (armazenamento falso)

class DirStorage(Storage):
    """Um "cartão" que é uma pasta; remontar pode corromper um arquivo (simula cartão ruim)."""

    def __init__(self, path, card_):
        self.path, self.card, self.corrupt, self.mounts, self.gone = path, card_, None, 0, False

    def list_cards(self):
        return [] if self.gone else [self.card]

    def refresh(self, c):
        return None if self.gone else self.card

    def mount(self, c):
        self.mounts += 1
        if self.corrupt and self.mounts == 2:          # a releitura depois de gravar
            with open(os.path.join(self.path, self.corrupt), "r+b") as f:
                f.seek(10)
                f.write(b"\xff")
        return self.path

    def unmount(self, c):
        pass

    def eject(self, c):
        pass


@pytest.fixture
def sd(tmp_path):
    root = tmp_path / "sd"
    (root / "iGO").mkdir(parents=True)
    (root / "iGO" / "mapa.fbl").write_bytes(b"mapa" * 1000)
    (root / "BigFoston").mkdir()
    (root / "BigFoston" / "bfagent.exe").write_bytes(b"agente antigo")
    (root / "BigFoston" / "data").mkdir()
    (root / "BigFoston" / "data" / "settings.conf").write_text("home_mode = 3\n")
    return str(root)


def run_install(sd, release_dir, corrupt=None, cancel_at=None):
    st = DirStorage(sd, card())
    st.corrupt = corrupt
    pkg = open_package(release_dir)
    steps = []
    ins = None

    def report(s):
        steps.append(s)
        if cancel_at and s.stage == "write" and s.done > cancel_at:
            ins.cancel()
    ins = inst.Installer(st, card(), pkg, report)
    return ins, ins.run, steps


def test_install_ok(sd, release_dir):
    ins, run, steps = run_install(sd, release_dir)
    info = run()
    b = os.path.join(sd, "BigFoston")
    assert sorted(os.listdir(os.path.join(b, "update"))) == ["SHA256SUMS", "fs707dc.dtb", "initrd.img", "zImage"]
    assert open(os.path.join(b, "bfagent.exe.old"), "rb").read() == b"agente antigo"
    assert open(os.path.join(sd, "iGO", "mapa.fbl"), "rb").read() == b"mapa" * 1000      # do usuário: intacto
    assert open(os.path.join(b, "data", "settings.conf")).read() == "home_mode = 3\n"
    sums = open(os.path.join(b, "update", "SHA256SUMS")).read()
    assert "  zImage\n" in sums and "linux/" not in sums and "bfagent" not in sums     # o formato do agente
    assert info["version"] == "0.5.1" and json.load(open(os.path.join(b, "install.json")))["agent_version"] == "0.8.0"
    st = inst.card_state(sd)
    assert st["pending_update"] and st["prepared"]["version"] == "0.5.1"
    assert steps[-1].stage == "done" and steps[-1].done == steps[-1].total
    # progresso real e crescente dentro de cada fase
    writes = [s.done for s in steps if s.stage == "write"]
    assert writes == sorted(writes) and writes[-1] == open_package(release_dir).release.total_size
    assert not os.path.exists(os.path.join(b, "update.tmp")) and not os.path.exists(os.path.join(b, "bfagent.exe.new"))


def test_install_corrupt_card_leaves_card_as_it_was(sd, release_dir):
    ins, run, _ = run_install(sd, release_dir, corrupt="BigFoston/update.tmp/initrd.img")
    with pytest.raises(StorageError, match="initrd.img: o que ficou no cartão não confere"):
        run()
    b = os.path.join(sd, "BigFoston")
    assert sorted(os.listdir(b)) == ["bfagent.exe", "data"]
    assert open(os.path.join(b, "bfagent.exe"), "rb").read() == b"agente antigo"


def test_install_cancel_cleans_up(sd, release_dir):
    ins, run, _ = run_install(sd, release_dir, cancel_at=200_000)
    with pytest.raises(inst.Cancelled):
        run()
    assert sorted(os.listdir(os.path.join(sd, "BigFoston"))) == ["bfagent.exe", "data"]


def test_install_replaces_pending_update(sd, release_dir, tmp_path):
    run_install(sd, release_dir)[1]()
    other = str(tmp_path / "rel2")
    write_release(other, version="0.5.2", seed=7)
    run_install(sd, other)[1]()
    st = inst.card_state(sd)
    assert st["prepared"]["version"] == "0.5.2"
    pkg = open_package(other)
    want = {f.name.split("/")[-1]: f.sha256 for f in pkg.release.files}
    got = dict(reversed(ln.split("  ")) for ln in open(os.path.join(sd, "BigFoston", "update", "SHA256SUMS")).read().split("\n") if ln)
    assert all(want[k] == v for k, v in got.items())
    assert inst.verify_card(DirStorage(sd, card()), card(), lambda s: None) == []


def test_install_card_removed(sd, release_dir):
    st = DirStorage(sd, card())
    st.gone = True
    with pytest.raises(StorageError, match="removido"):
        inst.Installer(st, card(), open_package(release_dir), lambda s: None).run()


def test_install_other_card_inserted(sd, release_dir):
    st = DirStorage(sd, card(fs_uuid="9999-0000"))
    with pytest.raises(StorageError, match="outro cartão"):
        inst.Installer(st, card(), open_package(release_dir), lambda s: None).run()


def test_install_no_space(sd, release_dir, monkeypatch):
    real = os.statvfs

    class Fake:
        f_bavail, f_frsize = 10, 4096
    monkeypatch.setattr(inst.os, "statvfs", lambda p: Fake if p == sd else real(p))
    with pytest.raises(StorageError, match="Espaço insuficiente"):
        run_install(sd, release_dir)[1]()


def test_verify_card_detects_damage(sd, release_dir):
    run_install(sd, release_dir)[1]()
    with open(os.path.join(sd, "BigFoston", "update", "zImage"), "r+b") as f:
        f.write(b"\0\0\0")
    problems = inst.verify_card(DirStorage(sd, card()), card(), lambda s: None)
    assert problems == ["zImage está diferente do que foi preparado (corrompido)."]
