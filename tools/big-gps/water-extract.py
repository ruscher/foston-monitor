#!/usr/bin/env python3
"""Extract the ocean polygons of a region for bgmap-build --water.

  water-extract.py water-polygons-split-4326.zip OUT.bin --bbox MINLON,MINLAT,MAXLON,MAXLAT

Input: the "water polygons" made from the OpenStreetMap coastline by
osmdata.openstreetmap.de (ODbL, same attribution as the map).  Output (little endian):
  "BGW1" u32 npolygons, per polygon { u32 nrings, per ring { u32 npts, npts x (f64 lon, f64 lat) } }
Needs pyshp.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import io
import struct
import sys
import zipfile

import shapefile


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("zip")
    ap.add_argument("out")
    ap.add_argument("--bbox", required=True)
    a = ap.parse_args()
    x0, y0, x1, y1 = (float(v) for v in a.bbox.split(","))
    z = zipfile.ZipFile(a.zip)
    names = z.namelist()
    base = next(n[:-4] for n in names if n.endswith(".shp"))
    shp = io.BytesIO(z.read(base + ".shp"))
    shx = io.BytesIO(z.read(base + ".shx"))
    dbf = io.BytesIO(z.read(base + ".dbf"))
    r = shapefile.Reader(shp=shp, shx=shx, dbf=dbf)
    polys, pts = 0, 0
    with open(a.out, "wb") as f:
        f.write(b"BGW1" + struct.pack("<I", 0))
        for s in r.iterShapes():
            bx0, by0, bx1, by1 = s.bbox
            if bx1 < x0 or bx0 > x1 or by1 < y0 or by0 > y1:
                continue
            parts = list(s.parts) + [len(s.points)]
            rings = [s.points[parts[i]:parts[i + 1]] for i in range(len(parts) - 1)]
            f.write(struct.pack("<I", len(rings)))
            for ring in rings:
                f.write(struct.pack("<I", len(ring)))
                f.write(b"".join(struct.pack("<dd", p[0], p[1]) for p in ring))
                pts += len(ring)
            polys += 1
        f.seek(4)
        f.write(struct.pack("<I", polys))
    print(f"{a.out}: {polys} polígonos, {pts} pontos")


if __name__ == "__main__":
    sys.exit(main())
