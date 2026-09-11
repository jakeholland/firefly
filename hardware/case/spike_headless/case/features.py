"""Corner-block feature -- build123d port of firefly_case.py:1611
add_lanyard_corner_block (the 'capsule + outward wedge + conical root
collar + two M2 pilots' construction), instantiated twice for this spike:

  1. the real lanyard-end corner block (side=-1, screws A+B1, pilots at
     PARAMS['top_pilot_z'] = (10.0, 19.1)) -- item 2 of the spike brief.

  2. a second instance at a different corner-screw pair (C+B2, side=+1)
     standing in for the brief's 'candidate-5 ear' -- see NOTE below for
     why this substitution was made, spelled out again in the spike doc.

NOTE on the 'candidate-5 ear' substitution: firefly_case.py has no
function or comment named 'candidate-5', and no feature seats at exactly
z=21.55 with a Ø1.62 pilot (top_post_pilot_z is (14.1, 20.6), top_pilot_z
is (10.0, 19.1) -- neither matches). The nearest real match to the
brief's own description ('capsule + wedge into the dome wall + collar +
seat ... with a Ø1.62 pilot') is this exact add_lanyard_corner_block
construction, which the brief's item 2 already assigns to the lanyard
end. Rather than block the spike on an apparently-nonexistent name, this
second instance reuses the identical technique at the OTHER corner pair
(C+B2) -- which is what the spike actually needs to test (does OCC
handle two independent instances of this compound boolean -- stadium
capsule + oriented wedge + inner-cavity clip + ring-radius clip +
stack-footprint cut + two full-height cores + two conical collars + two
pilot cuts -- as cleanly and as fast as Fusion does), even though the
exact 'ear' name/z-seat couldn't be located.
"""
import math

import build123d as bd

from . import shell


ROOT_FILLET_R = 1.8         # firefly_case.py:1338
ROOT_COLLAR_RISE = 1.5      # firefly_case.py:1339
ROOT_COLLAR_OVERLAP = 0.05  # firefly_case.py:1342
BOSS_CORE_R = 2.6           # firefly_case.py:1531
CORNER_BLOCK_PAD = 0.0            # firefly_case.py:1582
CORNER_BLOCK_REACH = 10.0         # firefly_case.py:1583
CORNER_BLOCK_RING_CLEARANCE = 0.5  # firefly_case.py:1588
CORNER_BLOCK_STACK_MARGIN = 0.8    # firefly_case.py:1592


def build_inner_cavity_clip_tool(p, safety_margin=0.1):
    """Port of firefly_case.py:630 build_inner_cavity_clip_tool."""
    inner = shell.build_inner_pill_solid(p)
    return bd.offset(inner, amount=-safety_margin)


def _oriented_stadium_solid(center, axis1, axis2, normal, L, W, depth):
    """Port of firefly_case.py:322 oriented_stadium_prism -- built
    directly in 3D (see shell.py's module docstring for why build123d
    doesn't need Fusion's canonical-then-move two-step here)."""
    r = W / 2.0
    half = max(L / 2.0 - r, 0.0)

    def pt(s1, s2):
        return (center[0] + s1 * axis1[0] + s2 * axis2[0],
                center[1] + s1 * axis1[1] + s2 * axis2[1],
                center[2] + s1 * axis1[2] + s2 * axis2[2])

    p_tr, p_br = pt(half, r), pt(half, -r)
    p_tl, p_bl = pt(-half, r), pt(-half, -r)
    p_right, p_left = pt(half + r, 0.0), pt(-half - r, 0.0)
    segs = [
        bd.Line(p_tl, p_tr),
        bd.ThreePointArc(p_tr, p_right, p_br),
        bd.Line(p_br, p_bl),
        bd.ThreePointArc(p_bl, p_left, p_tl),
    ]
    face = bd.make_face(segs)
    return bd.extrude(face, amount=depth, dir=normal)


def _oriented_box_solid(center, axis1, axis2, normal, L, W, depth):
    """Port of firefly_case.py:350 oriented_box_prism."""
    hl, hw = L / 2.0, W / 2.0

    def pt(s1, s2):
        return (center[0] + s1 * axis1[0] + s2 * axis2[0],
                center[1] + s1 * axis1[1] + s2 * axis2[1],
                center[2] + s1 * axis1[2] + s2 * axis2[2])

    pts = [pt(-hl, -hw), pt(hl, -hw), pt(hl, hw), pt(-hl, hw)]
    segs = [bd.Line(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    face = bd.make_face(segs)
    return bd.extrude(face, amount=depth, dir=normal)


def _cone_frustum(cx, cy, r_lo, r_hi, z_lo, z_hi):
    return shell._cone_frustum(cx, cy, r_lo, r_hi, z_lo, z_hi)


def add_root_reinforcement(body, cx, cy, r, z_root, direction):
    """Port of firefly_case.py:1397 add_root_reinforcement: a plain
    unconditional conical collar (the version that shipped -- the file's
    own docstring explains why a real Fillet feature was dropped in favor
    of this)."""
    collar_rise = ROOT_COLLAR_RISE
    if direction == 'up':
        z_lo, z_hi = z_root - collar_rise, z_root + ROOT_COLLAR_OVERLAP
        r_lo, r_hi = r + ROOT_COLLAR_OVERLAP, r + collar_rise
    else:
        z_lo, z_hi = z_root - ROOT_COLLAR_OVERLAP, z_root + collar_rise
        r_lo, r_hi = r + collar_rise, r + ROOT_COLLAR_OVERLAP
    collar = _cone_frustum(cx, cy, r_lo, r_hi, z_lo, z_hi)
    return body + collar


def add_corner_block(p, top, near_xy, far_xy, inner_clip_tool):
    """Port of firefly_case.py:1611 add_lanyard_corner_block for one
    (near, far) screw-centre pair. Returns the updated `top` solid."""
    (nx, ny), (fx, fy) = near_xy, far_xy
    boss_r = p['boss_dia'] / 2.0
    cap_r = boss_r + CORNER_BLOCK_PAD
    z0, z1 = p['split_z'], p['top_ceiling_underside_z']
    ay = p['spine_a'][1]

    dx, dy = fx - nx, fy - ny
    seg_len = math.hypot(dx, dy)
    axis1 = (dx / seg_len, dy / seg_len, 0.0)
    axis2 = (-axis1[1], axis1[0], 0.0)
    mx, my = (nx + fx) / 2.0, (ny + fy) / 2.0
    # orient axis2 OUTWARD (away from spine_a), matching the live dot-
    # product check in firefly_case.py rather than assuming a sign.
    if (axis2[0] * mx + axis2[1] * (my - ay)) < 0:
        axis2 = (-axis2[0], -axis2[1], -axis2[2])

    capsule = _oriented_stadium_solid(
        (mx, my, z0), axis1, axis2, (0.0, 0.0, 1.0), seg_len + 2.0 * cap_r, 2.0 * cap_r, z1 - z0)

    wedge_offset = cap_r + CORNER_BLOCK_REACH / 2.0
    wedge_center = (mx + axis2[0] * wedge_offset, my + axis2[1] * wedge_offset, z0)
    wedge = _oriented_box_solid(
        wedge_center, axis1, axis2, (0.0, 0.0, 1.0), seg_len + 2.0 * cap_r, CORNER_BLOCK_REACH, z1 - z0)

    wide = capsule + wedge
    wide = wide & inner_clip_tool

    ring_limit_r = p['lip_r'][0] - CORNER_BLOCK_RING_CLEARANCE
    ring_limit = bd.Pos(0, ay, z0 - 1.0) * bd.Cylinder(
        radius=ring_limit_r, height=(z1 - z0) + 2.0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
    wide = wide & ring_limit

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        m = CORNER_BLOCK_STACK_MARGIN
        x0, x1 = pcb['x'][0] - m, pcb['x'][1] + m
        y0, y1 = pcb['y'][0] - m, pcb['y'][1] + m
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        keepout = bd.Pos(cx, cy, z0 - 0.5) * bd.Box(x1 - x0, y1 - y0, (z1 - z0) + 1.0)
        wide = wide - keepout

    core_n = bd.Pos(nx, ny, z0) * bd.Cylinder(
        radius=BOSS_CORE_R, height=z1 - z0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
    core_f = bd.Pos(fx, fy, z0) * bd.Cylinder(
        radius=BOSS_CORE_R, height=z1 - z0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
    block = wide + core_n + core_f

    top = top + block

    pilot_r = p['top_pilot_dia'] / 2.0
    pz0, pz1 = p['top_pilot_z']
    for (cx, cy) in (near_xy, far_xy):
        pilot = bd.Pos(cx, cy, pz0) * bd.Cylinder(
            radius=pilot_r, height=pz1 - pz0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
        top = top - pilot

    for (cx, cy) in (near_xy, far_xy):
        top = add_root_reinforcement(top, cx, cy, boss_r, z1, direction='up')

    return top


def lanyard_corner_pair(p, side):
    """Port of firefly_case.py:1602 _lanyard_corner_pair."""
    by_name = {s['name']: s['xy'] for s in p['screws_ABC']}
    return (by_name['A'], by_name['B1']) if side < 0 else (by_name['C'], by_name['B2'])
