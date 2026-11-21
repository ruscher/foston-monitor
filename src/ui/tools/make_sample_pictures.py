#!/usr/bin/env python3
"""Imagens de exemplo (geradas aqui, sem direitos de terceiros) para testar e
fotografar a Galeria no simulador: JPEG, PNG com transparência, GIF animado,
BMP 24 bits e paleta, e um SVG.

Uso:  python3 tools/make_sample_pictures.py PASTA
"""
import math
import os
import sys

from PIL import Image, ImageDraw


def gradient(w, h, c1, c2):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            s = (t + x / max(1, w - 1)) / 2
            px[x, y] = tuple(int(c1[i] * (1 - s) + c2[i] * s) for i in range(3))
    return img


def landscape(w, h, sky1, sky2, sun, hills):
    img = gradient(w, h, sky1, sky2)
    d = ImageDraw.Draw(img)
    r = h // 7
    d.ellipse((w * 0.7 - r, h * 0.3 - r, w * 0.7 + r, h * 0.3 + r), fill=sun)
    for k, col in enumerate(hills):
        pts = [(0, h)]
        for x in range(0, w + 1, 8):
            y = h * (0.55 + 0.12 * k) + math.sin(x / (w / (3 + k)) + k) * h * 0.06
            pts.append((x, y))
        pts.append((w, h))
        d.polygon(pts, fill=col)
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "pictures"
    os.makedirs(out, exist_ok=True)
    landscape(1600, 1200, (255, 170, 110), (90, 60, 160), (255, 236, 160),
              [(70, 40, 90), (40, 30, 60), (20, 15, 30)]).save(os.path.join(out, "por-do-sol.jpg"), quality=88)
    landscape(1024, 768, (120, 200, 255), (220, 240, 255), (255, 255, 220),
              [(90, 170, 90), (50, 130, 60), (30, 90, 40)]).save(os.path.join(out, "campo.jpg"), quality=88)
    landscape(640, 960, (20, 30, 70), (5, 10, 25), (240, 240, 255),
              [(30, 40, 80), (20, 25, 50), (10, 12, 25)]).save(os.path.join(out, "noite-retrato.jpg"), quality=88)
    # PNG com transparência (fica sobre o fundo escuro)
    p = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    d = ImageDraw.Draw(p)
    for i in range(12):
        a = i * math.pi / 6
        x, y = 256 + 150 * math.cos(a), 256 + 150 * math.sin(a)
        d.ellipse((x - 70, y - 70, x + 70, y + 70), fill=(255, 90 + i * 12, 60 + i * 15, 170))
    d.ellipse((186, 186, 326, 326), fill=(255, 230, 90, 255))
    p.save(os.path.join(out, "flor.png"))
    # GIF animado: bolinha quicando
    frames = []
    for f in range(16):
        fr = Image.new("P", (320, 240), 0)
        fr.putpalette([20, 24, 34, 230, 80, 75, 250, 200, 80, 80, 160, 240] + [0] * 756)
        dd = ImageDraw.Draw(fr)
        y = 200 - abs(math.sin(f / 16 * math.pi)) * 160
        dd.rectangle((0, 220, 320, 240), fill=3)
        dd.ellipse((140, y - 20, 180, y + 20), fill=1 if f % 2 else 2)
        frames.append(fr)
    frames[0].save(os.path.join(out, "bola.gif"), save_all=True, append_images=frames[1:], duration=60, loop=0)
    gradient(400, 300, (30, 120, 200), (230, 60, 140)).save(os.path.join(out, "degrade.bmp"))
    gradient(256, 256, (240, 220, 60), (40, 160, 90)).convert("P", palette=Image.ADAPTIVE).save(
        os.path.join(out, "paleta-8bits.bmp"))
    with open(os.path.join(out, "estrela.svg"), "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">'
                '<rect width="200" height="200" rx="24" fill="#2a6fd0"/>'
                '<polygon points="100,20 124,76 186,80 138,120 154,180 100,146 46,180 62,120 14,80 76,76" '
                'fill="#ffd23f" stroke="#ffffff" stroke-width="6"/></svg>')
    print(f"imagens de exemplo em {out}")


if __name__ == "__main__":
    main()
