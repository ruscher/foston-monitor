#!/usr/bin/env python3
"""Tela de abertura do BigFoston OS para o console de boot do kernel.

Desenha a marca "BigFoston_" (Montserrat, as cores do shell) sobre o fundo do
tema escuro, 800x480, e grava firmware/kernel/overlay/drivers/video/fbdev/
mt3351-splash.h como RGB565 em RLE (pares contagem, cor). A barra de
progresso é desenhada pelo kernel na posição SPLASH_BAR_*.
Uso: make_splash.py [--png saida.png]  (requer Pillow)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
from PIL import Image, ImageDraw, ImageFont

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT = os.path.join(TOP, "src/ui/tools/fonts/Montserrat-Medium.ttf")
OUT = os.path.join(TOP, "firmware/kernel/overlay/drivers/video/fbdev/mt3351-splash.h")
W, H = 800, 480
BG, TEXT, BLUE, CYAN, DIM = (0x1b, 0x1f, 0x27), (0xe9, 0xed, 0xf4), (0x35, 0x84, 0xe4), (0x33, 0xc7, 0xde), (0x8a, 0x93, 0xa3)
BAR = (220, 330, 360, 6)          # x, y, largura, altura da barra de progresso


def rgb565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="também grava uma prévia em PNG")
    args = ap.parse_args()
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    big = ImageFont.truetype(FONT, 76)
    small = ImageFont.truetype(FONT, 22)
    w1 = d.textlength("Big", font=big)
    w2 = d.textlength("Foston", font=big)
    total = w1 + w2 + 14 + 34
    x = (W - total) / 2
    y = 160
    d.text((x, y), "Big", font=big, fill=TEXT)
    d.text((x + w1, y), "Foston", font=big, fill=BLUE)
    asc, desc = big.getmetrics()
    cx = x + w1 + w2 + 14
    d.rectangle([cx, y + asc - 10, cx + 34, y + asc + 2], fill=CYAN)   # cursor de bloco
    sub = "Linux no FOSTON FS-707DC"
    d.text(((W - d.textlength(sub, font=small)) / 2, 268), sub, font=small, fill=DIM)
    if args.png:
        img.save(args.png)

    data = img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()
    px = [rgb565(p) for p in data]
    runs = []
    i = 0
    while i < len(px):
        j = i
        while j < len(px) and px[j] == px[i] and j - i < 65535:
            j += 1
        runs += [j - i, px[i]]
        i = j
    with open(OUT, "w") as f:
        f.write("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        f.write("/* Gerado por tools/bigfoston/make_splash.py: não editar. RLE (contagem, cor RGB565). */\n")
        f.write(f"#define SPLASH_W {W}\n#define SPLASH_H {H}\n")
        f.write(f"#define SPLASH_BG 0x{rgb565(BG):04x}\n#define SPLASH_FG 0x{rgb565(BLUE):04x}\n#define SPLASH_TRACK 0x{rgb565((0x2e, 0x34, 0x40)):04x}\n")
        f.write(f"#define SPLASH_BAR_X {BAR[0]}\n#define SPLASH_BAR_Y {BAR[1]}\n#define SPLASH_BAR_W {BAR[2]}\n#define SPLASH_BAR_H {BAR[3]}\n")
        f.write(f"static const u16 splash_rle[{len(runs)}] __initconst = {{\n")
        for k in range(0, len(runs), 16):
            f.write("\t" + ", ".join(f"0x{v:04x}" for v in runs[k:k + 16]) + ",\n")
        f.write("};\n")
    print(f"{OUT}: {len(runs) // 2} trechos, {len(runs) * 2} bytes")


if __name__ == "__main__":
    main()
