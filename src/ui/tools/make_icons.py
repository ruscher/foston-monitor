#!/usr/bin/env python3
"""Gera os ícones do BigFoston como imagens LVGL 9 (formato A8, 1 byte/pixel).

Cada ícone é um glifo Font Awesome 5 Free (CC BY 4.0) renderizado com Pillow,
centralizado numa caixa quadrada e salvo como máscara alfa (A8). Na tela, a cor
vem do estilo (image_recolor), então o mesmo ícone serve aos temas escuro/claro.

Uso:  python3 tools/make_icons.py      (gera assets/icons/bf_icons.c e .h)
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FA = os.path.join(HERE, "fonts", "FontAwesome5-Solid+Brands+Regular.woff")
OUT_DIR = os.path.join(HERE, "..", "assets", "icons")

SIZE = 56          # caixa do ícone (px) — ícones grandes da grade inicial
GLYPH = 44         # altura nominal do glifo dentro da caixa

# nome -> codepoint Font Awesome 5
ICONS = [
    ("multimedia", 0xF144),  # play-circle
    ("tv",         0xF26C),  # tv
    ("bluetooth",  0xF294),  # bluetooth-b (brands)
    ("book",       0xF518),  # book-open
    ("gps",        0xF5A0),  # map-marked-alt
    ("files",      0xF07C),  # folder-open
    ("terminal",   0xF120),  # terminal
    ("run",        0xF135),  # rocket
    ("bigscreen",  0xF109),  # laptop
    ("monitor",    0xF3FD),  # tachometer-alt
    ("settings",   0xF013),  # cog
    ("fm",         0xF519),  # broadcast-tower
    ("usb",        0xF287),  # usb (brands)
    ("info",       0xF05A),  # info-circle
    ("power",      0xF011),  # power-off
    ("headphones", 0xF025),  # headphones (aviso de dispositivo)
    ("plug",       0xF1E6),  # plug (carregador)
    ("sdcard",     0xF7C2),  # sd-card
    ("gallery",    0xF302),  # images
    ("phone",      0xF095),  # phone (Telefone)
    ("web",        None),    # Internet: globo com órbita (desenho próprio, render_web)
]


def render_web():
    """Internet (BigFoston Web): o globo do Font Awesome (F0AC) com uma órbita inclinada
    que passa por trás e pela frente dele e um satélite, como as órbitas do logo FOSTONOS.
    Desenhado em 4x e reduzido (bordas suaves); a órbita da frente tem um vão em volta."""
    from PIL import ImageChops
    k = 4
    S = SIZE * k
    c = S / 2
    font = ImageFont.truetype(FA, int(GLYPH * 0.78 * k))
    l, t, r, b = font.getbbox(chr(0xF0AC))
    globe = Image.new("L", (S, S), 0)
    ImageDraw.Draw(globe).text((c - (r - l) / 2 - l, c - (b - t) / 2 - t), chr(0xF0AC), font=font, fill=255)
    rad = max(r - l, b - t) / 2
    solid = Image.new("L", (S, S), 0)                       # silhueta do globo (oclusão)
    ImageDraw.Draw(solid).ellipse((c - rad, c - rad, c + rad, c + rad), fill=255)

    def ring(width):
        im = Image.new("L", (S, S), 0)
        rw, rh = S * 0.485, S * 0.17
        ImageDraw.Draw(im).ellipse((c - rw, c - rh, c + rw, c + rh), outline=255, width=width)
        return im.rotate(22, resample=Image.BICUBIC, center=(c, c))

    def half(front):                                         # metade de baixo = frente
        im = Image.new("L", (S, S), 0)
        ImageDraw.Draw(im).rectangle((0, c, S, S) if front else (0, 0, S, c), fill=255)
        return im.rotate(22, resample=Image.NEAREST, center=(c, c))

    thin, gap = ring(3 * k), ring(9 * k)
    front, back = half(True), half(False)
    # globo sem o vão da órbita da frente; órbita de trás escondida pelo globo
    out = ImageChops.subtract(globe, ImageChops.multiply(gap, front))
    out = ImageChops.lighter(out, ImageChops.multiply(thin, front))
    out = ImageChops.lighter(out, ImageChops.subtract(ImageChops.multiply(thin, back), solid))
    # satélite na órbita, em cima à direita, com vão em volta
    import math
    a = math.radians(-22 - 38)
    rw, rh = S * 0.485, S * 0.17
    x, y = c + rw * math.cos(math.radians(-38)), c + rh * math.sin(math.radians(-38))
    ang = math.radians(-22)
    sx = c + (x - c) * math.cos(ang) - (y - c) * math.sin(ang)
    sy = c + (x - c) * math.sin(ang) + (y - c) * math.cos(ang)
    d = ImageDraw.Draw(out)
    d.ellipse((sx - 6.5 * k, sy - 6.5 * k, sx + 6.5 * k, sy + 6.5 * k), fill=0)
    d.ellipse((sx - 3.5 * k, sy - 3.5 * k, sx + 3.5 * k, sy + 3.5 * k), fill=255)
    del a
    return out.resize((SIZE, SIZE), Image.LANCZOS)


def render(cp):
    if cp is None:
        return render_web()
    font = ImageFont.truetype(FA, GLYPH)
    ch = chr(cp)
    l, t, r, b = font.getbbox(ch)
    w, h = r - l, b - t
    scale = 1.0
    if max(w, h) > GLYPH:          # glifos largos (tv, usb, terminal) encolhem
        scale = GLYPH / max(w, h)
        font = ImageFont.truetype(FA, int(GLYPH * scale))
        l, t, r, b = font.getbbox(ch)
        w, h = r - l, b - t
    img = Image.new("L", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(img)
    d.text(((SIZE - w) / 2 - l, (SIZE - h) / 2 - t), ch, font=font, fill=255)
    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    c = ["/* Gerado por tools/make_icons.py — não editar à mão.",
         " * Glifos: Font Awesome 5 Free (ícones CC BY 4.0). Formato LVGL A8. */",
         '#include "bf_icons.h"', ""]
    h = ["/* Gerado por tools/make_icons.py */", "#ifndef BF_ICONS_H", "#define BF_ICONS_H",
         '#include "lvgl.h"', "", f"#define BF_ICON_SIZE {SIZE}", ""]
    for name, cp in ICONS:
        img = render(cp)
        data = img.tobytes()
        c.append(f"static const uint8_t {name}_map[] = {{")
        for i in range(0, len(data), 24):
            c.append("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
        c.append("};")
        c.append(f"const lv_image_dsc_t bf_icon_{name} = {{")
        c.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        c.append("    .header.cf = LV_COLOR_FORMAT_A8,")
        c.append(f"    .header.w = {SIZE}, .header.h = {SIZE}, .header.stride = {SIZE},")
        c.append(f"    .data_size = sizeof({name}_map), .data = {name}_map,")
        c.append("};")
        c.append("")
        h.append(f"extern const lv_image_dsc_t bf_icon_{name};")
    h += ["", "#endif", ""]
    with open(os.path.join(OUT_DIR, "bf_icons.c"), "w") as f:
        f.write("\n".join(c))
    with open(os.path.join(OUT_DIR, "bf_icons.h"), "w") as f:
        f.write("\n".join(h))
    # pré-visualização para revisão rápida
    sheet = Image.new("L", (SIZE * len(ICONS), SIZE), 0)
    for i, (_, cp) in enumerate(ICONS):
        sheet.paste(render(cp), (i * SIZE, 0))
    sheet.save(os.path.join(HERE, "..", "build", "icons_preview.png")) if os.path.isdir(
        os.path.join(HERE, "..", "build")) else None
    print(f"{len(ICONS)} ícones gerados em {os.path.normpath(OUT_DIR)}")


if __name__ == "__main__":
    main()
