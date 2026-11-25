"""Pacote de uma versão do BigFoston OS (pasta ou .zip) e a conferência por SHA-256.

O .zip é lido com uma lista fechada de nomes (nada de caminhos livres, "..", links
simbólicos ou arquivos gigantes) e extraído para uma pasta temporária; cada arquivo é
conferido por streaming (sem carregar o initrd inteiro na memória) contra o manifesto, e o
SHA256SUMS precisa dizer o mesmo que o manifesto.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import hashlib
import os
import shutil
import stat
import tempfile
import zipfile
from dataclasses import dataclass
from typing import Callable

from .manifest import AGENT_FILE, LINUX_FILES, MAX_SIZE, ManifestError, Release, parse_manifest

CHUNK = 1 << 20
ALLOWED = {"manifest.json", "SHA256SUMS", AGENT_FILE, *LINUX_FILES}
Progress = Callable[[int, int], None]          # (feito, total) em bytes


def sha256_file(path: str, progress: Progress | None = None, done: int = 0, total: int = 0) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    return h.hexdigest()


def parse_sha256sums(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise ManifestError("SHA256SUMS: linha inválida")
        out[parts[1].lstrip("*")] = parts[0].lower()
    return out


@dataclass
class Package:
    root: str              # pasta com os arquivos (a original ou a temporária do .zip)
    release: Release
    source: str            # o que o usuário escolheu
    _tmp: str | None = None

    def path(self, name: str) -> str:
        return os.path.join(self.root, *name.split("/"))

    def verify(self, progress: Progress | None = None) -> None:
        """Confere tamanho e SHA-256 de cada arquivo; ManifestError na primeira diferença."""
        total = self.release.total_size
        done = 0
        for f in self.release.files:
            p = self.path(f.name)
            st = os.lstat(p)
            if not stat.S_ISREG(st.st_mode):
                raise ManifestError(f"{f.name}: não é um arquivo comum")
            if st.st_size != f.size:
                raise ManifestError(f"{f.name}: tamanho {st.st_size}, o manifesto diz {f.size}")
            if sha256_file(p, progress, done, total) != f.sha256:
                raise ManifestError(f"{f.name}: SHA-256 não confere (arquivo corrompido ou alterado)")
            done += f.size

    def close(self) -> None:
        if self._tmp:
            shutil.rmtree(self._tmp, ignore_errors=True)
            self._tmp = None


def _check_sums(root: str, release: Release) -> None:
    try:
        with open(os.path.join(root, "SHA256SUMS"), encoding="ascii") as f:
            sums = parse_sha256sums(f.read())
    except FileNotFoundError:
        raise ManifestError("o pacote não tem SHA256SUMS") from None
    except UnicodeDecodeError:
        raise ManifestError("SHA256SUMS ilegível") from None
    want = {f.name: f.sha256 for f in release.files}
    if sums != want:
        raise ManifestError("SHA256SUMS e manifest.json não dizem o mesmo")


def _read_manifest(root: str) -> Release:
    p = os.path.join(root, "manifest.json")
    if not os.path.isfile(p) or os.path.islink(p):
        raise ManifestError("o pacote não tem manifest.json")
    if os.path.getsize(p) > 64 << 10:
        raise ManifestError("manifest.json grande demais")
    with open(p, encoding="utf-8") as f:
        return parse_manifest(f.read())


def _extract_zip(path: str) -> str:
    tmp = tempfile.mkdtemp(prefix="bigfoston-pkg-")
    try:
        with zipfile.ZipFile(path) as z:
            names = {}
            for info in z.infolist():
                if info.is_dir():
                    continue
                name = info.filename
                # um nível de pasta no topo é aceito (BigFoston-OS-FS707DC-0.5.1/...)
                parts = name.split("/")
                if len(parts) > 1 and parts[0] and "/".join(parts[1:]) in ALLOWED:
                    name = "/".join(parts[1:])
                if name not in ALLOWED:
                    raise ManifestError(f"o pacote tem um arquivo inesperado: {info.filename!r}")
                if name in names:
                    raise ManifestError(f"o pacote tem {name} repetido")
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ManifestError(f"o pacote tem um link simbólico: {info.filename!r}")
                limit = MAX_SIZE.get(name, 64 << 10)
                if info.file_size > limit:
                    raise ManifestError(f"{name}: grande demais para este aparelho")
                names[name] = info
            for name, info in names.items():
                dest = os.path.join(tmp, *name.split("/"))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(info) as src, open(dest, "wb") as out:
                    copied = 0
                    while chunk := src.read(CHUNK):
                        copied += len(chunk)
                        if copied > info.file_size:     # zip mentindo o tamanho
                            raise ManifestError(f"{name}: tamanho diferente do declarado no .zip")
                        out.write(chunk)
        return tmp
    except (zipfile.BadZipFile, OSError) as e:
        shutil.rmtree(tmp, ignore_errors=True)
        raise ManifestError(f"não foi possível ler o pacote: {e}") from None
    except ManifestError:
        shutil.rmtree(tmp, ignore_errors=True)
        raise


def open_package(path: str) -> Package:
    """Abre uma pasta ou .zip de versão e confere manifesto x SHA256SUMS (não os hashes: verify())."""
    tmp = None
    if os.path.isdir(path):
        root = path
    elif zipfile.is_zipfile(path):
        tmp = root = _extract_zip(path)
    else:
        raise ManifestError("escolha a pasta ou o arquivo .zip de uma versão do BigFoston OS")
    try:
        release = _read_manifest(root)
        _check_sums(root, release)
    except ManifestError:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        raise
    return Package(root=root, release=release, source=path, _tmp=tmp)
