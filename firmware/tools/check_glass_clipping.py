#!/usr/bin/env python3
"""check_glass_clipping.py — docs/reviews/puck-ux-usability-2026-09-15.md.

## Why this exists

The goldens under `firmware/tests/golden/` are SQUARE 412x412 PNGs of a
framebuffer that, on the real puck, is seen through a ROUND bezel window
sitting ~5 px right of the pixel array: `FF_THEME_GLASS_CX/CY/R =
(208, 206, 200)` (see `docs/hardware/glass-offset.md`). Anything a face
paints outside that circle is invisible on glass — but perfectly visible
in the golden, and therefore perfectly invisible to a human reviewing
goldens.

`targets/sim/tests/test_tap_target_sizing.c` already checks that every
*interactive* control's hit rect stays inside the glass circle. Nothing
checks the PAINTED pixels: a label, a card shoulder, a battery percentage
or a list row's right edge can run off the glass without any control
doing so.

## What it checks

For every golden PNG, counts pixels that are (a) outside the glass circle
and (b) not background. "Background" is the theme's own
`FF_THEME_COLOR_BG` (0x0B0B10) plus pure black, within `--tol` per
channel — the letterbox corners the sim paints are one of those two.

Reported per file as `lit_outside` (count) and the farthest such pixel's
distance from the glass centre, so a 3 px anti-aliasing fringe is
distinguishable from a battery readout losing its digits.

## Usage

    firmware/tools/check_glass_clipping.py firmware/tests/golden/*.png
    firmware/tools/check_glass_clipping.py --min 200 firmware/tests/golden/*.png
    firmware/tools/check_glass_clipping.py --csv out.csv firmware/tests/golden/*.png

Exit status is 0 always: this is a MEASUREMENT tool, not a gate. Which
overruns are real defects and which are deliberate bleed (the inbox
compose FAB's masked corner, for one) is a judgement the reviewer makes
from the numbers, not something a threshold can decide.

Requires Pillow (`pip install pillow`).
"""

import argparse
import csv
import math
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment-dependent
    sys.stderr.write("check_glass_clipping.py: needs Pillow (pip install pillow)\n")
    sys.exit(2)

# Keep these in sync with firmware/app/theme/ff_theme.h.
GLASS_CX = 208
GLASS_CY = 206
GLASS_R = 200
THEME_BG = (0x0B, 0x0B, 0x10)
PX_PER_MM = 11.44


def is_background(px, tol):
    r, g, b = px[0], px[1], px[2]
    if r <= tol and g <= tol and b <= tol:
        return True  # pure-black letterbox
    return (
        abs(r - THEME_BG[0]) <= tol
        and abs(g - THEME_BG[1]) <= tol
        and abs(b - THEME_BG[2]) <= tol
    )


def scan(path, tol):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()
    r2 = GLASS_R * GLASS_R
    lit = 0
    far = 0.0
    far_at = None
    for y in range(h):
        dy = y - GLASS_CY
        for x in range(w):
            dx = x - GLASS_CX
            d2 = dx * dx + dy * dy
            if d2 <= r2:
                continue
            if is_background(px[x, y], tol):
                continue
            lit += 1
            d = math.sqrt(d2)
            if d > far:
                far = d
                far_at = (x, y)
    return lit, far, far_at


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pngs", nargs="+", help="golden PNGs to scan")
    ap.add_argument("--tol", type=int, default=6, help="per-channel tolerance for 'is background' (default 6)")
    ap.add_argument("--min", type=int, default=1, help="only report files with at least this many lit pixels outside")
    ap.add_argument("--csv", help="also write the full table here")
    args = ap.parse_args(argv)

    rows = []
    for path in sorted(args.pngs):
        lit, far, far_at = scan(path, args.tol)
        rows.append((path, lit, far, far_at))

    print(f"glass circle: centre ({GLASS_CX},{GLASS_CY}) r={GLASS_R}  ({PX_PER_MM:.2f} px/mm)")
    print(f"{'golden':46s} {'lit_outside':>11s} {'max_dist_px':>11s} {'overrun_mm':>10s}  at")
    for path, lit, far, far_at in rows:
        if lit < args.min:
            continue
        name = path.rsplit("/", 1)[-1]
        over_mm = (far - GLASS_R) / PX_PER_MM if far > GLASS_R else 0.0
        print(f"{name:46s} {lit:11d} {far:11.1f} {over_mm:10.2f}  {far_at}")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            wtr = csv.writer(fh)
            wtr.writerow(["golden", "lit_outside", "max_dist_px", "overrun_mm", "x", "y"])
            for path, lit, far, far_at in rows:
                over_mm = (far - GLASS_R) / PX_PER_MM if far > GLASS_R else 0.0
                x, y = far_at if far_at else ("", "")
                wtr.writerow([path.rsplit("/", 1)[-1], lit, f"{far:.1f}", f"{over_mm:.2f}", x, y])

    return 0


if __name__ == "__main__":
    sys.exit(main())
