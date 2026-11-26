"""Peças comuns da interface: tarefas em segundo plano, tamanhos, imagens e o esqueleto das páginas.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import gettext
import os
import threading
import traceback
from importlib import resources

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, GLib, Gtk  # noqa: E402

DOMAIN = "bigfoston-installer"
_t = gettext.translation(DOMAIN, localedir=os.path.join(os.path.dirname(__file__), "..", "locale"), fallback=True)
_ = _t.gettext


def data_path(*parts: str) -> str:
    return str(resources.files("bigfoston_installer").joinpath("data", *parts))


def human_size(n: int) -> str:
    if n < 0:
        return "—"
    for unit, div in (("GB", 1000 ** 3), ("MB", 1000 ** 2), ("KB", 1000)):
        if n >= div:
            v = n / div
            return f"{v:.1f} {unit}".replace(".", ",") if v < 100 else f"{v:.0f} {unit}"
    return f"{n} B"


def run_bg(fn, on_done=None, on_error=None):
    """Roda fn() numa thread; o resultado (ou o erro) volta na thread da interface."""
    def worker():
        try:
            result = fn()
        except Exception as e:                       # noqa: BLE001 — a mensagem vai para o usuário
            traceback.print_exc()
            if on_error:
                GLib.idle_add(on_error, e)
            return
        if on_done:
            GLib.idle_add(on_done, result)
    t = threading.Thread(target=worker, daemon=True)
    t.start()
    return t


def picture(name: str, height: int, can_shrink=True) -> Gtk.Picture:
    pic = Gtk.Picture.new_for_filename(data_path("images", name))
    pic.set_content_fit(Gtk.ContentFit.CONTAIN)
    pic.set_can_shrink(can_shrink)
    pic.set_size_request(-1, height)
    return pic


def label(text: str, css: tuple[str, ...] = (), wrap=True, xalign=0.0, markup=False) -> Gtk.Label:
    lb = Gtk.Label(wrap=wrap, xalign=xalign)
    if markup:
        lb.set_markup(text)
    else:
        lb.set_text(text)
    for c in css:
        lb.add_css_class(c)
    return lb


class Page(Adw.NavigationPage):
    """Página com cabeçalho, conteúdo rolável e uma barra embaixo com o botão principal."""

    _serial = 0

    def __init__(self, title: str, tag: str, next_label: str | None = None, on_next=None, width=720):
        # etiqueta única (um fluxo pode empilhar duas páginas do mesmo tipo); "home" é fixa
        Page._serial += 1
        super().__init__(title=title, tag=tag if tag == "home" else f"{tag}-{Page._serial}")
        self.kind = tag
        self.toolbar = Adw.ToolbarView()
        self.header = Adw.HeaderBar()
        self.toolbar.add_top_bar(self.header)
        clamp = Adw.Clamp(maximum_size=width, tightening_threshold=int(width * 0.8))
        self.box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18, margin_top=24, margin_bottom=24,
                           margin_start=18, margin_end=18)
        clamp.set_child(self.box)
        scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vexpand=True)
        scroll.set_child(clamp)
        self.toolbar.set_content(scroll)
        self.next_button = None
        if next_label:
            bar = Gtk.Box(spacing=12, margin_top=12, margin_bottom=12, margin_start=18, margin_end=18,
                          halign=Gtk.Align.END)
            self.bottom = bar
            self.next_button = Gtk.Button(label=next_label)
            self.next_button.add_css_class("suggested-action")
            self.next_button.add_css_class("pill")
            if on_next:
                self.next_button.connect("clicked", lambda *_: on_next())
            bar.append(self.next_button)
            self.toolbar.add_bottom_bar(bar)
        self.set_child(self.toolbar)

    def add(self, widget):
        self.box.append(widget)
        return widget


def status_icon(state: str) -> str:
    return {"tested": "emblem-ok-symbolic", "partial": "dialog-warning-symbolic",
            "experimental": "applications-science-symbolic", "untested": "dialog-question-symbolic",
            "incompatible": "action-unavailable-symbolic"}.get(state, "dialog-question-symbolic")


STATE_TEXT = {
    "tested": _("Compatível e testado em hardware real"),
    "partial": _("Compatibilidade parcial"),
    "experimental": _("Experimental"),
    "untested": _("Não testado"),
    "incompatible": _("Incompatível"),
}


def load_css():
    css = """
    .hero-title { font-size: 26pt; font-weight: 800; }
    .choice-card { padding: 18px; border-radius: 16px; }
    .choice-card image { color: @accent_color; }
    .usage-bar trough { min-height: 8px; border-radius: 4px; }
    .usage-bar progress { min-height: 8px; border-radius: 4px; }
    .step-number { min-width: 34px; min-height: 34px; border-radius: 17px; background: alpha(@accent_bg_color, 0.18);
                   color: @accent_color; font-weight: 800; }
    .mono-log { font-family: monospace; font-size: 9pt; }
    .device-photo { border-radius: 12px; }
    """
    prov = Gtk.CssProvider()
    prov.load_from_string(css)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), prov,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
