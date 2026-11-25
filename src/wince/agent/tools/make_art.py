#!/usr/bin/env python3
"""Gera src/wince/agent/art.h: a arte da tela Dual Boot do agente (Windows CE).

O Windows CE do FOSTON só tem Tahoma e Arial, sem anti-aliasing garantido e sem SVG; então
o que é fixo vem pronto daqui, desenhado no PC:
  - o símbolo FOSTONOS em versão "symbolic" (só a forma, máscara alfa, tingida na tela);
  - ícones symbolic do BigFoston (Font Awesome 5, os mesmos do shell: src/ui/tools/fonts);
  - a bandeira clássica do Windows (4 painéis ondulados), colorida, com alfa;
  - os títulos ("Dual Boot", "BigFoston", "Windows CE") na Montserrat do BigFoston.
Máscaras são A8 (1 byte/pixel); a bandeira é RGBA. Desenhado em 4x e reduzido (bordas
suaves).

Uso:  python3 src/wince/agent/tools/make_art.py      (Pillow e rsvg-convert)
SPDX-License-Identifier: GPL-3.0-or-later
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
UI_TOOLS = os.path.join(HERE, "..", "..", "..", "ui", "tools")
sys.path.insert(0, UI_TOOLS)
import logo_svg  # noqa: E402

FA = os.path.join(UI_TOOLS, "fonts", "FontAwesome5-Solid+Brands+Regular.woff")
MONT = os.path.join(UI_TOOLS, "fonts", "Montserrat-Medium.ttf")
OUT = os.path.join(HERE, "..", "art.h")
SS = 4                                   # supersampling


def fa_icon(cp, size, glyph=None):
    """Glifo Font Awesome centralizado numa caixa size x size, máscara A8."""
    glyph = glyph or size * 0.82
    big = size * SS
    font = ImageFont.truetype(FA, int(glyph * SS))
    img = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(img)
    ch = chr(cp)
    x0, y0, x1, y1 = d.textbbox((0, 0), ch, font=font)
    d.text(((big - (x1 - x0)) / 2 - x0, (big - (y1 - y0)) / 2 - y0), ch, font=font, fill=255)
    return img.resize((size, size), Image.LANCZOS)


def text_mask(text, px):
    """Texto na Montserrat, recortado justo (altura fixa da linha), A8."""
    font = ImageFont.truetype(MONT, px * SS)
    asc, desc = font.getmetrics()
    w = int(font.getlength(text)) + 2 * SS
    img = Image.new("L", (w, asc + desc), 0)
    ImageDraw.Draw(img).text((SS, 0), text, font=font, fill=255)
    out = img.resize(((w + SS - 1) // SS, (asc + desc + SS - 1) // SS), Image.LANCZOS)
    return out


def logo_mark(height):
    """Símbolo FOSTONOS só com a forma (todas as cores viram a mesma máscara)."""
    img = logo_svg.render("mark", height=height * SS)
    a = img.getchannel("A")
    return a.resize(((img.width + SS - 1) // SS, height), Image.LANCZOS)


def windows_flag(size):
    """A bandeira clássica do Windows: 4 painéis ondulando, com o rastro à esquerda."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cols = [(0xf2, 0x50, 0x22), (0x7f, 0xba, 0x00), (0x00, 0xa4, 0xef), (0xff, 0xb9, 0x00)]

    def wave(x):                          # ondulação vertical ao longo da bandeira
        return math.sin(x * math.pi) * 0.06

    def pane(u0, u1, v0, v1, col):
        """Painel entre u0..u1 (horizontal) e v0..v1 (vertical) na bandeira inclinada."""
        pts = []
        n = 16
        for i in range(n + 1):
            u = u0 + (u1 - u0) * i / n
            pts.append(flag_xy(u, v0))
        for i in range(n, -1, -1):
            u = u0 + (u1 - u0) * i / n
            pts.append(flag_xy(u, v1))
        d.polygon(pts, fill=col)

    def flag_xy(u, v):
        # u 0..1 da haste para fora, v 0..1 de cima para baixo; leve perspectiva
        x = 0.31 + u * 0.60
        y = 0.20 + v * 0.58 - wave(u) - u * 0.05 + v * u * 0.04
        return (x * big, y * big)

    gap = 0.035
    pane(0.0, 0.5 - gap, 0.0, 0.5 - gap, cols[0])
    pane(0.5 + gap, 1.0, 0.0, 0.5 - gap, cols[1])
    pane(0.0, 0.5 - gap, 0.5 + gap, 1.0, cols[2])
    pane(0.5 + gap, 1.0, 0.5 + gap, 1.0, cols[3])
    # rastro: três faixas que diminuem à esquerda (como no logotipo dos anos 90)
    for k, (wid, off) in enumerate([(0.07, 0.03), (0.05, 0.13), (0.035, 0.21)]):
        x1 = 0.30 - off
        x0 = x1 - wid
        top = 0.20 + 0.02 * k
        mid = 0.49
        bot = 0.78 - 0.02 * k
        d.polygon([(x0 * big, (top + 0.04) * big), (x1 * big, top * big), (x1 * big, (mid - 0.02) * big),
                   (x0 * big, (mid - 0.02 + 0.03) * big)], fill=cols[0])
        d.polygon([(x0 * big, (mid + 0.05) * big), (x1 * big, (mid + 0.02) * big), (x1 * big, bot * big),
                   (x0 * big, (bot + 0.03) * big)], fill=cols[2])
    return img.resize((size, size), Image.LANCZOS)


def c_array(name, data, per_line=24):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    masks = [
        ("art_logo_sm", logo_mark(26)),            # cabeçalho
        ("art_logo_lg", logo_mark(48)),            # cartão BigFoston
        ("art_title", text_mask("Dual Boot", 26)),
        ("art_bigfoston", text_mask("BigFoston", 26)),
        ("art_wince", text_mask("Windows CE", 26)),
        ("art_ic_power", fa_icon(0xF011, 18)),     # iniciar
        ("art_ic_check", fa_icon(0xF058, 16)),     # ok
        ("art_ic_warn", fa_icon(0xF071, 16)),      # alerta
        ("art_ic_info", fa_icon(0xF05A, 16)),      # informação
        ("art_ic_usb", fa_icon(0xF287, 16)),       # pedido do PC
        ("art_ic_term", fa_icon(0xF120, 16)),      # cabeçalho do registro
        ("art_ic_linux", fa_icon(0xF17C, 18)),     # Tux (rodapé do cartão)
        ("art_ic_back", fa_icon(0xF2EA, 18)),      # voltar ao CE
    ]
    flag = windows_flag(60)
    out = ["/* art.h — GERADO por tools/make_art.py; não editar à mão.",
           " * Máscaras A8 (tingidas na tela) e a bandeira do Windows em RGBA.",
           " * Font Awesome 5 Free (CC BY 4.0), Montserrat (OFL), logotipo FOSTONOS (GPL-3.0). */",
           "#ifndef BF_AGENT_ART_H", "#define BF_AGENT_ART_H",
           "typedef struct { int w, h; const unsigned char *px; } art_t;   /* A8 ou RGBA */", ""]
    for name, img in masks:
        out.append(c_array(name + "_px", img.tobytes()))
        out.append(f"static const art_t {name} = {{ {img.width}, {img.height}, {name}_px }};")
    out.append(c_array("art_flag_px", flag.tobytes()))
    out.append(f"static const art_t art_flag = {{ {flag.width}, {flag.height}, art_flag_px }};   /* RGBA */")
    out += ["", "#endif", ""]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(OUT, sum(i.width * i.height for _, i in masks) + 60 * 60 * 4, "bytes de pixels")


if __name__ == "__main__":
    main()
