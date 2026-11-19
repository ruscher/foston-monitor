#!/usr/bin/env python3
"""Artwork of the README (docs/art): banner, key numbers, charts, step cards.

Reuses the Big Screen Monitor Display's own font, icon loader and colours
(Adwaita dark), so the README looks like the application.
Usage: python3 tools/make_readme_art.py   (needs Pillow, numpy, matplotlib, rsvg-convert)
"""

import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
APP = os.environ.get("BSMD_APP", "/home/ruscher/Documentos/Git/big-screen-monitor-display/usr/share/"
                                 "big-screen-monitor-display")
sys.path.insert(0, APP)

from bsmd import touch  # noqa: E402
from bsmd.bigmedia.paint import font  # noqa: E402

ART = os.path.join(ROOT, "docs", "art")
IMAGES = os.path.join(ROOT, "docs", "images")
ACCENT = touch.ACCENT                      # #3584e4, single-series colour (validated >= 3:1 on dark)
SURFACE = (26, 26, 25)                     # dark chart surface (#1a1a19)
CARD = (40, 40, 46)
TEXT = (240, 239, 236)                     # primary ink (dark)
SECONDARY = (195, 194, 183)                # secondary ink (dark)
MUTED = (120, 120, 128)
GOOD = (46, 194, 126)


def rounded(image, radius):
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, image.width - 1, image.height - 1), radius=radius, fill=255)
    out = Image.new("RGBA", image.size, (0, 0, 0, 0))
    out.paste(image.convert("RGBA"), (0, 0), mask)
    return out


def background(w, h):
    top, bottom = np.array((30, 30, 36)), np.array((14, 14, 18))
    grad = np.repeat((top + (bottom - top) * np.linspace(0, 1, h)[:, None])[:, None, :], w, axis=1)
    image = Image.fromarray(grad.astype(np.uint8), "RGB")
    glow = Image.new("L", (w, h), 0)
    ImageDraw.Draw(glow).ellipse((-w // 6, -h // 2, w // 2, h), fill=110)
    glow = glow.filter(ImageFilter.GaussianBlur(h // 4))
    return Image.composite(Image.new("RGB", (w, h), ACCENT), image, glow.point(lambda v: v // 3)).convert("RGBA")


def icon(name, size, color=TEXT):
    image = touch.symbolic_icon(name, size, color)
    if image is None:
        raise SystemExit(f"missing icon {name}")
    return image


def device_cutout():
    """Product photo with the menu screenshot in perspective, white background removed."""
    S = 2
    photo = Image.open(os.path.join(ART, "foston-fs707dc-produto.png")).convert("RGB").resize((500 * S, 500 * S),
                                                                                              Image.LANCZOS)
    quad = [(185, 185), (755, 200), (815, 520), (215, 557)]
    screen = Image.open(os.path.join(IMAGES, "menu.png")).convert("RGB")
    w, h = screen.size
    a, b = [], []
    for (x, y), (u, v) in zip(quad, [(0, 0), (w, 0), (w, h), (0, h)]):
        a += [[x, y, 1, 0, 0, 0, -u * x, -u * y], [0, 0, 0, x, y, 1, -v * x, -v * y]]
        b += [u, v]
    coeffs = np.linalg.solve(np.array(a, float), np.array(b, float)).tolist()
    warped = screen.transform(photo.size, Image.PERSPECTIVE, coeffs, Image.BICUBIC)
    mask = Image.new("L", photo.size, 0)
    ImageDraw.Draw(mask).polygon(quad, fill=255)
    out = Image.composite(warped, photo, mask.filter(ImageFilter.GaussianBlur(0.8)))
    glare = Image.new("L", photo.size, 0)
    ImageDraw.Draw(glare).polygon([(185, 185), (520, 192), (300, 557), (215, 557)], fill=26)
    glare = Image.composite(glare, Image.new("L", photo.size, 0), mask).filter(ImageFilter.GaussianBlur(30))
    out = Image.composite(Image.new("RGB", photo.size, (255, 255, 255)), out, glare)
    out.save(os.path.join(ART, "foston-com-menu.png"))
    # white background -> transparent: only the white connected to the photo border (flood fill),
    # so white text on the screen and the silver frame stay; the photo's brand logo is removed.
    rgb = np.asarray(out).astype(int)
    near_white = (rgb.min(axis=2) > 228).astype(np.uint8) * 255
    flood = Image.fromarray(near_white, "L").copy()      # fromarray shares a read-only buffer
    for seed in ((0, 0), (flood.width - 1, 0), (0, flood.height - 1), (flood.width - 1, flood.height - 1)):
        if flood.getpixel(seed) == 255:
            ImageDraw.floodfill(flood, seed, 128)
    background_px = np.asarray(flood) == 128
    alpha = np.where(background_px, 0, 255).astype(np.uint8)
    alpha[740:860, 540:920] = 0
    alpha_img = Image.fromarray(alpha).filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.0))
    cut = out.convert("RGBA")
    cut.putalpha(alpha_img)
    return cut.crop((40, 40, 960, 860))


def banner():
    w, h = 1280, 520
    image = background(w, h)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, w, 6), fill=(*ACCENT, 255))
    device = device_cutout()
    device.thumbnail((620, 560), Image.LANCZOS)
    shadow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse((90, h - 70, 560, h - 30), fill=(0, 0, 0, 150))
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(16)))
    image.alpha_composite(device, (20, h - device.height - 20))
    x = 660
    app = touch.app_icon(84)
    image.alpha_composite(app, (x, 70))
    draw.text((x + 100, 78), "Big Screen Monitor", font=font(26, True), fill=SECONDARY)
    draw.text((x + 100, 112), "Touch Display", font=font(20), fill=(*ACCENT, 255))
    draw.text((x, 180), "FOSTON", font=font(66, True), fill=TEXT)
    draw.text((x, 252), "FS-707DC", font=font(66, True), fill=TEXT)
    draw.text((x, 336), "Um GPS de 2010 virou painel touch do BigLinux,", font=font(21), fill=SECONDARY)
    draw.text((x, 364), "sem gravar firmware e sem abrir o aparelho.", font=font(21), fill=SECONDARY)
    chips = (("computer", "Windows CE 5"), ("emblem-system", "MediaTek MT3351"),
             ("input-touchpad", "800×480 touch"))
    cx = x
    for name, label in chips:
        f = font(15, True)
        tw = draw.textlength(label, font=f)
        box = (cx, 420, cx + int(tw) + 50, 456)
        draw.rounded_rectangle(box, radius=18, fill=(*CARD, 255), outline=(255, 255, 255, 30))
        image.alpha_composite(icon(name, 20, (*ACCENT,)), (cx + 13, 428))
        draw.text((cx + 40, 438), label, font=f, fill=TEXT, anchor="lm")
        cx = box[2] + 10
    image.convert("RGB").save(os.path.join(ART, "banner.png"), optimize=True)


def key_numbers():
    tiles = (("speedometer", "media-playback-start", "7,3", "fps", "quadro inteiro 800×480"),
             (None, "view-refresh", "130", "fps", "atualizações pequenas (relógio)"),
             (None, "network-wired", "2", "ms", "PING ida e volta pelo USB"),
             (None, "drive-harddisk-usb", "52", "KB", "cliente instalado no GPS"))
    w, h, pad = 1280, 210, 18
    image = Image.new("RGBA", (w, h), (*SURFACE, 255))
    draw = ImageDraw.Draw(image)
    tw = (w - pad * 5) / 4
    for i, (_unused, name, value, unit, label) in enumerate(tiles):
        x0 = pad + i * (tw + pad)
        box = (int(x0), pad, int(x0 + tw), h - pad)
        draw.rounded_rectangle(box, radius=18, fill=(*CARD, 255))
        image.alpha_composite(icon(name, 26, ACCENT), (box[0] + 22, box[1] + 22))
        vf = font(60, True)
        draw.text((box[0] + 22, box[1] + 64), value, font=vf, fill=TEXT)
        vw = draw.textlength(value, font=vf)
        draw.text((box[0] + 30 + vw, box[1] + 96), unit, font=font(24, True), fill=SECONDARY)
        draw.text((box[0] + 22, box[3] - 30), label, font=font(17), fill=SECONDARY)
    image.convert("RGB").save(os.path.join(ART, "numeros.png"), optimize=True)


def bar_chart(path, title, subtitle, labels, values, value_text, colors, xmax, unit):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    family = None
    for candidate in ("DejaVu Sans", "Cantarell"):
        if any(f.name == candidate for f in font_manager.fontManager.ttflist):
            family = candidate
            break
    plt.rcParams.update({"font.family": family or "sans-serif", "font.size": 12})
    hexc = lambda c: "#%02x%02x%02x" % c[:3]  # noqa: E731
    fig, ax = plt.subplots(figsize=(12.8, 1.2 + 0.85 * len(labels)), dpi=100)
    fig.patch.set_facecolor(hexc(SURFACE))
    ax.set_facecolor(hexc(SURFACE))
    ypos = list(range(len(labels)))[::-1]
    ax.barh([y for y, v in zip(ypos, values) if v > 0], [v for v in values if v > 0], height=0.56,
            color=[hexc(c) for c, v in zip(colors, values) if v > 0], linewidth=0)
    for y, value, text in zip(ypos, values, value_text):
        ax.text(max(value, 0) + xmax * 0.012, y, text, va="center", ha="left", color=hexc(TEXT),
                fontsize=14, fontweight="bold")
    ax.set_yticks(ypos)
    ax.set_yticklabels(labels, color=hexc(SECONDARY), fontsize=13)
    ax.set_xlim(0, xmax * 1.18)
    ax.set_ylim(-0.6, len(labels) - 0.4)
    ax.tick_params(axis="x", colors=hexc(MUTED), labelsize=11, length=0)
    ax.tick_params(axis="y", length=0, pad=10)
    ax.set_xlabel(unit, color=hexc(MUTED), fontsize=11)
    ax.grid(axis="x", color=hexc(SECONDARY), alpha=0.12, linewidth=1)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_visible(False)
    fig.suptitle(title, x=0.012, ha="left", color=hexc(TEXT), fontsize=17, fontweight="bold", y=0.97)
    fig.text(0.012, 0.97 - 0.42 / fig.get_figheight(), subtitle, ha="left", va="top", color=hexc(SECONDARY),
             fontsize=12)
    fig.subplots_adjust(left=0.30, right=0.97, top=1 - 1.05 / fig.get_figheight(), bottom=0.55 / fig.get_figheight())
    fig.savefig(path, facecolor=fig.get_facecolor())
    plt.close(fig)


def charts():
    grey = (110, 110, 118)
    bar_chart(os.path.join(ART, "grafico-vazao.png"), "Quadros inteiros por segundo",
              "Medido no FOSTON real, quadro 800×480 trocando a cada envio (pior caso)",
              ["Cliente 1.0 (leitura bloqueante)", "Cliente 1.1 + leitura antiga do PC", "Cliente 1.1 + leitura corrigida"],
              [0, 5.0, 7.3], ["perdia dados", "5,0 fps", "7,3 fps"], [grey, grey, ACCENT], 8, "quadros por segundo")
    bar_chart(os.path.join(ART, "grafico-reparos.png"), "Quadros refeitos por minuto (menor é melhor)",
              "Perdas do driver USB do Windows CE, corrigidas sozinhas pelo serviço em uso normal",
              ["1.1.1 · janela 64 KiB", "1.2.0 · leitora prioritária + 32 KiB"],
              [1.1, 0.8], ["1,1 / min", "0,8 / min"], [grey, ACCENT], 1.3, "reparos por minuto")


STEPS = (
    (1, "package-x-generic", "Instale o Big Screen Monitor", "Pacote com o suporte ao FOSTON",
     ("utilities-terminal",), "sudo pacman -U big-screen-monitor-display-*.pkg.tar"),
    (2, "drive-harddisk-usb", "Conecte o FOSTON pelo USB", "Ele aparece no PC como um pendrive",
     ("computer", "go-next", "media-removable"), "045e:ffff · Windows CE Mass Storage"),
    (3, "document-save", "Copie o cliente para o GPS", "Meus Displays → FOSTON → Instalar o cliente",
     ("application-x-executable", "go-next", "folder"), "BigScreen\\bsmd-ce.exe"),
    (4, "system-reboot", "Ejete e dê reset no FOSTON", "O WinCE guarda os arquivos antigos em cache",
     ("media-removable", "go-next", "view-refresh"), "botão RESET (furinho) ou desligar 10 s"),
    (5, "preferences-system", "Aponte o caminho da navegação", "Só na primeira vez, no menu do GPS",
     ("emblem-system", "go-next", "folder", "go-next", "application-x-executable"),
     "Configurações → Caminho da navegação → BigScreen → bsmd-ce.exe"),
    (6, "media-playback-start", "Toque em Navegação e conecte o cabo", "O painel aparece em ~1 segundo",
     ("input-touchpad", "go-next", "network-wired", "go-next", "computer"), "045e:00ce · /dev/ttyUSB0"),
)


def step_cards():
    w, h = 1280, 190
    for number, main, title, subtitle, flow, detail in STEPS:
        image = Image.new("RGBA", (w, h), (*SURFACE, 255))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((8, 8, w - 8, h - 8), radius=22, fill=(*CARD, 255))
        cx, cy, r = 92, h // 2, 54
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(*ACCENT, 255))
        draw.text((cx, cy - 2), str(number), font=font(56, True), fill=(255, 255, 255), anchor="mm")
        image.alpha_composite(icon(main, 30, ACCENT), (180, 36))
        draw.text((222, 50), title, font=font(30, True), fill=TEXT, anchor="lm")
        draw.text((180, 92), subtitle, font=font(19), fill=SECONDARY, anchor="lm")
        # flow of icons on the right
        x = w - 40
        items = list(flow)[::-1]
        for name in items:
            size = 26 if name == "go-next" else 44
            col = MUTED if name == "go-next" else TEXT
            x -= size
            if name != "go-next":
                draw.rounded_rectangle((x - 10, 40, x + size + 10, 40 + size + 20), radius=14,
                                       fill=(*touch.mix(CARD, (255, 255, 255), 0.06)[:3], 255))
                image.alpha_composite(icon(name, size, col), (x, 50))
            else:
                image.alpha_composite(icon(name, size, col), (x, 60))
            x -= 22
        # detail line (monospace look)
        f = font(18, False, True)
        tw = draw.textlength(detail, font=f)
        box = (180, 124, 180 + int(tw) + 28, 160)
        draw.rounded_rectangle(box, radius=10, fill=(20, 20, 24, 255))
        draw.text((194, 142), detail, font=f, fill=(*GOOD,), anchor="lm")
        image.convert("RGB").save(os.path.join(ART, f"passo-{number}.png"), optimize=True)


if __name__ == "__main__":
    os.makedirs(ART, exist_ok=True)
    banner()
    key_numbers()
    charts()
    step_cards()
    print("ok:", sorted(os.listdir(ART)))
