#!/usr/bin/env python3
"""Converte as telas cruas (RGB565, 800x480) do preview.c em PNG.

  preview.py PASTA      (cada .565 vira .png ao lado)
SPDX-License-Identifier: GPL-3.0-or-later
"""
import glob
import os
import sys

from PIL import Image

W, H = 800, 480


def main():
    for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.565"))):
        data = open(path, "rb").read()
        px = bytearray(W * H * 3)
        for i in range(W * H):
            v = data[2 * i] | data[2 * i + 1] << 8
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            px[3 * i:3 * i + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        out = path[:-4] + ".png"
        Image.frombytes("RGB", (W, H), bytes(px)).save(out)
        os.remove(path)
        print(out)


if __name__ == "__main__":
    main()
