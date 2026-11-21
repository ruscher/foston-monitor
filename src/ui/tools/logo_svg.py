"""Variantes do logotipo FOSTONOS (assets/logo/fostonos.svg) rasterizadas com
rsvg-convert. Usado por tools/make_logo.py (imagens do shell) e por
tools/bigfoston/make_splash.py (tela de abertura do kernel).

Variantes (recortes do viewBox 1024x1024 original):
  icon   — o ícone completo (quadrado azul-marinho arredondado, símbolo e nome)
  nobg   — símbolo e nome, sem o quadrado de fundo
  mark   — só o símbolo (órbitas e esfera), sem nome nem fundo
"""
import io
import os
import re
import shutil
import subprocess

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "..", "assets", "logo", "fostonos.svg")

# caixas (x, y, largura, altura) no sistema do viewBox original
BOX_NOBG = (168, 220, 696, 636)
BOX_MARK = (188, 222, 676, 508)


def _variant(kind):
    with open(SVG, encoding="utf-8") as f:
        svg = f.read()
    if kind == "icon":
        return svg
    svg = re.sub(r'<rect width="1024" height="1024"[^>]*/>', "", svg, count=1)
    box = BOX_NOBG
    if kind == "mark":
        svg = re.sub(r'<g fill="#f6f7ff" aria-label="FOSTONOS">.*?</g>', "", svg, count=1, flags=re.S)
        box = BOX_MARK
    vb = " ".join(str(v) for v in box)
    svg = re.sub(r'viewBox="[^"]*" width="1024" height="1024"',
                 f'viewBox="{vb}" width="{box[2]}" height="{box[3]}"', svg, count=1)
    return svg


def render(kind, width=None, height=None):
    """Imagem RGBA (Pillow) da variante, com a largura ou a altura pedida."""
    exe = shutil.which("rsvg-convert")
    if not exe:
        raise SystemExit("rsvg-convert não encontrado (pacote librsvg)")
    cmd = [exe, "--format=png"]
    if width:
        cmd += ["--width", str(width)]
    if height:
        cmd += ["--height", str(height)]
    if width and height:
        cmd.append("--keep-aspect-ratio")
    png = subprocess.run(cmd, input=_variant(kind).encode(), capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")
