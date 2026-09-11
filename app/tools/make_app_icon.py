#!/usr/bin/env python3
"""
make_app_icon.py — render the Firefly flare mark into the app icon
masters, and every catalog size XcodeGen wires up, DETERMINISTICALLY
from the same geometry table the puck itself rasterises.

Why generate rather than hand-draw in Figma/Sketch: firmware/core/
include/ff_flare_mark.h's own doc comment states its whole reason for
existing — "the two places in this repo that draw the mark stay
pixel-identical instead of two hand-copied literals that can silently
drift apart" (scr_flare.c's LVGL takeover screen and ff_display.c's raw
boot splash). A hand-authored icon PNG would be a THIRD hand-copied copy
of that shape — exactly what the header exists to avoid. This script
instead PARSES the header's literals at generation time (never retypes
them), so a future change to ray fractions/lengths/stroke width
propagates into the icon the next time this script runs, the same
guarantee the header gives its other two consumers.

Tools on this machine (checked before writing this script):
    python3 -c "import PIL"   -> Pillow 12.3.0 present
    which swift                -> present, but unused: Pillow covers it
So: Python 3 + Pillow. No `swift` + CoreGraphics fallback was needed.

Colors: firmware/app/theme/ff_theme.h —
    FF_THEME_COLOR_BG    0x0B0B10  puck background
    FF_THEME_COLOR_AMBER 0xFFC66B  primary accent

Usage:
    python3 app/tools/make_app_icon.py

Writes every PNG (+ a freshly generated Contents.json) directly into:
    app/Firefly/Resources/Assets.xcassets/AppIcon.appiconset/
"""
from __future__ import annotations

import json
import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

# ---------------------------------------------------------------------------
# 1. Geometry — parsed from firmware/core/include/ff_flare_mark.h, not
#    retyped. See module doc comment above for why.
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
FLARE_HEADER = REPO_ROOT / "firmware/core/include/ff_flare_mark.h"
APPICONSET = REPO_ROOT / "app/Firefly/Resources/Assets.xcassets/AppIcon.appiconset"

BG_COLOR = (0x0B, 0x0B, 0x10, 255)  # FF_THEME_COLOR_BG
AMBER_COLOR = (0xFF, 0xC6, 0x6B, 255)  # FF_THEME_COLOR_AMBER
WHITE = (0xFF, 0xFF, 0xFF, 255)

# How large the mark renders on a 1024px canvas: the ray-tip + half
# stroke "reach" (see native_reach below) is scaled to land at this many
# pixels from center. Chosen for a generous safe margin — the mark's
# bounding circle covers well under half the 1024 canvas width — while
# staying legible at small (e.g. 40x40pt Home Screen) sizes. Used for
# BOTH the iOS full-bleed master and the macOS content square: both
# masters are authored on the same 1024x1024 canvas (the macOS one just
# leaves everything outside its rounded-rect content area transparent),
# so one absolute pixel size keeps the mark visually consistent across
# platforms.
MARK_REACH_PX = 340.0

# Apple's macOS "Big Sur+" icon grid: an 824x824 content square,
# centered, in the 1024x1024 canvas, with a continuous rounded-rect
# ("squircle") shape. 184px corner radius ~= 824 * 0.2237, the ratio
# commonly used for that grid (Apple does not publish an exact number;
# this is the standard approximation every icon-generation tool in
# common use converges on).
MAC_CONTENT_PX = 824
MAC_CORNER_RADIUS_PX = 184

SUPERSAMPLE = 4  # render at 4x canvas size, downsample w/ LANCZOS for AA


def parse_flare_geometry() -> dict:
    text = FLARE_HEADER.read_text()

    def scalar(name: str) -> float:
        m = re.search(rf"#define {name}\s+([0-9.]+)f?", text)
        if not m:
            raise ValueError(f"{name} not found in {FLARE_HEADER}")
        return float(m.group(1))

    n_rays_m = re.search(r"#define FF_FLARE_MARK_N_RAYS\s+(\d+)", text)
    if not n_rays_m:
        raise ValueError("FF_FLARE_MARK_N_RAYS not found")
    n_rays = int(n_rays_m.group(1))

    frac_m = re.search(
        r"FF_FLARE_MARK_RAY_FRAC\[FF_FLARE_MARK_N_RAYS\]\s*=\s*\{([^}]+)\}", text
    )
    if not frac_m:
        raise ValueError("FF_FLARE_MARK_RAY_FRAC table not found")
    fracs = [float(x) for x in re.findall(r"[0-9.]+", frac_m.group(1))]
    if len(fracs) != n_rays:
        raise ValueError(f"expected {n_rays} ray fractions, parsed {len(fracs)}")

    geo = {
        "n_rays": n_rays,
        "frac": fracs,
        "max_len": scalar("FF_FLARE_MARK_MAX_LEN_PX"),
        "center_r": scalar("FF_FLARE_MARK_CENTER_R_PX"),
        "line_w": scalar("FF_FLARE_MARK_LINE_W_PX"),
    }
    print(
        f"[make_app_icon] parsed {FLARE_HEADER.relative_to(REPO_ROOT)}: "
        f"n_rays={geo['n_rays']} max_len={geo['max_len']} "
        f"center_r={geo['center_r']} line_w={geo['line_w']} "
        f"frac={geo['frac']}"
    )
    return geo


def ray_offset(i: int, max_len: float, geo: dict) -> tuple[float, float]:
    """Mirrors ff_flare_mark_ray_offset() in ff_flare_mark.h exactly:
    clockwise from north (i=0 == straight up), screen +Y is down."""
    idx = i % geo["n_rays"]
    angle_deg = idx * (360.0 / geo["n_rays"])
    rad = math.radians(angle_deg)
    length = max_len * geo["frac"][idx]
    dx = math.sin(rad) * length
    dy = -math.cos(rad) * length
    return dx, dy


# ---------------------------------------------------------------------------
# 2. Rendering
# ---------------------------------------------------------------------------


def draw_flare_mark(
    draw: ImageDraw.ImageDraw, cx: float, cy: float, scale: float, geo: dict, color: tuple
) -> None:
    max_len = geo["max_len"] * scale
    half_w = (geo["line_w"] * scale) / 2.0
    center_r = geo["center_r"] * scale

    for i in range(geo["n_rays"]):
        dx, dy = ray_offset(i, max_len, geo)
        ex, ey = cx + dx, cy + dy
        draw.line([(cx, cy), (ex, ey)], fill=color, width=max(1, round(half_w * 2)))
        # Round cap at the ray tip — the center-end cap is already
        # covered by the filled center dot drawn below (center_r >
        # half_w by construction, same relationship the puck's own
        # flare_mark_pixel_hit relies on).
        draw.ellipse([ex - half_w, ey - half_w, ex + half_w, ey + half_w], fill=color)

    draw.ellipse([cx - center_r, cy - center_r, cx + center_r, cy + center_r], fill=color)


def render_master(
    *,
    size: int,
    geo: dict,
    mark_color: tuple,
    bg_color: tuple | None,
    squircle: bool,
) -> Image.Image:
    ss = size * SUPERSAMPLE
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    if squircle:
        pad = (ss - MAC_CONTENT_PX * SUPERSAMPLE) / 2.0
        draw.rounded_rectangle(
            [pad, pad, ss - pad, ss - pad],
            radius=MAC_CORNER_RADIUS_PX * SUPERSAMPLE,
            fill=bg_color,
        )
    elif bg_color is not None:
        draw.rectangle([0, 0, ss, ss], fill=bg_color)

    # native_reach: the mark's own farthest extent from its center, in
    # its NATIVE (firmware) pixel units — the north ray's full length
    # plus half the stroke width, for the round cap. Scaling this onto
    # MARK_REACH_PX is what keeps every rendered size proportionally
    # identical to the puck's own splash/takeover-screen mark.
    native_reach = geo["max_len"] + geo["line_w"] / 2.0
    scale = (MARK_REACH_PX * SUPERSAMPLE) / native_reach

    draw_flare_mark(draw, ss / 2.0, ss / 2.0, scale, geo, mark_color)

    return img.resize((size, size), Image.LANCZOS)


# ---------------------------------------------------------------------------
# 3. Catalog assembly
# ---------------------------------------------------------------------------

# (filename, mac point-size, scale) — the classic macOS icon ladder.
MAC_SIZES = [
    ("icon-mac-16.png", 16, "1x"),
    ("icon-mac-16@2x.png", 16, "2x"),
    ("icon-mac-32.png", 32, "1x"),
    ("icon-mac-32@2x.png", 32, "2x"),
    ("icon-mac-128.png", 128, "1x"),
    ("icon-mac-128@2x.png", 128, "2x"),
    ("icon-mac-256.png", 256, "1x"),
    ("icon-mac-256@2x.png", 256, "2x"),
    ("icon-mac-512.png", 512, "1x"),
    ("icon-mac-512@2x.png", 512, "2x"),
]


def build_contents_json() -> dict:
    images = [
        # iOS 18+ single-size icon (Xcode 15+): one 1024 master for the
        # default ("Any") appearance...
        {
            "filename": "icon-1024.png",
            "idiom": "universal",
            "platform": "ios",
            "size": "1024x1024",
        },
        # ...the dark appearance, identical artwork — the app is dark
        # themed already (FF_THEME_COLOR_BG), so "dark mode" is just
        # "the icon", per the task spec ("dark: same").
        {
            "appearances": [{"appearance": "luminosity", "value": "dark"}],
            "filename": "icon-1024-dark.png",
            "idiom": "universal",
            "platform": "ios",
            "size": "1024x1024",
        },
        # ...and the tinted appearance: white-on-transparent grayscale
        # mask, per Apple's guidance that a tinted-appearance image
        # should carry no background of its own — the system supplies
        # one and tints the mark.
        {
            "appearances": [{"appearance": "luminosity", "value": "tinted"}],
            "filename": "icon-1024-tinted.png",
            "idiom": "universal",
            "platform": "ios",
            "size": "1024x1024",
        },
    ]
    for filename, pt, scale in MAC_SIZES:
        images.append(
            {
                "filename": filename,
                "idiom": "mac",
                "scale": scale,
                "size": f"{pt}x{pt}",
            }
        )
    return {"images": images, "info": {"author": "xcode", "version": 1}}


def main() -> None:
    geo = parse_flare_geometry()
    APPICONSET.mkdir(parents=True, exist_ok=True)

    # --- iOS: 1024 full-bleed, no rounded corners (iOS masks it) ---
    ios_any = render_master(size=1024, geo=geo, mark_color=AMBER_COLOR, bg_color=BG_COLOR, squircle=False)
    ios_any.save(APPICONSET / "icon-1024.png", optimize=True)
    # Dark == same artwork, per spec.
    ios_any.save(APPICONSET / "icon-1024-dark.png", optimize=True)

    ios_tinted = render_master(size=1024, geo=geo, mark_color=WHITE, bg_color=None, squircle=False)
    ios_tinted.save(APPICONSET / "icon-1024-tinted.png", optimize=True)

    # --- macOS: 824px rounded-rect content square in a 1024 canvas ---
    mac_master = render_master(size=1024, geo=geo, mark_color=AMBER_COLOR, bg_color=BG_COLOR, squircle=True)
    for filename, pt, scale in MAC_SIZES:
        px = pt * (2 if scale == "2x" else 1)
        img = mac_master if px == 1024 else mac_master.resize((px, px), Image.LANCZOS)
        img.save(APPICONSET / filename, optimize=True)

    contents = build_contents_json()
    (APPICONSET / "Contents.json").write_text(json.dumps(contents, indent=2) + "\n")

    total_bytes = sum(f.stat().st_size for f in APPICONSET.glob("*.png"))
    print(
        f"[make_app_icon] wrote {len(list(APPICONSET.glob('*.png')))} PNGs + "
        f"Contents.json to {APPICONSET.relative_to(REPO_ROOT)} "
        f"({total_bytes / 1024:.1f} KiB total)"
    )


if __name__ == "__main__":
    main()
