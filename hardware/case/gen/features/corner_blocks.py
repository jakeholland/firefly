"""Case screws A/C/D -- build123d port of firefly_case.py's
add_case_boss (:2702, Bottom-side boss + counterbore),
add_single_corner_block (:2293, Top-side wall-anchored wedge block), and
add_root_reinforcement (:1636, the 45-degree conical collar every
post/boss root gets). add_case_screws (:2813) is the driver, ported here
as `add_case_screws`.

Deleted-as-quirk (see docs/hardware/headless-port-plan.md): `dedupe_body`/
`_refetch_by_name` (OCC's `+`/`-` operators never leave a stale same-named
duplicate the way Fusion's timeline-based Combine features did -- there
is no "name" to go stale), and the collar-vs-wedge non-manifold-edge
workaround's OWN Fusion-specific symptom (`CORNER_BLOCK_WEDGE_OVERLAP`
grows the wedge past its tangent point with the capsule -- this IS kept,
since it is real geometry the design needs regardless of kernel, not a
Fusion quirk; only the trim-then-join collar fix and the wedge-overlap
constant are ported, not any retry/refetch scaffolding around them).
"""
import math

import build123d as bd

from .. import geometry as geo

ROOT_FILLET_R = 1.8              # firefly_case.py:1573 (unused directly -- see add_root_reinforcement)
ROOT_COLLAR_RISE = 1.5           # firefly_case.py:1574
ROOT_COLLAR_OVERLAP = 0.05       # firefly_case.py:1577
ROOT_COLLAR_TRIM_MARGIN = 0.2    # firefly_case.py:1579
BOSS_CORE_R = 2.6                # firefly_case.py:1591

CORNER_BLOCK_PAD = 0.0
CORNER_BLOCK_WEDGE_OVERLAP = 0.5
CORNER_BLOCK_REACH = 10.0
CORNER_BLOCK_RING_CLEARANCE = 0.5
CORNER_BLOCK_STACK_MARGIN = 0.8

DISPLAY_KEEPOUT_CLEARANCE = 0.5  # firefly_case.py:1938


def add_root_reinforcement(body, cx, cy, r, z_root, direction,
                            collar_rise=ROOT_COLLAR_RISE, z_floor=None):
    """Port of add_root_reinforcement -- the fillet-first/collar-fallback
    path (superseded in the real generator itself, pass 13) is not
    ported; only the shipped, unconditional-collar design is. Trims the
    body within the collar's own radial footprint over EXACTLY the
    collar's own z-band before joining the collar (pass-16 item-1 fix,
    closes the wedge/collar non-manifold seam).

    `z_floor` (phase-2 addition, no firefly_case.py equivalent needed --
    see below): every A/C/D corner block's own `z_root` sits comfortably
    more than `collar_rise` above `split_z`, so a direction='up' collar
    never had reason to reach below the Top/Bottom parting plane there.
    The S1/S3 ears' own wall-root is NOT so comfortable -- `ear_root_
    cap_z1` can cap `z_root` (button-height-limited, see features/ears.py)
    to as little as ~1mm above `split_z`, and an uncapped `collar_rise`
    (1.5mm) then dips the collar 0.5mm below the parting plane, into
    Bottom's own territory -- live-found this port (a real, if tiny,
    0.126mm^3 Top-vs-Bottom overlap for the 'current' variant's S3 ear).
    `z_floor` clamps the collar's low end at that plane, recomputing the
    matching radius by linear interpolation along the SAME cone surface
    (not a flat truncation) so the collar's own taper angle is
    unchanged, just shorter. A no-op for every existing call site
    (z_floor=None) -- only ears.py passes one."""
    if direction == 'up':
        z_lo, z_hi = z_root - collar_rise, z_root + ROOT_COLLAR_OVERLAP
        r_lo, r_hi = r + ROOT_COLLAR_OVERLAP, r + collar_rise
        if z_floor is not None and z_lo < z_floor < z_hi:
            t = (z_floor - z_lo) / (z_hi - z_lo)
            r_lo = r_lo + t * (r_hi - r_lo)
            z_lo = z_floor
    else:
        z_lo, z_hi = z_root - ROOT_COLLAR_OVERLAP, z_root + collar_rise
        r_lo, r_hi = r + collar_rise, r + ROOT_COLLAR_OVERLAP

    trim_r = max(r_lo, r_hi) + ROOT_COLLAR_TRIM_MARGIN
    trim_z0, trim_z1 = min(z_lo, z_hi), max(z_lo, z_hi)
    trim_cut = geo.cylinder_solid(cx, cy, trim_r, trim_z0, trim_z1)
    body = body - trim_cut

    collar = geo.cone_frustum_solid(cx, cy, r_lo, r_hi, z_lo, z_hi)
    return body + collar


def _xy_overlap(bx0, bx1, by0, by1, cx0, cx1, cy0, cy1):
    """firefly_case.py:8300 _xy_overlap."""
    ox0, ox1 = max(bx0, cx0), min(bx1, cx1)
    oy0, oy1 = max(by0, cy0), min(by1, cy1)
    if ox0 < ox1 and oy0 < oy1:
        return (ox0, ox1, oy0, oy1)
    return None


def _ear_root_z1(p, cx, cy, r, z1_nominal):
    """firefly_case.py:2137 _ear_root_z1 -- caps a wall-anchored member's
    own z1 below the real display module's housing underside wherever its
    own XY footprint overlaps the display's real XY bbox."""
    db = p['display_bbox']
    dz = p.get('display_z_offset', 0.0)
    overlap = _xy_overlap(cx - r, cx + r, cy - r, cy + r, db['x'][0], db['x'][1], db['y'][0], db['y'][1])
    if overlap is None:
        return z1_nominal
    display_z0 = db['z'][0] + dz
    return min(z1_nominal, display_z0 - DISPLAY_KEEPOUT_CLEARANCE)


def _corner_block_ring_limit_r(p, cx, cy):
    """firefly_case.py:2232 _corner_block_ring_limit_r."""
    boss_r = p['boss_dia'] / 2.0
    nearer_y = geo._nearer_spine_y(p, cy)
    own_reach = math.hypot(cx, cy - nearer_y) + boss_r + CORNER_BLOCK_REACH + 2.5
    return max(p['lip_r'][0] - CORNER_BLOCK_RING_CLEARANCE, own_reach)


def add_single_corner_block(bodies, p, screw):
    """Port of add_single_corner_block (:2293) -- a plain cylinder at the
    screw's own centre unioned with an oversized outward wedge reaching
    toward the true dome wall, clipped by the inner cavity + the lip/
    anchor ring-clearance cylinder + the L76K stack keep-out, plus a
    full-height core and the unconditional root-reinforcement collar."""
    cx, cy = screw['xy']
    boss_r = p['boss_dia'] / 2.0
    z0, z1_nominal = p['split_z'], p['top_ceiling_underside_z']
    axis1, axis2 = geo.wall_outward_axes(p, cx, cy)
    z1 = _ear_root_z1(p, cx, cy, boss_r + CORNER_BLOCK_REACH, z1_nominal)

    capsule = geo.cylinder_solid(cx, cy, boss_r, z0, z1)
    wedge_len = CORNER_BLOCK_REACH + CORNER_BLOCK_WEDGE_OVERLAP
    wedge_offset = boss_r + CORNER_BLOCK_REACH / 2.0 - CORNER_BLOCK_WEDGE_OVERLAP / 2.0
    wedge_center = (cx + axis2[0] * wedge_offset, cy + axis2[1] * wedge_offset, z0)
    wedge = geo.oriented_box_prism(wedge_center, axis1, axis2, (0.0, 0.0, 1.0),
                                    2.0 * boss_r, wedge_len, z1 - z0)
    wide = capsule + wedge
    wide = wide & (geo.build_inner_cavity_clip_tool(p))

    ring_limit_r = _corner_block_ring_limit_r(p, cx, cy)
    ring_limit = geo.cylinder_solid(0.0, geo._nearer_spine_y(p, cy), ring_limit_r, z0 - 1.0, z1 + 1.0)
    wide = wide & ring_limit

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        m = CORNER_BLOCK_STACK_MARGIN
        stack_keepout = geo.box_solid(pcb['x'][0] - m, pcb['x'][1] + m, pcb['y'][0] - m, pcb['y'][1] + m,
                                       z0 - 0.5, z1 + 0.5)
        wide = wide - stack_keepout

    core = geo.cylinder_solid(cx, cy, BOSS_CORE_R, z0, z1)
    block = wide + core

    top = bodies['Top'] + block

    pilot = geo.cylinder_solid(cx, cy, p['top_pilot_dia'] / 2.0, p['top_pilot_z'][0], p['top_pilot_z'][1])
    top = top - pilot

    top = add_root_reinforcement(top, cx, cy, boss_r, z1, direction='up')
    # pass-15 lesson: re-cut the pilot AFTER the collar joins (a
    # solid-to-the-axis collar can silently replug a hole in its own band).
    top = top - pilot

    bodies['Top'] = top
    return bodies


def add_case_boss(bodies, cx, cy, p, core_r=None):
    """Port of add_case_boss's Bottom-side half (build_top=False; the
    Top-side pilot lives in add_single_corner_block, per pass 16)."""
    boss_r = p['boss_dia'] / 2.0
    if core_r is None:
        core_r = BOSS_CORE_R

    clip_tool = geo.build_inner_cavity_clip_tool(p)
    wide = geo.cylinder_solid(cx, cy, boss_r, 2.0, p['split_z'])
    wide_clipped = wide & clip_tool
    core = geo.cylinder_solid(cx, cy, core_r, 2.0, p['split_z'])
    bottom_boss = wide_clipped + core

    bottom = bodies['Bottom'] + bottom_boss

    hole = geo.cylinder_solid(cx, cy, p['screw_hole_dia'] / 2.0, -0.5, p['split_z'] + 0.5)
    bottom = bottom - hole
    cb_h = p['counterbore_ABC_h']
    cb = geo.cylinder_solid(cx, cy, p['counterbore_ABC_dia'] / 2.0, -0.5, cb_h)
    bottom = bottom - cb

    bottom = add_root_reinforcement(bottom, cx, cy, boss_r, 2.0, direction='down')

    # pass-15 item 8: re-cut hole+counterbore after the collar join (the
    # collar's own solid-to-axis revolve silently replugs both otherwise).
    bottom = bottom - hole
    bottom = bottom - cb

    bodies['Bottom'] = bottom
    return bodies


def add_case_screws(bodies, p):
    """Port of add_case_screws (:2813, pass 16): every Bottom-side boss
    (A, C, D) built identically, then every Top-side pilot lives inside
    its own independent single-pilot corner block."""
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        bodies = add_case_boss(bodies, cx, cy, p)
    for s in p['screws_ABC'] + p['screws_D12']:
        bodies = add_single_corner_block(bodies, p, s)
    return bodies
