"""Preparar o cartão SD com uma versão do BigFoston OS, como uma transação.

O que fica no cartão (nada mais é tocado):
    BigFoston/bfagent.exe          o agente (aberto pelo "Caminho da navegação" do CE)
    BigFoston/bfagent.exe.sha256
    BigFoston/bfagent.exe.old      o agente que estava lá antes (se havia)
    BigFoston/update/              zImage, initrd.img, fs707dc.dtb e SHA256SUMS (por último):
                                   o agente confere e grava na memória interna ao ligar
    BigFoston/install.json         a versão preparada (lida pelo instalador e pelo agente)

Passos: conferir o cartão e o espaço -> gravar em update.tmp e bfagent.exe.new (fsync) ->
sincronizar, desmontar e montar de novo (a releitura vem do cartão, não do cache) -> conferir
cada arquivo por SHA-256 -> só então trocar os nomes (update.tmp -> update, .new -> .exe).
Cancelar só é aceito antes da troca. Uma falha antes dela deixa o cartão como estava (as
sobras .tmp/.new são apagadas); um update anterior que ainda não foi aplicado é substituído.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import threading
import time
from dataclasses import dataclass
from typing import Callable

from .manifest import AGENT_FILE, ManifestError, Release
from .package import CHUNK, Package
from .storage import Card, Storage, StorageError

DIR = "BigFoston"
MARGIN = 4 * 1000 * 1000           # folga além dos arquivos


class Cancelled(Exception):
    pass


@dataclass
class Step:
    """Um passo do progresso: o que a interface mostra (nada inventado: bytes de verdade)."""
    stage: str          # check, write, sync, verify, commit, done
    message: str
    done: int = 0       # bytes
    total: int = 0
    file: str = ""


Report = Callable[[Step], None]


def _update_name(name: str) -> str:
    return name.split("/", 1)[1] if name.startswith("linux/") else name


def card_state(mount: str) -> dict:
    """O que já existe no cartão: versão preparada, atualização pendente e o último resultado."""
    base = os.path.join(mount, DIR)
    st = {"has_dir": os.path.isdir(base), "prepared": None, "pending_update": False,
          "last_update": None, "agent": os.path.isfile(os.path.join(base, AGENT_FILE))}
    try:
        with open(os.path.join(base, "install.json"), encoding="utf-8") as f:
            st["prepared"] = json.load(f)
    except (OSError, ValueError):
        pass
    st["pending_update"] = os.path.isfile(os.path.join(base, "update", "SHA256SUMS"))
    try:
        with open(os.path.join(base, "update.log"), encoding="utf-8", errors="replace") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
        st["last_update"] = lines[-1] if lines else None
    except OSError:
        pass
    return st


class Installer:
    def __init__(self, storage: Storage, card: Card, package: Package, report: Report):
        self.storage, self.card, self.pkg, self.report = storage, card, package, report
        self.cancel_event = threading.Event()
        self.committing = False

    # ------------------------------------------------------------ utilidades

    def _check_cancel(self):
        if self.cancel_event.is_set() and not self.committing:
            raise Cancelled()

    def cancel(self) -> bool:
        """Pede para parar; False se já está na troca final (não dá para parar ali)."""
        if self.committing:
            return False
        self.cancel_event.set()
        return True

    def _copy(self, src: str, dst: str, done: int, total: int, label: str) -> int:
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            while chunk := fi.read(CHUNK):
                self._check_cancel()
                fo.write(chunk)
                done += len(chunk)
                self.report(Step("write", f"Copiando {label}", done, total, label))
            fo.flush()
            os.fsync(fo.fileno())
        return done

    def _hash(self, path: str, done: int, total: int, label: str) -> tuple[str, int]:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            while chunk := f.read(CHUNK):
                self._check_cancel()
                h.update(chunk)
                done += len(chunk)
                self.report(Step("verify", f"Conferindo {label}", done, total, label))
        return h.hexdigest(), done

    def _remount(self) -> str:
        self.report(Step("sync", "Gravando no cartão (sincronizando)"))
        os.sync()
        self.storage.unmount(self.card)
        time.sleep(0.5)
        self.card = self.storage.same_card(self.card)
        return self.storage.mount(self.card)

    # ------------------------------------------------------------ a transação

    def run(self) -> dict:
        rel: Release = self.pkg.release
        self.report(Step("check", "Conferindo o cartão"))
        self.card = self.storage.same_card(self.card)
        mount = self.storage.mount(self.card)
        st = os.statvfs(mount)
        free = st.f_bavail * st.f_frsize
        need = rel.total_size + MARGIN
        if free < need:
            raise StorageError(f"Espaço insuficiente no cartão: faltam {(need - free) // 1000000 + 1} MB.")
        base = os.path.join(mount, DIR)
        tmp = os.path.join(base, "update.tmp")
        agent_new = os.path.join(base, AGENT_FILE + ".new")
        try:
            os.makedirs(base, exist_ok=True)
            shutil.rmtree(tmp, ignore_errors=True)     # sobra de uma tentativa anterior
            os.makedirs(tmp)
            total, done = rel.total_size, 0
            # 1. gravar (o agente por último na contagem, é pequeno)
            for f in rel.files:
                if f.name == AGENT_FILE:
                    continue
                done = self._copy(self.pkg.path(f.name), os.path.join(tmp, _update_name(f.name)), done, total,
                                  _update_name(f.name))
            agent = rel.file(AGENT_FILE)
            done = self._copy(self.pkg.path(AGENT_FILE), agent_new, done, total, AGENT_FILE)
            # 2. reler do cartão de verdade
            mount = self._remount()
            base = os.path.join(mount, DIR)
            tmp, agent_new = os.path.join(base, "update.tmp"), os.path.join(base, AGENT_FILE + ".new")
            done = 0
            for f in rel.files:
                p = agent_new if f.name == AGENT_FILE else os.path.join(tmp, _update_name(f.name))
                got, done = self._hash(p, done, total, _update_name(f.name))
                if got != f.sha256:
                    raise StorageError(f"{_update_name(f.name)}: o que ficou no cartão não confere "
                                       "(cartão com defeito?). Nada foi trocado; tente outro cartão.")
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            try:
                os.remove(agent_new)
            except OSError:
                pass
            raise
        # 3. trocar os nomes: daqui em diante não se cancela
        self.committing = True
        self.report(Step("commit", "Finalizando"))
        sums = "".join(f"{f.sha256}  {_update_name(f.name)}\n" for f in rel.files if f.name != AGENT_FILE)
        with open(os.path.join(tmp, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as fo:
            fo.write(sums)
            fo.flush()
            os.fsync(fo.fileno())
        upd = os.path.join(base, "update")
        if os.path.isdir(upd):
            shutil.rmtree(upd)                          # atualização anterior ainda não aplicada
        os.rename(tmp, upd)
        exe = os.path.join(base, AGENT_FILE)
        if os.path.isfile(exe):
            os.replace(exe, exe + ".old")
        os.rename(agent_new, exe)
        with open(exe + ".sha256", "w", encoding="ascii", newline="\n") as fo:
            fo.write(f"{agent.sha256}  {AGENT_FILE}\n")
        info = {"product": "BigFoston OS", "version": rel.version, "device": rel.device,
                "agent_version": rel.agent_version, "channel": rel.channel,
                "prepared": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "files": {_update_name(f.name): f.sha256 for f in rel.files}}
        with open(os.path.join(base, "install.json"), "w", encoding="utf-8", newline="\n") as fo:
            json.dump(info, fo, indent=1, ensure_ascii=False)
            fo.write("\n")
        # 4. conferência final com os nomes definitivos
        mount = self._remount()
        base = os.path.join(mount, DIR)
        done = 0
        for f in rel.files:
            p = os.path.join(base, AGENT_FILE) if f.name == AGENT_FILE else \
                os.path.join(base, "update", _update_name(f.name))
            got, done = self._hash(p, done, rel.total_size, _update_name(f.name))
            if got != f.sha256:
                raise StorageError(f"{_update_name(f.name)}: falhou na conferência final.")
        self.report(Step("done", "Cartão preparado e conferido", rel.total_size, rel.total_size))
        return info


def verify_card(storage: Storage, card: Card, report: Report) -> list[str]:
    """Confere o que está no cartão contra o install.json e o update/SHA256SUMS (só leitura).
    Devolve a lista de problemas (vazia: tudo certo)."""
    mount = storage.mount(card)
    base = os.path.join(mount, DIR)
    st = card_state(mount)
    problems = []
    info = st["prepared"]
    if not info:
        return ["O cartão não tem uma versão do BigFoston OS preparada (install.json)."]
    files = info.get("files", {})
    for name, sha in files.items():
        if name not in ("bfagent.exe", "zImage", "initrd.img", "fs707dc.dtb"):
            problems.append(f"install.json cita um arquivo desconhecido: {name}")
            continue
        p = os.path.join(base, name) if name == AGENT_FILE else os.path.join(base, "update", name)
        if name != AGENT_FILE and not st["pending_update"]:
            continue                                    # já aplicada no aparelho (update.ok)
        if not os.path.isfile(p):
            problems.append(f"Falta {name}.")
            continue
        h = hashlib.sha256()
        with open(p, "rb") as f:
            while chunk := f.read(CHUNK):
                h.update(chunk)
        report(Step("verify", f"Conferindo {name}", 0, 0, name))
        if h.hexdigest() != sha:
            problems.append(f"{name} está diferente do que foi preparado (corrompido).")
    return problems


__all__ = ["Installer", "Step", "Cancelled", "card_state", "verify_card", "ManifestError", "StorageError"]
