"""Manifesto de uma versão do BigFoston OS e lista de aparelhos compatíveis.

Um pacote de versão (pasta ou .zip) tem:
    manifest.json        este formato (abaixo)
    SHA256SUMS           os mesmos hashes, no formato do sha256sum
    bfagent.exe          o agente do Windows CE (ARM)
    linux/zImage, linux/initrd.img, linux/fs707dc.dtb

manifest.json:
    {"format": 1, "product": "BigFoston OS", "version": "0.5.1", "date": "2026-11-25",
     "channel": "stable", "device": "foston-fs707dc",
     "agent": {"version": "0.8.0", "file": "bfagent.exe", "sha256": "...", "size": 98816},
     "linux": [{"file": "linux/zImage", "sha256": "...", "size": 2114336}, ...],
     "notes": "...", "min_free_mb": 16}

Kernel, initrd, DTB e agente vêm sempre do mesmo manifesto: o instalador não mistura peças
de versões diferentes.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from importlib import resources

FORMAT = 1
CHANNELS = ("stable", "beta", "experimental")
# os únicos arquivos que um pacote pode trazer (nada de caminhos livres)
AGENT_FILE = "bfagent.exe"
LINUX_FILES = ("linux/zImage", "linux/initrd.img", "linux/fs707dc.dtb")
# tetos de sanidade (o agente recusa acima disso: upd_max em bfagent.c)
MAX_SIZE = {AGENT_FILE: 1 << 20, "linux/zImage": 8 << 20, "linux/initrd.img": 12 << 20,
            "linux/fs707dc.dtb": 64 << 10}
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_VERSION = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.]+)?$")


class ManifestError(ValueError):
    """Pacote ou manifesto inválido: a mensagem é para o usuário."""


@dataclass(frozen=True)
class FileEntry:
    name: str          # "bfagent.exe" ou "linux/zImage"
    sha256: str
    size: int


@dataclass(frozen=True)
class Release:
    version: str
    device: str
    channel: str
    agent_version: str
    files: tuple[FileEntry, ...]
    date: str = ""
    notes: str = ""
    min_free_mb: int = 16

    @property
    def total_size(self) -> int:
        return sum(f.size for f in self.files)

    def file(self, name: str) -> FileEntry:
        for f in self.files:
            if f.name == name:
                return f
        raise KeyError(name)


def _entry(obj, name_expected=None) -> FileEntry:
    if not isinstance(obj, dict):
        raise ManifestError("manifesto: entrada de arquivo inválida")
    name, sha, size = obj.get("file"), obj.get("sha256"), obj.get("size")
    if name not in MAX_SIZE:
        raise ManifestError(f"manifesto: arquivo não permitido: {name!r}")
    if name_expected is not None and name != name_expected:
        raise ManifestError(f"manifesto: esperado {name_expected}, veio {name!r}")
    if not isinstance(sha, str) or not _HEX64.match(sha):
        raise ManifestError(f"manifesto: SHA-256 inválido para {name}")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_SIZE[name]:
        raise ManifestError(f"manifesto: tamanho inválido para {name}")
    return FileEntry(name, sha, size)


def parse_manifest(text: str) -> Release:
    try:
        m = json.loads(text)
    except json.JSONDecodeError as e:
        raise ManifestError(f"manifesto ilegível: {e.msg} (linha {e.lineno})") from None
    if not isinstance(m, dict) or m.get("format") != FORMAT:
        raise ManifestError("manifesto de formato desconhecido (atualize o instalador)")
    if m.get("product") != "BigFoston OS":
        raise ManifestError("este pacote não é do BigFoston OS")
    version = m.get("version")
    if not isinstance(version, str) or not _VERSION.match(version):
        raise ManifestError("manifesto: versão inválida")
    channel = m.get("channel", "stable")
    if channel not in CHANNELS:
        raise ManifestError(f"manifesto: canal desconhecido {channel!r}")
    device = m.get("device")
    if not isinstance(device, str) or not device:
        raise ManifestError("manifesto: aparelho não informado")
    agent = m.get("agent")
    if not isinstance(agent, dict) or not isinstance(agent.get("version"), str):
        raise ManifestError("manifesto: agente não informado")
    files = [_entry(agent, AGENT_FILE)]
    linux = m.get("linux")
    if not isinstance(linux, list) or len(linux) != len(LINUX_FILES):
        raise ManifestError("manifesto: faltam arquivos do Linux (zImage, initrd.img, fs707dc.dtb)")
    for obj, name in zip(sorted(linux, key=lambda o: str(o.get("file", "")) if isinstance(o, dict) else ""),
                         sorted(LINUX_FILES)):
        files.append(_entry(obj, name))
    notes = m.get("notes", "")
    min_free = m.get("min_free_mb", 16)
    if not isinstance(min_free, int) or isinstance(min_free, bool) or not 0 < min_free < 4096:
        raise ManifestError("manifesto: espaço mínimo inválido")
    return Release(version=version, device=device, channel=channel, agent_version=agent["version"],
                   files=tuple(files), date=str(m.get("date", "")), notes=str(notes)[:4000],
                   min_free_mb=min_free)


def sha256sums_text(release: Release) -> str:
    """SHA256SUMS no formato do sha256sum (o que o pacote e o agente usam)."""
    return "".join(f"{f.sha256}  {f.name}\n" for f in release.files)


# ------------------------------------------------------------------ compatibilidade

STATES = ("tested", "partial", "experimental", "untested", "incompatible")


@dataclass(frozen=True)
class Device:
    id: str
    name: str
    state: str
    cpu: str
    os: str
    screen: str
    notes: str = ""
    # o que o agente devolve em "info" e confirma o modelo (todas precisam bater)
    match: dict = field(default_factory=dict)

    @property
    def installable(self) -> bool:
        return self.state in ("tested", "partial")


def load_devices() -> list[Device]:
    """Os aparelhos conhecidos (data/devices.json, dentro do pacote do instalador)."""
    text = resources.files("bigfoston_installer").joinpath("data/devices.json").read_text("utf-8")
    raw = json.loads(text)
    out = []
    for d in raw["devices"]:
        if d["state"] not in STATES:
            raise ManifestError(f"devices.json: estado desconhecido {d['state']!r}")
        out.append(Device(id=d["id"], name=d["name"], state=d["state"], cpu=d["cpu"], os=d["os"],
                          screen=d["screen"], notes=d.get("notes", ""), match=d.get("match", {})))
    return out


def device_by_id(device_id: str) -> Device | None:
    for d in load_devices():
        if d.id == device_id:
            return d
    return None


def identify(agent_info: dict[str, str]) -> Device | None:
    """O aparelho cujo "match" bate com o que o agente informou (info do bfagent.py)."""
    for d in load_devices():
        if d.match and all(agent_info.get(k, "").strip().startswith(v) for k, v in d.match.items()):
            return d
    return None
