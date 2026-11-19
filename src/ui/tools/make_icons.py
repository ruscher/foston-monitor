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
]


def render(cp):
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
