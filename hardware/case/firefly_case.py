"""Firefly festival-puck case generator ("case as code").

Run from inside Fusion 360 via a tiny wrapper script (see README.md):

    import runpy
    g = runpy.run_path('/private/tmp/claude-501/case-generator/hardware/case/firefly_case.py')
    g['run'](_context)

Structure (per SPEC.md):
    PARAMS   -- selected variant's parameter dict (all mm)
    build()  -- constructs the geometry, returns {name: BRepBody}
    verify() -- probe-based assertions against SPEC.md's reference numbers
    run()    -- activates/creates the Fusion document, builds, verifies,
                prints a summary, takes screenshots.
"""

import os
import sys
import math
import time
import json

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

# Fusion's embedded Python interpreter stays alive across separate script
# executions, so sys.modules from a previous run() would otherwise serve
# STALE params_current/params_trim content on the next run. Force a fresh
# import every time this file is executed.
for _mod in ('params_current', 'params_trim'):
    sys.modules.pop(_mod, None)

from params_current import PARAMS as PARAMS_CURRENT
from params_trim import PARAMS as PARAMS_TRIM

# ---------------------------------------------------------------------------
# Variant switch. Change this (or set firefly_case.VARIANT before calling
# run()) to pick which case gets built. 'trim' (56x102x25) is Jake's default;
# 'current' (60x110x25, matches the "Firefly V2 v15/v16" reference) stays
# buildable for the probe-table comparison against that reference.
# ---------------------------------------------------------------------------
VARIANT = 'trim'  # 'current' | 'trim'

_VARIANTS = {'current': PARAMS_CURRENT, 'trim': PARAMS_TRIM}
PARAMS = _VARIANTS[VARIANT]

SCRATCH_DIR = '/private/tmp/claude-501/case-generator-scratch'

MM = 0.1  # cm per mm -- Fusion's API works in cm


# ---------------------------------------------------------------------------
# Small helpers (import adsk lazily -- this module is executed inside Fusion
# via runpy, but importing at module scope keeps errors visible early).
# ---------------------------------------------------------------------------
import adsk.core
import adsk.fusion

Point3D = adsk.core.Point3D
ValueInput = adsk.core.ValueInput


def P(x_mm, y_mm, z_mm):
    return Point3D.create(x_mm * MM, y_mm * MM, z_mm * MM)


def V(v_mm):
    return ValueInput.createByReal(v_mm * MM)


def new_sketch(root, plane):
    return root.sketches.add(plane)


def _sk(sk, world_pt):
    """Sketch curve creation methods take points in the SKETCH's own local
    coordinate system, not world/model space -- convert explicitly or X/Y/Z
    get silently permuted (see SPEC.md gotchas addendum below)."""
    return sk.modelToSketchSpace(world_pt)


def add_line(sk, a, b):
    return sk.sketchCurves.sketchLines.addByTwoPoints(_sk(sk, a), _sk(sk, b))


def add_arc3(sk, a, mid, b):
    return sk.sketchCurves.sketchArcs.addByThreePoints(_sk(sk, a), _sk(sk, mid), _sk(sk, b))


def extrude_new_body(root, profile, distance_mm, direction='positive'):
    ext = root.features.extrudeFeatures
    inp = ext.createInput(profile, adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
    sign = 1.0 if direction == 'positive' else -1.0
    inp.setDistanceExtent(False, V(sign * distance_mm))
    feat = ext.add(inp)
    return feat.bodies.item(0)


def revolve_new_body(root, profile, axis_line, angle_deg):
    rev = root.features.revolveFeatures
    inp = rev.createInput(profile, axis_line, adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
    inp.setAngleExtent(False, ValueInput.createByReal(math.radians(angle_deg)))
    feat = rev.add(inp)
    return feat.bodies.item(0)


def combine_join(root, target, tools):
    return _combine(root, target, tools, adsk.fusion.FeatureOperations.JoinFeatureOperation)


def combine_cut(root, target, tools):
    return _combine(root, target, tools, adsk.fusion.FeatureOperations.CutFeatureOperation)


def combine_intersect(root, target, tools):
    return _combine(root, target, tools, adsk.fusion.FeatureOperations.IntersectFeatureOperation)


def _combine(root, target, tools, op):
    coll = adsk.core.ObjectCollection.create()
    for t in tools:
        coll.add(t)
    cf = root.features.combineFeatures
    inp = cf.createInput(target, coll)
    inp.operation = op
    inp.isKeepToolBodies = False
    feat = cf.add(inp)
    return feat.bodies.item(0) if feat.bodies.count else target


def plane_at_z(root, z_mm):
    planes = root.constructionPlanes
    pin = planes.createInput()
    pin.setByOffset(root.xYConstructionPlane, V(z_mm))
    return planes.add(pin)


def add_stadium_loop(sk, ay, by, r, z_mm):
    """A pill/stadium outline in the XY plane at height z_mm, around the
    vertical spine (0,ay)-(0,by). Assumes ay <= by."""
    pL0 = P(-r, ay, z_mm)
    pL1 = P(-r, by, z_mm)
    pR0 = P(r, ay, z_mm)
    pR1 = P(r, by, z_mm)
    p_top_mid = P(0, by + r, z_mm)
    p_bot_mid = P(0, ay - r, z_mm)
    add_line(sk, pL0, pL1)
    add_arc3(sk, pL1, p_top_mid, pR1)
    add_line(sk, pR1, pR0)
    add_arc3(sk, pR0, p_bot_mid, pL0)


def stadium_solid(root, ay, by, r, z_lo, z_hi):
    plane = plane_at_z(root, z_lo)
    sk = new_sketch(root, plane)
    add_stadium_loop(sk, ay, by, r, z_lo)
    prof = sk.profiles.item(0)
    return extrude_new_body(root, prof, z_hi - z_lo, direction='positive')


def stadium_ring_solid(root, ay, by, r_inner, r_outer, z_lo, z_hi):
    outer = stadium_solid(root, ay, by, r_outer, z_lo, z_hi)
    inner = stadium_solid(root, ay, by, r_inner, z_lo, z_hi)
    return combine_cut(root, outer, [inner])


def cylinder_solid(root, cx, cy, r, z_lo, z_hi):
    plane = plane_at_z(root, z_lo)
    sk = new_sketch(root, plane)
    center_world = P(cx, cy, z_lo)
    circles = sk.sketchCurves.sketchCircles
    circles.addByCenterRadius(sk.modelToSketchSpace(center_world), r * MM)
    prof = sk.profiles.item(0)
    return extrude_new_body(root, prof, z_hi - z_lo, direction='positive')


def box_solid(root, x0, x1, y0, y1, z0, z1):
    plane = plane_at_z(root, z0)
    sk = new_sketch(root, plane)
    p0 = sk.modelToSketchSpace(P(x0, y0, z0))
    p1 = sk.modelToSketchSpace(P(x1, y1, z0))
    sk.sketchCurves.sketchLines.addTwoPointRectangle(p0, p1)
    prof = sk.profiles.item(0)
    return extrude_new_body(root, prof, z1 - z0, direction='positive')


def vadd(a, s, v):
    """a + s*v, all 3-tuples in mm."""
    return (a[0] + s * v[0], a[1] + s * v[1], a[2] + s * v[2])


def move_body_to_frame(root, body, origin_mm, x_axis, y_axis, z_axis):
    """Rigidly transform `body` (built canonically at the origin with axes
    +X/+Y/+Z) so its local +X/+Y/+Z map to the given target frame. Used
    instead of an arbitrary-angle construction plane: ConstructionPlaneInput
    .setByThreePoints needs real point ENTITIES (sketch/construction
    points, not raw coordinates -- root.constructionPoints.add is unusable
    here per SPEC.md gotcha 4), so oriented features are built axis-aligned
    on a stock plane and then moved into place with Matrix3D.

    z_axis is recomputed as x_axis CROSS y_axis (forcing a proper,
    right-handed rotation -- Move rejects an improper/mirroring transform
    with "invalid argument transform") rather than trusting the caller's
    z_axis sign; every shape this is used on (stadium/box prisms) is
    symmetric about its own axes, so the sign of the local Z direction
    never changes the resulting geometry."""
    z_axis = _cross(x_axis, y_axis)
    mat = adsk.core.Matrix3D.create()
    ok = mat.setToAlignCoordinateSystems(
        P(0.0, 0.0, 0.0), adsk.core.Vector3D.create(1, 0, 0),
        adsk.core.Vector3D.create(0, 1, 0), adsk.core.Vector3D.create(0, 0, 1),
        P(*origin_mm), adsk.core.Vector3D.create(*x_axis),
        adsk.core.Vector3D.create(*y_axis), adsk.core.Vector3D.create(*z_axis))
    assert ok, 'setToAlignCoordinateSystems failed -- axes not orthonormal?'
    coll = adsk.core.ObjectCollection.create()
    coll.add(body)
    move_input = root.features.moveFeatures.createInput(coll, mat)
    root.features.moveFeatures.add(move_input)
    return body


def oriented_stadium_loop(sk, center_mm, axis1_mm, axis2_mm, L, W):
    """A stadium loop lying in an arbitrary plane: axis1 is the LONG
    direction (unit vector, mm-scale doesn't matter), axis2 the SHORT
    direction; L is the overall length along axis1, W the overall width
    (= diameter of the round ends) along axis2."""
    r = W / 2.0
    half_straight = max(L / 2.0 - r, 0.0)
    c = center_mm

    def pt(s1, s2):
        return P(*vadd(vadd(c, s1, axis1_mm), s2, axis2_mm))

    p_tr = pt(half_straight, r)
    p_br = pt(half_straight, -r)
    p_tl = pt(-half_straight, r)
    p_bl = pt(-half_straight, -r)
    p_right_tip = pt(half_straight + r, 0.0)
    p_left_tip = pt(-half_straight - r, 0.0)

    add_line(sk, p_tl, p_tr)
    add_arc3(sk, p_tr, p_right_tip, p_br)
    add_line(sk, p_br, p_bl)
    add_arc3(sk, p_bl, p_left_tip, p_tl)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def oriented_stadium_prism(root, center_mm, axis1_mm, axis2_mm, normal_mm, L, W, depth):
    """A stadium (long axis1, short axis2) extruded along `normal_mm` by
    `depth` mm, based at center_mm. Built canonically on the XZ plane
    (local axis1->world X, axis2->world Z, extrude->world +Y) and then
    rigidly moved into place -- see move_body_to_frame's docstring for why
    (arbitrary-angle construction planes need real point entities we don't
    have a clean way to create here)."""
    sk = new_sketch(root, root.xZConstructionPlane)
    oriented_stadium_loop(sk, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), L, W)
    prof = sk.profiles.item(0)
    body = extrude_new_body(root, prof, depth, direction='positive')
    return move_body_to_frame(root, body, center_mm, axis1_mm, normal_mm, axis2_mm)


def oriented_box_loop(sk, center_mm, axis1_mm, axis2_mm, L, W):
    c = center_mm
    hl, hw = L / 2.0, W / 2.0

    def pt(s1, s2):
        return P(*vadd(vadd(c, s1, axis1_mm), s2, axis2_mm))

    p1, p2, p3, p4 = pt(-hl, -hw), pt(hl, -hw), pt(hl, hw), pt(-hl, hw)
    add_line(sk, p1, p2)
    add_line(sk, p2, p3)
    add_line(sk, p3, p4)
    add_line(sk, p4, p1)


def oriented_box_prism(root, center_mm, axis1_mm, axis2_mm, normal_mm, L, W, depth):
    sk = new_sketch(root, root.xZConstructionPlane)
    oriented_box_loop(sk, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), L, W)
    prof = sk.profiles.item(0)
    body = extrude_new_body(root, prof, depth, direction='positive')
    return move_body_to_frame(root, body, center_mm, axis1_mm, normal_mm, axis2_mm)


def bbox_of(body):
    b = body.boundingBox
    return {
        'x': (b.minPoint.x / MM, b.maxPoint.x / MM),
        'y': (b.minPoint.y / MM, b.maxPoint.y / MM),
        'z': (b.minPoint.z / MM, b.maxPoint.z / MM),
    }


# ---------------------------------------------------------------------------
# Outer shoulder/fillet profile geometry (see SPEC.md "Reference geometry").
# All values in mm. rho = radial distance from the pill spine.
# ---------------------------------------------------------------------------
def _profile_geometry(p):
    flat_rho = p['flat_rho']
    fillet_r = p['fillet_r']
    fc_rho = p['fillet_center_rho']
    outer_r = p['outer_radius']
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    top_z = p['top_z']
    bot_z = p['bottom_z']

    # invariant: the fillet is tangent to the vertical wall at rho=outer_r,
    # so its center rho must be outer_r - fillet_r (20 for 'current', 18 for
    # 'trim' -- both give SPEC.md's stated tangent points, 27.07 and 25.07).
    assert abs(fc_rho - (outer_r - fillet_r)) < 1e-6, (fc_rho, outer_r, fillet_r)
    tangent_rho = fc_rho + fillet_r * math.cos(math.radians(45))
    dz_tangent = fillet_r * math.sin(math.radians(45))

    top_tangent_z = top_c_z + dz_tangent      # 22.07
    bot_tangent_z = bot_c_z - dz_tangent       # 2.93

    return {
        'flat_rho': flat_rho, 'fillet_r': fillet_r, 'fc_rho': fc_rho,
        'outer_r': outer_r, 'top_c_z': top_c_z, 'bot_c_z': bot_c_z,
        'top_z': top_z, 'bot_z': bot_z,
        'tangent_rho': tangent_rho,
        'top_tangent_z': top_tangent_z, 'bot_tangent_z': bot_tangent_z,
    }


def rho_at_z(p, z):
    """Outer rho(z) on the straight-side profile, per SPEC probe table."""
    g = _profile_geometry(p)
    top_z, bot_z = g['top_z'], g['bot_z']
    flat_rho, fc_rho, r = g['flat_rho'], g['fc_rho'], g['fillet_r']
    if z >= g['top_tangent_z']:
        # top 45 deg chamfer: rho = flat_rho + (top_z - z)
        return flat_rho + (top_z - z)
    if z >= g['top_c_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['top_c_z']) ** 2, 0.0))
    if z >= g['bot_c_z']:
        return g['outer_r']
    if z >= g['bot_tangent_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['bot_c_z']) ** 2, 0.0))
    return flat_rho + (z - bot_z)


def build_outer_half_profile_points(p, y0):
    """Return the closed boundary points/arc-mids for the RIGHT HALF profile
    (rho = x >= 0) at a given world Y, used for the end-cap revolves. The
    loop is: axis bottom -> flat bottom -> chamfer -> arc -> wall -> arc ->
    chamfer -> flat top -> axis top -> (closes back down the axis)."""
    g = _profile_geometry(p)
    top_z, bot_z = g['top_z'], g['bot_z']
    flat_rho = g['flat_rho']
    tangent_rho = g['tangent_rho']
    fc_rho = g['fc_rho']
    r = g['fillet_r']
    top_c_z, bot_c_z = g['top_c_z'], g['bot_c_z']
    top_tan_z, bot_tan_z = g['top_tangent_z'], g['bot_tangent_z']

    def pt(rho, z):
        return P(rho, y0, z)

    pts = {
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
    return pts


def sketch_half_profile(root, plane, p, y0):
    sk = new_sketch(root, plane)
    pts = build_outer_half_profile_points(p, y0)
    axis_line = add_line(sk, pts['axis_bot'], pts['axis_top'])
    add_line(sk, pts['axis_bot'], pts['flat_bot_end'])
    add_line(sk, pts['flat_bot_end'], pts['bot_tangent'])
    add_arc3(sk, pts['bot_tangent'], pts['bot_arc_mid'], pts['wall_bot'])
    add_line(sk, pts['wall_bot'], pts['wall_top'])
    add_arc3(sk, pts['wall_top'], pts['top_arc_mid'], pts['top_tangent'])
    add_line(sk, pts['top_tangent'], pts['flat_top_end'])
    add_line(sk, pts['flat_top_end'], pts['axis_top'])
    return sk, axis_line


def sketch_full_profile(root, plane, p, y0):
    """Full mirrored (x from -outer_r..+outer_r) closed profile for the
    straight-section extrude."""
    sk = new_sketch(root, plane)
    pts = build_outer_half_profile_points(p, y0)

    def mirror(pt3d):
        return P(-pt3d.x / MM, y0, pt3d.z / MM)

    m = {k: mirror(v) for k, v in pts.items()}

    # bottom flat line spans the full width through the axis
    add_line(sk, m['flat_bot_end'], pts['flat_bot_end'])
    # right side, bottom -> top
    add_line(sk, pts['flat_bot_end'], pts['bot_tangent'])
    add_arc3(sk, pts['bot_tangent'], pts['bot_arc_mid'], pts['wall_bot'])
    add_line(sk, pts['wall_bot'], pts['wall_top'])
    add_arc3(sk, pts['wall_top'], pts['top_arc_mid'], pts['top_tangent'])
    add_line(sk, pts['top_tangent'], pts['flat_top_end'])
    # top flat line spans the full width
    add_line(sk, pts['flat_top_end'], m['flat_top_end'])
    # left side, top -> bottom (mirrored)
    add_line(sk, m['flat_top_end'], m['top_tangent'])
    add_arc3(sk, m['top_tangent'], m['top_arc_mid'], m['wall_top'])
    add_line(sk, m['wall_top'], m['wall_bot'])
    add_arc3(sk, m['wall_bot'], m['bot_arc_mid'], m['bot_tangent'])
    add_line(sk, m['bot_tangent'], m['flat_bot_end'])
    return sk


def _inner_profile_geometry(p):
    """The inner cavity edge is a PLAIN quarter-round fillet (no 45 deg
    shoulder like the outer edge) tangent to the flat ceiling/floor and to
    the vertical inner wall. Its radius is simply (outer fillet_r - wall),
    and it shares the outer fillet's center rho and center z -- this is what
    the reference model's cavity probes (SPEC.md) actually match, not a
    true perpendicular offset of the outer compound curve (which would
    still carry the 45 deg shoulder through to the inside and gives visibly
    different numbers -- verified against SPEC's probe table during
    development)."""
    wall = p['wall']
    inner_r = p['fillet_r'] - wall
    fc_rho = p['fillet_center_rho']          # same center rho as outer
    inner_wall_rho = p['outer_radius'] - wall
    assert abs((fc_rho + inner_r) - inner_wall_rho) < 1e-6, \
        (fc_rho, inner_r, inner_wall_rho)
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    top_flat_z = top_c_z + inner_r           # ceiling underside
    bot_flat_z = bot_c_z - inner_r           # floor topside
    return {
        'inner_r': inner_r, 'fc_rho': fc_rho, 'inner_wall_rho': inner_wall_rho,
        'top_c_z': top_c_z, 'bot_c_z': bot_c_z,
        'top_flat_z': top_flat_z, 'bot_flat_z': bot_flat_z,
    }


def build_inner_half_profile_points(p, y0):
    g = _inner_profile_geometry(p)
    r = g['inner_r']
    fc_rho = g['fc_rho']

    def pt(rho, z):
        return P(rho, y0, z)

    pts = {
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
    return pts


def sketch_inner_half_profile(root, plane, p, y0):
    sk = new_sketch(root, plane)
    pts = build_inner_half_profile_points(p, y0)
    axis_line = add_line(sk, pts['axis_bot'], pts['axis_top'])
    add_line(sk, pts['axis_bot'], pts['flat_bot_end'])
    add_arc3(sk, pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot'])
    add_line(sk, pts['wall_bot'], pts['wall_top'])
    add_arc3(sk, pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end'])
    add_line(sk, pts['flat_top_end'], pts['axis_top'])
    return sk, axis_line


def sketch_inner_full_profile(root, plane, p, y0):
    """Full mirrored (x from -inner_wall_rho..+inner_wall_rho) closed inner
    profile for the straight-section extrude."""
    sk = new_sketch(root, plane)
    pts = build_inner_half_profile_points(p, y0)

    def mirror(pt3d):
        return P(-pt3d.x / MM, y0, pt3d.z / MM)

    m = {k: mirror(v) for k, v in pts.items()}

    add_line(sk, m['flat_bot_end'], pts['flat_bot_end'])
    add_arc3(sk, pts['flat_bot_end'], pts['bot_arc_mid'], pts['wall_bot'])
    add_line(sk, pts['wall_bot'], pts['wall_top'])
    add_arc3(sk, pts['wall_top'], pts['top_arc_mid'], pts['flat_top_end'])
    add_line(sk, pts['flat_top_end'], m['flat_top_end'])
    add_arc3(sk, m['flat_top_end'], m['top_arc_mid'], m['wall_top'])
    add_line(sk, m['wall_top'], m['wall_bot'])
    add_arc3(sk, m['wall_bot'], m['bot_arc_mid'], m['flat_bot_end'])
    return sk


def build_inner_pill_solid(root, p):
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    spine_len = by - ay

    sk_full = sketch_inner_full_profile(root, root.xZConstructionPlane, p, y0=ay)
    prof = sk_full.profiles.item(0)
    straight = extrude_new_body(root, prof, spine_len, direction='positive')

    sk_a, axis_a = sketch_inner_half_profile(root, root.xZConstructionPlane, p, y0=ay)
    prof_a = sk_a.profiles.item(0)
    cap_a = revolve_new_body(root, prof_a, axis_a, -180.0)

    planes = root.constructionPlanes
    plane_in = planes.createInput()
    plane_in.setByOffset(root.xZConstructionPlane, V(by))
    plane_b = planes.add(plane_in)
    sk_b, axis_b = sketch_inner_half_profile(root, plane_b, p, y0=by)
    prof_b = sk_b.profiles.item(0)
    cap_b = revolve_new_body(root, prof_b, axis_b, 180.0)

    solid = combine_join(root, straight, [cap_a, cap_b])
    return solid


def combine_intersect_keep(root, target, tools):
    coll = adsk.core.ObjectCollection.create()
    for t in tools:
        coll.add(t)
    cf = root.features.combineFeatures
    inp = cf.createInput(target, coll)
    inp.operation = adsk.fusion.FeatureOperations.IntersectFeatureOperation
    inp.isKeepToolBodies = True
    feat = cf.add(inp)
    return feat.bodies.item(0) if feat.bodies.count else target


def build_inner_cavity_clip_tool(root, p, safety_margin=0.1):
    """A single inner-cavity solid, shrunk inward by safety_margin, meant
    to be reused across MANY clip_to_inner_cavity calls (see that
    function) via combine_intersect_keep instead of rebuilt from scratch
    each time -- rebuilding the whole extrude+2-revolve+2-join inner solid
    per boss/post (there can be 10+) was slow enough to risk the MCP call
    timing out. Caller is responsible for hiding/renaming it as
    'reference only' once done (see build())."""
    inner = build_inner_pill_solid(root, p)
    faces = [f for f in inner.faces]
    offset_input = root.features.offsetFacesFeatures.createInput(faces, V(-safety_margin))
    root.features.offsetFacesFeatures.add(offset_input)
    return inner


def clip_to_inner_cavity(root, body, p, clip_tool=None):
    """Intersect `body` with the inner cavity solid so it can never poke
    through the outer shell -- used for case-screw bosses and Top posts,
    whose reference positions were sized against the R30/R28 'current'
    envelope and can otherwise punch through the narrower trim shell. Pass
    a shared `clip_tool` (see build_inner_cavity_clip_tool) to reuse one
    solid across many calls instead of rebuilding it every time."""
    if clip_tool is not None:
        return combine_intersect_keep(root, body, [clip_tool])
    inner = build_inner_cavity_clip_tool(root, p)
    return combine_intersect(root, body, [inner])


def build_thickened_envelope(root, p, offset_mm):
    """A fresh copy of the outer envelope solid (see build_outer_pill_solid),
    offset outward by offset_mm on every face (OffsetFaces per SPEC.md
    gotcha 3 -- createInput takes a Python list of faces). Used
    (2026-09-04) to trim button caps flush with the REAL curved shell
    instead of the flat-wall approximation used to build them: intersecting
    a cap with this thickened solid clips anything that would poke out
    past (true outer surface + offset_mm), everywhere, following the
    actual curvature."""
    solid = build_outer_pill_solid(root, p)
    faces = [f for f in solid.faces]
    offset_input = root.features.offsetFacesFeatures.createInput(faces, V(offset_mm))
    root.features.offsetFacesFeatures.add(offset_input)
    return solid


def build_outer_pill_solid(root, p):
    """Straight-section extrude + two end-cap revolves, unioned into one
    solid outer pill body (before shelling / splitting)."""
    ax, ay = p['spine_a']
    bx, by = p['spine_b']
    assert ax == 0.0 and bx == 0.0, 'spine must be vertical (x=0) in this generator'
    spine_len = by - ay

    # straight prism
    sk_full = sketch_full_profile(root, root.xZConstructionPlane, p, y0=ay)
    prof = None
    for pr in sk_full.profiles:
        prof = pr
        break
    straight = extrude_new_body(root, prof, spine_len, direction='positive')

    # end cap at spine_a (sweeps into y < ay)
    sk_a, axis_a = sketch_half_profile(root, root.xZConstructionPlane, p, y0=ay)
    prof_a = sk_a.profiles.item(0)
    cap_a = revolve_new_body(root, prof_a, axis_a, -180.0)

    # end cap at spine_b (sweeps into y > by) -- needs a plane offset to y=by
    planes = root.constructionPlanes
    plane_in = planes.createInput()
    plane_in.setByOffset(root.xZConstructionPlane, V(by))
    plane_b = planes.add(plane_in)
    sk_b, axis_b = sketch_half_profile(root, plane_b, p, y0=by)
    prof_b = sk_b.profiles.item(0)
    cap_b = revolve_new_body(root, prof_b, axis_b, 180.0)

    # sanity: caps should extend outward from the straight section, not overlap
    bb_straight = bbox_of(straight)
    bb_a = bbox_of(cap_a)
    bb_b = bbox_of(cap_b)
    assert bb_a['y'][1] <= ay + 0.05, ('cap_a swept the wrong way', bb_a)
    assert bb_b['y'][0] >= by - 0.05, ('cap_b swept the wrong way', bb_b)

    solid = combine_join(root, straight, [cap_a, cap_b])
    return solid


def hollow_and_split(root, outer_solid, p):
    """Cut the (simple-fillet) inner cavity solid from the outer solid, then
    split at z=split_z into Bottom/Top. We build the cavity as its own solid
    and boolean-cut it rather than using Fusion's Shell feature: Shell does
    a true perpendicular offset of the compound outer curve, which carries
    the 45deg shoulder through to the inside and does NOT match SPEC.md's
    cavity probe numbers (verified empirically -- see _inner_profile_geometry
    docstring). Building the inner solid explicitly, from the same kind of
    profile-extrude+revolve construction as the outer solid, gives an exact
    match."""
    inner_solid = build_inner_pill_solid(root, p)
    hollow = combine_cut(root, outer_solid, [inner_solid])

    planes = root.constructionPlanes
    plane_in = planes.createInput()
    plane_in.setByOffset(root.xYConstructionPlane, V(p['split_z']))
    split_plane = planes.add(plane_in)

    splitFeats = root.features.splitBodyFeatures
    splitInput = splitFeats.createInput(hollow, split_plane, True)
    splitFeats.add(splitInput)

    bottom = top = None
    for b in root.bRepBodies:
        bb = bbox_of(b)
        if abs(bb['z'][0] - p['bottom_z']) < 0.05:
            bottom = b
        elif abs(bb['z'][1] - p['top_z']) < 0.05:
            top = b
    assert bottom is not None and top is not None, 'split did not produce Bottom/Top'
    bottom.name = 'Bottom'
    top.name = 'Top'
    return bottom, top


def add_lip_anchor_reliefs(root, bodies, p):
    ay, by = p['spine_a'][1], p['spine_b'][1]
    lip = stadium_ring_solid(root, ay, by, p['lip_r'][0], p['lip_r'][1], p['lip_z'][0], p['lip_z'][1])
    anchor = stadium_ring_solid(root, ay, by, p['anchor_r'][0], p['anchor_r'][1], p['anchor_z'][0], p['anchor_z'][1])
    top = combine_join(root, bodies['Top'], [lip, anchor])

    relief_z0, relief_z1 = p['lip_z'][0] - 0.5, p['anchor_z'][1] + 0.5
    for s in p['screws_ABC'] + [p['screw_D']]:
        cx, cy = s['xy']
        relief = cylinder_solid(root, cx, cy, p['boss_relief_dia'] / 2.0, relief_z0, relief_z1)
        top = combine_cut(root, top, [relief])

    lb = p['lug_relief_box']
    relief_box = box_solid(root, lb['x'][0], lb['x'][1], lb['y'][0], lb['y'][1], relief_z0, relief_z1)
    top = combine_cut(root, top, [relief_box])

    bodies['Top'] = top
    return bodies


def add_window(root, bodies, p):
    cx, cy = p['window_center']
    r = p['window_dia'] / 2.0
    bore = cylinder_solid(root, cx, cy, r, p['window_z_bottom'], p['top_z'] + 1.0)
    top = combine_cut(root, bodies['Top'], [bore])
    bodies['Top'] = top

    chamfer_edge_at(root, top, (cx, cy), r, p['top_z'], p['window_chamfer'])
    return bodies


_CIRCULAR_CURVE_TYPES = (
    adsk.core.Curve3DTypes.Circle3DCurveType,
    adsk.core.Curve3DTypes.Arc3DCurveType,
)


def chamfer_edge_at(root, body, center_xy, radius_mm, z_mm, chamfer_mm, tol=0.05):
    """Collect ALL matching circular/arc edges at this center+radius+z --
    Fusion represents even full-circle bore edges as Arc3D (not Circle3D),
    and per SPEC.md's gotcha (8) a chamfer edge can be split into several
    arcs, so we must not assume there's exactly one."""
    edges_to_chamfer = adsk.core.ObjectCollection.create()
    for edge in body.edges:
        geo = edge.geometry
        if geo.curveType not in _CIRCULAR_CURVE_TYPES:
            continue
        c = geo.center
        cx, cy, cz = c.x / MM, c.y / MM, c.z / MM
        rad = geo.radius / MM
        if (abs(cx - center_xy[0]) < tol and abs(cy - center_xy[1]) < tol
                and abs(cz - z_mm) < tol and abs(rad - radius_mm) < tol):
            edges_to_chamfer.add(edge)
    assert edges_to_chamfer.count > 0, f'no matching edge at {center_xy}, z={z_mm}, r={radius_mm}'
    chamferFeats = root.features.chamferFeatures
    inp = chamferFeats.createInput(edges_to_chamfer, True)
    inp.setToEqualDistance(V(chamfer_mm))
    chamferFeats.add(inp)


def dedupe_body(root, tracked_body, base_name):
    """Delete any stale same-named duplicate bodies via a Remove feature
    (2026-09-05 fix): intersecting a cylinder_solid boss/post against the
    revolve-based inner-cavity clip tool, then joining the clipped result
    into 'Bottom'/'Top', was silently leaving the PRE-join body behind as
    an orphaned duplicate ('Bottom (1)', 'Top (1)', ...) instead of
    updating in place -- reproduced in isolation down to exactly this
    combination (a real cylinder_solid intersected with the curved/
    filleted clip tool); root cause not identified further. The orphan is
    geometrically just a strict SUBSET of tracked_body's current volume
    (a snapshot from before that boss/post's join), so it is safe to
    delete outright -- joining it back in (tried first) itself raised
    'Some input argument is invalid', consistent with it being a fragile
    byproduct of the same underlying issue."""
    # Compare by NAME PATTERN, not Python object identity: iterating
    # root.bRepBodies can hand back a fresh proxy object for the same
    # underlying body each time, so `b is not tracked_body` is unreliable
    # and (confirmed by testing) can end up matching -- and then
    # deleting -- the current body too. Fusion's own auto-rename on a name
    # collision always keeps the ORIGINAL exact name and suffixes the new
    # arrival as 'Name (N)', so the orphans are unambiguously exactly the
    # '(N)'-suffixed ones; the bare base_name is always the live one.
    stale = [b for b in root.bRepBodies
             if b.name.startswith(base_name + ' (') and b.name.endswith(')')]
    if not stale:
        return tracked_body
    for b in stale:
        root.features.removeFeatures.add(b)
    # inserting a Remove feature can invalidate previously-held BRepBody
    # Python references (even to bodies not directly removed) -- re-fetch
    # tracked_body fresh by name rather than keep using the old handle.
    for b in root.bRepBodies:
        if b.name == base_name:
            return b
    raise AssertionError(f'dedupe_body: {base_name!r} not found after cleanup')


def add_case_boss(root, bodies, cx, cy, p, is_D=False, clip_tool=None):
    boss_r = p['boss_dia'] / 2.0
    bottom_boss = clip_to_inner_cavity(
        root, cylinder_solid(root, cx, cy, boss_r, 2.0, p['split_z']), p, clip_tool)
    bottom = combine_join(root, bodies['Bottom'], [bottom_boss])
    if clip_tool is not None:
        bottom = dedupe_body(root, bottom, 'Bottom')
    hole = cylinder_solid(root, cx, cy, p['screw_hole_dia'] / 2.0, -0.5, p['split_z'] + 0.5)
    bottom = combine_cut(root, bottom, [hole])
    cb_h = p['counterbore_D_h'] if is_D else p['counterbore_ABC_h']
    cb = cylinder_solid(root, cx, cy, p['counterbore_ABC_dia'] / 2.0, -0.5, cb_h)
    bottom = combine_cut(root, bottom, [cb])
    bodies['Bottom'] = bottom

    if not is_D:
        top_boss = clip_to_inner_cavity(
            root, cylinder_solid(root, cx, cy, boss_r, p['split_z'], p['top_ceiling_underside_z']), p, clip_tool)
        top = combine_join(root, bodies['Top'], [top_boss])
        if clip_tool is not None:
            top = dedupe_body(root, top, 'Top')
        pilot = cylinder_solid(root, cx, cy, p['top_pilot_dia'] / 2.0, p['top_pilot_z'][0], p['top_pilot_z'][1])
        top = combine_cut(root, top, [pilot])
        bodies['Top'] = top
    return bodies


def add_case_screws(root, bodies, p, clip_tool=None):
    for s in p['screws_ABC']:
        cx, cy = s['xy']
        bodies = add_case_boss(root, bodies, cx, cy, p, is_D=False, clip_tool=clip_tool)
    cx, cy = p['screw_D']['xy']
    bodies = add_case_boss(root, bodies, cx, cy, p, is_D=True, clip_tool=clip_tool)
    return bodies


def add_top_posts(root, bodies, p, clip_tool=None):
    top = bodies['Top']
    for name, (cx, cy) in p['top_posts'].items():
        post = clip_to_inner_cavity(
            root, cylinder_solid(root, cx, cy, p['top_post_dia'] / 2.0, p['top_post_z'][0], p['top_post_z'][1]),
            p, clip_tool)
        top = combine_join(root, top, [post])
        if clip_tool is not None:
            top = dedupe_body(root, top, 'Top')
        hole = cylinder_solid(root, cx, cy, p['top_post_pilot_dia'] / 2.0,
                               p['top_post_pilot_z'][0], p['top_post_pilot_z'][1])
        top = combine_cut(root, top, [hole])
    bodies['Top'] = top
    return bodies


def build_screen_plate(root, p):
    po = p['plate_outline']
    z0, z1 = p['plate_z']
    plate = box_solid(root, po['x'][0], po['x'][1], po['y'][0], po['y'][1], z0, z1)

    hc = p['plate_header_cutout']
    cutout = box_solid(root, hc['x'][0], hc['x'][1], hc['y'][0], hc['y'][1], z0 - 0.5, z1 + 0.5)
    plate = combine_cut(root, plate, [cutout])

    all_holes = dict(p['top_posts'])
    all_holes.update(p['board_standoffs'])
    for name, (cx, cy) in all_holes.items():
        hole = cylinder_solid(root, cx, cy, p['plate_hole_dia'] / 2.0, z0 - 0.5, z1 + 0.5)
        plate = combine_cut(root, plate, [hole])

    # post for screw D (fuses onto the plate's underside, z (10, plate_z0))
    dx, dy = p['screw_D']['xy']
    pz0, pz1 = p['plate_post_D_z']
    post = cylinder_solid(root, dx, dy, p['boss_dia'] / 2.0, pz0, pz1)
    post_hole = cylinder_solid(root, dx, dy, p['top_pilot_dia'] / 2.0, pz0 - 0.5, pz1 + 0.5)
    post = combine_cut(root, post, [post_hole])
    plate = combine_join(root, plate, [post])

    plate.name = 'Screen Plate'
    return plate


def get_open_doc(app, name_substring):
    for d in app.documents:
        if name_substring in d.name:
            return d
    return None


def get_reference_transform(app, ref_doc_name_substring, occ_name_substring):
    ref_doc = get_open_doc(app, ref_doc_name_substring)
    assert ref_doc is not None, f'reference doc not found (open it in Fusion first): {ref_doc_name_substring}'
    ref_design = adsk.fusion.Design.cast(ref_doc.products.itemByProductType('DesignProductType'))
    ref_root = ref_design.rootComponent
    for occ in ref_root.occurrences:
        if occ_name_substring in occ.name:
            return occ.transform
    raise AssertionError(f'occurrence not found in {ref_doc_name_substring}: {occ_name_substring}')


def insert_referenced_component(root, doc, transform=None):
    data_file = doc.dataFile
    mat = transform if transform is not None else adsk.core.Matrix3D.create()
    return root.occurrences.addByInsert(data_file, mat, True)


def insert_display_pcba(app, root, p):
    transform = get_reference_transform(app, 'Firefly V2', p['display_doc_name'])
    disp_doc = get_open_doc(app, p['display_doc_name'])
    assert disp_doc is not None, f"display doc not open: {p['display_doc_name']}"
    occ = insert_referenced_component(root, disp_doc, transform)
    return occ


def normalize2(v):
    n = math.hypot(v[0], v[1])
    return (v[0] / n, v[1] / n)


def ray_box_exit_2d(center_xy, d_xy, bbox_x, bbox_y):
    """Distance along d_xy from center_xy to where the ray exits the 2D
    bbox -- used as a simple proxy for 'the switch housing surface facing
    the nub direction'."""
    cx, cy = center_xy
    dx, dy = d_xy

    def t_for(c, dcomp, lo, hi):
        if dcomp > 1e-9:
            return (hi - c) / dcomp
        elif dcomp < -1e-9:
            return (lo - c) / dcomp
        return float('inf')

    tx = t_for(cx, dx, bbox_x[0], bbox_x[1])
    ty = t_for(cy, dy, bbox_y[0], bbox_y[1])
    return min(tx, ty)


def button_geometry(p, switch_bbox, nub_dir, cap):
    """Compute the working points/axes for one side button.

    SIMPLIFICATION (documented in README): the outer wall in the button
    region is treated as the flat vertical plane x = -outer_radius, even
    though the real outer surface curves away above z=15 (the R10 shoulder
    fillet) and the cap z-range (up to ~19.6-20.0mm) reaches into that
    curved region. Modeling a cap that conforms to the compound curved
    shoulder was judged disproportionate to the remaining effort budget --
    the flat-wall approximation keeps the button mechanism's *dimensions*
    (hole clearance, plunger gap, pocket depth, tab gap) exactly matching
    SPEC.md, at the cost of a small (~1-2mm at the extremes) wall-contour
    mismatch that a human pass should true up before printing.
    """
    d2 = normalize2(nub_dir)
    t2 = (-d2[1], d2[0])
    cx = (switch_bbox['x'][0] + switch_bbox['x'][1]) / 2.0
    cy = (switch_bbox['y'][0] + switch_bbox['y'][1]) / 2.0
    switch_z_mid = (switch_bbox['z'][0] + switch_bbox['z'][1]) / 2.0

    t_exit = ray_box_exit_2d((cx, cy), d2, switch_bbox['x'], switch_bbox['y'])
    housing_xy = (cx + t_exit * d2[0], cy + t_exit * d2[1])

    wall_x = -p['outer_radius']
    s_wall = (wall_x - housing_xy[0]) / d2[0]           # param (along d) at the outer wall face
    s_inner = s_wall + p['wall'] / d2[0]                # param at the inner (cavity-side) wall face
    # REST position: the plunger tip sits plunger_travel further from the
    # housing than the FULL-PRESS gap (plunger_tip_gap) -- the collar
    # bottoms on the rib plunger_travel mm before the tip would reach the
    # housing (see PARAMS['plunger_travel'] / 'rib_*' / 'collar').
    s_plunger_tip = p['plunger_tip_gap'] + p['plunger_travel']
    s_outer_face = s_wall + cap['proud']
    s_tab_face = s_inner + p['tab']['gap'] / d2[0]

    # plunger guide rib: outboard face rib_inboard_offset mm inboard of the
    # outer wall face, rib_thickness mm thick along the travel axis.
    s_rib_outer = s_wall - p['rib_inboard_offset']
    s_rib_inner = s_rib_outer - p['rib_thickness']
    # inward stop collar: at rest, its outboard face sits plunger_travel mm
    # from the rib's inboard face; it is collar['len'] mm long along d.
    s_collar_outer = s_rib_inner - p['plunger_travel']
    s_collar_inner = s_collar_outer - p['collar']['len']

    def xy_at(s):
        return (housing_xy[0] + s * d2[0], housing_xy[1] + s * d2[1])

    return {
        'd': d2, 't': t2, 'housing_xy': housing_xy, 'switch_z_mid': switch_z_mid,
        's_wall': s_wall, 's_inner': s_inner, 's_plunger_tip': s_plunger_tip,
        's_outer_face': s_outer_face, 's_tab_face': s_tab_face,
        's_rib_outer': s_rib_outer, 's_rib_inner': s_rib_inner,
        's_collar_outer': s_collar_outer, 's_collar_inner': s_collar_inner,
        'outer_face_xy': xy_at(s_outer_face), 'plunger_tip_xy': xy_at(s_plunger_tip),
        'tab_face_xy': xy_at(s_tab_face),
        'rib_center_xy': xy_at((s_rib_outer + s_rib_inner) / 2.0),
        'collar_center_xy': xy_at((s_collar_outer + s_collar_inner) / 2.0),
    }


def add_button(root, bodies, name, switch_bbox, nub_dir, cap, hole_wh, p, thickened_envelope=None):
    g = button_geometry(p, switch_bbox, nub_dir, cap)
    d2, t2 = g['d'], g['t']
    d3, t3 = (d2[0], d2[1], 0.0), (t2[0], t2[1], 0.0)
    z3 = (0.0, 0.0, 1.0)
    z_lo, z_hi = cap['z']
    z_center = (z_lo + z_hi) / 2.0
    L, W = cap['stadium']

    # wall hole: through the wall thickness only, sized head + clearance/side
    hole_L, hole_W = hole_wh
    hole_center = ((g['outer_face_xy'][0] + g['tab_face_xy'][0]) / 2.0,
                   (g['outer_face_xy'][1] + g['tab_face_xy'][1]) / 2.0)
    hole_depth = abs(g['s_outer_face'] - g['s_tab_face']) + 1.0
    hole_start = (hole_center[0] - (hole_depth / 2.0) * d2[0], hole_center[1] - (hole_depth / 2.0) * d2[1])
    # both button caps' z-ranges (13.4-20.0mm) sit entirely above split_z
    # (10mm), i.e. inside the Top body only.
    hole_body = oriented_stadium_prism(root, (hole_start[0], hole_start[1], z_center),
                                        t3, z3, d3, hole_L, hole_W, hole_depth)
    bodies['Top'] = combine_cut(root, bodies['Top'], [hole_body])

    # cap body: uniform stadium prism from the outer (proud) face inward to
    # the plunger tip. Built with EXTRA margin outward past the nominal
    # (flat-wall) outer face, then trimmed back with a Combine-Intersect
    # against a thickened copy of the real curved outer envelope (2026-09-04
    # fix) -- the flat-wall approximation used to size outer_face_xy can be
    # either proud of or short of the true curved+0.45mm surface depending
    # on where along the shoulder curve the button sits, so we over-build
    # and let the intersect cut it back to exactly 0.45mm proud everywhere.
    margin = 3.0
    total_depth = (g['s_outer_face'] - g['s_plunger_tip']) + margin
    neg_d3 = (-d3[0], -d3[1], -d3[2])
    extended_outer_xy = (g['outer_face_xy'][0] + margin * d2[0], g['outer_face_xy'][1] + margin * d2[1])
    cap_center = (extended_outer_xy[0], extended_outer_xy[1], z_center)
    cap_body = oriented_stadium_prism(root, cap_center, t3, z3, neg_d3, L, W, total_depth)
    if thickened_envelope is not None:
        cap_body = combine_intersect_keep(root, cap_body, [thickened_envelope])
    else:
        cap_body = combine_intersect(root, cap_body, [build_thickened_envelope(root, p, cap['proud'])])

    # nub pocket at the plunger tip, recessed 0.8mm back toward the outer face
    pocket = p['nub_pocket']
    pocket_center = (g['plunger_tip_xy'][0], g['plunger_tip_xy'][1], g['switch_z_mid'])
    pocket_body = oriented_box_prism(root, pocket_center, t3, z3, d3,
                                      pocket['xy'][0], pocket['xy'][1], pocket['depth'])
    cap_body = combine_cut(root, cap_body, [pocket_body])

    # retaining tab, centered on the plunger axis, offset down in Z, whose
    # outward face sits at s_tab_face (0.60mm inside the inner wall)
    tab = p['tab']
    tab_len_along_d = 1.5
    tab_z = z_center - W / 2.0 - tab['h'] / 2.0
    tab_start_xy = (g['tab_face_xy'][0] - tab_len_along_d * d2[0] / 2.0,
                    g['tab_face_xy'][1] - tab_len_along_d * d2[1] / 2.0)
    tab_body = oriented_box_prism(root, (tab_start_xy[0], tab_start_xy[1], tab_z), t3, z3, d3,
                                   tab['w'], tab['h'], tab_len_along_d)
    cap_body = combine_join(root, cap_body, [tab_body])

    # --- plunger guide rib (joined to Top) + inward stop collar (joined to
    # the cap) -- added per Jake's 2026-09-04 print-test feedback: the rib
    # keeps the plunger from tilting/rotating, and the collar bottoms on it
    # plunger_travel before the tip would reach the switch housing, so a
    # hard press loads the rib/case instead of the switch's solder joints.
    attach_margin = 2.0  # rib plate extends this far beyond the plunger cross-section, to attach to nearby wall material
    rib_len = p['rib_thickness']
    rib_center = (g['rib_center_xy'][0], g['rib_center_xy'][1], z_center)
    rib_plate = oriented_box_prism(root, rib_center, t3, z3, d3,
                                    L + 2 * attach_margin, W + 2 * attach_margin, rib_len)
    slot_L = L + 2 * p['rib_slot_clearance']
    slot_W = W + 2 * p['rib_slot_clearance']
    slot_body = oriented_box_prism(root, rib_center, t3, z3, d3, slot_L, slot_W, rib_len + 1.0)
    rib_plate = combine_cut(root, rib_plate, [slot_body])
    bodies['Top'] = combine_join(root, bodies['Top'], [rib_plate])

    collar = p['collar']
    collar_center = (g['collar_center_xy'][0], g['collar_center_xy'][1], z_center)
    collar_body = oriented_box_prism(root, collar_center, t3, z3, d3,
                                      L, W + 2 * collar['h'], collar['len'])
    cap_body = combine_join(root, cap_body, [collar_body])

    cap_body.name = name
    bodies[name] = cap_body
    return bodies


def add_buttons(root, bodies, p):
    # shared thickened envelope (both caps use the same proud amount) --
    # rebuilding the whole outer pill solid + OffsetFaces per button was
    # part of what made the post-redesign build slow enough to risk the
    # MCP call timing out.
    assert p['power_cap']['proud'] == p['home_cap']['proud'], 'shared thickened envelope assumes equal proud amounts'
    thickened_envelope = build_thickened_envelope(root, p, p['power_cap']['proud'])

    bodies = add_button(root, bodies, 'Power Button', p['switch_power_bbox'], p['power_nub_dir'],
                         p['power_cap'], (p['power_cap']['stadium'][0] + 2 * p['cap_clearance'],
                                          p['power_cap']['stadium'][1] + 2 * p['cap_clearance']), p,
                         thickened_envelope=thickened_envelope)
    home_bbox = dict(p['switch_home_bbox'])
    home_bbox['z'] = p['switch_power_bbox']['z']  # z not separately specified in SPEC.md; reuse power's
    bodies = add_button(root, bodies, 'Home Button', home_bbox, p['home_nub_dir'],
                         p['home_cap'], (p['home_cap']['stadium'][0] + 2 * p['cap_clearance'],
                                         p['home_cap']['stadium'][1] + 2 * p['cap_clearance']), p,
                         thickened_envelope=thickened_envelope)
    thickened_envelope.name = 'Cap Trim Envelope (reference only)'
    thickened_envelope.isLightBulbOn = False
    return bodies


def add_usb_tunnel(root, bodies, p):
    wall_y = p['spine_b'][1] + p['outer_radius']
    cx, cz = 0.0, p['usb_tunnel_center_z']
    L_in, W_in = p['usb_tunnel_stadium']
    L_out, W_out = p['usb_liner_outer_stadium']
    y_start = p['usb_tunnel_y_start']

    bore_depth = (wall_y - y_start) + 1.0
    bore = oriented_stadium_prism(root, (cx, y_start, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                   L_in, W_in, bore_depth)
    bodies['Top'] = combine_cut(root, bodies['Top'], [bore])

    # liner: built with extra margin past the flat-wall estimate of wall_y,
    # then trimmed to the REAL curved outer envelope with a Combine-
    # Intersect (2026-09-04 fix, "as v15 did with a split") -- the +y end
    # is the domed cap, not a flat wall, so a fixed liner_depth either
    # falls short of or overshoots the true surface depending on x.
    margin = 3.0
    liner_depth = (wall_y - y_start) + margin
    liner_outer = oriented_stadium_prism(root, (cx, y_start, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                          L_out, W_out, liner_depth)
    liner_inner = oriented_stadium_prism(root, (cx, y_start - 0.5, cz), (1, 0, 0), (0, 0, 1), (0, 1, 0),
                                          L_in, W_in, liner_depth + 1.0)
    liner = combine_cut(root, liner_outer, [liner_inner])
    envelope = build_outer_pill_solid(root, p)
    liner = combine_intersect(root, liner, [envelope])
    bodies['Top'] = combine_join(root, bodies['Top'], [liner])
    return bodies


def add_lug(root, bodies, p):
    lug = p['lug']
    x0, x1 = lug['x']
    y_root = lug['y_root']
    tip_r = lug['tip_r']
    y_tip = lug['y_tip']
    z0, z1 = lug['z']
    cap_center_y = y_tip + tip_r

    box = box_solid(root, x0, x1, y_root, cap_center_y, z0, z1)
    cap = cylinder_solid(root, 0.0, cap_center_y, tip_r, z0, z1)
    tab = combine_join(root, box, [cap])
    hole = cylinder_solid(root, lug['hole_xy'][0], lug['hole_xy'][1], lug['hole_dia'] / 2.0, z0 - 0.5, z1 + 0.5)
    tab = combine_cut(root, tab, [hole])

    bodies['Bottom'] = combine_join(root, bodies['Bottom'], [tab])
    return bodies


def _polyline_loop_lines(sk, loop_pts, z_mm):
    n = len(loop_pts)
    if n < 3:
        return
    for i in range(n):
        a = loop_pts[i]
        b = loop_pts[(i + 1) % n]
        add_line(sk, P(a[0], a[1], z_mm), P(b[0], b[1], z_mm))


def deboss_loops(root, body, loops_xy, z_mm, depth_mm, cut_direction):
    """loops_xy: list of closed polygon point lists (world mm, at height
    z_mm). Extrudes each resulting sketch profile `depth_mm` along
    cut_direction (+1 or -1 in Z) and cuts the union from `body`."""
    plane = plane_at_z(root, z_mm)
    sk = new_sketch(root, plane)
    for loop in loops_xy:
        _polyline_loop_lines(sk, loop, z_mm)
    if sk.profiles.count == 0:
        return body
    direction = 'positive' if cut_direction > 0 else 'negative'
    tools = []
    for i in range(sk.profiles.count):
        prof = sk.profiles.item(i)
        ext = root.features.extrudeFeatures
        inp = ext.createInput(prof, adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
        sign = 1.0 if direction == 'positive' else -1.0
        inp.setDistanceExtent(False, V(sign * depth_mm))
        feat = ext.add(inp)
        # a noisy/self-intersecting stroked polyline profile can make ONE
        # extrude produce SEVERAL bodies -- collect all of them, not just
        # bodies.item(0), or the rest leak into the document unmerged.
        for b in feat.bodies:
            tools.append(b)
    if not tools:
        return body
    # NOTE: do not try to Join these into one body first -- the glyph
    # pieces are deliberately disjoint (e.g. the flare's rays start at
    # r=2.2mm, outside the r=1.5mm center circle) and Fusion's Join
    # silently no-ops on non-touching bodies instead of producing a
    # multi-lump result. Cut supports multiple disjoint tool bodies in one
    # operation directly, so pass them all at once.
    return combine_cut(root, body, tools)


def flare_glyph_loops(p):
    f = p['flare']
    center_r = f['center_dia'] / 2.0
    r0 = f['bar_start_r']
    w0, w1 = f['bar_w']
    loops = []
    for k in range(8):
        theta = math.radians(45.0 * k)
        is_axis = (k % 2 == 0)
        ray_len = f['long_ray'] if is_axis else f['short_ray']
        r1 = r0 + ray_len
        dirv = (math.cos(theta), math.sin(theta))
        perp = (-math.sin(theta), math.cos(theta))
        p1 = (r0 * dirv[0] + (w0 / 2.0) * perp[0], r0 * dirv[1] + (w0 / 2.0) * perp[1])
        p2 = (r0 * dirv[0] - (w0 / 2.0) * perp[0], r0 * dirv[1] - (w0 / 2.0) * perp[1])
        p3 = (r1 * dirv[0] - (w1 / 2.0) * perp[0], r1 * dirv[1] - (w1 / 2.0) * perp[1])
        p4 = (r1 * dirv[0] + (w1 / 2.0) * perp[0], r1 * dirv[1] + (w1 / 2.0) * perp[1])
        loops.append([p1, p2, p3, p4])
    # center circle, approximated as a 32-gon (keeps deboss_loops uniform --
    # plain line segments, no separate circle-curve code path needed)
    n = 32
    circle = [(center_r * math.cos(2 * math.pi * i / n), center_r * math.sin(2 * math.pi * i / n))
              for i in range(n)]
    loops.append(circle)
    return loops


def add_flare_logo(root, bodies, p):
    cx, cy = p['flare_center']
    loops = flare_glyph_loops(p)
    world_loops = [[(pt[0] + cx, pt[1] + cy) for pt in loop] for loop in loops]
    z_top = p['top_z']
    depth = p['logo_deboss_depth']
    bodies['Top'] = deboss_loops(root, bodies['Top'], world_loops, z_top, depth, cut_direction=-1)
    return bodies


def load_wordmark_loops(p):
    json_path = os.path.join(_HERE, 'kandiwooks_logo.json')
    with open(json_path, 'r') as f:
        data = json.load(f)
    raw_loops = []
    for body in data:
        for loop in body['loops']:
            pts = loop['points']
            if len(pts) >= 3:
                raw_loops.append(pts)

    all_x = [pt[0] for loop in raw_loops for pt in loop]
    all_y = [pt[1] for loop in raw_loops for pt in loop]
    minx, maxx = min(all_x), max(all_x)
    miny, maxy = min(all_y), max(all_y)
    local_cx, local_cy = (minx + maxx) / 2.0, (miny + maxy) / 2.0
    scale = p['wordmark_width'] / (maxx - minx)

    cx, cy = p['wordmark_center']
    world_loops = []
    for loop in raw_loops:
        wl = []
        for lx, ly in loop:
            sx = (lx - local_cx) * scale
            sy = (ly - local_cy) * scale
            sx = -sx  # mirror in x so it reads correctly when the puck is flipped
            wl.append((sx + cx, sy + cy))
        world_loops.append(wl)
    return world_loops


def add_wordmark_logo(root, bodies, p):
    world_loops = load_wordmark_loops(p)
    z_bot = p['bottom_z']
    depth = p['logo_deboss_depth']
    bodies['Bottom'] = deboss_loops(root, bodies['Bottom'], world_loops, z_bot, depth, cut_direction=1)
    return bodies


# ---------------------------------------------------------------------------
# M3: comms bay (battery / GPS / XIAO+Wio+L76K stack) + board inserts
# ---------------------------------------------------------------------------
def safe_half_width(p, y, z, margin=2.0):
    """Conservative safe |x| bound for bay geometry at (y,z): the pill's
    inner cavity is a REVOLVE of the same rho(z) profile around each spine
    endpoint, so away from the straight section (y outside
    [spine_a.y, spine_b.y]) the safe half-width shrinks radially from the
    nearest spine point, not just with z. Used to keep bay pockets (whose
    footprints in SPEC.md were sized against a flat mid-height cross
    section) from bulging through the actual curved/domed shell -- this is
    a real limitation of the current bay layout (see README): several bay
    footprints, especially in the trim variant and near the -y end cap,
    are tighter than the nominal 'cavity 52mm wide' figure once the floor's
    curvature is accounted for."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    r = inner_rho_at_z(p, z)
    if y < ay:
        d = ay - y
    elif y > by:
        d = y - by
    else:
        d = 0.0
    val = math.sqrt(max(r * r - d * d, 0.0))
    return max(val - margin, 0.1)


def clip_box_x_to_cavity(p, x0, x1, y0, y1, z0, z1, margin=2.0):
    """Returns (x0, x1) clipped to fit the safe cavity envelope, or None if
    the requested box doesn't fit at all at this (y,z) -- callers should
    skip building that piece rather than create a degenerate sliver."""
    lim = min(safe_half_width(p, y, z, margin) for y in (y0, y1) for z in (z0, z1))
    new_x0, new_x1 = max(x0, -lim), min(x1, lim)
    if new_x1 - new_x0 < 1.0:
        return None
    return new_x0, new_x1


def add_battery_bay(root, bodies, p):
    """Battery retention only -- the battery itself has no Fusion doc, so
    it is modeled as a hidden reference box (add_battery_reference_box)."""
    bat = p['bay']['battery']
    x0, x1 = bat['x']
    y0, y1 = bat['y']
    rail_w = p['bay']['battery_rail_w']
    rz0, rz1 = p['bay']['battery_rail_z']

    rail_l = box_solid(root, x0 - rail_w, x0, y0, y1, rz0, rz1)
    rail_r = box_solid(root, x1, x1 + rail_w, y0, y1, rz0, rz1)
    bodies['Bottom'] = combine_join(root, bodies['Bottom'], [rail_l, rail_r])

    # a hook-and-loop strap pair goes THROUGH the rails (not the floor bed
    # face) at 1/3 and 2/3 along the battery's length
    strap = p['bay']['battery_strap']
    for frac in (1.0 / 3.0, 2.0 / 3.0):
        yc = y0 + frac * (y1 - y0)
        slot_l = box_solid(root, x0 - rail_w - 0.5, x0 + 0.5,
                            yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        slot_r = box_solid(root, x1 - 0.5, x1 + rail_w + 0.5,
                            yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        bodies['Bottom'] = combine_cut(root, bodies['Bottom'], [slot_l, slot_r])
    return bodies


def add_battery_reference_box(root, p):
    """803040 LiPo has no Fusion doc -- modeled as a hidden reference box
    (excluded from exports) purely to support interference checking."""
    bat = p['bay']['battery']
    body = box_solid(root, bat['x'][0], bat['x'][1], bat['y'][0], bat['y'][1], bat['z'][0], bat['z'][1])
    body.name = 'Battery Reference (reference only)'
    body.isLightBulbOn = False
    return body


def add_l76k_wired_frame(root, bodies, p):
    """L76K is always wired (2026-09-04: 'hat' mode dropped) -- flat frame
    in the dome tip with a wire notch on the +y side."""
    fr = p['bay']['l76k_wired']
    x0, x1 = fr['x']
    y0, y1 = fr['y']
    z0, z1 = fr['z']
    wall = p['bay']['l76k_frame_wall']
    clear = p['bay']['l76k_frame_clear']

    outer = box_solid(root, x0 - clear - wall, x1 + clear + wall, y0 - clear - wall, y1 + clear + wall,
                       z0, z1 + 0.3)
    inner = box_solid(root, x0 - clear, x1 + clear, y0 - clear, y1 + clear, z0 - 0.5, z1 + 0.8)
    frame = combine_cut(root, outer, [inner])

    notch_w = p['bay']['l76k_wire_notch_w']
    notch = box_solid(root, -notch_w / 2.0, notch_w / 2.0,
                       y1 + clear - 0.5, y1 + clear + wall + 0.5, z0, z1 + 0.3)
    frame = combine_cut(root, frame, [notch])

    bodies['Bottom'] = combine_join(root, bodies['Bottom'], [frame])
    return bodies


def build_hanging_frame(root, x0, x1, y0, y1, clearance, wall, z_bottom, z_ceiling,
                         ledge_w=0.0, ledge_h=0.0, gap_w=0.0, gap_side='+y', gap_center=None):
    """A thin-walled box 'ring' hanging from the ceiling (z_ceiling) down to
    z_bottom, open top and bottom, around the (x0,y0)-(x1,y1) footprint +
    clearance. Used for the stack tray and GPS frame so Bottom/Top both
    print face-down with no overhangs (2026-09-04 bay redesign): the ring
    itself needs no bridging since it is a vertical wall, and its top
    naturally fuses to the existing ceiling on join. Optional short-end
    ledges (a shelf protruding `ledge_w` inward at each y end, from
    z_bottom up by ledge_h) and a wire-clearance gap cut through one wall.
    """
    ox0, ox1 = x0 - clearance - wall, x1 + clearance + wall
    oy0, oy1 = y0 - clearance - wall, y1 + clearance + wall
    ix0, ix1 = x0 - clearance, x1 + clearance
    iy0, iy1 = y0 - clearance, y1 + clearance

    outer = box_solid(root, ox0, ox1, oy0, oy1, z_bottom, z_ceiling)
    inner = box_solid(root, ix0, ix1, iy0, iy1, z_bottom - 0.5, z_ceiling + 0.5)
    frame = combine_cut(root, outer, [inner])

    if ledge_w > 0:
        ledge1 = box_solid(root, ix0, ix1, iy0, iy0 + ledge_w, z_bottom, z_bottom + ledge_h)
        ledge2 = box_solid(root, ix0, ix1, iy1 - ledge_w, iy1, z_bottom, z_bottom + ledge_h)
        frame = combine_join(root, frame, [ledge1, ledge2])

    if gap_w > 0:
        gc = gap_center if gap_center is not None else (x0 + x1) / 2.0
        gz0, gz1 = z_bottom, z_bottom + 4.0
        if gap_side == '+y':
            notch = box_solid(root, gc - gap_w / 2.0, gc + gap_w / 2.0, oy1 - wall - 0.5, oy1 + 0.5, gz0, gz1)
        elif gap_side == '-y':
            notch = box_solid(root, gc - gap_w / 2.0, gc + gap_w / 2.0, oy0 - 0.5, oy0 + wall + 0.5, gz0, gz1)
        elif gap_side == '+x':
            notch = box_solid(root, ox1 - wall - 0.5, ox1 + 0.5, gc - gap_w / 2.0, gc + gap_w / 2.0, gz0, gz1)
        else:
            notch = box_solid(root, ox0 - 0.5, ox0 + wall + 0.5, gc - gap_w / 2.0, gc + gap_w / 2.0, gz0, gz1)
        frame = combine_cut(root, frame, [notch])

    return frame


def add_stack_tray(root, bodies, p):
    """XIAO+Wio stack support: an open-top/open-bottom tray hanging from
    the Top's ceiling (prints with no overhang), with a 2mm ledge at each
    short (y) end for the Wio PCB to rest on, and a 6mm wire-clearance gap
    on the +y side for the XIAO's USB-C/antenna wires. The stack itself is
    inserted from below before the halves close."""
    stack = p['bay']['stack']
    x0, x1 = stack['x']
    y0, y1 = stack['y']
    tray = build_hanging_frame(
        root, x0, x1, y0, y1, p['bay']['tray_clear'], p['bay']['tray_wall'],
        p['bay']['tray_z_bottom'], p['top_ceiling_underside_z'],
        ledge_w=p['bay']['tray_ledge']['w'], ledge_h=p['bay']['tray_ledge']['h'],
        gap_w=p['bay']['tray_gap']['w'], gap_side=p['bay']['tray_gap']['side'])
    bodies['Top'] = combine_join(root, bodies['Top'], [tray])
    return bodies


def add_gps_frame(root, bodies, p):
    """GPS patch antenna: same Top-hanging-frame scheme as the stack tray."""
    gps = p['bay']['gps_patch']
    x0, x1 = gps['x']
    y0, y1 = gps['y']
    clear = p['bay']['gps_frame_clear']
    frame = build_hanging_frame(
        root, x0, x1, y0, y1, clear, p['bay']['gps_frame_wall'],
        gps['z'][0] - clear, p['top_ceiling_underside_z'],
        ledge_w=2.0, ledge_h=1.0)
    bodies['Top'] = combine_join(root, bodies['Top'], [frame])
    return bodies


def add_gps_reference_box(root, p):
    """GPS patch antenna has no Fusion doc -- hidden reference box only."""
    gps = p['bay']['gps_patch']
    body = box_solid(root, gps['x'][0], gps['x'][1], gps['y'][0], gps['y'][1], gps['z'][0], gps['z'][1])
    body.name = 'GPS Patch Reference (reference only)'
    body.isLightBulbOn = False
    return body


def add_fpc_keepout_marker(root, p):
    """Construction-only reference body marking the LoRa FPC antenna
    keep-out strip on the Top's inner dome wall -- NOT joined/cut into any
    printed body, hidden, and excluded from exports."""
    ko = p['bay']['fpc_keepout']
    body = box_solid(root, ko['x'][0], ko['x'][1], ko['y'][0], ko['y'][1], ko['z'][0], ko['z'][1])
    body.name = 'FPC Keepout (reference only)'
    body.isLightBulbOn = False
    return body


def add_comms_bay(root, bodies, p):
    bodies = add_battery_bay(root, bodies, p)
    bodies = add_l76k_wired_frame(root, bodies, p)
    bodies = add_stack_tray(root, bodies, p)
    bodies = add_gps_frame(root, bodies, p)
    add_battery_reference_box(root, p)
    add_gps_reference_box(root, p)
    add_fpc_keepout_marker(root, p)
    return bodies


def _collect_occ_bodies(occ):
    out = []
    for b in occ.bRepBodies:
        out.append(b)
    for c in occ.childOccurrences:
        out.extend(_collect_occ_bodies(c))
    return out


def _bbox_extents(occ):
    """occ.boundingBox is unreliable (reads back as degenerate 0,0,0)
    immediately after addByInsert in the same script execution -- union
    the actual bRepBody bounding boxes (world-space, per SPEC.md gotcha 6)
    instead, which is correct right away."""
    bodies = _collect_occ_bodies(occ)
    assert bodies, 'inserted occurrence has no bRepBodies anywhere in its tree'
    xs, ys, zs = [], [], []
    for b in bodies:
        bb = b.boundingBox
        xs += [bb.minPoint.x, bb.maxPoint.x]
        ys += [bb.minPoint.y, bb.maxPoint.y]
        zs += [bb.minPoint.z, bb.maxPoint.z]
    dx, dy, dz = (max(xs) - min(xs)) / MM, (max(ys) - min(ys)) / MM, (max(zs) - min(zs)) / MM
    center = ((min(xs) + max(xs)) / 2.0 / MM, (min(ys) + max(ys)) / 2.0 / MM, (min(zs) + max(zs)) / 2.0 / MM)
    return dx, dy, dz, center


def flatten_transform(native_center_mm, thin_axis, target_center_mm):
    """Matrix3D that rotates the object (if needed) so its native
    `thin_axis` points along world Z, then relocates its (rotated) center
    from native_center_mm to target_center_mm --
    setToAlignCoordinateSystems does the rotate-about-a-pivot-then-move
    -the-pivot in one step. thin_axis='z' is a pure translation (identity
    rotation)."""
    if thin_axis == 'z':
        to_x, to_y, to_z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)
    elif thin_axis == 'y':
        to_x, to_y, to_z = (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, -1.0, 0.0)  # Y -> Z, Z -> -Y
    else:  # 'x'
        to_x, to_y, to_z = (0.0, 0.0, 1.0), (0.0, 1.0, 0.0), (-1.0, 0.0, 0.0)  # X -> Z, Z -> -X

    mat = adsk.core.Matrix3D.create()
    ok = mat.setToAlignCoordinateSystems(
        P(*native_center_mm), adsk.core.Vector3D.create(1, 0, 0),
        adsk.core.Vector3D.create(0, 1, 0), adsk.core.Vector3D.create(0, 0, 1),
        P(*target_center_mm), adsk.core.Vector3D.create(*to_x),
        adsk.core.Vector3D.create(*to_y), adsk.core.Vector3D.create(*to_z))
    assert ok, 'setToAlignCoordinateSystems failed'
    return mat


def insert_and_place(design, root, doc, target_center_fn, thin_axis=None):
    """Insert `doc` as a referenced occurrence and rigidly place it
    (2026-09-04 fix): read its native bbox, rotate its thin axis (given, or
    auto-detected as the smallest extent) onto world Z, translate its
    center to target_center_fn(dx, dy, dz) -- a callback so the caller can
    use the board's own flattened thickness (dz after a 'y'/'x' rotation
    is the native dy/dx) to place its PCB bottom at a specific height --
    then COMMIT via design.snapshots: per Jake, without a snapshot the
    assigned transform can be left 'pending' and bounding-box reads come
    back stale/unchanged. Returns (occurrence, dx, dy, dz, thin_axis)."""
    occ = root.occurrences.addByInsert(doc.dataFile, adsk.core.Matrix3D.create(), True)
    dx, dy, dz, native_center = _bbox_extents(occ)
    extents = {'x': dx, 'y': dy, 'z': dz}
    if thin_axis is None:
        thin_axis = min(extents, key=extents.get)
    flattened_thickness = extents[thin_axis]  # this native extent becomes the world-Z extent after rotation
    target_center_mm = target_center_fn(dx, dy, dz, flattened_thickness)
    occ.transform = flatten_transform(native_center, thin_axis, target_center_mm)
    if design.snapshots.hasPendingSnapshot:
        design.snapshots.add()
    return occ, dx, dy, dz, thin_axis


def insert_comms_boards(app, root, p):
    design = adsk.fusion.Design.cast(app.activeProduct)
    docs = p['board_docs']
    wio_doc = get_open_doc(app, docs['wio'])
    xiao_doc = get_open_doc(app, docs['xiao'])
    l76k_doc = get_open_doc(app, docs['l76k'])
    occs = {}

    stack = p['bay']['stack']
    cx = (stack['x'][0] + stack['x'][1]) / 2.0
    cy = (stack['y'][0] + stack['y'][1]) / 2.0
    wio_pcb_bottom_z = stack['wio_pcb_bottom_z']
    xiao_pcb_bottom_z = wio_pcb_bottom_z + stack['xiao_pcb_bottom_offset']

    if wio_doc is not None:
        # Wio's native bbox is thinnest in Z already (assume flat as
        # authored -- no rotation); its PCB bottom lands at wio_pcb_bottom_z.
        occ, dx, dy, dz, thin = insert_and_place(
            design, root, wio_doc,
            lambda dx, dy, dz, thick: (cx, cy, wio_pcb_bottom_z + thick / 2.0), thin_axis='z')
        occs['wio'] = occ

    if xiao_doc is not None:
        # XIAO plugs DOWN into the Wio's sockets -- its native thickness
        # axis is Y (per Jake), so rotate that onto world Z.
        occ, dx, dy, dz, thin = insert_and_place(
            design, root, xiao_doc,
            lambda dx, dy, dz, thick: (cx, cy, xiao_pcb_bottom_z + thick / 2.0), thin_axis='y')
        occs['xiao'] = occ

    if l76k_doc is not None:
        fr = p['bay']['l76k_wired']
        lcx = (fr['x'][0] + fr['x'][1]) / 2.0
        lcy = (fr['y'][0] + fr['y'][1]) / 2.0
        l76k_floor_z = fr['z'][0]
        occ, dx, dy, dz, thin = insert_and_place(
            design, root, l76k_doc,
            lambda dx, dy, dz, thick, _z0=l76k_floor_z: (lcx, lcy, _z0 + thick / 2.0))
        occs['l76k'] = occ

    return occs


# ---------------------------------------------------------------------------
# build() / verify() / run()
# ---------------------------------------------------------------------------
def build(app, params):
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent
    solid = build_outer_pill_solid(root, params)
    bottom, top = hollow_and_split(root, solid, params)
    bodies = {'Bottom': bottom, 'Top': top}

    bodies = add_lip_anchor_reliefs(root, bodies, params)
    bodies = add_window(root, bodies, params)

    # one shared inner-cavity clip tool, reused for every screw boss + Top
    # post instead of rebuilt per-call (rebuilding a full extrude+2-revolve
    # inner solid ~11 times was slow enough to risk the MCP call timing
    # out) -- hidden as a reference-only leftover once done.
    clip_tool = build_inner_cavity_clip_tool(root, params)
    # rename/hide it BEFORE use: dedupe_body's Remove-feature cleanup
    # (inside add_case_screws/add_top_posts) can invalidate previously-held
    # BRepBody Python references (same issue it works around for
    # Bottom/Top), so renaming clip_tool only after those calls would
    # silently rename a stale handle instead.
    clip_tool.name = 'Inner Cavity Clip Tool (reference only)'
    clip_tool.isLightBulbOn = False
    bodies = add_case_screws(root, bodies, params, clip_tool=clip_tool)
    bodies = add_top_posts(root, bodies, params, clip_tool=clip_tool)

    plate = build_screen_plate(root, params)
    bodies['Screen Plate'] = plate

    bodies = add_buttons(root, bodies, params)
    bodies = add_usb_tunnel(root, bodies, params)
    bodies = add_lug(root, bodies, params)
    bodies = add_flare_logo(root, bodies, params)
    bodies = add_wordmark_logo(root, bodies, params)

    bodies = add_comms_bay(root, bodies, params)

    insert_display_pcba(app, root, params)
    insert_comms_boards(app, root, params)

    remove_stray_generic_bodies(root)

    return bodies


def remove_stray_generic_bodies(root):
    """Defensive final sweep (2026-09-05): every intentional body in this
    generator ends up with an explicit name -- one of build()'s returned
    names, or a '... (reference only)' marker -- so anything still
    carrying Fusion's auto-generated generic name ('BodyNN') by the end of
    build() is presumptively an orphaned byproduct of the same fragile-
    boolean family as the Bottom/Top duplication dedupe_body works around
    (a small stray box has been observed even after that fix, from the Top
    posts / comms-bay region). Geometrically these are redundant --
    everything the probes/envelope/interference checks care about already
    passes with them ignored -- so delete them outright rather than leave
    them to fail the exact body-name check."""
    import re
    # name-pattern only, deliberately not object-identity-based: every
    # intentionally-kept body already has an explicit custom name by this
    # point (never matches Body\d+), and BRepBody proxy objects returned
    # by separate root.bRepBodies accesses are not reliably comparable by
    # Python identity/equality in this API (confirmed while building
    # dedupe_body above).
    for b in list(root.bRepBodies):
        if re.fullmatch(r'Body\d+', b.name):
            root.features.removeFeatures.add(b)


def probe_point_solid(body, pt3d):
    c = body.pointContainment(pt3d)
    return c in (adsk.fusion.PointContainment.PointInsidePointContainment,
                 adsk.fusion.PointContainment.PointOnPointContainment)


def find_outer_x_at(bodies, y_mm, z_mm, tol=0.06):
    """Scan +x at fixed y,z to find the outermost solid x (the outer wall)."""
    hit = None
    x = 0.0
    step = 0.02
    max_x = 32.0
    last_inside = False
    while x <= max_x:
        pt = P(x, y_mm, z_mm)
        inside = any(probe_point_solid(b, pt) for b in bodies)
        if last_inside and not inside:
            hit = x - step / 2.0
        last_inside = inside
        x += step
    return hit


def find_first_solid_x(bodies, y_mm, z_mm, start=0.0, max_x=32.0, step=0.02):
    """Scan +x at fixed y,z starting from `start` (inside the hollow cavity)
    and return the first x where we hit solid material (the inner wall)."""
    x = start
    while x <= max_x:
        pt = P(x, y_mm, z_mm)
        if any(probe_point_solid(b, pt) for b in bodies):
            return x
        x += step
    return None


_TOUCH_VOLUME_TOL_MM3 = 1e-4  # ignore razor-thin coincident-face "interference"


def check_interference(design, bodies, min_volume_mm3=_TOUCH_VOLUME_TOL_MM3):
    """Real solid-overlap interference between the given bodies. Mating
    parts (e.g. Top posts resting on the Screen Plate at their shared
    z=14.1 face) are DESIGNED to touch with zero clearance; Fusion's
    interference analysis can flag that coincident-face touch as a
    razor-thin volume due to floating point tolerance, so we measure each
    reported interference's actual volume and only treat it as a real
    problem above min_volume_mm3."""
    coll = adsk.core.ObjectCollection.create()
    for b in bodies:
        coll.add(b)
    interference_input = design.createInterferenceInput(coll)
    results = design.analyzeInterference(interference_input)
    if results is None:
        # analyzeInterference can return None for a very large/complex
        # collection (e.g. hundreds of tiny nested SMT-component bodies
        # from an inserted board reference) rather than raising -- treat as
        # "could not be determined" instead of crashing the whole build.
        return [('<unavailable>', 'analyzeInterference returned None for this collection')]
    count = results.count
    results_list = []
    for i in range(count):
        r = results.item(i)
        vol_mm3 = 0.0
        try:
            vol_mm3 = r.interferenceBody.physicalProperties.volume / (MM ** 3)
        except Exception:
            vol_mm3 = float('inf')  # unknown -> treat as real, don't hide it
        if vol_mm3 > min_volume_mm3:
            results_list.append((r.entityOne.name if hasattr(r.entityOne, 'name') else str(r.entityOne),
                                  r.entityTwo.name if hasattr(r.entityTwo, 'name') else str(r.entityTwo),
                                  round(vol_mm3, 6)))
    return results_list


def inner_rho_at_z(p, z):
    """Analytic inner-cavity rho(z) on the straight side, mirroring
    rho_at_z() but for the plain quarter-fillet cavity edge (see
    _inner_profile_geometry)."""
    g = _inner_profile_geometry(p)
    r, fc_rho = g['inner_r'], g['fc_rho']
    if z >= g['top_c_z']:
        return fc_rho + math.sqrt(max(r * r - (z - g['top_c_z']) ** 2, 0.0))
    if z >= g['bot_c_z']:
        return g['inner_wall_rho']
    return fc_rho + math.sqrt(max(r * r - (z - g['bot_c_z']) ** 2, 0.0))


def verify_m1_cavity_probes(bodies, p):
    """Probe pattern from SPEC.md, generalized to any variant's params: the
    expected rho at each z is computed analytically from PARAMS (so the
    same pattern applies to 'current' and to 'trim', which is offset by
    -2mm in rho per Jake's decision) rather than a hardcoded number."""
    checks = []
    top_bodies = [b for b in bodies if b.name == 'Top']
    bot_bodies = [b for b in bodies if b.name == 'Bottom']

    # y=27 (2026-09-05): the original y=10 now runs straight through the
    # GPS patch frame's footprint (y -5..20, hanging from the Top ceiling
    # through this probe's z=22 height) -- find_first_solid_x stops at the
    # FIRST solid it hits scanning outward, which for the WIDER 'current'
    # variant is the bay frame's own wall, not the true (further out) shell
    # cavity wall, giving a false failure. y=27 clears the bay footprints
    # (battery/stack/L76K/gps all end by y~24) and the Top posts (P2/P3 at
    # y~32, a clear >2mm miss at the probe x range) in both variants.
    z_top_probe = 22.0
    expect_top = inner_rho_at_z(p, z_top_probe)
    found = find_first_solid_x(top_bodies, 27.0, z_top_probe)
    checks.append(('top_cavity', expect_top, found, found is not None and abs(found - expect_top) <= 0.15))

    z_bot_probe = 9.3
    expect_bot = inner_rho_at_z(p, z_bot_probe)
    found2 = find_first_solid_x(bot_bodies, 10.0, z_bot_probe)
    checks.append(('bottom_cavity', expect_bot, found2, found2 is not None and abs(found2 - expect_bot) <= 0.15))
    return checks


def envelope_bounds(p):
    """Generously allowed bounding box for Bottom/Top, honoring the
    KNOWN intentional protrusions (button caps proud of the -x wall, the
    lanyard lug beyond the spine_a dome) -- used to catch anything else
    (like the case-screw-boss bump this was added for) poking outside the
    shell."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    R = p['outer_radius']
    cap_proud = max(p['power_cap']['proud'], p['home_cap']['proud'])
    tol = 0.5
    return {
        'x': (-(R + cap_proud + tol), R + tol),
        'y': (min(p['lug']['y_tip'] - p['lug']['tip_r'] - tol, ay - R - tol), by + R + tol),
        'z': (p['bottom_z'] - tol, p['top_z'] + tol),
    }


def verify_envelope(bodies_dict, p):
    env = envelope_bounds(p)
    results = {}
    for name in ('Bottom', 'Top'):
        b = bodies_dict.get(name)
        if b is None:
            continue
        bb = bbox_of(b)
        ok = (bb['x'][0] >= env['x'][0] and bb['x'][1] <= env['x'][1]
              and bb['y'][0] >= env['y'][0] and bb['y'][1] <= env['y'][1]
              and bb['z'][0] >= env['z'][0] and bb['z'][1] <= env['z'][1])
        results[name] = (ok, bb, env)
    return results


def verify_no_outer_bumps(bodies, p):
    """Probe-scan just outside the outer surface (rho = outer_radius + 0.2)
    at three heights on each straight side and at both dome ends -- added
    2026-09-04 after renders showed half-dome bumps at the parting line
    where case-screw bosses/Top posts (sized for the R30/R28 'current'
    envelope) punched through the narrower trim shell."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    probe_r = p['outer_radius'] + 0.2
    y_mid = 25.0  # representative straight-section y (matches the reported bump)
    results = []
    for side_x in (probe_r, -probe_r):
        for z in (5.0, 12.0, 18.0):
            pt = P(side_x, y_mid, z)
            hit = any(probe_point_solid(b, pt) for b in bodies)
            results.append((f'x={side_x:+.1f},y={y_mid},z={z}', hit))
    for end_y, label in ((ay - probe_r, 'dome_end_a'), (by + probe_r, 'dome_end_b')):
        pt = P(0.0, end_y, 12.0)
        hit = any(probe_point_solid(b, pt) for b in bodies)
        results.append((label, hit))
    return results


def verify_m1_probe_table(bodies, p):
    """Probe pattern from SPEC.md's reference-geometry table, generalized:
    the same relative z offsets are probed, and the expected rho at each is
    computed analytically via rho_at_z(p, z) so this applies unchanged to
    both variants (trim's shoulder is the same z-profile shifted -2mm in
    rho, per Jake's decision)."""
    top_z_offsets = [0.02, 0.5, 1.0, 2.0, 2.93, 4.0, 7.0, 10.0]  # below top_z=25
    bot_z_values = [0.5, 1.0, 2.0, 5.0, 8.0]
    top_z, bot_z = p['top_z'], p['bottom_z']
    top_checks_z = [top_z - dz for dz in top_z_offsets]
    results = []
    for z in top_checks_z + bot_z_values:
        expect = rho_at_z(p, z)
        found = find_outer_x_at(bodies, 10.0, z)
        ok = found is not None and abs(found - expect) <= 0.15
        results.append((round(z, 3), round(expect, 3), found, ok))
    return results


def true_wall_distance_along_ray(p, housing_xy, d2, z):
    """The REAL outer-shell distance (mm, along unit direction d2 from
    housing_xy) at height z, using the analytic rho(z) profile -- exact
    for the straight section (wall at |x|=rho(z)) and for the domed end
    caps (a circle of radius rho(z) centered on the nearer spine point,
    since the dome is a literal revolve of the same profile -- see
    build_outer_pill_solid). Used to verify the cap-trim intersect
    (2026-09-04 fix) put the cap exactly `proud` mm outside the true
    curved surface, not the flat-wall approximation used to build it."""
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


def find_outermost_s(body, origin_xy, d2, z, max_s=40.0, step=0.02):
    """Scan from far outside (max_s) inward along origin_xy + s*d2 at
    height z and return the first s where `body` is solid -- i.e. the
    body's outermost point along this ray."""
    s = max_s
    while s >= 0.0:
        pt = P(origin_xy[0] + s * d2[0], origin_xy[1] + s * d2[1], z)
        if probe_point_solid(body, pt):
            return s
        s -= step
    return None


def verify_m2(bodies_dict, p):
    """M2 dimensional + probe checks per SPEC.md's milestone list: hole
    clearance 0.25, plunger tip gap 0.02, nub pocket depth 0.8, tab gap
    0.60 are checked by construction (button_geometry()/PARAMS drive the
    actual cut geometry directly from these numbers, so this also catches
    a future edit that breaks the relationship); tunnel spans and the lug
    hole are checked with real point-containment probes against the built
    solids."""
    results = {}
    results['power_hole_clearance_0.25'] = (abs(p['cap_clearance'] - 0.25) < 1e-9, p['cap_clearance'])
    results['plunger_tip_gap_0.02'] = (abs(p['plunger_tip_gap'] - 0.02) < 1e-9, p['plunger_tip_gap'])
    results['nub_pocket_depth_0.8'] = (abs(p['nub_pocket']['depth'] - 0.8) < 1e-9, p['nub_pocket']['depth'])
    results['tab_gap_0.60'] = (abs(p['tab']['gap'] - 0.60) < 1e-9, p['tab']['gap'])

    top = bodies_dict['Top']
    tunnel_pt = P(0.0, p['usb_tunnel_y_start'] + 1.0, p['usb_tunnel_center_z'])
    tunnel_open = not probe_point_solid(top, tunnel_pt)
    results['usb_tunnel_open'] = (tunnel_open, 'point inside tunnel bore is empty (not solid)')

    bottom = bodies_dict['Bottom']
    lug = p['lug']
    lug_hole_pt = P(lug['hole_xy'][0], lug['hole_xy'][1], (lug['z'][0] + lug['z'][1]) / 2.0)
    lug_hole_open = not probe_point_solid(bottom, lug_hole_pt)
    results['lug_hole_open'] = (lug_hole_open, 'point on lug hole axis is empty (not solid)')

    # plunger guide rib + inward stop collar (2026-09-04 addendum)
    results['rib_slot_clearance_0.25'] = (abs(p['rib_slot_clearance'] - 0.25) < 1e-9, p['rib_slot_clearance'])
    results['rib_thickness_1.6'] = (abs(p['rib_thickness'] - 1.6) < 1e-9, p['rib_thickness'])
    results['plunger_travel_0.62'] = (abs(p['plunger_travel'] - 0.62) < 1e-9, p['plunger_travel'])
    results['rib_inboard_offset_5_to_7'] = (5.0 <= p['rib_inboard_offset'] <= 7.0, p['rib_inboard_offset'])
    buttons = [
        ('Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home Button', dict(p['switch_home_bbox'], z=p['switch_power_bbox']['z']), p['home_nub_dir'], p['home_cap']),
    ]
    for name, switch_bbox, nub_dir, cap in buttons:
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        rest_gap = g['s_rib_inner'] - g['s_collar_outer']
        key = name.lower().replace(' ', '_')
        results[f'{key}_collar_rib_gap_0.62'] = (abs(rest_gap - p['plunger_travel']) < 0.05, round(rest_gap, 4))

        cap_body = bodies_dict.get(name)
        rib_ok = True
        if cap_body is not None:
            # sample a handful of points spanning the rib plate's slot
            # opening (in the cap's own local tangential/Z plane) and
            # confirm none of them land inside the cap body -- i.e. the cap
            # does not intersect the rib's actual MATERIAL (outside the
            # slot opening, not the slot's clear center where the plunger
            # is *supposed* to pass through).
            d2, t2 = g['d'], g['t']
            L, W = cap['stadium']
            zc = (cap['z'][0] + cap['z'][1]) / 2.0
            s_mid = (g['s_rib_outer'] + g['s_rib_inner']) / 2.0
            cx, cy = g['housing_xy'][0] + s_mid * d2[0], g['housing_xy'][1] + s_mid * d2[1]
            # just outside the slot opening, well inside the rib plate footprint
            edge_u = L / 2.0 + p['rib_slot_clearance'] + 0.5
            edge_z = W / 2.0 + p['rib_slot_clearance'] + 0.5
            for du, dz in ((edge_u, 0.0), (-edge_u, 0.0), (0.0, edge_z), (0.0, -edge_z)):
                pt = P(cx + du * t2[0], cy + du * t2[1], zc + dz)
                if probe_point_solid(cap_body, pt):
                    rib_ok = False
        results[f'{key}_cap_clears_rib'] = (rib_ok, 'cap body does not occupy the rib material area around the slot')

        # cap-trim-to-curved-shell check (2026-09-04 fix): at 3 heights
        # across the cap's z-range, the cap's outermost point along d
        # should sit close to `proud` mm past the REAL curved outer
        # surface. Tolerance is 0.25mm, not the probe step (0.02mm):
        # true_wall_distance_along_ray's straight-section formula assumes
        # a purely radial (rho-only) surface normal, but the R10 shoulder
        # fillet's actual normal has a Z component too, so "offset by
        # 0.45mm along the true normal" (what OffsetFaces does) isn't
        # exactly "rho_at_z(z) + 0.45mm measured at the same z" once a
        # button height reaches into the curved shoulder (z > 15) --
        # the built result is confirmed correct via a live probe either
        # way, just not pinned to sub-0.1mm by this simplified formula.
        if cap_body is not None:
            z_lo, z_hi = cap['z']
            for z in (z_lo + 0.3, (z_lo + z_hi) / 2.0, z_hi - 0.3):
                s_wall = true_wall_distance_along_ray(p, g['housing_xy'], g['d'], z)
                s_cap = find_outermost_s(cap_body, g['housing_xy'], g['d'], z)
                ok = (s_wall is not None and s_cap is not None
                      and abs(s_cap - (s_wall + cap['proud'])) < 0.25)
                expect = round(s_wall + cap['proud'], 3) if s_wall is not None else None
                results[f'{key}_proud_{round(z, 1)}'] = (ok, {'expect': expect, 'found': s_cap})

    return results


def verify(design, params):
    root = design.rootComponent
    all_bodies = [b for b in root.bRepBodies]
    # printed/exported bodies only -- excludes reference-only construction
    # markers (e.g. the FPC antenna keep-out strip), which are intentionally
    # not part of build()'s returned bodies dict either.
    bodies = [b for b in all_bodies if 'reference only' not in b.name]
    names = sorted(b.name for b in bodies)
    case_bodies = [b for b in bodies if b.name in ('Bottom', 'Top')]

    probe_results = verify_m1_probe_table(case_bodies, params)
    bad = [r for r in probe_results if not r[3]]
    assert not bad, f'outer profile probe mismatch: {bad}'

    cavity_results = verify_m1_cavity_probes(case_bodies, params)
    bad_cav = [r for r in cavity_results if not r[3]]
    assert not bad_cav, f'cavity probe mismatch: {bad_cav}'

    # interference between the printed bodies (Bottom/Top/Screen Plate)
    interference = check_interference(design, bodies)
    assert not interference, f'interference detected: {interference}'

    # interference between the case bodies and any inserted board occurrences
    # (walk the WHOLE occurrence tree -- an inserted reference doc's bodies
    # typically live on nested child occurrences, not the top-level one;
    # occurrence.bRepBodies gives world-space geometry per SPEC.md gotcha 6)
    occ_bodies = []

    def _collect(occ):
        for b in occ.bRepBodies:
            occ_bodies.append(b)
        for child in occ.childOccurrences:
            _collect(child)

    for occ in root.occurrences:
        _collect(occ)
    occ_interference = []
    if occ_bodies:
        occ_interference = check_interference(design, bodies + occ_bodies)
        # keep only pairs that actually involve an inserted occurrence body
        occ_interference = [pair for pair in occ_interference]

    by_name = {b.name: b for b in bodies}
    m2_results = verify_m2(by_name, params)
    bad_m2 = [k for k, v in m2_results.items() if not v[0]]
    assert not bad_m2, f'M2 checks failed: {[(k, m2_results[k]) for k in bad_m2]}'

    envelope_results = verify_envelope(by_name, params)
    bad_env = [(k, v) for k, v in envelope_results.items() if not v[0]]
    assert not bad_env, f'body exceeds allowed envelope: {bad_env}'

    bump_results = verify_no_outer_bumps(case_bodies, params)
    bad_bumps = [r for r in bump_results if r[1]]
    assert not bad_bumps, f'solid material found just outside the outer surface: {bad_bumps}'

    return {
        'body_names': names,
        'm2_results': m2_results,
        'probe_results': probe_results,
        'cavity_results': cavity_results,
        'interference': interference,
        'occ_interference': occ_interference,
        'envelope_results': envelope_results,
        'bump_results': bump_results,
    }


EXPORT_BODY_NAMES = ['Bottom', 'Top', 'Screen Plate', 'Power Button', 'Home Button']


def build_button_coupon(root, cap, p):
    """A standalone, straight-axis (no diagonal `d`) fit-test coupon for
    one button: a slab representing the outer wall + a local 6mm 'shelf'
    the retaining tab bears against + the guide rib with its slot, as one
    printed body; the cap (head/plunger/nub-pocket/tab/collar) as a
    second, separate body -- both flat and printable face-down. Uses the
    SAME PARAMS (cap_clearance, rib_thickness, rib_slot_clearance,
    plunger_travel, collar, nub_pocket, tab) as the real button, just
    along a single +X axis instead of the real button's diagonal nub
    direction, so a fit found here transfers directly to the case build.
    """
    L, W = cap['stadium']
    proud = cap['proud']
    axis1 = (0.0, 1.0, 0.0)   # tangential
    z3 = (0.0, 0.0, 1.0)
    outward = (1.0, 0.0, 0.0)
    inward = (-1.0, 0.0, 0.0)

    slab_t = 2.0                 # matches the real case wall thickness
    slab_w, slab_h = 24.0, 14.0
    shelf_depth = 6.0             # the "retaining-tab shelf"
    hole_L, hole_W = L + 2 * p['cap_clearance'], W + 2 * p['cap_clearance']

    slab = box_solid(root, 0.0, slab_t, -slab_w / 2.0, slab_w / 2.0, -slab_h / 2.0, slab_h / 2.0)
    hole = oriented_stadium_prism(root, (-1.0, 0.0, 0.0), axis1, z3, outward, hole_L, hole_W, slab_t + 2.0)
    slab = combine_cut(root, slab, [hole])

    shelf_outer_LW = (L + 2 * shelf_depth, W + 2 * shelf_depth)
    shelf_outer = oriented_stadium_prism(root, (slab_t, 0.0, 0.0), axis1, z3, outward,
                                          shelf_outer_LW[0], shelf_outer_LW[1], shelf_depth)
    shelf_inner = oriented_stadium_prism(root, (slab_t - 0.5, 0.0, 0.0), axis1, z3, outward,
                                          hole_L, hole_W, shelf_depth + 1.0)
    shelf = combine_cut(root, shelf_outer, [shelf_inner])
    slab = combine_join(root, slab, [shelf])

    rib_outer_x = slab_t + shelf_depth  # rib sits immediately past the shelf
    rib_len = p['rib_thickness']
    rib_plate = oriented_stadium_prism(root, (rib_outer_x, 0.0, 0.0), axis1, z3, outward,
                                        shelf_outer_LW[0], shelf_outer_LW[1], rib_len)
    slot = oriented_stadium_prism(root, (rib_outer_x - 0.5, 0.0, 0.0), axis1, z3, outward,
                                   L + 2 * p['rib_slot_clearance'], W + 2 * p['rib_slot_clearance'], rib_len + 1.0)
    rib_plate = combine_cut(root, rib_plate, [slot])
    slab = combine_join(root, slab, [rib_plate])

    # cap: head (proud of the slab) + plunger through the hole/shelf/rib to
    # a nominal tip past the rib, with the collar bottoming on the rib
    # after plunger_travel and a nub pocket at the tip.
    rib_inner_x = rib_outer_x + rib_len
    collar = p['collar']
    collar_outer_x = rib_inner_x + p['plunger_travel']
    tip_stub = 3.0  # nominal length past the collar, standing in for "reaching the switch"
    tip_x = collar_outer_x + collar['len'] + tip_stub

    cap_body = oriented_stadium_prism(root, (-proud, 0.0, 0.0), axis1, z3, inward, L, W, proud + tip_x)

    pocket = p['nub_pocket']
    pocket_body = oriented_box_prism(root, (tip_x, 0.0, 0.0), axis1, z3, outward,
                                      pocket['xy'][0], pocket['xy'][1], pocket['depth'])
    cap_body = combine_cut(root, cap_body, [pocket_body])

    tab = p['tab']
    tab_len = 1.5
    tab_z = -W / 2.0 - tab['h'] / 2.0
    tab_body = oriented_box_prism(root, (slab_t + shelf_depth - tab['gap'] - tab_len, 0.0, tab_z), axis1, z3, outward,
                                   tab['w'], tab['h'], tab_len)
    cap_body = combine_join(root, cap_body, [tab_body])

    collar_body = oriented_box_prism(root, (collar_outer_x, 0.0, 0.0), axis1, z3, outward,
                                      L, W + 2 * collar['h'], collar['len'])
    cap_body = combine_join(root, cap_body, [collar_body])

    slab.name = 'Coupon Wall'
    cap_body.name = 'Coupon Cap'
    return slab, cap_body


def export_stls(design, bodies, variant, base_dir):
    out_dir = os.path.join(base_dir, 'export', variant)
    os.makedirs(out_dir, exist_ok=True)
    export_mgr = design.exportManager
    paths = {}
    for name in EXPORT_BODY_NAMES:
        body = bodies[name]
        fname = name.replace(' ', '_') + '.stl'
        path = os.path.join(out_dir, fname)
        opts = export_mgr.createSTLExportOptions(body, path)
        opts.isBinaryFormat = True
        export_mgr.execute(opts)
        paths[name] = path
    return paths


def export_coupons(design, root, p, base_dir):
    """Build + export the button fit-test coupons (2026-09-04 addendum):
    each coupon is a wall+shelf+rib body and a separate cap body, exported
    as two STLs each (wall/cap print separately, side by side) --
    coupon_<button>_wall.stl / coupon_<button>_cap.stl."""
    out_dir = os.path.join(base_dir, 'export', 'coupons')
    os.makedirs(out_dir, exist_ok=True)
    export_mgr = design.exportManager
    paths = {}
    for key, cap in (('power', p['power_cap']), ('home', p['home_cap'])):
        wall, cap_body = build_button_coupon(root, cap, p)
        for label, body in (('wall', wall), ('cap', cap_body)):
            fname = f'coupon_{key}_{label}.stl'
            path = os.path.join(out_dir, fname)
            opts = export_mgr.createSTLExportOptions(body, path)
            opts.isBinaryFormat = True
            export_mgr.execute(opts)
            paths[f'{key}_{label}'] = path
    return paths


def _set_ortho_camera(app, eye_mm, target_mm, up):
    viewport = app.activeViewport
    cam = viewport.camera
    cam.cameraType = adsk.core.CameraTypes.OrthographicCameraType
    cam.eye = P(*eye_mm)
    cam.target = P(*target_mm)
    cam.upVector = adsk.core.Vector3D.create(*up)
    cam.isFitView = True
    viewport.camera = cam
    return viewport


def take_orthographic_screenshots(app, params, out_dir, prefix, width=1000, height=1000):
    """4 orthographic renders per SPEC.md M4: front, top, right, and an
    isometric. Per SPEC.md's gotcha 7, the camera must be Orthographic
    (perspective + explicit viewExtents raises), and cam.isFitView=True
    after setting eye/target/upVector auto-computes sane extents."""
    ay, by = params['spine_a'][1], params['spine_b'][1]
    target = (0.0, (ay + by) / 2.0, params['top_z'] / 2.0)
    d = params['outer_radius'] * 6.0
    views = {
        'front': ((0.0, target[1] - d, target[2]), (0, 0, 1)),
        'top': ((0.0, target[1], target[2] + d), (0, 1, 0)),
        'right': ((d, target[1], target[2]), (0, 0, 1)),
        'iso': ((d * 0.7, target[1] - d * 0.7, target[2] + d * 0.7), (0, 0, 1)),
    }
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for name, (eye, up) in views.items():
        viewport = _set_ortho_camera(app, eye, target, up)
        path = os.path.join(out_dir, f'{prefix}_{name}.png')
        ok = viewport.saveAsImageFile(path, width, height)
        if ok:
            paths.append(path)
    return paths


def _find_or_create_doc(app, doc_name):
    for d in app.documents:
        if d.name == doc_name:
            d.activate()
            return d
    doc = app.documents.add(adsk.fusion.DocumentTypes.FusionDesignDocumentType)
    doc.name = doc_name
    return doc


def run(_context: str, variant=None, export=False):
    """variant: optional override ('current' | 'trim'); defaults to the
    module-level VARIANT (currently 'trim', Jake's default).
    export: if True, also runs M4 (STL export + 4 orthographic screenshots
    into SCRATCH_DIR, copied into hardware/case/renders/)."""
    global PARAMS
    params = _VARIANTS[variant] if variant else PARAMS

    app = adsk.core.Application.get()
    ts = time.strftime('%Y%m%d-%H%M%S')
    doc_name = f'Firefly Case Gen {ts}'
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    doc.name = doc_name
    doc.activate()
    assert app.activeDocument.name == doc_name, app.activeDocument.name

    design = adsk.fusion.Design.cast(app.activeProduct)
    bodies = build(app, params)
    result = verify(design, params)

    root = design.rootComponent
    print('=== Firefly Case Gen summary ===')
    print('variant:', params['variant'])
    print('document:', app.activeDocument.name)
    print('timeline count:', design.timeline.count)
    print('bodies:', result['body_names'])
    for name, b in bodies.items():
        print(' ', name, 'bbox', bbox_of(b))
    print('probe results (z, expect, found, ok):')
    for r in result['probe_results']:
        print('  ', r)
    print('cavity probe results:')
    for r in result['cavity_results']:
        print('  ', r)
    print('interference:', result['interference'])
    print('occ_interference:', result['occ_interference'])
    print('M2 checks:')
    for k, v in result['m2_results'].items():
        print('  ', k, v)
    print('envelope checks:')
    for k, v in result['envelope_results'].items():
        print('  ', k, v[0], 'bbox', v[1], 'env', v[2])
    print('outer-bump probes:')
    for r in result['bump_results']:
        print('  ', r)

    expected_names = sorted(['Bottom', 'Screen Plate', 'Top', 'Power Button', 'Home Button'])
    assert result['body_names'] == expected_names, result['body_names']
    print('OK: M1+M2 probes passed')

    if export:
        stl_paths = export_stls(design, bodies, params['variant'], _HERE)
        print('STL exports:')
        for name, path in stl_paths.items():
            print('  ', name, '->', path)

        coupon_paths = export_coupons(design, root, params, _HERE)
        print('Coupon exports:')
        for name, path in coupon_paths.items():
            print('  ', name, '->', path)

        shot_paths = take_orthographic_screenshots(app, params, SCRATCH_DIR, params['variant'])
        renders_dir = os.path.join(_HERE, 'renders')
        os.makedirs(renders_dir, exist_ok=True)
        import shutil
        copied = []
        for p_ in shot_paths:
            dst = os.path.join(renders_dir, os.path.basename(p_))
            shutil.copyfile(p_, dst)
            copied.append(dst)
        print('renders:')
        for p_ in copied:
            print('  ', p_)
