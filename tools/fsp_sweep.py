#!/usr/bin/env python3
"""Sweep credit window x message size on the real FOSTON; reports stability and speed."""
import json, sys, time, statistics
sys.path.insert(0, "/home/ruscher/Documentos/Git/big-screen-monitor-display/usr/share/big-screen-monitor-display")
sys.path.insert(0, "/home/ruscher/Documentos/Git/foston-monitor/tools")
from fsp_bench import bars, grid
from bsmd.displays.foston.driver import FostonDisplay
from bsmd.displays.base import DisplayError
imgs = [bars(800, 480), grid(800, 480)]
combos = [(int(a), int(b)) for a, b in (c.split("x") for c in sys.argv[1:])]
for window, max_raw in combos:
    d = FostonDisplay("/dev/ttyUSB0"); d.WINDOW = window; d.MAX_RAW = max_raw; d.PRESENT_TIMEOUT = 4
    try:
        d.open()
    except DisplayError as e:
        print(json.dumps({"window": window, "max_raw": max_raw, "open_error": str(e)})); time.sleep(1.5); continue
    times, ok, err, nbytes = [], 0, None, 0
    end = time.monotonic() + float(__import__("os").environ.get("SWEEP_S", "8"))
    i = 0
    while time.monotonic() < end:
        d.invalidate()
        try:
            t = d.present(imgs[i % 2]); times.append(t.duration); nbytes += t.bytes; ok += 1
        except DisplayError as e:
            err = str(e); break
        i += 1
    d.close()
    r = {"window": window, "max_raw": max_raw, "frames_ok": ok, "error": err}
    if times:
        r.update(fps=round(ok / sum(times), 2), ms_median=round(statistics.median(times) * 1000), kib_s=round(nbytes / sum(times) / 1024))
    print(json.dumps(r), flush=True)
    time.sleep(1.5)
