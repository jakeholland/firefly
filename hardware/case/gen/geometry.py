"""Shared analytic profile math + primitive solid builders -- build123d
port of firefly_case.py's own geometry helpers. Every derivation here is
copied verbatim from that file (line numbers in each docstring refer to
the case-pass16 branch, the newest generator, per the port brief) --
this module does not invent any new numbers, only re-expresses the same
math against build123d/OCP instead of Fusion's adsk API.

Fusion-quirk workarounds deliberately NOT ported here (see
docs/hardware/headless-port-plan.md for the full classification):
`move_body_to_frame`'s Matrix3D two-step (build123d's `Line`/
`ThreePointArc` take plain 3D points, so an oriented prism is a single
sketch+extrude, no canonical-build-then-move needed), `dedupe_body`/
`_refetch_by_name` (OCC booleans do not leave orphaned same-named
duplicate bodies the way Fusion's timeline did), and the various
`except RuntimeError: pass` best-effort-fillet fallbacks (ported as
plain try/except around `bd.fillet`, but not load-bearing here since OCC
booleans do not depend on a prior fillet succeeding).
"""
import math

import build123d as bd


# ---------------------------------------------------------------------------
# Outer shoulder/fillet profile (firefly_case.py:474 _profile_geometry /
# :503 rho_at_z / :520 build_outer_half_profile_points).
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


def rho_from_spine(p, x, y):
    """firefly_case.py:6406 rho_from_spine."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= y <= by:
        return abs(x)
    center_y = ay if y < ay else by
    return math.hypot(x, y - center_y)


def true_wall_distance_along_ray(p, housing_xy, d2, z):
    """firefly_case.py:6481 true_wall_distance_along_ray."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    hx, hy = housing_xy
    r = rho_at_z(p, z)
    if ay <= hy <= by:
        if abs(d2[0]) < 1e-9:
            return None
        target = r if d2[0] > 0 else -r
        return (target - hx) / d2[0]
    center = (0.0, ay if hy < ay else by)
    cx, cy = hx - center[0], hy - center[1]
    b = 2.0 * (cx * d2[0] + cy * d2[1])
    c_coef = cx * cx + cy * cy - r * r
    disc = b * b - 4.0 * c_coef
    if disc < 0:
        return None
    root_disc = disc ** 0.5
    return max((-b + root_disc) / 2.0, (-b - root_disc) / 2.0)


def _nearer_spine_y(p, cy):
    """firefly_case.py:2213 _nearer_spine_y."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= cy <= by:
        return cy
    return ay if cy < ay else by


def wall_outward_axes(p, cx, cy):
    """firefly_case.py:2184 _wall_outward_axes."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= cy <= by:
        ref = (0.0, cy)
    else:
        ref = (0.0, ay if cy < ay else by)
    dx, dy = cx - ref[0], cy - ref[1]
    dlen = math.hypot(dx, dy) or 1.0
    axis2 = (dx / dlen, dy / dlen, 0.0)
    axis1 = (-axis2[1], axis2[0], 0.0)
    return axis1, axis2


def inner_profile_geometry(p):
    """firefly_case.py:598 _inner_profile_geometry -- a PLAIN quarter-round
    fillet, not a true offset of the compound outer curve."""
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


def inner_rho_at_z(p, z):
    """firefly_case.py:6313 inner_rho_at_z.

    Phase 2c bug fix (live-found while debugging the comms-stack frame's
    own inner-cavity clip): the flat/arc branch boundary here was wrong
    -- it compared `z` against `top_flat_z`/`bot_flat_z` (the OUTER edges
    of each fillet arc, where it meets the flat cap) instead of
    `top_c_z`/`bot_c_z` (the fillet centers, where the arc meets the
    straight wall), matching the SOURCE's own actual branch structure
    (`if z >= top_c_z: <top arc>`, `if z >= bot_c_z: <flat>`, else
    `<bottom arc>`). The old, wrong version made the flat branch fire for
    `bot_flat_z <= z <= top_flat_z` -- which INCLUDES the entire arc
    region -- so the arc formula was only ever evaluated outside its own
    valid domain (where `abs(z - center) > inner_r` always), permanently
    clamped to 0 by the `max(..., 0.0)` guard: `inner_rho_at_z` returned
    a constant `fc_rho` for the ENTIRE bottom cap (any z below
    `bot_flat_z`) instead of a smooth quarter-circle rising from `fc_rho`
    at `bot_flat_z` to `inner_wall_rho` at `bot_c_z` -- a real
    discontinuity at `z == bot_flat_z` (20mm vs. 28mm, 'current') this
    function's own docstring/name ('a PLAIN quarter-round fillet') never
    intended. Not caught earlier because nothing in `gen/` called this
    function until phase 2c went looking to understand the comms-stack
    frame's own inner-cavity clip behavior -- the actual 3D solid
    (`build_inner_pill_solid`/`_inner_half_points`) builds its own arc
    directly from points/`ThreePointArc`, sidestepping this bug
    entirely, so no built geometry was ever wrong from this."""
    g = inner_profile_geometry(p)
    if z >= g['top_c_z']:
        dz = z - g['top_c_z']
    elif z >= g['bot_c_z']:
        return g['inner_wall_rho']
    else:
        dz = z - g['bot_c_z']
    return g['fc_rho'] + math.sqrt(max(g['inner_r'] ** 2 - dz ** 2, 0.0))


# ---------------------------------------------------------------------------
# Outer/inner pill solids (firefly_case.py:568 sketch_full_profile / :810
# build_outer_pill_solid / :660 sketch_inner_full_profile / :682
# build_inner_pill_solid).
# ---------------------------------------------------------------------------
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


def _mirror_xz(pt3, y0):
    return (-pt3[0], y0, pt3[2])


def _outer_half_face(p, y0):
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
    return bd.make_face(segs)


def _outer_full_face(p, y0):
    pts = _outer_half_points(p, y0)
    m = {k: _mirror_xz(v, y0) for k, v in pts.items()}
    loop = [
        bd.Line(m['flat_bot_end'], pts['flat_bot_end']),
        bd.Line(pts['flat_bot_end'], pts['bot_tangent']),
        bd.ThreePointArc(pts['bot_tangent'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['top_tangent']),
        bd.Line(pts['top_tangent'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], m['flat_top_end']),
        bd.Line(m['flat_top_end'], m['top_tangent']),
        bd.ThreePointArc(m['top_tangent'], m['top_arc_mid'], m['wall_top']),
        bd.Line(m['wall_top'], m['wall_bot']),
        bd.ThreePointArc(m['wall_bot'], m['bot_arc_mid'], m['bot_tangent']),
        bd.Line(m['bot_tangent'], m['flat_bot_end']),
    ]
    return bd.make_face(loop)


def build_outer_pill_solid(p):
    """firefly_case.py:810 build_outer_pill_solid."""
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    assert ax == 0.0 and bx == 0.0
    spine_len = by - ay

    straight = bd.extrude(_outer_full_face(p, ay), amount=spine_len, dir=(0, 1, 0))
    cap_a = bd.revolve(_outer_half_face(p, ay), axis=bd.Axis((0, ay, 0), (0, 0, 1)), revolution_arc=-180.0)
    cap_b = bd.revolve(_outer_half_face(p, by), axis=bd.Axis((0, by, 0), (0, 0, 1)), revolution_arc=180.0)

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


def _inner_half_face(p, y0):
    pts = _inner_half_points(p, y0)
    segs = [
        bd.Line(pts['axis_bot'], pts['flat_bot_end']),
        bd.ThreePointArc(pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], pts['axis_top']),
        bd.Line(pts['axis_top'], pts['axis_bot']),
    ]
    return bd.make_face(segs)


def _inner_full_face(p, y0):
    pts = _inner_half_points(p, y0)
    m = {k: _mirror_xz(v, y0) for k, v in pts.items()}
    loop = [
        bd.Line(m['flat_bot_end'], pts['flat_bot_end']),
        bd.ThreePointArc(pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot']),
        bd.Line(pts['wall_bot'], pts['wall_top']),
        bd.ThreePointArc(pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end']),
        bd.Line(pts['flat_top_end'], m['flat_top_end']),
        bd.ThreePointArc(m['flat_top_end'], m['top_arc_mid'], m['wall_top']),
        bd.Line(m['wall_top'], m['wall_bot']),
        bd.ThreePointArc(m['wall_bot'], m['bot_arc_mid'], m['flat_bot_end']),
    ]
    return bd.make_face(loop)


def build_inner_pill_solid(p):
    """firefly_case.py:682 build_inner_pill_solid."""
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    spine_len = by - ay

    straight = bd.extrude(_inner_full_face(p, ay), amount=spine_len, dir=(0, 1, 0))
    cap_a = bd.revolve(_inner_half_face(p, ay), axis=bd.Axis((0, ay, 0), (0, 0, 1)), revolution_arc=-180.0)
    cap_b = bd.revolve(_inner_half_face(p, by), axis=bd.Axis((0, by, 0), (0, 0, 1)), revolution_arc=180.0)
    return straight + cap_a + cap_b


def build_inner_cavity_clip_tool(p, safety_margin=0.1):
    """firefly_case.py:719 build_inner_cavity_clip_tool -- shrunk (or, for
    a negative margin, grown -- see add_fpc_relief) inward by
    safety_margin on every face."""
    inner = build_inner_pill_solid(p)
    return bd.offset(inner, amount=-safety_margin)


def build_thickened_envelope(p, offset_mm):
    """firefly_case.py:794 build_thickened_envelope."""
    outer = build_outer_pill_solid(p)
    return bd.offset(outer, amount=offset_mm)


# ---------------------------------------------------------------------------
# Primitive solids (firefly_case.py:309/317/323/333 stadium/cylinder/box,
# :1604 cone_frustum_solid, :186/217/241 the pass-16 taper wedges, :411/439
# oriented stadium/box prisms).
# ---------------------------------------------------------------------------
def cylinder_solid(cx, cy, r, z_lo, z_hi):
    return bd.Pos(cx, cy, z_lo) * bd.Cylinder(
        radius=r, height=z_hi - z_lo, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))


def box_solid(x0, x1, y0, y1, z0, z1):
    return bd.Pos((x0 + x1) / 2.0, (y0 + y1) / 2.0, z0) * bd.Box(
        x1 - x0, y1 - y0, z1 - z0, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN))


def _stadium_face(ay, by, r):
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
    """firefly_case.py:309 stadium_solid."""
    face3d = bd.Pos(0, 0, z_lo) * _stadium_face(ay, by, r)
    return bd.extrude(face3d, amount=(z_hi - z_lo), dir=(0, 0, 1))


def stadium_ring_solid(ay, by, r_inner, r_outer, z_lo, z_hi):
    """firefly_case.py:317 stadium_ring_solid."""
    return stadium_solid(ay, by, r_outer, z_lo, z_hi) - stadium_solid(ay, by, r_inner, z_lo, z_hi)


def cone_frustum_solid(cx, cy, r_lo, r_hi, z_lo, z_hi):
    """firefly_case.py:1604 cone_frustum_solid -- solid of revolution
    around the vertical world-Z axis through (cx, cy); the profile has
    two corners AT the axis, so this is a filled cone/frustum, not a
    hollow napkin ring."""
    pts = [(cx, cy, z_lo), (cx + r_lo, cy, z_lo), (cx + r_hi, cy, z_hi), (cx, cy, z_hi)]
    segs = [bd.Line(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    face = bd.make_face(segs)
    return bd.revolve(face, axis=bd.Axis((cx, cy, 0), (0, 0, 1)), revolution_arc=360.0)


def revolve_taper_wedge(cx, cy, r_lo, r_hi, z_lo, z_hi, angle_deg):
    """firefly_case.py:186 revolve_taper_wedge -- a hollow 'napkin ring'
    wedge (neither profile corner touches the axis), revolved angle_deg
    around the vertical axis through (cx, cy). angle_deg follows
    build_outer_pill_solid's own cap_a(-180)/cap_b(+180) convention."""
    a = (cx + r_lo, cy, z_lo)
    b = (cx + r_hi, cy, z_hi)
    c = (cx + r_lo, cy, z_hi)
    segs = [bd.Line(a, b), bd.Line(b, c), bd.Line(c, a)]
    face = bd.make_face(segs)
    return bd.revolve(face, axis=bd.Axis((cx, cy, 0), (0, 0, 1)), revolution_arc=angle_deg)


def extrude_taper_wedge_along_y(x_sign, r_lo, r_hi, z_lo, z_hi, y0, y1):
    """firefly_case.py:217 extrude_taper_wedge_along_y."""
    a = (x_sign * r_lo, y0, z_lo)
    b = (x_sign * r_hi, y0, z_hi)
    c = (x_sign * r_lo, y0, z_hi)
    face = bd.make_face([bd.Line(a, b), bd.Line(b, c), bd.Line(c, a)])
    return bd.extrude(face, amount=y1 - y0, dir=(0, 1, 0))


def extrude_taper_cut_along_y(x_sign, r_lo, r_hi, z_lo, z_hi, y0, y1):
    """firefly_case.py:241 extrude_taper_cut_along_y -- filled to x=0."""
    a = (x_sign * r_lo, y0, z_lo)
    b = (x_sign * r_hi, y0, z_hi)
    c = (0.0, y0, z_hi)
    d = (0.0, y0, z_lo)
    face = bd.make_face([bd.Line(a, b), bd.Line(b, c), bd.Line(c, d), bd.Line(d, a)])
    return bd.extrude(face, amount=y1 - y0, dir=(0, 1, 0))


def _vadd(a, s, v):
    return (a[0] + s * v[0], a[1] + s * v[1], a[2] + s * v[2])


def oriented_stadium_prism(center, axis1, axis2, normal, L, W, depth):
    """firefly_case.py:411 oriented_stadium_prism -- built directly in 3D
    (build123d's Line/ThreePointArc take plain 3D points, so this needs
    no canonical-build-then-move step, unlike Fusion's move_body_to_frame --
    see this module's own docstring)."""
    r = W / 2.0
    half = max(L / 2.0 - r, 0.0)

    def pt(s1, s2):
        return _vadd(_vadd(center, s1, axis1), s2, axis2)

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


def oriented_box_prism(center, axis1, axis2, normal, L, W, depth):
    """firefly_case.py:439 oriented_box_prism."""
    hl, hw = L / 2.0, W / 2.0

    def pt(s1, s2):
        return _vadd(_vadd(center, s1, axis1), s2, axis2)

    pts = [pt(-hl, -hw), pt(hl, -hw), pt(hl, hw), pt(-hl, hw)]
    segs = [bd.Line(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    face = bd.make_face(segs)
    return bd.extrude(face, amount=depth, dir=normal)


def probe_point_solid(solid, pt3):
    """Headless equivalent of firefly_case.py:6147 probe_point_solid --
    OCP's BRepClass3d_SolidClassifier via build123d's Shape.is_inside
    (treats on-boundary as solid too, matching Fusion's PointOn/PointInside
    pair)."""
    shape = solid.solid() if hasattr(solid, 'solid') else solid
    return shape.is_inside(pt3) or _on_surface(shape, pt3)


def _on_surface(shape, pt3, tol=1e-4):
    try:
        return shape.distance_to(bd.Vector(*pt3)) <= tol
    except Exception:
        return False
