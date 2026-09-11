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
them), so a future change to the ray-fraction TABLE (the 8 relative ray
lengths that give the mark its "long north ray, uneven taper" identity)
propagates into the icon the next time this script runs, the same
guarantee the header gives its other two consumers.

What the header does NOT dictate for the icon: FF_FLARE_MARK_MAX_LEN_PX,
_LINE_W_PX and _CENTER_R_PX are the puck's OWN tuned absolute pixel
values, sized for a 412x412 display viewed close up. Naively scaling
those literals up to a 1024px icon canvas (the previous version of this
script) reproduced the puck's proportions faithfully but read thin and
small once shrunk back down to a 40-60pt Home Screen glyph — small
glyphs need a BOLDER stroke-to-length ratio than a large display does to
stay legible, the same reason UI icon sets thicken strokes at small
sizes. So this script keeps the header's ray-fraction TABLE (§1) as the
one shared source of the mark's silhouette, but derives its own
icon-appropriate absolute scale (§2: how much of the tile the longest
ray spans, how thick the stroke is relative to that ray, how big the
center disc is relative to the stroke) — tuned for Home Screen legibility
specifically, the same way the puck's own numbers were tuned for the
puck.

Tools on this machine (checked before writing this script):
    python3 -c "import PIL"   -> Pillow 12.3.0 present
    python3 -c "import numpy" -> not installed; not needed (see the
                                  glow/vignette gradients below — built
                                  from a small float-precision grid then
                                  LANCZOS-upscaled, no numpy required)
    which swift                -> present, but unused: Pillow covers it

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

from PIL import Image, ImageChops, ImageDraw

# ---------------------------------------------------------------------------
# 1. Geometry — parsed from firmware/core/include/ff_flare_mark.h, not
#    retyped. See module doc comment above for why, and for why only the
#    ray-fraction TABLE (n_rays + frac[]) is treated as shared with the
#    puck — max_len/line_w/center_r are parsed too (and printed) but the
#    icon derives its OWN absolute scale from them; see §2.
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
FLARE_HEADER = REPO_ROOT / "firmware/core/include/ff_flare_mark.h"
APPICONSET = REPO_ROOT / "app/Firefly/Resources/Assets.xcassets/AppIcon.appiconset"

BG_COLOR = (0x0B, 0x0B, 0x10, 255)  # FF_THEME_COLOR_BG
AMBER_COLOR = (0xFF, 0xC6, 0x6B, 255)  # FF_THEME_COLOR_AMBER
WHITE = (0xFF, 0xFF, 0xFF, 255)

# Apple's macOS "Big Sur+" icon grid: an 824x824 content square,
# centered, in the 1024x1024 canvas, with a continuous rounded-rect
# ("squircle") shape. 184px corner radius ~= 824 * 0.2237, the ratio
# commonly used for that grid (Apple does not publish an exact number;
# this is the standard approximation every icon-generation tool in
# common use converges on).
MAC_CONTENT_PX = 824
MAC_CORNER_RADIUS_PX = 184

SUPERSAMPLE = 4  # render at 4x canvas size, downsample w/ LANCZOS for AA

# ---------------------------------------------------------------------------
# 2. Icon-specific scale — tuned for Home Screen legibility (PR #280:
#    the header-literal scaling this replaced "reads thin and small"
#    next to other icons at 40-60pt). Independent of the puck's own
#    absolute pixel values; only the ray-fraction table above is shared.
# ---------------------------------------------------------------------------

# The mark's vertical reach — the long north ray's tip-to-center
# distance PLUS the (shorter) south ray's tip-to-center distance, i.e.
# the full top-to-bottom span of the glyph along its dominant axis — as
# a fraction of the 1024 tile. Because the north ray (frac 1.00) is
# much longer than the south ray (frac 0.58), fitting this span
# requires shifting the mark's center DOWN from the tile's geometric
# center so the top and bottom margins come out equal — see
# compute_icon_geometry's anchor_y below ("centre offset so the long
# north ray and the mark's visual centre balance in the tile").
SPAN_FRACTION = 0.72

# Ray stroke width as a fraction of the longest (north) ray's raw
# length. The puck's own ratio (LINE_W_PX / MAX_LEN_PX = 5/42 ≈ 0.119)
# is tuned for a display viewed close up; a small Home Screen glyph
# reads better a touch leaner per-ray so the taper between the 8 rays
# stays legible, hence 9-10% here rather than reusing the puck's ratio.
STROKE_RATIO = 0.095

# Center disc radius as a multiple of the (icon-specific) stroke width
# — "scaled to match" per the task spec. Reuses the puck's own
# disc-to-stroke ratio (CENTER_R_PX / LINE_W_PX = 7/5 = 1.4) so the
# disc grows in step with the bolder stroke instead of looking
# undersized next to it.
CENTER_DISC_RATIO = 1.4

# Soft amber glow behind the colored (non-tinted) mark: low opacity,
# sized off the mark's own vertical span so it scales with the mark
# rather than the canvas. Never applied to the tinted (grayscale mask)
# variant — see main().
GLOW_PEAK_ALPHA = 60          # out of 255; ~24% opacity at the very center
GLOW_DIAMETER_RATIO = 1.2     # multiple of the mark's own vertical span
GLOW_FALLOFF_POWER = 1.8

# Very subtle vignette on the #0B0B10 background so the tile doesn't
# read as flat black. Confined to the already-opaque background region
# (masked by its own alpha) so it never bleeds past the mac squircle's
# rounded corners.
VIGNETTE_STRENGTH = 24        # out of 255; alpha of black at the corners
VIGNETTE_POWER = 2.2


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
        # Parsed + printed for visibility, but NOT used for the icon's
        # own absolute scale — see the §2 comment block above.
        "max_len": scalar("FF_FLARE_MARK_MAX_LEN_PX"),
        "center_r": scalar("FF_FLARE_MARK_CENTER_R_PX"),
        "line_w": scalar("FF_FLARE_MARK_LINE_W_PX"),
    }
    print(
        f"[make_app_icon] parsed {FLARE_HEADER.relative_to(REPO_ROOT)}: "
        f"n_rays={geo['n_rays']} frac={geo['frac']} "
        f"(puck's own max_len={geo['max_len']} center_r={geo['center_r']} "
        f"line_w={geo['line_w']} — reference only, icon uses its own scale)"
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


def compute_icon_geometry(geo: dict, tile_px: float) -> dict:
    """Derive the icon's absolute mark scale (§2 constants) for a
    tile_px x tile_px canvas: the north ray's raw length S (such that
    ray i's raw length is S * frac[i] — frac[0] == 1.0 by construction
    of the table, so S IS the north ray's own length), the stroke
    half-width, the center disc radius, and the (anchor_x, anchor_y)
    center point that balances the mark's bounding box in the tile.

    Generic across the 4 cardinal directions (not hard-coded to index 4
    == south) so a future edit to the ray-fraction table's cardinality
    or ordering doesn't silently miscenter the icon.
    """
    n = geo["n_rays"]
    frac = geo["frac"]
    north_frac = frac[0]
    south_frac = frac[n // 2]
    east_frac = frac[n // 4]
    west_frac = frac[(3 * n) // 4]

    # vertical_span = (S*north_frac + half_stroke) + (S*south_frac + half_stroke)
    #               = S * (north_frac + south_frac + stroke_ratio*north_frac)
    denom = north_frac + south_frac + STROKE_RATIO * north_frac
    span_px = SPAN_FRACTION * tile_px
    S = span_px / denom

    stroke_w = STROKE_RATIO * S * north_frac
    half_stroke = stroke_w / 2.0
    center_r = stroke_w * CENTER_DISC_RATIO

    north_reach = S * north_frac + half_stroke
    south_reach = S * south_frac + half_stroke
    east_reach = S * east_frac + half_stroke
    west_reach = S * west_frac + half_stroke

    anchor_x = tile_px / 2.0 + (west_reach - east_reach) / 2.0
    anchor_y = tile_px / 2.0 + (north_reach - south_reach) / 2.0

    return {
        "S": S,
        "half_stroke": half_stroke,
        "center_r": center_r,
        "anchor_x": anchor_x,
        "anchor_y": anchor_y,
        "north_reach": north_reach,
        "south_reach": south_reach,
        "east_reach": east_reach,
        "west_reach": west_reach,
    }


# ---------------------------------------------------------------------------
# 3. Rendering
# ---------------------------------------------------------------------------


def draw_flare_mark(
    draw: ImageDraw.ImageDraw,
    cx: float,
    cy: float,
    S: float,
    half_stroke: float,
    center_r: float,
    geo: dict,
    color: tuple,
) -> None:
    for i in range(geo["n_rays"]):
        dx, dy = ray_offset(i, S, geo)
        ex, ey = cx + dx, cy + dy
        draw.line([(cx, cy), (ex, ey)], fill=color, width=max(1, round(half_stroke * 2)))
        # Round cap at the ray tip — the center-end cap is already
        # covered by the filled center dot drawn below (center_r >
        # half_stroke by construction, same relationship the puck's own
        # flare_mark_pixel_hit relies on).
        draw.ellipse(
            [ex - half_stroke, ey - half_stroke, ex + half_stroke, ey + half_stroke],
            fill=color,
        )

    draw.ellipse([cx - center_r, cy - center_r, cx + center_r, cy + center_r], fill=color)


def _radial_alpha_grid(base: int, peak_alpha: int, falloff_power: float, corner_norm: bool) -> Image.Image:
    """A small "L" (grayscale/alpha) image with a smooth radial falloff,
    computed per-pixel in float (no discrete banding steps). Callers
    resize() this up with LANCZOS to the size they actually need —
    resampling a smooth source stays smooth, which is what keeps the
    glow/vignette band-free without numpy.

    corner_norm=True normalizes by the distance to the CORNER (so alpha
    reaches `peak_alpha` only right at the corners — used for the
    vignette); corner_norm=False normalizes by the distance to the
    nearest EDGE (so alpha reaches 0 at the edge of the circle — used
    for the glow).
    """
    grad = Image.new("L", (base, base), 0)
    px = grad.load()
    cx = cy = (base - 1) / 2.0
    maxr = math.hypot(cx, cy) if corner_norm else base / 2.0
    for y in range(base):
        for x in range(base):
            r = math.hypot(x - cx, y - cy) / maxr
            if r >= 1.0:
                a = 1.0 if corner_norm else 0.0
            else:
                a = r if corner_norm else (1.0 - r)
            a = a ** falloff_power
            px[x, y] = int(a * peak_alpha)
    return grad


def apply_vignette(img: Image.Image, ss: int) -> Image.Image:
    """Very subtle radial darkening from center to edge, masked to the
    image's OWN existing opaque region (so it can never bleed past a
    mac squircle's rounded corners) via ImageChops.multiply against the
    current alpha channel."""
    base = 128
    grad = _radial_alpha_grid(base, VIGNETTE_STRENGTH, VIGNETTE_POWER, corner_norm=True)
    grad = grad.resize((ss, ss), Image.LANCZOS)

    mask = img.split()[3]
    combined = ImageChops.multiply(grad, mask)  # zero outside the existing bg shape
    zero = Image.new("L", (ss, ss), 0)
    overlay = Image.merge("RGBA", (zero, zero, zero, combined))
    return Image.alpha_composite(img, overlay)


def apply_glow(img: Image.Image, ss: int, geom: dict, color: tuple) -> Image.Image:
    """Soft amber radial glow centered on the tile's own center (which
    IS the mark's balanced bounding-box center post anchor-offset —
    see compute_icon_geometry), sized off the mark's own vertical span
    so it scales with the mark rather than a fixed canvas fraction.
    Masked to the existing opaque bg region for the same reason as the
    vignette."""
    span = geom["north_reach"] + geom["south_reach"]
    # Clamped to the canvas so the composite below always gets a
    # non-negative destination offset (also keeps the glow from being
    # wastefully larger than the tile it's drawn on).
    diameter = min(max(int(span * GLOW_DIAMETER_RATIO), 2), ss)

    base = 160
    grad = _radial_alpha_grid(base, GLOW_PEAK_ALPHA, GLOW_FALLOFF_POWER, corner_norm=False)
    grad = grad.resize((diameter, diameter), Image.LANCZOS)

    glow = Image.new("RGBA", (diameter, diameter), color[:3] + (0,))
    glow.putalpha(grad)

    layer = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    px = int(round(ss / 2 - diameter / 2))
    py = int(round(ss / 2 - diameter / 2))
    # NOTE: Image.paste(src, box, mask=src) would use src's own alpha
    # band as BOTH the blend source and the mask, squaring the visible
    # opacity (a real bug caught during PR #280 tuning — the glow
    # rendered almost invisibly faint until this was fixed). alpha_composite
    # is the correct Porter-Duff "over" op for compositing an RGBA image
    # to a fully-transparent destination.
    layer.alpha_composite(glow, dest=(px, py))

    mask = img.split()[3]
    layer_alpha = ImageChops.multiply(layer.split()[3], mask)
    layer.putalpha(layer_alpha)
    return Image.alpha_composite(img, layer)


def render_master(
    *,
    size: int,
    geo: dict,
    mark_color: tuple,
    bg_color: tuple | None,
    squircle: bool,
    glow: bool,
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

    if bg_color is not None:
        img = apply_vignette(img, ss)

    geom = compute_icon_geometry(geo, tile_px=ss)

    if glow:
        img = apply_glow(img, ss, geom, AMBER_COLOR)

    draw = ImageDraw.Draw(img)  # re-acquire after compositing vignette/glow layers
    draw_flare_mark(
        draw,
        geom["anchor_x"],
        geom["anchor_y"],
        geom["S"],
        geom["half_stroke"],
        geom["center_r"],
        geo,
        mark_color,
    )

    return img.resize((size, size), Image.LANCZOS)


# ---------------------------------------------------------------------------
# 4. Catalog assembly
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
        # one and tints the mark. No glow either: a flat mask reads
        # cleanest once the system re-tints and re-lights it.
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
    ios_any = render_master(
        size=1024, geo=geo, mark_color=AMBER_COLOR, bg_color=BG_COLOR, squircle=False, glow=True
    )
    ios_any.save(APPICONSET / "icon-1024.png", optimize=True)
    # Dark == same artwork, per spec.
    ios_any.save(APPICONSET / "icon-1024-dark.png", optimize=True)

    ios_tinted = render_master(
        size=1024, geo=geo, mark_color=WHITE, bg_color=None, squircle=False, glow=False
    )
    ios_tinted.save(APPICONSET / "icon-1024-tinted.png", optimize=True)

    # --- macOS: 824px rounded-rect content square in a 1024 canvas ---
    mac_master = render_master(
        size=1024, geo=geo, mark_color=AMBER_COLOR, bg_color=BG_COLOR, squircle=True, glow=True
    )
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
