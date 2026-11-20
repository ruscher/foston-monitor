#!/usr/bin/env python3
"""Tela de abertura do BigFoston OS para o console de boot do kernel.

Compõe o logotipo FOSTONOS (src/ui/assets/logo/fostonos.svg, rasterizado com
rsvg-convert) sobre o degradê azul-marinho do próprio ícone, 800x480, e grava
firmware/kernel/overlay/drivers/video/fbdev/mt3351-splash.h em RGB565
compactado. O fundo (degradê radial, com pontilhado ordenado 4x4 contra as
faixas do RGB565) é calculado pelo próprio kernel com a fórmula de
SPLASH_CX/CY/K e das cores SPLASH_C*/SPLASH_E*; os dados trazem só o que vai
por cima (logotipo e legenda):
  - palavra n < 0x4000          -> pula n pixels (fica o fundo)
  - palavra 0x4000 | n, cor c   -> n pixels da cor c
  - palavra 0x8000 | n, n cores -> n pixels literais
A barra de progresso é desenhada pelo kernel em SPLASH_BAR_*, com o degradê
verde-amarelo do logotipo (splash_bar) e as pontas arredondadas (splash_round).
Uso: make_splash.py [--png saida.png]  (requer Pillow e rsvg-convert)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(TOP, "src/ui/tools"))
import logo_svg  # noqa: E402

FONT = os.path.join(TOP, "src/ui/tools/fonts/Montserrat-Medium.ttf")
OUT = os.path.join(TOP, "firmware/kernel/overlay/drivers/video/fbdev/mt3351-splash.h")
W, H = 800, 480
CENTER, EDGE = (0x0a, 0x22, 0x6a), (0x03, 0x10, 0x2e)   # degradê radial do ícone
SUB = (0x8d, 0x9c, 0xc8)
TRACK = (0x1a, 0x2c, 0x66)
BAR_FROM, BAR_TO = (0x26, 0xdd, 0xa8), (0xd6, 0xf3, 0x46)   # verde -> amarelo do símbolo
LOGO_W = 330
BAR = (280, 392, 240, 6)          # x, y, largura, altura


def rgb565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


CX, CY = 400, 200
K = (256 << 24) // (440 * 440 + 336 * 336)    # t = d2 * K >> 24  (0..256)


def bg_t(x, y):
    d2 = (x - CX) ** 2 + (y - CY) ** 2
    return min(256, (d2 * K) >> 24)


def background():
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = mix(CENTER, EDGE, bg_t(x, y) / 256)
    return img


def encode(px, opaque):
    """Pula o fundo; trechos repetidos e literais no que vai por cima."""
    out = []
    lit = []

    def flush():
        while lit:
            n = min(len(lit), 0x7fff)
            out.append(0x8000 | n)
            out.extend(lit[:n])
            del lit[:n]

    i = 0
    total = len(px)
    while i < total:
        j = i
        if not opaque[i]:
            while j < total and not opaque[j] and j - i < 0x3fff:
                j += 1
            flush()
            out.append(j - i)
        else:
            while j < total and opaque[j] and px[j] == px[i] and j - i < 0x3fff:
                j += 1
            if j - i >= 3:
                flush()
                out += [0x4000 | (j - i), px[i]]
            else:
                lit.extend(px[i:j])
        i = j
    flush()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="também grava uma prévia em PNG")
    args = ap.parse_args()
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    logo = logo_svg.render("nobg", width=LOGO_W)
    over.paste(logo, ((W - logo.width) // 2, 40), logo)
    d = ImageDraw.Draw(over)
    small = ImageFont.truetype(FONT, 18)
    sub = "Linux no FOSTON FS-707DC"
    d.text(((W - d.textlength(sub, font=small)) / 2, 420), sub, font=small, fill=SUB + (255,))
    img = background()
    img.paste(over, (0, 0), over)
    alpha = over.getchannel("A").tobytes()
    # prévia com a barra em 60 %
    prev = img.copy()
    pd = ImageDraw.Draw(prev)
    pd.rounded_rectangle([BAR[0], BAR[1], BAR[0] + BAR[2] - 1, BAR[1] + BAR[3] - 1], radius=3, fill=TRACK)
    for x in range(BAR[2] * 60 // 100):
        pd.line([BAR[0] + x, BAR[1] + 1, BAR[0] + x, BAR[1] + BAR[3] - 2], fill=mix(BAR_FROM, BAR_TO, x / BAR[2]))
    if args.png:
        prev.save(args.png)

    data = img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()
    px = [rgb565(p) for p in data]
    words = encode(px, [a > 0 for a in alpha])
    bar = [rgb565(mix(BAR_FROM, BAR_TO, x / (BAR[2] - 1))) for x in range(BAR[2])]
    # pixels do fundo a manter em cada ponta, por linha (cantos arredondados de raio 3)
    rnd = [2, 1, 0, 0, 1, 2][:BAR[3]]
    with open(OUT, "w") as f:
        f.write("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        f.write("/* Gerado por tools/bigfoston/make_splash.py: não editar.\n")
        f.write(" * Fundo calculado pelo kernel. n < 0x4000: pula n pixels; 0x4000|n: n pixels\n")
        f.write(" * da cor seguinte; 0x8000|n: n cores literais. */\n")
        f.write(f"#define SPLASH_W {W}\n#define SPLASH_H {H}\n")
        f.write(f"#define SPLASH_TRACK 0x{rgb565(TRACK):04x}\n")
        f.write(f"#define SPLASH_CX {CX}\n#define SPLASH_CY {CY}\n#define SPLASH_K {K}u\n")
        for n, c in (("C", CENTER), ("E", EDGE)):
            f.write(f"#define SPLASH_{n}R {c[0]}\n#define SPLASH_{n}G {c[1]}\n#define SPLASH_{n}B {c[2]}\n")
        f.write(f"#define SPLASH_BAR_X {BAR[0]}\n#define SPLASH_BAR_Y {BAR[1]}\n#define SPLASH_BAR_W {BAR[2]}\n#define SPLASH_BAR_H {BAR[3]}\n")
        f.write(f"static const u8 splash_round[SPLASH_BAR_H] = {{ {', '.join(map(str, rnd))} }};\n")
        f.write("static const u16 splash_bar[SPLASH_BAR_W] = {\n")
        for k in range(0, len(bar), 16):
            f.write("\t" + ", ".join(f"0x{v:04x}" for v in bar[k:k + 16]) + ",\n")
        f.write("};\n")
        f.write(f"static const u16 splash_rle[{len(words)}] __initconst = {{\n")
        for k in range(0, len(words), 16):
            f.write("\t" + ", ".join(f"0x{v:04x}" for v in words[k:k + 16]) + ",\n")
        f.write("};\n")
    print(f"{OUT}: {len(words)} palavras, {len(words) * 2} bytes")


if __name__ == "__main__":
    main()
