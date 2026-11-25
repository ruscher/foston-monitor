"""Cartões pelo UDisks2 (D-Bus de sistema): listar, montar, desmontar e ejetar como usuário
comum — o UDisks pede autorização ao polkit só se precisar. Nada aqui roda como root.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import os

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

from ..core.storage import SYSTEM_MOUNTS, Card, Storage, StorageError  # noqa: E402

BUS = "org.freedesktop.UDisks2"
ROOT = "/org/freedesktop/UDisks2"
I_BLOCK = "org.freedesktop.UDisks2.Block"
I_FS = "org.freedesktop.UDisks2.Filesystem"
I_DRIVE = "org.freedesktop.UDisks2.Drive"
I_PART = "org.freedesktop.UDisks2.Partition"


def _bytes(v) -> str:
    """ay do D-Bus (terminado em zero) -> str."""
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).rstrip(b"\0").decode("utf-8", "replace")
    return bytes(bytearray(v)).rstrip(b"\0").decode("utf-8", "replace") if v else ""


def _usb_vendor(dev: str) -> str:
    """idVendor do aparelho USB por trás de /dev/sdX (vazio se não for USB)."""
    name = os.path.basename(dev).rstrip("0123456789")
    if name.startswith("mmcblk"):
        return ""
    try:
        node = os.path.realpath(f"/sys/block/{name}/device")
    except OSError:
        return ""
    while node != "/":
        p = os.path.join(node, "idVendor")
        if os.path.isfile(p):
            try:
                with open(p) as f:
                    return f.read().strip()
            except OSError:
                return ""
        node = os.path.dirname(node)
    return ""


class UDisksStorage(Storage):
    def __init__(self):
        try:
            self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        except GLib.Error as e:
            raise StorageError(f"Não foi possível falar com o UDisks2: {e.message}") from None

    def _call(self, path, iface, method, args, reply="()", timeout=60000):
        try:
            return self.bus.call_sync(BUS, path, iface, method, args, GLib.VariantType(reply),
                                      Gio.DBusCallFlags.NONE, timeout, None)
        except GLib.Error as e:
            raise StorageError(e.message.split(": ", 1)[-1]) from None

    def _objects(self) -> dict:
        r = self._call(ROOT, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", None,
                       "(a{oa{sa{sv}}})")
        return r.unpack()[0]

    def _cards(self, objs: dict) -> list[Card]:
        # discos que têm alguma partição montada num lugar do sistema
        system_drives = set()
        for path, ifs in objs.items():
            fs, blk = ifs.get(I_FS), ifs.get(I_BLOCK)
            if fs and blk:
                mps = [_bytes(m) for m in fs.get("MountPoints", [])]
                if any(m in SYSTEM_MOUNTS for m in mps):
                    system_drives.add(blk.get("Drive", "/"))
        cards = []
        for path, ifs in objs.items():
            blk, fs = ifs.get(I_BLOCK), ifs.get(I_FS)
            if not blk or not fs or blk.get("HintIgnore"):
                continue
            drive_path = blk.get("Drive", "/")
            drive = objs.get(drive_path, {}).get(I_DRIVE, {})
            dev = _bytes(blk.get("PreferredDevice") or blk.get("Device"))
            mps = [_bytes(m) for m in fs.get("MountPoints", [])]
            mp = mps[0] if mps else ""
            free = -1
            if mp:
                try:
                    st = os.statvfs(mp)
                    free = st.f_bavail * st.f_frsize
                except OSError:
                    pass
            card = Card(
                key=path, device=dev, label=blk.get("IdLabel", ""), fs_type=blk.get("IdType", ""),
                fs_uuid=blk.get("IdUUID", ""), size=int(blk.get("Size", 0)),
                drive_vendor=drive.get("Vendor", "").strip(), drive_model=drive.get("Model", "").strip(),
                drive_serial=drive.get("Serial", "").strip(), connection=drive.get("ConnectionBus", ""),
                removable=bool(drive.get("Removable") or drive.get("MediaRemovable")),
                read_only=bool(blk.get("ReadOnly")), mount_point=mp, free=free,
                usb_vendor=_usb_vendor(dev), system_disk=drive_path in system_drives,
                has_bigfoston=bool(mp) and os.path.isdir(os.path.join(mp, "BigFoston")))
            card.extra["drive"] = drive_path
            # testes de integração: um disco de loop faz o papel do cartão, só com a variável
            if dev.startswith("/dev/loop") and os.environ.get("BIGFOSTON_INSTALLER_TEST_LOOP") == "1":
                card.removable, card.connection = True, "loop"
            cards.append(card)
        cards.sort(key=lambda c: (c.system_disk, not c.removable, c.device))
        return cards

    def list_cards(self) -> list[Card]:
        return self._cards(self._objects())

    def refresh(self, card: Card) -> Card | None:
        for c in self.list_cards():
            if c.key == card.key:
                return c
        return None

    def mount(self, card: Card) -> str:
        now = self.refresh(card)
        if now and now.mount_point:
            return now.mount_point
        r = self._call(card.key, I_FS, "Mount", GLib.Variant("(a{sv})", ({},)), "(s)")
        return r.unpack()[0]

    def unmount(self, card: Card) -> None:
        now = self.refresh(card)
        if now and now.mount_point:
            self._call(card.key, I_FS, "Unmount", GLib.Variant("(a{sv})", ({},)))

    def eject(self, card: Card) -> None:
        self.unmount(card)
        drive = card.extra.get("drive")
        if drive and drive != "/":
            try:
                self._call(drive, I_DRIVE, "Eject", GLib.Variant("(a{sv})", ({},)))
            except StorageError:
                pass                       # leitores embutidos não ejetam: desmontado basta
