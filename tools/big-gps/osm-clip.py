#!/usr/bin/env python3
"""Clip an OSM .pbf to a bounding box (complete ways and relations) with pyosmium.

  osm-clip.py IN.osm.pbf OUT.osm.pbf --bbox MINLON,MINLAT,MAXLON,MAXLAT

Keeps every node inside the box, every way with at least one node inside (with
all its nodes) and every relation with a kept member (members completed by
osmium.BackReferenceWriter).  Runs on the PC (map builder); needs pyosmium >= 4.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import sys

import osmium


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--bbox", required=True)
    a = ap.parse_args()
    x0, y0, x1, y1 = (float(v) for v in a.bbox.split(","))
    if os.path.exists(a.out):
        os.unlink(a.out)
    nodes, ways = set(), set()
    n = [0, 0, 0]
    with osmium.BackReferenceWriter(a.out, ref_src=a.inp, overwrite=True, remove_tags=False) as w:
        for o in osmium.FileProcessor(a.inp).with_locations():
            if o.is_node():
                if o.location.valid() and x0 <= o.location.lon <= x1 and y0 <= o.location.lat <= y1:
                    nodes.add(o.id)
                    w.add(o)
                    n[0] += 1
            elif o.is_way():
                if any(r.ref in nodes for r in o.nodes):
                    ways.add(o.id)
                    w.add(o)
                    n[1] += 1
            elif o.is_relation():
                if any((m.type == "n" and m.ref in nodes) or (m.type == "w" and m.ref in ways) for m in o.members):
                    w.add(o)
                    n[2] += 1
    print(f"{a.out}: {n[0]} nós, {n[1]} vias, {n[2]} relações no recorte; {os.path.getsize(a.out)} bytes")


if __name__ == "__main__":
    sys.exit(main())
