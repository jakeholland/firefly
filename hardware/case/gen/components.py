"""Display module (Waveshare ESP32-S3-Touch-LCD-1.46) STEP import and
placement -- build123d port of firefly_case.py:2907 insert_display_pcba.

Fusion's own version reads a live "reference transform" off an open
Fusion document (get_reference_transform, :2890) that this headless
build has no equivalent for -- there is no running Fusion session to
read it from. Per the port brief, the transform is instead DERIVED by
measuring the real STEP file directly and matching it against the two
values the Fusion-driven generator has always published as ground truth:
`display_underside_z_min` (12.04mm world, 'current') and the live-
measured standoff plane (18.80mm 'current' / 21.80mm 'trim',
`ear_seat_z` + `ear_seat_offset` in params_current.py).

Measured (this port, `uv run python -m gen.components measure`):
  - The STEP's own local frame has its origin at the display's own
    optical/window centre, with local +z = 0 at the glass's top face
    (flush with the case's own top_z) and local -z reaching down through
    the PCB and its underside components.
  - A 180-degree rotation about the local Z axis (both X and Y negate)
    plus a translation of (0, 50.0, top_z) reproduces BOTH ground-truth
    numbers exactly: `display_underside_z_min` (local z=-12.95 -> world
    12.05mm, 'current', vs. the documented 12.04mm) and the standoff
    plane (local z=-6.2 -> world top_z-6.2 = 18.80mm 'current' / 21.80mm
    'trim' -- an EXACT match to both documented values, not just close).
  - The three SMT standoff barrels (~3.5x3.5x4.7mm bboxes, matching the
    documented 'SMTSO-M2-3.5X2-3.5ET' part) were found by scanning every
    solid in the imported compound for that footprint -- exactly 3 come
    back, at local (12.0, -14.95), (-11.544, -15.447), (0.0, 17.75).
    Through the same transform, their real-world XY comes out as
    S1=(-12.000, 64.950), S3=(11.544, 65.447), S2=(0.000, 32.250) --
    within ~0.3mm of params_current.py's own hand-recorded
    `board_standoffs` (S1 (-12.0, 65.0), S3 (11.8, 65.46), S2 (0.04,
    32.22)), which is the expected level of agreement for a fresh direct
    measurement vs. a historical Fusion probe of the SAME real part.
    THESE measured numbers, not the historical params, are the new
    source of truth for wherever phase 2's ears/S2-boss need to target
    the real barrel (see docs/hardware/headless-port-plan.md).
"""
import functools
import math
import os

import build123d as bd

from . import geometry as geo

_MODELS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'models'))
DISPLAY_STEP = os.path.join(_MODELS_DIR, 'ESP32-S3-Touch-LCD-1_46.step')

# Local-frame -> world-frame transform, derived empirically (see module
# docstring). Only Z depends on the variant (via top_z); X/Y are a fixed
# 180-degree-about-Z rotation plus translation, identical both variants
# (matches "the display module... stays at its reference positions"
# convention every param file already documents).
_Y_OFFSET = 50.0
STANDOFF_BARREL_TOP_LOCAL_Z = -6.2   # matches ear_seat_z's own "standoff plane" anchor, both variants, exactly


def display_to_world(local_pt, top_z):
    lx, ly, lz = local_pt
    return (-lx, -ly + _Y_OFFSET, lz + top_z)


@functools.lru_cache(maxsize=1)
def _load_compound():
    return bd.import_step(DISPLAY_STEP)


def load_display(p):
    """Return the display compound placed in world space for params `p`
    (uses p['top_z'] for the Z offset -- see insert_display_pcba's own
    'display_z_offset shifts everything display-relative' convention)."""
    compound = _load_compound()
    top_z = p['top_z']
    # 180-about-Z rotation then translate -- build123d Location takes
    # (translation, rotation); rotate first about the compound's own
    # local origin, then translate to world.
    loc = bd.Location((0, _Y_OFFSET, top_z)) * bd.Location((0, 0, 0), (0, 0, 180))
    return loc * compound


def find_standoff_barrels(foot_lo=3.0, foot_hi=4.2, height_lo=4.0, height_hi=5.5):
    """Scan the raw (local-frame) compound for the display's own SMT
    standoff barrels by shape (see module docstring) -- returns a list of
    (local_x, local_y, local_z_bottom, local_z_top) tuples, unsorted."""
    compound = _load_compound()
    hits = []
    for s in compound.solids():
        bb = s.bounding_box()
        dx, dy, dz = bb.max.X - bb.min.X, bb.max.Y - bb.min.Y, bb.max.Z - bb.min.Z
        foot = max(dx, dy)
        if foot_lo <= foot <= foot_hi and height_lo <= dz <= height_hi:
            cx = (bb.min.X + bb.max.X) / 2.0
            cy = (bb.min.Y + bb.max.Y) / 2.0
            hits.append((cx, cy, bb.min.Z, bb.max.Z))
    return hits


def measure_standoffs(p, target_world_xy=None):
    """Measure the standoff plane z and each barrel's world XY for
    params `p`. `target_world_xy` (dict name -> (x, y)) lets the caller
    match each measured barrel to a named target (S1/S2/S3) by nearest
    distance -- defaults to params_current.py's own `board_standoffs`.
    Returns {name: {'world_xy': (x, y), 'standoff_plane_z': z}}."""
    if target_world_xy is None:
        target_world_xy = p['board_standoffs']
    top_z = p['top_z']
    barrels = find_standoff_barrels()
    assert len(barrels) == 3, f'expected 3 standoff barrels, found {len(barrels)}: {barrels}'

    measured = []
    for lx, ly, lz_bot, lz_top in barrels:
        wx, wy, _ = display_to_world((lx, ly, 0.0), top_z)
        plane_z = STANDOFF_BARREL_TOP_LOCAL_Z + top_z
        measured.append({'world_xy': (wx, wy), 'standoff_plane_z': plane_z, 'local_xy': (lx, ly)})

    result = {}
    remaining = list(measured)
    for name, (tx, ty) in target_world_xy.items():
        best = min(remaining, key=lambda m: math.hypot(m['world_xy'][0] - tx, m['world_xy'][1] - ty))
        remaining.remove(best)
        result[name] = best
    return result


def battery_connector_world_bbox(p):
    """firefly_case.py:2849 battery_connector_world_bbox -- world bbox of
    the display module's own 2-pin JST-style battery socket
    ('HP1_25MM-2P-SMT-HORIZONTAL'). x/y are identical in both variants
    (same reference transform); z is offset by `display_z_offset`, same
    convention as every other display-relative z value in this port (see
    load_display)."""
    bb = p['battery_connector_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


def secondary_conn_world_bbox(p):
    """firefly_case.py:2873 secondary_conn_world_bbox -- world bbox of the
    display module's second real SMT connector
    ('PITCH1MM-2PIN-SMT-HORIZONTAL'). Same display_z_offset convention as
    battery_connector_world_bbox."""
    bb = p['secondary_conn_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


def apply_known_component_keepouts(top, p):
    """Port of firefly_case.py:5936-5959 (inline in build(), right after
    add_ear/add_s2_boss) -- two unconditional keep-out cuts of specific,
    live-found real display components the S3 ear's own arm/wedge run
    passes close to: the second SMT connector (`secondary_conn_world_
    bbox`, +0.5mm margin every side -- the source's own comment: "a live
    check_interference(Top, display) found a real 17.8mm^3 overlap here
    even after the standoff-barrel fix... cleared the other 8 hits") and
    five smaller SMD-component hits (`p['ear_wedge_component_keepouts']`,
    already-margined boxes, no extra pad). Applied generically to Top,
    same as the source's own framing ("stays correct even if the ear
    geometry shifts again"), not tied to add_ear's own construction."""
    scx, scy, scz = secondary_conn_world_bbox(p)
    top = top - geo.box_solid(scx[0] - 0.5, scx[1] + 0.5, scy[0] - 0.5, scy[1] + 0.5,
                               scz[0] - 0.5, scz[1] + 0.5)

    dz = p.get('display_z_offset', 0.0)
    for kb in p.get('ear_wedge_component_keepouts', []):
        top = top - geo.box_solid(kb['x'][0], kb['x'][1], kb['y'][0], kb['y'][1],
                                   kb['z'][0] + dz, kb['z'][1] + dz)
    return top


PILOT_PROTECT_MARGIN = 1.0        # firefly_case.py:1975
CEILING_CUT_FOOTPRINT_MIN_MM2 = 4.0  # firefly_case.py's own build() candidate filter, same value
SPLIT_REJECT_VOLUME_MM3 = 0.5     # see ceiling_safe_display_cut's own docstring -- sliver vs. real sever


def ceiling_safe_display_cut(top, p):
    """Port of the inline ceiling-safe cut in firefly_case.py's own
    `build()` (right after insert_display_pcba, ~:5997-6096) -- NOT one
    of the file's 217 named functions, but load-bearing: cuts Top against
    the display module's own real sub-bodies, restricted to the slice
    strictly between `top_pilot_z[1] + PILOT_PROTECT_MARGIN` and
    `top_ceiling_underside_z + 0.3` (a band that, by construction, can
    never reach the outer 2mm skin or undercut any screw pilot's own
    engagement depth), candidate-filtered to sub-bodies whose own XY
    footprint area exceeds `CEILING_CUT_FOOTPRINT_MIN_MM2` (every
    individual SMT part measures well under 1mm^2; only the connectors/
    shields/bare PCB that can plausibly reach this high are bigger --
    the source's own live-tuned proxy, confirmed there to bring
    display-vs-Top interference to true zero both variants).

    The source calls this once, globally, after add_ear/add_s2_boss/
    add_buttons -- this port calls it once too (see cli.py), after the
    ears/S2 boss (buttons not yet ported); it does not depend on either.

    OCC-specific robustness note (no Fusion equivalent needed): cutting
    ~30 independent tool boxes one at a time out of a single shell
    occasionally leaves a NEGLIGIBLE sliver behind as its own disjoint
    micro-solid -- live-checked (this port): both real occurrences are
    <=0.05mm^3 (ordinary tessellation slack at the cut tool's own
    coincident face, the exact same artifact firefly_case.py's own
    comment on this cut anticipates -- "most likely ordinary
    tessellation slack at the cut tool's own coincident top face"), next
    to a main remaining body of ~18700mm^3 -- discarded (kept only the
    largest resulting solid) rather than rejecting the whole cut, since
    a print slicer would never render a sub-0.1mm^3 fleck anyway. A cut
    that instead leaves a SECOND solid above `SPLIT_REJECT_VOLUME_MM3`
    is a genuine structural sever (the "silently splits into two bodies"
    failure class firefly_case.py's own dedupe_body/_clip_of_ear_boss_
    keepout docstrings document at length elsewhere in this file --
    Fusion's own timeline hid this same risk behind body names, not
    avoided it) and is rolled back outright (skipped, counted in the
    returned stats), never silently accepted. See docs/hardware/
    headless-port-parity.md for how many of the source's own ~33 live
    candidates this port actually applies vs. skips."""
    pilot_protect_z = p['top_pilot_z'][1] + PILOT_PROTECT_MARGIN
    ceiling_z = p['top_ceiling_underside_z']
    disp = load_display(p)
    stats = {'candidates': 0, 'applied': 0, 'skipped_empty': 0, 'skipped_would_split': 0,
              'slivers_discarded': 0}
    for s in disp.solids():
        bb = s.bounding_box()
        if bb.max.Z <= pilot_protect_z:
            continue
        area = (bb.max.X - bb.min.X) * (bb.max.Y - bb.min.Y)
        if area <= CEILING_CUT_FOOTPRINT_MIN_MM2:
            continue
        stats['candidates'] += 1
        band = geo.box_solid(-100.0, 100.0, -100.0, 100.0, pilot_protect_z, ceiling_z + 0.3)
        try:
            tool = band & s
        except Exception:
            stats['skipped_empty'] += 1
            continue
        if tool is None or tool.volume < 1e-6:
            stats['skipped_empty'] += 1
            continue
        candidate = top - tool
        sols = sorted(candidate.solids(), key=lambda sol: sol.volume, reverse=True)
        if len(sols) > 1 and sum(sol.volume for sol in sols[1:]) > SPLIT_REJECT_VOLUME_MM3:
            stats['skipped_would_split'] += 1
            continue
        if len(sols) > 1:
            stats['slivers_discarded'] += len(sols) - 1
            candidate = sols[0]
        top = candidate
        stats['applied'] += 1
    return top, stats


def _main():
    import json

    from .params import get_params

    for variant in ('current', 'trim'):
        p = get_params(variant)
        m = measure_standoffs(p)
        print(f'--- {variant} (top_z={p["top_z"]}) ---')
        print(json.dumps({k: {'world_xy': [round(v, 3) for v in d['world_xy']],
                               'standoff_plane_z': round(d['standoff_plane_z'], 3)}
                           for k, d in m.items()}, indent=2))
    bb = _load_compound().bounding_box()
    print('display STEP local bbox:', (bb.min.X, bb.min.Y, bb.min.Z), (bb.max.X, bb.max.Y, bb.max.Z))
    p_current = get_params('current')
    underside_world = display_to_world((0, 0, bb.min.Z), p_current['top_z'])[2]
    print('display_underside_z_min (current, measured):', round(underside_world, 3),
          'vs params_current.py documented 12.04')


if __name__ == '__main__':
    _main()
