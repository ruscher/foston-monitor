"""BigFoston OS Installer (GTK 4 + Libadwaita).

    python3 -m bigfoston_installer          (ou o atalho do menu, depois de instalado)

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio  # noqa: E402

from .common import load_css  # noqa: E402

APP_ID = "br.org.biglinux.BigFostonInstaller"


class App(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        win = self.props.active_window
        if not win:
            from .window import InstallerWindow
            storage, err = None, ""
            try:
                if sys.platform.startswith("linux"):
                    from ..platforms.linux_udisks import UDisksStorage
                    storage = UDisksStorage()
                else:
                    err = "plataforma ainda não suportada"
            except Exception as e:                   # noqa: BLE001
                err = str(e)
            load_css()
            win = InstallerWindow(self, storage, err)
        win.present()


def main(argv=None) -> int:
    return App().run(argv if argv is not None else sys.argv)
