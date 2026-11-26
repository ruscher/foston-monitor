"""Integração com o UDisks2 de verdade: uma imagem FAT32 num disco de loop faz o papel do
cartão (montar, gravar, desmontar, montar de novo e conferir). Pulado se o polkit não deixar
criar o loop sem perguntar; BIGFOSTON_TEST_POLKIT=1 deixa perguntar. SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import subprocess
import time

import pytest

from bigfoston_installer.core import install as inst
from bigfoston_installer.core.package import open_package
from bigfoston_installer.core.storage import blockers


@pytest.fixture
def loop_card(tmp_path, monkeypatch):
    img = str(tmp_path / "sd.img")
    with open(img, "wb") as f:
        f.truncate(64 << 20)
    subprocess.run(["mkfs.fat", "-F", "32", "-n", "TESTSD", img], check=True, capture_output=True)
    # só pede autorização ao polkit (pode abrir uma janela de senha) com BIGFOSTON_TEST_POLKIT=1
    flags = [] if os.environ.get("BIGFOSTON_TEST_POLKIT") == "1" else ["--no-user-interaction"]
    r = subprocess.run(["udisksctl", "loop-setup", *flags, "-f", img], capture_output=True, text=True)
    if r.returncode:
        pytest.skip("udisksctl loop-setup não permitido: " + r.stderr.strip())
    dev = re.search(r"(/dev/loop\d+)", r.stdout).group(1)
    monkeypatch.setenv("BIGFOSTON_INSTALLER_TEST_LOOP", "1")
    from bigfoston_installer.platforms.linux_udisks import UDisksStorage
    st = UDisksStorage()
    card = None
    for _ in range(50):
        card = next((c for c in st.list_cards() if c.device == dev), None)
        if card:
            break
        time.sleep(0.1)
    try:
        yield st, card
    finally:
        try:
            st.unmount(card)
        except Exception:
            pass
        subprocess.run(["udisksctl", "loop-delete", "--no-user-interaction", "-b", dev], capture_output=True)


def test_prepare_real_fat32(loop_card, release_dir):
    st, card = loop_card
    assert card is not None and card.fs_type == "vfat" and blockers(card) == []
    info = inst.Installer(st, card, open_package(release_dir), lambda s: None).run()
    mount = st.mount(card)
    state = inst.card_state(mount)
    assert state["pending_update"] and state["prepared"]["version"] == info["version"]
    assert inst.verify_card(st, card, lambda s: None) == []
    st.unmount(card)


def test_loop_blocked_without_test_flag(loop_card, monkeypatch):
    st, card = loop_card
    monkeypatch.delenv("BIGFOSTON_INSTALLER_TEST_LOOP")
    c = st.refresh(card)
    assert c is not None and blockers(c)          # sem a variável, um loop não é cartão
