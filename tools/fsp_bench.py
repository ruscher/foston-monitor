#!/usr/bin/env python3
"""Bench test of a FOSTON running bsmd-ce, using the project's real driver.

Usage: fsp_bench.py [/dev/ttyUSB0] [--seconds N] [--touch N]

1. HELLO handshake (prints what the device reports);
2. colour bars, grid and text patterns (visual check on the device);
3. throughput: full frames (worst case) and small dirty rectangles;
4. PING round trips;
5. touch: prints every event for N seconds (tap the panel).

All numbers are measured here; results go to stdout as JSON lines too.
"""

import argparse
import glob
import json
import os
import statistics
import sys
import time

REPO = os.environ.get("BSMD_REPO", "/home/ruscher/Documentos/Git/big-screen-monitor-display")
sys.path.insert(0, os.path.join(REPO, "usr/share/big-screen-monitor-display"))

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

from bsmd.displays.foston.driver import FostonDisplay  # noqa: E402


def font(size):
    for path in ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def bars(w, h):
    image = Image.new("RGB", (w, h))
    draw = ImageDraw.Draw(image)
    colors = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0),
              (0, 0, 255), (0, 0, 0)]
    for i, color in enumerate(colors):
        draw.rectangle((i * w // 8, 0, (i + 1) * w // 8, h * 2 // 3), fill=color)
    for x in range(w):
        v = x * 255 // (w - 1)
        draw.line((x, h * 2 // 3, x, h * 5 // 6), fill=(v, v, v))
    draw.text((w // 2, h * 11 // 12), "FOSTON FS-707DC - Big Screen Monitor", font=font(28), fill=(255, 255, 255),
              anchor="mm")
    return image


def grid(w, h):
    image = Image.new("RGB", (w, h), (10, 12, 18))
    draw = ImageDraw.Draw(image)
    for x in range(0, w, 40):
        draw.line((x, 0, x, h), fill=(60, 70, 90))
    for y in range(0, h, 40):
        draw.line((0, y, w, y), fill=(60, 70, 90))
    draw.rectangle((0, 0, w - 1, h - 1), outline=(255, 0, 0), width=2)
    for size, y in ((10, 40), (14, 70), (20, 110), (32, 170), (56, 260)):
        draw.text((20, y), f"{size}px Texto legível 0123456789", font=font(size), fill=(255, 255, 255))
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--touch", type=float, default=20.0)
    args = parser.parse_args()
    port = args.port or (sorted(glob.glob("/dev/ttyUSB*")) or [None])[0]
    if not port:
        sys.exit("no /dev/ttyUSB* (is bsmd-ce running and the cable connected?)")
    driver = FostonDisplay(port)
    t0 = time.monotonic()
    info = driver.open()
    print(json.dumps({"step": "hello", "ms": round((time.monotonic() - t0) * 1000, 1), "model": info.model,
                      "size": [info.width, info.height], "details": info.details}, ensure_ascii=False))
    w, h = driver.native_size
    results = {}
    for name, image in (("bars", bars(w, h)), ("grid", grid(w, h))):
        driver.invalidate()
        t = driver.present(image)
        print(json.dumps({"step": f"pattern:{name}", "bytes": t.bytes, "ms": round(t.duration * 1000, 1),
                          "device_decode_ms": driver.stats["decode_ms"], "device_blit_ms": driver.stats["blit_ms"]}))
        time.sleep(2.5)

    # worst case: every pixel changes (alternating noise-free full frames)
    times, sizes = [], []
    frames_ = [bars(w, h), grid(w, h)]
    end = time.monotonic() + args.seconds
    i = 0
    while time.monotonic() < end:
        driver.invalidate()
        t = driver.present(frames_[i % 2])
        times.append(t.duration)
        sizes.append(t.bytes)
        i += 1
    results["full_frame"] = {"frames": len(times), "fps": round(len(times) / sum(times), 2),
                             "ms_median": round(statistics.median(times) * 1000, 1),
                             "bytes_median": int(statistics.median(sizes)),
                             "throughput_kib_s": round(sum(sizes) / sum(times) / 1024, 1)}
    print(json.dumps({"step": "full_frames", **results["full_frame"]}))

    # typical dashboard update: a clock-sized region changes
    base = grid(w, h)
    driver.invalidate()
    driver.present(base)
    times = []
    end = time.monotonic() + args.seconds
    n = 0
    while time.monotonic() < end:
        image = base.copy()
        ImageDraw.Draw(image).text((560, 400), f"{n:06d}", font=font(40), fill=(255, 200, 0))
        t = driver.present(image)
        times.append(t.duration)
        n += 1
    results["small_update"] = {"frames": len(times), "fps": round(len(times) / sum(times), 2),
                               "ms_median": round(statistics.median(times) * 1000, 1),
                               "ms_p95": round(sorted(times)[int(len(times) * 0.95)] * 1000, 1)}
    print(json.dumps({"step": "small_updates", **results["small_update"]}))

    rtts = []
    for _ in range(20):
        t = time.monotonic()
        driver.probe()
        rtts.append((time.monotonic() - t) * 1000)
    print(json.dumps({"step": "ping", "ms_median": round(statistics.median(rtts), 2), "ms_max": round(max(rtts), 2)}))

    if args.touch > 0:
        driver.invalidate()
        image = grid(w, h)
        ImageDraw.Draw(image).text((w // 2, h // 2), "Toque na tela", font=font(48), fill=(255, 255, 0), anchor="mm")
        driver.present(image)
        print(json.dumps({"step": "touch", "seconds": args.touch}))
        canvas = image.copy()
        draw = ImageDraw.Draw(canvas)

        def on_input(event):
            lag = (time.monotonic() - event.received) * 1000
            print(json.dumps({"touch": event.kind, "x": event.x, "y": event.y, "tick": event.tick,
                              "dispatch_ms": round(lag, 2)}), flush=True)
            draw.ellipse((event.x - 6, event.y - 6, event.x + 6, event.y + 6), fill=(255, 60, 60))
        driver.on_input = on_input
        end = time.monotonic() + args.touch
        while time.monotonic() < end:
            driver.present(canvas)
            time.sleep(0.05)
    driver.close()
    print(json.dumps({"step": "done", "stats": {k: v for k, v in driver.stats.items() if k != "encodings"},
                      "encodings": dict(driver.stats["encodings"])}))


if __name__ == "__main__":
    main()
