"""Backup do cartão para uma pasta do computador, com manifesto e SHA-256 de cada arquivo.

    <destino>/BigFoston-backup-<data>-<rótulo>/
        files/...                os arquivos, com as mesmas pastas do cartão
        backup.json              data, cartão, escopo e {caminho: [tamanho, sha256]}

A cópia é por streaming; depois de copiar, cada arquivo do backup é relido e conferido.
O backup nunca fica no próprio cartão (o destino é conferido).

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import hashlib
import json
import os
import threading
import time
from typing import Callable

from .install import Cancelled, Step
from .package import CHUNK
from .storage import Card, StorageError

SCOPE_BIGFOSTON = "bigfoston"      # só a pasta BigFoston/ (rápido; o que o instalador muda)
SCOPE_ALL = "all"                  # o cartão inteiro (mapas e tudo)


def _walk(root: str, scope: str):
    top = os.path.join(root, "BigFoston") if scope == SCOPE_BIGFOSTON else root
    if not os.path.isdir(top):
        return
    for d, dirs, files in os.walk(top, followlinks=False):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(d, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            yield os.path.relpath(p, root)


def plan(mount: str, scope: str) -> tuple[list[str], int]:
    files = list(_walk(mount, scope))
    return files, sum(os.path.getsize(os.path.join(mount, f)) for f in files)


def _inside(path: str, parent: str) -> bool:
    path, parent = os.path.realpath(path), os.path.realpath(parent)
    return path == parent or path.startswith(parent.rstrip(os.sep) + os.sep)


class Backup:
    def __init__(self, card: Card, mount: str, dest_parent: str, scope: str, report: Callable[[Step], None]):
        if _inside(dest_parent, mount):
            raise StorageError("O backup não pode ficar no próprio cartão: escolha uma pasta do computador.")
        self.card, self.mount, self.scope, self.report = card, mount, scope, report
        stamp = time.strftime("%Y-%m-%d-%H%M%S")
        label = "".join(ch for ch in (card.label or "SD") if ch.isalnum() or ch in "-_") or "SD"
        self.dest = os.path.join(dest_parent, f"BigFoston-backup-{stamp}-{label}")
        self.cancel_event = threading.Event()

    def cancel(self) -> bool:
        self.cancel_event.set()
        return True

    def run(self) -> str:
        files, total = plan(self.mount, self.scope)
        st = os.statvfs(os.path.dirname(self.dest) or ".")
        if st.f_bavail * st.f_frsize < total + (16 << 20):
            raise StorageError("Não há espaço suficiente no computador para o backup.")
        os.makedirs(os.path.join(self.dest, "files"))
        index, done = {}, 0
        for rel in files:
            src = os.path.join(self.mount, rel)
            dst = os.path.join(self.dest, "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            h = hashlib.sha256()
            with open(src, "rb") as fi, open(dst, "wb") as fo:
                while chunk := fi.read(CHUNK):
                    if self.cancel_event.is_set():
                        raise Cancelled()
                    h.update(chunk)
                    fo.write(chunk)
                    done += len(chunk)
                    self.report(Step("write", f"Copiando {rel}", done, total, rel))
            index[rel.replace(os.sep, "/")] = [os.path.getsize(dst), h.hexdigest()]
        meta = {"format": 1, "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "scope": self.scope,
                "card": {"label": self.card.label, "uuid": self.card.fs_uuid, "size": self.card.size,
                         "model": self.card.title},
                "files": index}
        with open(os.path.join(self.dest, "backup.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1, ensure_ascii=False)
        problems = verify(self.dest, self.report)
        if problems:
            raise StorageError("O backup não confere: " + problems[0])
        self.report(Step("done", "Backup feito e conferido", total, total))
        return self.dest


def verify(dest: str, report: Callable[[Step], None] | None = None) -> list[str]:
    """Relê o backup e confere cada arquivo contra o backup.json."""
    try:
        with open(os.path.join(dest, "backup.json"), encoding="utf-8") as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return ["backup.json ausente ou ilegível"]
    files = meta.get("files", {})
    total = sum(v[0] for v in files.values())
    done, out = 0, []
    for rel, (size, sha) in files.items():
        p = os.path.join(dest, "files", *rel.split("/"))
        if not _inside(p, os.path.join(dest, "files")):
            out.append(f"{rel}: caminho inválido")
            continue
        h = hashlib.sha256()
        try:
            with open(p, "rb") as f:
                while chunk := f.read(CHUNK):
                    h.update(chunk)
                    done += len(chunk)
                    if report:
                        report(Step("verify", f"Conferindo {rel}", done, total, rel))
        except OSError:
            out.append(f"{rel}: ausente")
            continue
        if h.hexdigest() != sha:
            out.append(f"{rel}: diferente do original")
    return out
