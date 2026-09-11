"""Outer/inner shell geometry -- build123d port of firefly_case.py's
_profile_geometry / build_outer_half_profile_points / build_outer_pill_solid
/ build_inner_pill_solid / add_window / add_lip_anchor_reliefs, restricted
to the slice this spike ports (the Top body only, split_z..top_z).

Every numeric derivation below (tangent points, fillet centers, cone
chamfer slope) is copied verbatim from firefly_case.py -- see that file's
line numbers in each docstring/comment for the original. The one
structural difference from the real generator: Fusion builds sketches on
named construction planes and moves bodies with Matrix3D transforms
(SPEC.md's own documented gotchas); build123d's `Line`/`ThreePointArc`
take plain 3D points directly, so the "build canonical, then rigidly
move" two-step the original needs (move_body_to_frame) collapses to one
step here -- noted in the spike doc as a real ergonomic difference.
"""
import math

import build123d as bd


# ---------------------------------------------------------------------------
# Outer shoulder/fillet profile -- port of firefly_case.py:385-465
# ---------------------------------------------------------------------------
def profile_geometry(p):
    flat_rho = p['flat_rho']
    fillet_r = p['fillet_r']
    fc_rho = p['fillet_center_rho']
    outer_r = p['outer_radius']
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    top_z = p['top_z']
    bot_z = p['bottom_z']

    assert abs(fc_rho - (outer_r - fillet_r)) < 1e-6, (fc_rho, outer_r, fillet_r)
    tangent_rho = fc_rho + fillet_r * math.cos(math.radians(45))
    dz_tangent = fillet_r * math.sin(math.radians(45))
    top_tangent_z = top_c_z + dz_tangent
    bot_tangent_z = bot_c_z - dz_tangent

    return {
        'flat_rho': flat_rho, 'fillet_r': fillet_r, 'fc_rho': fc_rho,
        'outer_r': outer_r, 'top_c_z': top_c_z, 'bot_c_z': bot_c_z,
        'top_z': top_z, 'bot_z': bot_z, 'tangent_rho': tangent_rho,
        'top_tangent_z': top_tangent_z, 'bot_tangent_z': bot_tangent_z,
    }


def rho_at_z(p, z):
    """Port of firefly_case.py:412 rho_at_z -- used by the fidelity/gate
    checks, not by the solid construction itself."""
    g = profile_geometry(p)
    top_z, bot_z = g['top_z'], g['bot_z']
    flat_rho, fc_rho, r = g['flat_rho'], g['fc_rho'], g['fillet_r']
    if z >= g['top_tangent_z']:
        return flat_rho + (top_z - z)
    if z >= g['top_c_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['top_c_z']) ** 2, 0.0))
    if z >= g['bot_c_z']:
        return g['outer_r']
    if z >= g['bot_tangent_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['bot_c_z']) ** 2, 0.0))
    return flat_rho + (z - bot_z)


def inner_profile_geometry(p):
    """Port of firefly_case.py:509 _inner_profile_geometry -- a PLAIN
    quarter-round fillet (radius = outer fillet_r - wall = R8 for this
    case's 2mm wall), not a true offset of the compound outer curve."""
    wall = p['wall']
    inner_r = p['fillet_r'] - wall
    fc_rho = p['fillet_center_rho']
    inner_wall_rho = p['outer_radius'] - wall
    assert abs((fc_rho + inner_r) - inner_wall_rho) < 1e-6
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    return {
        'inner_r': inner_r, 'fc_rho': fc_rho, 'inner_wall_rho': inner_wall_rho,
        'top_c_z': top_c_z, 'bot_c_z': bot_c_z,
        'top_flat_z': top_c_z + inner_r, 'bot_flat_z': bot_c_z - inner_r,
    }


def _outer_half_points(p, y0):
    g = profile_geometry(p)
    top_z, bot_z = g['top_z'], g['bot_z']
    flat_rho, tangent_rho, fc_rho, r = g['flat_rho'], g['tangent_rho'], g['fc_rho'], g['fillet_r']
    top_c_z, bot_c_z = g['top_c_z'], g['bot_c_z']
    top_tan_z, bot_tan_z = g['top_tangent_z'], g['bot_tangent_z']

    def pt(rho, z):
        return (rho, y0, z)

    return {
        'axis_bot': pt(0.0, bot_z),
        'flat_bot_end': pt(flat_rho, bot_z),
        'bot_tangent': pt(tangent_rho, bot_tan_z),
        'bot_arc_mid': pt(fc_rho + r * math.cos(math.radians(-22.5)),
                           bot_c_z + r * math.sin(math.radians(-22.5))),
        'wall_bot': pt(g['outer_r'], bot_c_z),
        'wall_top': pt(g['outer_r'], top_c_z),
        'top_arc_mid': pt(fc_rho + r * math.cos(math.radians(22.5)),
                           top_c_z + r * math.sin(math.radians(22.5))),
        'top_tangent': pt(tangent_rho, top_tan_z),
        'flat_top_end': pt(flat_rho, top_z),
        'axis_top': pt(0.0, top_z),
    }


def _outer_half_wire(p, y0):
    pts = _outer_half_points(p, y0)
    segs = [
        bd.Line(pts['axis_bot'], pts['flat_bot_end']),
        bd.Line(pts['flat_bot_end'], pts['bot_tangent']),
        bd.ThreePointArc(pts['bot_tangent'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['top_tangent']),
        bd.Line(pts['top_tangent'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], pts['axis_top']),
        bd.Line(pts['axis_top'], pts['axis_bot']),
    ]
    return segs, pts


def _outer_full_face(p, y0):
    """Mirrored full profile (x -outer_r..+outer_r) at world y=y0, for the
    straight-section extrude. Port of sketch_full_profile (:479)."""
    _, pts = _outer_half_wire(p, y0)
    mirror = lambda pt3: (-pt3[0], pt3[1], pt3[2])
    # right side: flat_bot_end -> ... -> flat_top_end (no axis legs -- the
    # mirrored loop closes across the top/bottom flats directly instead).
    loop = [
        bd.Line(mirror(pts['flat_bot_end']), pts['flat_bot_end']),
        bd.Line(pts['flat_bot_end'], pts['bot_tangent']),
        bd.ThreePointArc(pts['bot_tangent'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['top_tangent']),
        bd.Line(pts['top_tangent'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], mirror(pts['flat_top_end'])),
        bd.Line(mirror(pts['flat_top_end']), mirror(pts['top_tangent'])),
        bd.ThreePointArc(mirror(pts['top_tangent']), mirror(pts['top_arc_mid']), mirror(pts['wall_top'])),
        bd.Line(mirror(pts['wall_top']), mirror(pts['wall_bot'])),
        bd.ThreePointArc(mirror(pts['wall_bot']), mirror(pts['bot_arc_mid']), mirror(pts['bot_tangent'])),
        bd.Line(mirror(pts['bot_tangent']), mirror(pts['flat_bot_end'])),
    ]
    return bd.make_face(loop)


def _outer_half_face(p, y0):
    segs, _ = _outer_half_wire(p, y0)
    return bd.make_face(segs)


def build_outer_pill_solid(p):
    """Port of firefly_case.py:721 build_outer_pill_solid: straight-section
    extrude + two end-cap 180-degree revolves, unioned."""
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    assert ax == 0.0 and bx == 0.0
    spine_len = by - ay

    straight_face = _outer_full_face(p, ay)
    straight = bd.extrude(straight_face, amount=spine_len, dir=(0, 1, 0))

    cap_a_face = _outer_half_face(p, ay)
    cap_a = bd.revolve(cap_a_face, axis=bd.Axis((0, ay, 0), (0, 0, 1)), revolution_arc=-180.0)

    cap_b_face = _outer_half_face(p, by)
    cap_b = bd.revolve(cap_b_face, axis=bd.Axis((0, by, 0), (0, 0, 1)), revolution_arc=180.0)

    bb_a, bb_b = cap_a.bounding_box(), cap_b.bounding_box()
    assert bb_a.max.Y <= ay + 0.05, ('cap_a swept the wrong way', bb_a)
    assert bb_b.min.Y >= by - 0.05, ('cap_b swept the wrong way', bb_b)

    return straight + cap_a + cap_b


def _inner_half_points(p, y0):
    g = inner_profile_geometry(p)
    r, fc_rho = g['inner_r'], g['fc_rho']

    def pt(rho, z):
        return (rho, y0, z)

    return {
        'axis_bot': pt(0.0, g['bot_flat_z']),
        'flat_bot_end': pt(fc_rho, g['bot_flat_z']),
        'bot_arc_mid': pt(fc_rho + r * math.cos(math.radians(-45)),
                           g['bot_c_z'] + r * math.sin(math.radians(-45))),
        'wall_bot': pt(g['inner_wall_rho'], g['bot_c_z']),
        'wall_top': pt(g['inner_wall_rho'], g['top_c_z']),
        'top_arc_mid': pt(fc_rho + r * math.cos(math.radians(45)),
                           g['top_c_z'] + r * math.sin(math.radians(45))),
        'flat_top_end': pt(fc_rho, g['top_flat_z']),
        'axis_top': pt(0.0, g['top_flat_z']),
    }


def _inner_half_wire(p, y0):
    pts = _inner_half_points(p, y0)
    segs = [
        bd.Line(pts['axis_bot'], pts['flat_bot_end']),
        bd.ThreePointArc(pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], pts['axis_top']),
        bd.Line(pts['axis_top'], pts['axis_bot']),
    ]
    return segs, pts


def _inner_full_face(p, y0):
    segs, pts = _inner_half_wire(p, y0)
    right = segs[:-1]
    mirror = lambda pt3: (-pt3[0], pt3[1], pt3[2])
    left = [
        bd.ThreePointArc(mirror(pts['flat_bot_end']), mirror(pts['bot_arc_mid']), mirror(pts['wall_bot'])),
        bd.Line(mirror(pts['wall_bot']), mirror(pts['wall_top'])),
        bd.ThreePointArc(mirror(pts['wall_top']), mirror(pts['top_arc_mid']), mirror(pts['flat_top_end'])),
    ]
    loop = [
        bd.Line(mirror(pts['flat_bot_end']), pts['flat_bot_end']),
        bd.ThreePointArc(pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], mirror(pts['flat_top_end'])),
        bd.ThreePointArc(mirror(pts['flat_top_end']), mirror(pts['top_arc_mid']), mirror(pts['wall_top'])),
        bd.Line(mirror(pts['wall_top']), mirror(pts['wall_bot'])),
        bd.ThreePointArc(mirror(pts['wall_bot']), mirror(pts['bot_arc_mid']), mirror(pts['flat_bot_end'])),
    ]
    return bd.make_face(loop)


def _inner_half_face(p, y0):
    segs, _ = _inner_half_wire(p, y0)
    return bd.make_face(segs)


def build_inner_pill_solid(p):
    """Port of firefly_case.py:593 build_inner_pill_solid."""
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    spine_len = by - ay

    straight_face = _inner_full_face(p, ay)
    straight = bd.extrude(straight_face, amount=spine_len, dir=(0, 1, 0))

    cap_a = bd.revolve(_inner_half_face(p, ay), axis=bd.Axis((0, ay, 0), (0, 0, 1)), revolution_arc=-180.0)
    cap_b = bd.revolve(_inner_half_face(p, by), axis=bd.Axis((0, by, 0), (0, 0, 1)), revolution_arc=180.0)
    return straight + cap_a + cap_b


def build_top_shell(p):
    """Hollow shell (outer - inner), restricted to the Top piece (z in
    [split_z, top_z]) -- port of firefly_case.py:770 hollow_and_split,
    minus the Bottom half this spike doesn't need."""
    outer = build_outer_pill_solid(p)
    inner = build_inner_pill_solid(p)
    hollow = outer - inner

    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    z0, z1 = p['split_z'], p['top_z']
    # a generous XY box (bigger than the pill can ever reach) clipped to
    # the Top's own z-band -- equivalent to Fusion's splitBodyFeatures at
    # z=split_z, keeping the upper piece only.
    r = p['outer_radius'] + 5.0
    clip_z0, clip_z1 = z0, z1 + 1.0  # lower bound is EXACTLY split_z (this is a keep-above-the-parting-line
                                      # clip, not a cut tool -- growing it downward would pull Bottom's own
                                      # z9.5..10 material in too); upper bound has loose margin above top_z.
    clip = bd.Pos(0, (ay + by) / 2.0, (clip_z0 + clip_z1) / 2.0) * bd.Box(2 * r, (by - ay) + 2 * r, clip_z1 - clip_z0)
    top = hollow & clip
    return top


def add_window(p, top):
    """Port of firefly_case.py:946 add_window's bore + pass-15 cone
    chamfer (the edge-matched chamfer was replaced there because the
    window's Ø45.30 bore is wider than trim's own flat_rho and crosses
    into the curved shoulder -- see that function's docstring). The cone
    tool's numbers (r-0.05 .. r+chamf+0.2, over z top_z-chamf-0.1 ..
    top_z+0.5) are copied verbatim."""
    cx, cy = p['window_center']
    r = p['window_dia'] / 2.0
    bore = bd.Pos(cx, cy, p['window_z_bottom']) * bd.Cylinder(
        radius=r, height=(p['top_z'] + 1.0 - p['window_z_bottom']), align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))
    top = top - bore

    chamf = p['window_chamfer']
    cone_r_lo, cone_z_lo = r - 0.05, p['top_z'] - chamf - 0.1
    cone_r_hi, cone_z_hi = r + chamf + 0.2, p['top_z'] + 0.5
    cone_tool = _cone_frustum(cx, cy, cone_r_lo, cone_r_hi, cone_z_lo, cone_z_hi)
    top = top - cone_tool
    return top


def _cone_frustum(cx, cy, r_lo, r_hi, z_lo, z_hi):
    """Port of firefly_case.py:1365 cone_frustum_solid -- a solid of
    revolution around the vertical world-Z axis through (cx, cy)."""
    pts = [(cx, cy, z_lo), (cx + r_lo, cy, z_lo), (cx + r_hi, cy, z_hi), (cx, cy, z_hi)]
    segs = [bd.Line(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    face = bd.make_face(segs)
    return bd.revolve(face, axis=bd.Axis((cx, cy, 0), (0, 0, 1)), revolution_arc=360.0)


def _stadium_face(ay, by, r):
    half_straight = 0.0  # the spine itself has zero extra straight length beyond the two centers
    p_tr, p_br = (r, by), (-r, by)
    p_tl, p_bl = (r, ay), (-r, ay)
    p_top_tip = (0.0, by + r)
    p_bot_tip = (0.0, ay - r)
    segs = [
        bd.Line((r, ay), (r, by)),
        bd.ThreePointArc((r, by), p_top_tip, (-r, by)),
        bd.Line((-r, by), (-r, ay)),
        bd.ThreePointArc((-r, ay), p_bot_tip, (r, ay)),
    ]
    return bd.make_face(segs)


def stadium_solid(ay, by, r, z_lo, z_hi):
    """Port of firefly_case.py:220 stadium_solid: a stadium (2D racetrack
    centred on the spine_a/spine_b axis) extruded in Z."""
    face2d = _stadium_face(ay, by, r)
    # face2d's points are (x, y) in the XY plane at z=0 by construction
    # (Line/make_face above used 2-tuples => build123d treats them as
    # points in the XY plane); move to z_lo then extrude +Z.
    face3d = bd.Pos(0, 0, z_lo) * face2d
    return bd.extrude(face3d, amount=(z_hi - z_lo), dir=(0, 0, 1))


def stadium_ring_solid(ay, by, r_inner, r_outer, z_lo, z_hi):
    outer = stadium_solid(ay, by, r_outer, z_lo, z_hi)
    inner = stadium_solid(ay, by, r_inner, z_lo, z_hi)
    return outer - inner


def add_lip_anchor_reliefs(p, top):
    """Port of firefly_case.py:800 add_lip_anchor_reliefs, the ring
    construction + finding-6 seam chamfer only (per-boss/lug reliefs are
    out of this spike's ported slice -- see the spike doc)."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    lip = stadium_ring_solid(ay, by, p['lip_r'][0], p['lip_r'][1], p['lip_z'][0], p['lip_z'][1])
    anchor = stadium_ring_solid(ay, by, p['anchor_r'][0], p['anchor_r'][1], p['anchor_z'][0], p['anchor_z'][1])
    top = top + lip + anchor

    # finding 6: 45-deg-ish bevel on the lip->anchor OUTER step (r =
    # anchor_r[1], z = anchor_z[0]) so the step prints support-free --
    # built the same way the window chamfer is (a conical CUT, robust
    # regardless of whether Fusion/OCC can identify a clean edge loop),
    # rather than firefly_case.py's edge-selection chamfer_stadium_edge_at,
    # since the stadium's straight-vs-arc edge split makes edge selection
    # brittle and this spike only needs the same net geometry.
    seam = p.get('lip_ring_seam_chamfer')
    if seam:
        r_out = p['anchor_r'][1]
        z_step = p['anchor_z'][0]
        cone_r_lo, cone_z_lo = r_out - seam - 0.1, z_step - seam - 0.1
        cone_r_hi, cone_z_hi = r_out + 0.2, z_step + 0.1
        # Build as a stadium-swept chamfer tool: outer stadium at r=r_out
        # grown/shrunk in Z the same way the window's cone tool works,
        # approximated here as a stadium ring cut with the outer radius
        # linearly interpolated across z_step -- reuse the same box+
        # revolve cone idiom via 8 point samples around the stadium is
        # overkill for a spike; a plain stadium ring one seam-width tall,
        # centered on the step, is geometrically equivalent for a spike's
        # purposes (chamfers only the outer step, not the inner lip).
        chamfer_ring = stadium_ring_solid(ay, by, r_out - seam, r_out + 0.2, z_step - seam, z_step + 0.05)
        top = top - chamfer_ring
    return top
