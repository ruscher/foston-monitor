#!/usr/bin/env python3
"""Gera as imagens LVGL 9 do logotipo FOSTONOS a partir de assets/logo/fostonos.svg.

  bf_logo_icon  — ícone completo, 72 px (ARGB8888): Aparência, Informações
  bf_logo_mark  — só o símbolo, 30 px de altura (ARGB8888): barra superior
  bf_logo_big   — símbolo e nome, 260 px de largura (ARGB8888): tela de
                  encerramento (Desligando / Reiniciando)
  bf_logo_wm    — só o símbolo, 400 px de largura, máscara alfa (A8): marca
                  d'água do papel de parede, tingida pela cor do tema em tempo
                  de execução (core/wallpaper.c)

Uso:  python3 tools/make_logo.py      (gera assets/logo/bf_logo.c e .h; requer
Pillow e rsvg-convert)
"""
import os

import logo_svg

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "assets", "logo")


def argb(img):
    """RGBA (Pillow) -> bytes ARGB8888 do LVGL (little-endian: B, G, R, A)."""
    out = bytearray()
    for r, g, b, a in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()):
        out += bytes((b, g, r, a))
    return bytes(out)


def emit(c, h, name, img, fmt):
    if fmt == "A8":
        data = img.getchannel("A").tobytes()
        stride = img.width
        cf = "LV_COLOR_FORMAT_A8"
    else:
        data = argb(img)
        stride = img.width * 4
        cf = "LV_COLOR_FORMAT_ARGB8888"
    c.append(f"static const uint8_t {name}_map[] = {{")
    for i in range(0, len(data), 24):
        c.append("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    c.append("};")
    c.append(f"const lv_image_dsc_t {name} = {{")
    c.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    c.append(f"    .header.cf = {cf},")
    c.append(f"    .header.w = {img.width}, .header.h = {img.height}, .header.stride = {stride},")
    c.append(f"    .data_size = sizeof({name}_map), .data = {name}_map,")
    c.append("};")
    c.append("")
    h.append(f"extern const lv_image_dsc_t {name};   /* {img.width}x{img.height} {fmt} */")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    c = ["/* Gerado por tools/make_logo.py — não editar à mão.",
         " * Logotipo FOSTONOS (assets/logo/fostonos.svg). */",
         '#include "bf_logo.h"', ""]
    h = ["/* Gerado por tools/make_logo.py */", "#ifndef BF_LOGO_H", "#define BF_LOGO_H",
         '#include "lvgl.h"', ""]
    emit(c, h, "bf_logo_icon", logo_svg.render("icon", width=72), "ARGB8888")
    emit(c, h, "bf_logo_mark", logo_svg.render("mark", height=30), "ARGB8888")
    emit(c, h, "bf_logo_big", logo_svg.render("nobg", width=260), "ARGB8888")
    emit(c, h, "bf_logo_wm", logo_svg.render("mark", width=400), "A8")
    h += ["", "#endif", ""]
    with open(os.path.join(OUT_DIR, "bf_logo.c"), "w") as f:
        f.write("\n".join(c))
    with open(os.path.join(OUT_DIR, "bf_logo.h"), "w") as f:
        f.write("\n".join(h))
    print("assets/logo/bf_logo.c e bf_logo.h gerados")


if __name__ == "__main__":
    main()
