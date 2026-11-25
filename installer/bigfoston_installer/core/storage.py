"""Cartões SD vistos pelo computador e as regras de segurança para escolher um.

O núcleo só conhece a classe Card e a interface Storage; a implementação do Linux
(UDisks2) está em platforms/linux_udisks.py, a do Windows ficará em platforms/windows.py.

Regras (eligibility):
  - nunca um disco que contém o sistema do computador (/, /boot, /home, /usr, /var...);
  - nunca um disco fixo (não removível) nem um volume sem sistema de arquivos FAT;
  - nunca acima de 32 GB (SDXC/exFAT: não testados no FOSTON); só leitura: bloqueado;
  - o FOSTON pelo próprio cabo USB (modo pendrive) é marcado: só operações pequenas;
  - a identidade (aparelho, série, UUID e tamanho) é conferida de novo antes de gravar.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

from dataclasses import dataclass, field

GB = 1000 ** 3
MAX_CARD = 34 * GB                 # 32 GB nominal (o cartão "32 GB" tem ~31,9 GB)
FOSTON_USB_VENDOR = "045e"         # o FOSTON se apresenta como Microsoft (CE): 045e:ffff pendrive
SYSTEM_MOUNTS = ("/", "/boot", "/boot/efi", "/efi", "/home", "/usr", "/var", "/opt", "/srv", "/root")

# por que um volume não pode ser usado (mensagens para o usuário)
BLOCK_SYSTEM = "contém o sistema deste computador"
BLOCK_FIXED = "é um disco fixo, não um cartão removível"
BLOCK_FS = "não está em FAT (FAT32); o FOSTON só lê FAT"
BLOCK_BIG = "tem mais de 32 GB; cartões SDXC não foram testados no FOSTON"
BLOCK_RO = "está protegido contra gravação (trava do cartão ou leitor)"


@dataclass
class Card:
    key: str                     # identificador estável na plataforma (UDisks: caminho do objeto)
    device: str                  # /dev/sdc1, E:
    label: str
    fs_type: str                 # vfat, exfat, ntfs...
    fs_uuid: str
    size: int                    # bytes da partição
    drive_vendor: str = ""
    drive_model: str = ""
    drive_serial: str = ""
    connection: str = ""         # usb, sdio, ...
    removable: bool = True
    read_only: bool = False
    mount_point: str = ""
    free: int = -1               # bytes livres (-1: desconhecido, não montado)
    usb_vendor: str = ""         # idVendor do aparelho USB, se houver
    system_disk: bool = False    # o mesmo disco tem /, /home...
    has_bigfoston: bool = False  # já tem a pasta BigFoston/
    extra: dict = field(default_factory=dict)

    @property
    def is_foston_usb(self) -> bool:
        return self.usb_vendor.lower() == FOSTON_USB_VENDOR

    @property
    def identity(self) -> tuple:
        """O que precisa continuar igual entre a escolha e a gravação."""
        return (self.drive_serial, self.drive_model, self.fs_uuid, self.size)

    @property
    def title(self) -> str:
        name = " ".join(x for x in (self.drive_vendor, self.drive_model) if x).strip()
        if self.is_foston_usb:
            name = "FOSTON (cabo USB)"
        return name or "Cartão de memória"


def blockers(card: Card) -> list[str]:
    """Motivos que impedem gravar neste volume (vazio: pode)."""
    out = []
    if card.system_disk:
        out.append(BLOCK_SYSTEM)
    if not card.removable and card.connection not in ("usb", "sdio") and not card.is_foston_usb:
        out.append(BLOCK_FIXED)
    if card.fs_type != "vfat":
        out.append(BLOCK_FS)
    if card.size > MAX_CARD:
        out.append(BLOCK_BIG)
    if card.read_only:
        out.append(BLOCK_RO)
    return out


def warnings(card: Card) -> list[str]:
    out = []
    if card.is_foston_usb:
        out.append("Pelo cabo USB do FOSTON a cópia de arquivos grandes não é confiável: "
                   "use um leitor de cartões para instalar o sistema.")
    if 0 <= card.free < 64 * 1000 * 1000:
        out.append("Pouco espaço livre no cartão.")
    if card.size > 4 * GB and card.size <= MAX_CARD:
        out.append("Testado no FOSTON com cartões de 2 GB e de 32 GB (SDHC).")
    return out


class StorageError(RuntimeError):
    """Falha de montagem/leitura/gravação: a mensagem é para o usuário."""


class Storage:
    """O que cada plataforma implementa (operações bloqueantes: chamar fora da thread da GUI)."""

    def list_cards(self) -> list[Card]:
        raise NotImplementedError

    def refresh(self, card: Card) -> Card | None:
        """O mesmo volume relido agora (None se saiu)."""
        raise NotImplementedError

    def mount(self, card: Card) -> str:
        raise NotImplementedError

    def unmount(self, card: Card) -> None:
        raise NotImplementedError

    def eject(self, card: Card) -> None:
        raise NotImplementedError

    def same_card(self, card: Card) -> Card:
        """Relê o volume e confere a identidade: StorageError se mudou ou sumiu."""
        now = self.refresh(card)
        if now is None:
            raise StorageError("O cartão foi removido. Coloque-o de novo para continuar.")
        if now.identity != card.identity:
            raise StorageError("O volume mudou desde a escolha (outro cartão?). Escolha o cartão de novo.")
        problems = blockers(now)
        if problems:
            raise StorageError("Este volume não pode ser usado: " + "; ".join(problems) + ".")
        return now
