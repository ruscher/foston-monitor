"""A interface de verdade (GTK 4 + Libadwaita) num display virtual: o fluxo de instalação
inteiro com um "cartão" que é uma pasta, clicando nos botões. Salva uma captura por tela em
$BF_SHOTS (se definido). Rodar com:  xvfb-run -a python3 -m pytest installer/tests/test_ui.py

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import time

import pytest

if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
    pytest.skip("sem display (use xvfb-run)", allow_module_level=True)

import gi  # noqa: E402

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Graphene", "1.0")
from gi.repository import Adw, Gio, GLib, Graphene, Gtk  # noqa: E402

from bigfoston_installer.core import install as inst  # noqa: E402
from bigfoston_installer.core.package import open_package  # noqa: E402
from bigfoston_installer.core.storage import Card, Storage  # noqa: E402
from bigfoston_installer.ui.common import load_css  # noqa: E402
from bigfoston_installer.ui.window import InstallerWindow  # noqa: E402


class FakeStorage(Storage):
    def __init__(self, root):
        self.root = root
        self.cards = [
            Card(key="sd", device="/dev/sdf1", label="FOSTON", fs_type="vfat", fs_uuid="0897-EDDF", size=2 * 10 ** 9,
                 drive_vendor="Generic", drive_model="SD Reader", drive_serial="123", connection="usb",
                 mount_point=root, free=1_500_000_000, has_bigfoston=True),
            Card(key="ssd", device="/dev/sde1", label="Backup", fs_type="ntfs", fs_uuid="X", size=2 * 10 ** 12,
                 drive_vendor="SanDisk", drive_model="Portable SSD", connection="usb"),
            Card(key="sys", device="/dev/sdd2", label="", fs_type="btrfs", fs_uuid="Y", size=480 * 10 ** 9,
                 drive_model="KINGSTON SA400", removable=False, connection="ata", system_disk=True, mount_point="/"),
        ]
        self.ejected = False

    def list_cards(self):
        return list(self.cards)

    def refresh(self, c):
        return next((x for x in self.cards if x.key == c.key), None)

    def mount(self, c):
        return self.root

    def unmount(self, c):
        pass

    def eject(self, c):
        self.ejected = True

    def watch(self, cb):
        return 1

    def unwatch(self, sub):
        pass


def pump(sec=0.3):
    ctx = GLib.MainContext.default()
    end = time.monotonic() + sec
    while time.monotonic() < end:
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.01)


def wait_for(cond, sec=20):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        pump(0.05)
        if cond():
            return True
    return False


def shot(win, name):
    out = os.environ.get("BF_SHOTS")
    if not out:
        return
    pump(0.4)
    w, h = win.get_width(), win.get_height()
    paint = Gtk.WidgetPaintable.new(win)
    snap = Gtk.Snapshot.new()
    paint.snapshot(snap, w, h)
    node = snap.to_node()
    tex = win.get_renderer().render_texture(node, Graphene.Rect().init(0, 0, w, h))
    os.makedirs(out, exist_ok=True)
    tex.save_to_png(os.path.join(out, name + ".png"))


def find(widget, pred):
    if pred(widget):
        return widget
    child = widget.get_first_child()
    while child:
        r = find(child, pred)
        if r:
            return r
        child = child.get_next_sibling()
    return None


def find_all(widget, pred, out=None):
    out = [] if out is None else out
    if pred(widget):
        out.append(widget)
    child = widget.get_first_child()
    while child:
        find_all(child, pred, out)
        child = child.get_next_sibling()
    return out


@pytest.fixture(scope="module")
def app():
    Adw.init()
    load_css()
    a = Adw.Application(application_id="br.org.biglinux.BigFostonInstaller.Test",
                        flags=Gio.ApplicationFlags.NON_UNIQUE)
    a.register(None)
    return a


@pytest.fixture
def win(tmp_path, app):
    sd = tmp_path / "sd"
    (sd / "BigFoston" / "data").mkdir(parents=True)
    (sd / "BigFoston" / "data" / "settings.conf").write_text("home_mode = 0\n")
    (sd / "iGO").mkdir()
    (sd / "iGO" / "mapa.fbl").write_bytes(b"m" * 5000)
    st = FakeStorage(str(sd))
    w = InstallerWindow(app, st)
    w.state.backup_dest = str(tmp_path / "pc")
    w.set_default_size(900, 720)
    w.present()
    pump(0.8)
    yield w, st, str(sd)
    w.destroy()
    pump(0.2)


def top(w):
    return w.nav.get_visible_page()


def test_install_flow(win, release_dir, tmp_path):
    w, st, sd = win
    shot(w, "01-inicio")
    w.start_install()
    pump()
    page = top(w)
    assert page.kind == "terms" and not page.next_button.get_sensitive()
    checks = find_all(page, lambda x: isinstance(x, Gtk.CheckButton))
    assert len(checks) == 4
    for c in checks[:3]:
        c.set_active(True)
    pump(0.1)
    assert not page.next_button.get_sensitive()           # falta uma confirmação
    checks[3].set_active(True)
    pump(0.1)
    assert page.next_button.get_sensitive()
    shot(w, "02-termos")
    page.next_button.emit("clicked")
    pump()
    assert top(w).kind == "compat"
    shot(w, "03-compatibilidade")
    top(w).next_button.emit("clicked")
    pump()
    page = top(w)
    assert page.kind == "version" and not page.next_button.get_sensitive()
    # a escolha do arquivo (o diálogo do sistema) é simulada: o mesmo caminho da janela
    pkg = open_package(release_dir)
    pkg.verify()
    w.state.package = pkg
    w.nav.pop()
    pump()
    w.push(w.version_page())
    pump(0.6)
    assert top(w).next_button.get_sensitive()
    shot(w, "04-versao")
    top(w).next_button.emit("clicked")
    pump()
    assert top(w).kind == "method"
    shot(w, "05-metodo")
    top(w).next_button.emit("clicked")
    assert wait_for(lambda: top(w).kind == "card" and find(top(w), lambda x: isinstance(x, Adw.ActionRow)
                                                               and "SD Reader" in (x.get_title() or "")))
    page = top(w)
    assert not page.next_button.get_sensitive()
    radios = find_all(page, lambda x: isinstance(x, Gtk.CheckButton))
    assert len(radios) == 1                                  # só o cartão é escolhível
    bad = find_all(page, lambda x: isinstance(x, Adw.ActionRow) and "sistema deste computador" in (x.get_subtitle() or ""))
    assert len(bad) == 1
    radios[0].set_active(True)
    pump(0.1)
    assert page.next_button.get_sensitive() and w.state.card.key == "sd"
    shot(w, "06-cartao")
    page.next_button.emit("clicked")
    pump()
    assert top(w).kind == "backup"
    shot(w, "07-backup")
    top(w).next_button.emit("clicked")
    pump()
    assert top(w).kind == "summary"
    shot(w, "08-resumo")
    # o diálogo de confirmação: responde "Preparar"
    top(w).next_button.emit("clicked")
    pump(0.5)
    dlg = w.get_visible_dialog()
    assert isinstance(dlg, Adw.AlertDialog)
    shot(w, "09-confirmar")
    dlg.emit("response", "go")
    dlg.close()
    pump()
    assert top(w).kind == "progress"
    shot(w, "10-progresso")
    assert wait_for(lambda: top(w).kind == "guide", 30)
    shot(w, "11-guia")
    assert st.ejected
    state = inst.card_state(sd)
    assert state["pending_update"] and state["prepared"]["version"] == "0.5.1"
    assert os.path.isfile(os.path.join(sd, "iGO", "mapa.fbl"))
    assert w.state.backup_done and os.path.isfile(os.path.join(w.state.backup_done, "backup.json"))
    top(w).next_button.emit("clicked")
    pump()
    assert top(w).kind == "done"
    shot(w, "12-concluido")


def test_repair_and_restore(win, release_dir):
    w, st, sd = win
    inst.Installer(st, st.cards[0], open_package(release_dir), lambda s: None).run()
    with open(os.path.join(sd, "BigFoston", "update", "initrd.img"), "r+b") as f:
        f.write(b"\0" * 16)
    w.start_repair()
    assert wait_for(lambda: len(find_all(top(w), lambda x: isinstance(x, Gtk.CheckButton))) == 1)
    find_all(top(w), lambda x: isinstance(x, Gtk.CheckButton))[0].set_active(True)
    pump(0.1)
    top(w).next_button.emit("clicked")
    assert wait_for(lambda: find(top(w), lambda x: isinstance(x, Adw.StatusPage)) is not None)
    sp = find(top(w), lambda x: isinstance(x, Adw.StatusPage))
    assert "initrd.img" in sp.get_description()
    shot(w, "13-verificar-problema")
    w.nav.pop_to_tag("home")
    w.start_restore()
    pump()
    shot(w, "14-restaurar")
    assert top(w).kind == "restore"
