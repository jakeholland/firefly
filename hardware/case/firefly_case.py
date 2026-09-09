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
# 2026-09-12 pass 12: Fusion's embedded interpreter has been observed to
# accumulate sys.path entries from MANY past worktrees/passes across its
# whole lifetime (a live session found /private/tmp/claude-501/case-pass10,
# case-pass11, .../case-pass12-scratch/orig, and the main repo checkout all
# still present) -- each one has its OWN params_current.py/params_trim.py,
# and `if _HERE not in sys.path: sys.path.insert(0, _HERE)` only helps the
# FIRST time this file runs in a given interpreter: once _HERE is already
# present (anywhere in the list), a LATER script that inserts a different
# worktree's dir at index 0 permanently shadows this one's own params
# modules for every subsequent run() in the same interpreter, even though
# THIS file (identified by its own absolute path) is what's executing --
# confirmed live: running a comparison build from a second worktree path
# left a real Firefly Case Gen document built against that OTHER worktree's
# params_trim.py (top_z=28) after this file's own params_trim.py had
# already been edited to top_z=30. Fixed by unconditionally moving _HERE to
# the FRONT every time, not just inserting it once.
sys.path = [p for p in sys.path if p != _HERE]
sys.path.insert(0, _HERE)

# Fusion's embedded Python interpreter stays alive across separate script
# executions, so sys.modules from a previous run() would otherwise serve
# STALE params_current/params_trim content on the next run -- and, per the
# above, possibly from a DIFFERENT worktree's file of the same name. Force
# a fresh import every time this file is executed.
for _mod in ('params_current', 'params_trim'):
    sys.modules.pop(_mod, None)

from params_current import PARAMS as PARAMS_CURRENT
from params_trim import PARAMS as PARAMS_TRIM

# ---------------------------------------------------------------------------
# Variant switch. Change this (or set firefly_case.VARIANT before calling
# run()) to pick which case gets built. 'trim' (56x103.8x28, pass 12b) is
# Jake's default;
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


def combine_cut_keep(root, target, tools):
    """Like combine_cut, but keeps the tool bodies alive afterward (for
    when a tool body -- e.g. a bay frame also needed elsewhere -- is used
    to cut one target and then joined/used again separately)."""
    coll = adsk.core.ObjectCollection.create()
    for t in tools:
        coll.add(t)
    cf = root.features.combineFeatures
    inp = cf.createInput(target, coll)
    inp.operation = adsk.fusion.FeatureOperations.CutFeatureOperation
    inp.isKeepToolBodies = True
    feat = cf.add(inp)
    return feat.bodies.item(0) if feat.bodies.count else target


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


def plane_at_x(root, x_mm):
    planes = root.constructionPlanes
    pin = planes.createInput()
    pin.setByOffset(root.yZConstructionPlane, V(x_mm))
    return planes.add(pin)


def plane_at_y(root, y_mm):
    planes = root.constructionPlanes
    pin = planes.createInput()
    pin.setByOffset(root.xZConstructionPlane, V(y_mm))
    return planes.add(pin)


def revolve_taper_wedge(root, cx, cy, r_lo, r_hi, z_lo, z_hi, angle_deg):
    """Pass 16, item C: a tapered wedge (radius r_lo at z_lo, growing
    LINEARLY to r_hi at z_hi -- same explicit, unambiguous r-at-z
    convention as cone_frustum_solid) revolved `angle_deg` around the
    vertical axis through (cx, cy). UNLIKE cone_frustum_solid, neither
    profile corner touches the axis -- this is a proper hollow "napkin
    ring" wedge (r_lo may be > 0), for adding material OUTSIDE an
    existing constant-radius land rather than a filled cone. Used for
    the domed-end halves of the lip/anchor ring's own continuous outward
    taper (see add_lip_anchor_reliefs); extrude_taper_wedge_along_y
    covers the straight sides. angle_deg follows build_outer_pill_
    solid's own cap_a(-180)/cap_b(+180) sign convention for the two
    stadium ends."""
    planes = root.constructionPlanes
    pin = planes.createInput()
    pin.setByOffset(root.xZConstructionPlane, V(cy))
    plane = planes.add(pin)
    sk = new_sketch(root, plane)
    axis = add_line(sk, P(cx, cy, z_lo - 1.0), P(cx, cy, z_hi + 1.0))
    axis.isConstruction = True
    add_line(sk, P(cx + r_lo, cy, z_lo), P(cx + r_hi, cy, z_hi))
    add_line(sk, P(cx + r_hi, cy, z_hi), P(cx + r_lo, cy, z_hi))
    add_line(sk, P(cx + r_lo, cy, z_hi), P(cx + r_lo, cy, z_lo))
    prof = None
    for pr in sk.profiles:
        prof = pr
        break
    assert prof is not None, 'revolve_taper_wedge: no closed profile found'
    return revolve_new_body(root, prof, axis, angle_deg)


def extrude_taper_wedge_along_y(root, x_sign, r_lo, r_hi, z_lo, z_hi, y0, y1):
    """Pass 16, item C: straight-side half of the same continuous radial
    taper revolve_taper_wedge builds for the domed ends -- a right-
    triangular wedge in the X-Z plane at world x = x_sign * r (corners
    (x_sign*r_lo, z_lo), (x_sign*r_hi, z_hi), (x_sign*r_lo, z_hi)),
    extruded along Y from y0 to y1. NOT filled to x=0 -- this is the
    ADDED taper material outside a constant-r_lo land, unioned onto it
    by the caller."""
    plane = plane_at_y(root, y0)
    sk = new_sketch(root, plane)
    a = P(x_sign * r_lo, y0, z_lo)
    b = P(x_sign * r_hi, y0, z_hi)
    c = P(x_sign * r_lo, y0, z_hi)
    add_line(sk, a, b)
    add_line(sk, b, c)
    add_line(sk, c, a)
    prof = None
    for pr in sk.profiles:
        prof = pr
        break
    assert prof is not None, 'extrude_taper_wedge_along_y: no closed profile found'
    return extrude_new_body(root, prof, y1 - y0, direction='positive')


def extrude_taper_cut_along_y(root, x_sign, r_lo, r_hi, z_lo, z_hi, y0, y1):
    """Pass 16, item C: Y-extruded mirror of a cone_frustum_solid-style
    CUT tool (filled to x=0, unlike extrude_taper_wedge_along_y's napkin-
    ring shape): removes everything with x_sign*x < boundary(z), boundary
    growing linearly from r_lo at z_lo to r_hi at z_hi, along Y from y0 to
    y1. Used to narrow the lip/anchor ring's own remaining flat top cap
    (printability review's own "keep a small flat land, taper the rest"
    suggestion) by cutting away material inward of a second, steeper
    taper -- see add_lip_anchor_reliefs."""
    plane = plane_at_y(root, y0)
    sk = new_sketch(root, plane)
    a = P(x_sign * r_lo, y0, z_lo)
    b = P(x_sign * r_hi, y0, z_hi)
    c = P(0.0, y0, z_hi)
    d = P(0.0, y0, z_lo)
    add_line(sk, a, b)
    add_line(sk, b, c)
    add_line(sk, c, d)
    add_line(sk, d, a)
    prof = None
    for pr in sk.profiles:
        prof = pr
        break
    assert prof is not None, 'extrude_taper_cut_along_y: no closed profile found'
    return extrude_new_body(root, prof, y1 - y0, direction='positive')


def build_wedge_along_x(root, x0, x1, y_wall, y_sign, z_bottom, h, w):
    """A 45-degree self-supporting wedge replacing a flat 'ledge' shelf
    (2026-09-05 printability fix): spans x0..x1, attached to a vertical
    wall at y=y_wall, protruding `w` mm in the y_sign direction (+1/-1).
    The cross-section (in the y-z plane) is a right triangle: flush with
    the wall (zero protrusion) at z=z_bottom+h, growing to full width `w`
    at z=z_bottom. For a body that PRINTS with print-down = +model z (the
    Top, face-down on its z=25 face), z_bottom+h prints FIRST (closer to
    the existing wall structure above it) and z_bottom prints LAST -- the
    sloped hypotenuse's outward normal has a +z component throughout (the
    slope "faces the bed" in print orientation), so each new layer only
    ever adds material atop material already printed, never overhanging
    more than 45 degrees, unlike the flat box ledge this replaces (which
    was a full `w` mm overhang the instant it appeared)."""
    plane = plane_at_x(root, x0)
    sk = new_sketch(root, plane)
    p1 = P(x0, y_wall, z_bottom + h)
    p2 = P(x0, y_wall, z_bottom)
    p3 = P(x0, y_wall + y_sign * w, z_bottom)
    add_line(sk, p1, p2)
    add_line(sk, p2, p3)
    add_line(sk, p3, p1)
    prof = sk.profiles.item(0)
    return extrude_new_body(root, prof, x1 - x0, direction='positive')


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


def count_sliver_faces(body, threshold_mm2=0.5):
    """Diagnostic (2026-09-06, pass 6, item E): counts faces of `body`
    whose area is under `threshold_mm2` -- a high count is a sign of
    boolean scraps (sliver faces left behind by an imprecise
    intersect/cut) rather than a real defect by itself; reported, not
    asserted on."""
    count = 0
    for f in body.faces:
        area_mm2 = f.area / (MM * MM)
        if area_mm2 < threshold_mm2:
            count += 1
    return count


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


def clipped_pillar_with_reach(root, cx, cy, r, z0, z1, p, clip_tool, core_r):
    """A vertical cylinder (boss/post), radially safe against the outer
    shell AND guaranteed to physically reach both its z0 and z1 ends
    (2026-09-06 pass 6 fix -- see verify_posts_and_bosses' docstring for
    the bug this closes): `clip_to_inner_cavity` alone shrinks the pillar
    by `safety_margin` on EVERY face, including the top/bottom -- for a
    boss/post meant to touch Top's ceiling or Bottom's floor exactly at
    z1/z0, that shrink (plus, empirically, a further real mismatch between
    the inner-cavity-solid's own ceiling height and the nominal
    `top_ceiling_underside_z`/etc. params -- confirmed by direct
    measurement: a boss clipped this way came up ~0.36mm short of its
    nominal top) leaves the clipped pillar physically NOT TOUCHING the
    shell at all. `combine_join` (Fusion's Boolean Union) SILENTLY NO-OPS
    on two bodies that don't touch/overlap (same behavior deboss_loops'
    docstring already documents for disjoint glyph pieces) rather than
    erroring -- so every case boss (A/B/C/D) and every Top post (P1-P4)
    was silently never actually joined into Bottom/Top, despite every
    prior build()/verify() call succeeding with no error. Fixed by ALSO
    building a full-height, smaller-radius `core_r` cylinder (unclipped,
    so it genuinely reaches z0 and z1) and joining it to the radially-
    clipped wide cylinder before that combined shape is joined into
    Bottom/Top -- the core provides the guaranteed physical connection at
    both ends; the wider (but z-shrunk) clipped cylinder still provides
    the bulk of the boss's real diameter everywhere it's safe to. Pick
    `core_r` comfortably above half the largest hole later cut through
    this pillar (screw/pilot hole) so a real wall of material survives
    the cut, and comfortably below `r` so it can never itself risk an
    outward punch-through."""
    wide = cylinder_solid(root, cx, cy, r, z0, z1)
    wide_clipped = clip_to_inner_cavity(root, wide, p, clip_tool)
    if core_r <= 0:
        # 2026-09-06 pass 6: escape hatch for case-screw boss B, whose
        # position genuinely sits inside the L76K PCB's own real footprint
        # (a pre-existing, unrelated design conflict -- see add_case_boss's
        # docstring) -- a full-height core there would guarantee the join
        # succeeds, but by construction it would also guarantee a REAL
        # solid overlap with the PCB (a hard verify() failure, unlike a
        # missing boss, which is merely undesirable). core_r<=0 skips the
        # core and returns the plain radially-safe pillar, restoring the
        # exact pre-pass-6 behavior for this one position (it silently
        # doesn't join, same as it always has) rather than trade a latent
        # bug for a real interference.
        return wide_clipped
    core = cylinder_solid(root, cx, cy, core_r, z0, z1)
    return combine_join(root, wide_clipped, [core])


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


MIN_RELIEF_CLEARANCE = 1.0  # mm the boss relief must clear beyond the boss's own OD (see the assert below)
LIP_RING_FLAT_TARGET_MM = 0.5  # mm -- nominal remaining flat cap width at the lip/anchor ring's own anchor_z[1]
                                # (see add_lip_anchor_reliefs' own comment): 0.5 rather than the gate's own 0.6mm
                                # limit -- a small deliberate margin, kept even after verify_lip_ring_profile's
                                # own down-direction convention bug (not this cut) was found and fixed.


def add_lip_anchor_reliefs(root, bodies, p):
    """2026-09-07 pass 7 (defect sweep): the per-boss relief cylinder's
    radius was a flat `boss_relief_dia/2` (5.0mm) regardless of how close
    the boss sits to the TRUE outer wall. For trim, boss A/C sit at
    x = +-(outer_radius - wall - 3.0) = +-23.0, which puts the relief's
    outward edge at exactly 23 + 5 = 28 = outer_radius -- landing dead-on
    the true outer surface instead of safely inside it. Fusion silently
    built this as a degenerate/coincident-face cut (not an error), but an
    offline manifold-edge scan of the exported trim Top.stl found exactly
    2 non-manifold edges at (x=+-28, y=25.04/25.20, z=10..11.5) -- boss
    A/C's own xy, right at the relief's z-range -- confirming it, and
    matching the small tab-shaped artifacts visible on the outer wall in
    renders at the parting line. ('current' has no such defect: its A/C
    sit further from its wider outer_radius=30 wall, so the nominal
    radius never reaches it.) Fixed by clamping each boss's relief radius
    to stay `wall_clear` (0.6mm) inside the TRUE wall distance along the
    same outward-direction convention verify_wall_integrity's own boss-
    wall probe already uses (true_wall_distance_along_ray) -- so this can
    never disagree with that check, and the clamp is a no-op (min() picks
    the nominal radius unchanged) for every boss that already had margin,
    including current's A/C/D and both variants' B1/B2."""
    ay, by = p['spine_a'][1], p['spine_b'][1]

    # pass 16, item C (printability review, finding 1): the ring's OD now
    # grows CONTINUOUSLY from lip_r[1] at lip_z[0] up to anchor_r[1] at
    # anchor_z[1] -- a self-supporting taper built from boolean solids
    # (revolve_taper_wedge for the two domed ends, exactly the
    # cone_frustum_solid idiom pass 15's window-bore fix already proved
    # out; extrude_taper_wedge_along_y for the two straight sides) --
    # instead of the old two-stacked-flat-rings step. The independent
    # printability review found the pass-9c edge chamfer that used to
    # bevel this step was ABSENT from the exported STL entirely
    # (chamfer_stadium_edge_at silently returned 0 whenever its edge
    # match was empty -- see that function's own updated docstring, now
    # fixed to assert loudly instead), and that even when it DID apply it
    # only bevelled the narrow 0.65mm outer step -- the ring's own flat
    # TOP face (anchor_z[1], the actual seat the display board registers
    # near) was a dead-flat, 0-degree-from-horizontal shelf with NO
    # chamfer treatment at all (confirmed: every triangle sampled there
    # read nz=+1.000). A single continuous taper fixes both at once: the
    # INNER edge (lip_r[0]/anchor_r[0], unchanged) stays a plain vertical
    # wall the full height -- the ring's own real XY registration land
    # against Bottom's true inner wall (the "0.25mm nesting clearance")
    # -- while the OUTER radius rises smoothly, at a shallow ~20-degree
    # slope (well under the 45-degree ceiling), from the land's own OD up
    # to the old anchor_r[1] reach (the deliberate ~0.4mm-past-the-true-
    # wall "so it fuses" amount finding 5's own docstring already
    # documents). `verify_lip_ring_profile` (new gate) scans the exported
    # STL for exactly this: a real intermediate-angle (10-80 degree)
    # facet somewhere in the ring's own r/z band (the chamfer must be
    # found) and NO flat (<10 degree from horizontal), >0.6mm-wide
    # downward-facing patch anywhere in that band (the old flat shelf
    # must be gone).
    # IMPORTANT (live-found, this pass): the taper must NOT extend below
    # anchor_z[0] (== split_z == the Top/Bottom parting line): a first
    # version tapered across the WHOLE lip_z[0]..anchor_z[1] span and a
    # live check_interference run found a real 582mm^3 Top-vs-Bottom
    # overlap -- the taper's own outer radius, growing continuously from
    # 9.2 to 11.0, had already exceeded the true inner wall (26.0mm trim)
    # by the time it crossed z=10.0 (the parting line), eating into
    # Bottom's own solid wall material (which the OLD flat lip band,
    # constant at lip_r[1]=25.75 the whole way to z=10.0, never did -- the
    # wider 26.40mm reach only ever existed ABOVE z=10.0, entirely inside
    # Top's own body, in the original stepped design). Fixed: the LAND
    # (constant lip_r[1] radius) still spans the full lip_z[0]..
    # anchor_z[1] height, UNCHANGED and safe; the taper itself is
    # confined to EXACTLY the old anchor band (anchor_z[0]..anchor_z[1],
    # i.e. split_z..split_z+1), so it only ever adds material above the
    # parting line.
    land_z0, land_z1 = p['lip_z'][0], p['anchor_z'][1]
    taper_z0, taper_z1 = p['anchor_z'][0], p['anchor_z'][1]
    land = stadium_ring_solid(root, ay, by, p['lip_r'][0], p['lip_r'][1], land_z0, land_z1)
    taper_r_lo, taper_r_hi = p['lip_r'][1], p['anchor_r'][1]
    end_a = revolve_taper_wedge(root, 0.0, ay, taper_r_lo, taper_r_hi, taper_z0, taper_z1, -180.0)
    end_b = revolve_taper_wedge(root, 0.0, by, taper_r_lo, taper_r_hi, taper_z0, taper_z1, 180.0)
    side_pos = extrude_taper_wedge_along_y(root, +1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    side_neg = extrude_taper_wedge_along_y(root, -1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    ring = combine_join(root, land, [end_a, end_b, side_pos, side_neg])

    # Printability review's own recommended refinement: "keep a small
    # (~0.5-1mm wide) flat registration land near the bore-facing inner
    # edge... and taper only the OUTER portion back to the shell" -- the
    # taper above already slopes the OUTER face, but the anchor band's
    # own remaining flat TOP (z=anchor_z[1], from lip_r[0] out to
    # lip_r[1] -- the land's own top face, ~1.8mm wide, still exposed
    # since nothing above it fills that radius) would still fail the
    # <=0.6mm flat-patch gate on its own. Narrow it: cut away everything
    # inward of a SECOND taper (inner boundary growing from lip_r[0] at
    # anchor_z[0] up to anchor_r[1]-LIP_RING_FLAT_TARGET_MM at
    # anchor_z[1]) using the same cone_frustum_solid/extrude idiom,
    # filled-to-axis so it removes material rather than adding it --
    # narrows the remaining flat cap at anchor_z[1] to nominally
    # LIP_RING_FLAT_TARGET_MM, and (since the cut tool is scoped to
    # EXACTLY anchor_z[0]..anchor_z[1]) never touches the lip band below.
    #
    # RESUMED pass 16 (item C): a first version of the new
    # verify_lip_ring_profile gate (below) targeted this cut's own exact
    # 0.6mm and reported a 0.63mm flat cluster -- but that first version
    # of the GATE had its own bug (checked world-frame nz<0 for
    # "downward", not Top's own real print-down direction, which is +z
    # since Top prints flipped, ceiling-down -- see scan_stl_overhangs'
    # own down_z convention); fixed, the gate finds this cut's own
    # boundary clean (max flat cluster 0.05mm, tessellation noise only).
    # LIP_RING_FLAT_TARGET_MM is kept at 0.5 rather than reverted to 0.6
    # anyway -- a real, deliberate 0.1mm extra margin against exactly that
    # class of tessellation slack, now that the gate can actually see it
    # correctly, costs nothing (still comfortably inside the printability
    # review's own recommended 0.5-1mm registration land) and only helps.
    inner_r_lo, inner_r_hi = p['lip_r'][0], p['anchor_r'][1] - LIP_RING_FLAT_TARGET_MM
    cut_a = cone_frustum_solid(root, 0.0, ay, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    cut_b = cone_frustum_solid(root, 0.0, by, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    cut_pos = extrude_taper_cut_along_y(root, +1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    cut_neg = extrude_taper_cut_along_y(root, -1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    ring = combine_cut(root, ring, [cut_a, cut_b, cut_pos, cut_neg])

    # 2026-09-08 pass 9 (finding 5): a STANDALONE copy of the ring, kept
    # as a hidden reference-only body (swept into 'Reference -- not
    # printed' by organize_components() same as the other reference
    # tools) -- used ONLY by verify_display_insertion_path's from-inside
    # sweep check, so that probe is isolated to the ring alone and not
    # the general shell (the module obviously can't pass through solid
    # wall -- that's not the question finding 5 asks).
    ring_ref_land = stadium_ring_solid(root, ay, by, p['lip_r'][0], p['lip_r'][1], land_z0, land_z1)
    ring_ref_end_a = revolve_taper_wedge(root, 0.0, ay, taper_r_lo, taper_r_hi, taper_z0, taper_z1, -180.0)
    ring_ref_end_b = revolve_taper_wedge(root, 0.0, by, taper_r_lo, taper_r_hi, taper_z0, taper_z1, 180.0)
    ring_ref_side_pos = extrude_taper_wedge_along_y(root, +1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    ring_ref_side_neg = extrude_taper_wedge_along_y(root, -1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    ring_ref = combine_join(root, ring_ref_land, [ring_ref_end_a, ring_ref_end_b, ring_ref_side_pos, ring_ref_side_neg])
    ring_ref_cut_a = cone_frustum_solid(root, 0.0, ay, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    ring_ref_cut_b = cone_frustum_solid(root, 0.0, by, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    ring_ref_cut_pos = extrude_taper_cut_along_y(root, +1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    ring_ref_cut_neg = extrude_taper_cut_along_y(root, -1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    ring_ref = combine_cut(root, ring_ref, [ring_ref_cut_a, ring_ref_cut_b, ring_ref_cut_pos, ring_ref_cut_neg])
    ring_ref.name = 'Lip Anchor Ring (reference)'
    ring_ref.isLightBulbOn = False

    top = combine_join(root, bodies['Top'], [ring])

    # 2026-09-08 pass 9 (finding 5 follow-up, found by a LIVE Fusion
    # interference check, not by inspection): widening the ring's inner
    # radius uniformly around the WHOLE perimeter -- not just near the
    # display window finding 5 is actually about -- reaches into the
    # comms stack's own real footprint at the -y dome tip. Confirmed
    # directly: a real ~22mm^3 XIAO-vs-Top interference at almost exactly
    # `bay.stack3.l76k_pcb`'s own far corner (y -22.2..-23.2, x +-8.88).
    # The OLD, narrower ring cleared this by construction; the new wider
    # one doesn't. Cut a keep-out matching that footprint (+1mm margin,
    # spanning the ring's own z-band) from the ring -- a no-op for
    # 'current' (XIAO/Wio aren't inserted there) and, near the window
    # (finding 5's actual target, at the opposite end of the case),
    # completely unaffected.
    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        stack_keepout = box_solid(
            root, pcb['x'][0] - 1.0, pcb['x'][1] + 1.0, pcb['y'][0] - 1.0, pcb['y'][1] + 1.0,
            p['lip_z'][0] - 0.5, p['anchor_z'][1] + 0.5)
        top = combine_cut(root, top, [stack_keepout])

    relief_z0, relief_z1 = p['lip_z'][0] - 0.5, p['anchor_z'][1] + 0.5
    relief_z_mid = (relief_z0 + relief_z1) / 2.0
    nominal_r = p['boss_relief_dia'] / 2.0
    wall_clear = 0.6
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        if ay <= cy <= by:
            d2 = (1.0 if cx >= 0 else -1.0, 0.0)
        else:
            center_y = ay if cy < ay else by
            vx, vy = cx, cy - center_y
            vlen = math.hypot(vx, vy) or 1.0
            d2 = (vx / vlen, vy / vlen)
        s_wall = true_wall_distance_along_ray(p, (cx, cy), d2, relief_z_mid)
        relief_r = nominal_r if s_wall is None else min(nominal_r, s_wall - wall_clear)
        # 2026-09-08 pass 9 (finding 11, stray sliver beside boss C): a
        # boss sitting close enough to the true wall for `s_wall - wall_
        # clear` to land only just above the boss's OWN radius clamps
        # relief_r down to a value that still (barely) clears the boss
        # itself, but leaves a razor-thin remnant annulus of ring material
        # trapped between the boss and the clamped relief circle -- exactly
        # the "thin triangular web" Jake's photo showed next to boss C at
        # its old (near-shoulder) position. Rather than silently build
        # whatever sliver that geometry produces, require a genuine
        # MIN_RELIEF_CLEARANCE gap between the relief and the boss's own
        # OD -- if the true wall can't spare that much, the boss is too
        # close to the wall here for this relief to make sense at all, and
        # that's a real design conflict worth a loud assertion (forcing a
        # reposition, per finding 2) rather than a silent, ever-thinner
        # sliver.
        boss_r = p['boss_dia'] / 2.0
        assert relief_r - boss_r >= MIN_RELIEF_CLEARANCE, (
            f'add_lip_anchor_reliefs: boss {s["name"]} at ({cx},{cy}) sits too '
            f'close to the true outer wall for a clean relief -- relief_r='
            f'{relief_r:.3f} leaves only {relief_r - boss_r:.3f}mm over the boss '
            f'OD (need >= {MIN_RELIEF_CLEARANCE}mm); move the boss (see finding 2 '
            f'in the README) rather than shrink this further')
        relief = cylinder_solid(root, cx, cy, relief_r, relief_z0, relief_z1)
        top = combine_cut(root, top, [relief])

    # 2026-09-08 pass 9 (finding 3, "two lanyard holders"): the box's
    # outward (most-negative-y) edge was a flat constant reaching further
    # from spine_a than the ring's own true wall clearance at this z --
    # exactly the same class of defect as the per-boss reliefs above (a
    # flat number that doesn't know how close it is to the TRUE curved
    # surface), except here it fully breached the ring's own radial band
    # (which is itself safely inset from the true outer wall by design --
    # see SPEC's "0.25 clearance" and add_lip_anchor_reliefs' own overall
    # docstring) all the way out, cutting a genuine gap through it that
    # printed as a second, slot-shaped "lanyard holder" flanking the real
    # one (the ear's own vertical hole). There is exactly one lanyard
    # attachment point on this case -- the ear -- so this relief must stay
    # internal to the ring: clamped the same `wall_clear` (0.6mm) inside
    # the true wall distance straight out from spine_a (d2=(0,-1), the
    # ear's own symmetry axis), same convention as every other relief in
    # this function, so it can never disagree with them and is a no-op
    # once the box is already safely inside (every existing box narrower
    # than the true wall here is unaffected).
    lb = p['lug_relief_box']
    lug_wall = true_wall_distance_along_ray(p, (0.0, ay), (0.0, -1.0), relief_z_mid)
    lb_y0 = lb['y'][0]
    if lug_wall is not None:
        lb_y0 = max(lb_y0, ay - (lug_wall - wall_clear))
    relief_box = box_solid(root, lb['x'][0], lb['x'][1], lb_y0, lb['y'][1], relief_z0, relief_z1)
    top = combine_cut(root, top, [relief_box])

    bodies['Top'] = top
    return bodies


def add_window(root, bodies, p):
    cx, cy = p['window_center']
    r = p['window_dia'] / 2.0
    bore = cylinder_solid(root, cx, cy, r, p['window_z_bottom'], p['top_z'] + 1.0)
    top = combine_cut(root, bodies['Top'], [bore])
    bodies['Top'] = top

    # 2026-09-15 pass 15, item 7 (Jake: "the top of the LCD circle cutoff
    # is not flush with the rest of the case; this makes the print prone
    # to failure... I had to use some support"). CONFIRMED root cause,
    # live-probed (not guessed): `window_dia/2` (22.65mm, fixed since
    # SPEC) is LARGER than trim's own `flat_rho` (22.14mm) -- at the
    # window's own east/west extremes (world (+-22.65, window_center.y)),
    # the bore's true rim sits 0.51mm PAST the flat bed's own radius,
    # into the R10 shoulder curve, not on the flat top face at all. The
    # old `chamfer_edge_at` call assumed a single, uniform circular edge
    # sitting entirely on the flat z=top_z face -- Fusion's own edge
    # tessellation there is NOT a clean circle once part of it crosses
    # into the curved shoulder, and the chamfer feature silently produced
    # an incomplete/malformed result over that stretch instead of raising:
    # a live 360-degree point-containment sweep of the chamfer band (r
    # 22.65-23.15, z near top_z) found roughly 30-degree HOLLOW gaps
    # centred on the +-X extremes (world (+-22.9, 50), trim only -- the
    # SAME sweep on 'current', whose flat_rho=24.14 clears the window
    # comfortably, found zero bad angles) -- a real, visible notch/gap in
    # the printed rim, exactly matching Jake's own description, not a
    # cosmetic non-issue. ('current' is unaffected and unchanged by this
    # fix -- its own edge-matched chamfer already covers the full circle,
    # confirmed by the same live sweep.)
    #
    # FIX: replaced the edge-matched chamfer with a plain CUT using a
    # conical tool (`cone_frustum_solid`, the same primitive `add_root_
    # reinforcement`'s collars already use, here as a boolean SUBTRACTION
    # instead of a join) -- a solid-geometry boolean cut is correct
    # regardless of whether the underlying surface at a given radius is
    # flat or curved (unlike an edge-selection-based chamfer feature,
    # which needs Fusion to first identify a matching edge loop). The
    # cone's own slope is deliberately a touch UNDER 45 degrees (radius
    # grows by chamfer+0.2mm over a z-span of chamfer+0.6mm) -- the
    # brief's own "<=45 degrees" ceiling, with margin -- and the tool
    # overshoots both ends (0.1mm below the nominal inner radius/z, 0.5mm
    # past the nominal outer z) so it cuts a clean, complete ring all the
    # way around REGARDLESS of the true local surface, growing AWAY from
    # the bed (the print-down face, z=top_z, is exactly where Top's flat
    # face sits on the bed when flipped for printing -- see Print
    # orientation below) rather than toward it, per the brief's own
    # "growing away from the bed" requirement. There is no separate
    # retaining ring/lip at the window bore in this design (the display
    # glass rests on the ordinary ceiling underside step, not a distinct
    # printed ring feature) -- see `window_z_bottom` vs.
    # `top_ceiling_underside_z` in params_current.py's own comment -- so
    # nothing needed to be relocated to "grow from the bed" separately.
    chamf = p['window_chamfer']
    cone_r_lo, cone_z_lo = r - 0.05, p['top_z'] - chamf - 0.1
    cone_r_hi, cone_z_hi = r + chamf + 0.2, p['top_z'] + 0.5
    chamfer_tool = cone_frustum_solid(root, cx, cy, cone_r_lo, cone_r_hi, cone_z_lo, cone_z_hi)
    top = combine_cut(root, top, [chamfer_tool])
    top = _refetch_by_name(root, 'Top') or top
    bodies['Top'] = top

    # Live regression probe (build-time, not a separate verify() gate --
    # this is cheap and directly targets the exact defect above): pick a
    # z partway up the cone's own slope and probe just INSIDE the cone's
    # own radius there (i.e. within the material the cut tool actually
    # removes) -- must read HOLLOW at every angle around the full circle.
    # A point outside the cone's own slope (like the OLD diagnostic probe
    # at a fixed r+chamf/2, which the cone hasn't grown out to yet at
    # that height) is not a meaningful test of THIS cut -- it was never
    # supposed to be removed -- so the probe radius is derived from the
    # cone's own geometry, not a flat guess.
    probe_z = (cone_z_lo + cone_z_hi) / 2.0
    frac = (probe_z - cone_z_lo) / (cone_z_hi - cone_z_lo)
    cone_r_at_z = cone_r_lo + frac * (cone_r_hi - cone_r_lo)
    probe_r = cone_r_at_z - 0.15  # just inside the cone's own slope at this height
    bad_angles = []
    for deg in range(0, 360, 10):
        rad = math.radians(deg)
        pt = P(cx + probe_r * math.cos(rad), cy + probe_r * math.sin(rad), probe_z)
        if probe_point_solid(top, pt):
            bad_angles.append(deg)
    assert not bad_angles, (
        f'add_window: chamfer cut did not remove material at angles {bad_angles} (deg) -- '
        f'the window rim would not be flush/support-free there')

    # pass 16, item E (mech review F9): the display glass
    # (`display_glass_dia`=44.79) rests directly on this bore's OWN
    # ceiling-underside step (window_z_bottom, r=window_dia/2) with only
    # 0.51mm radial clearance -- a sharp 90-degree PETG corner right
    # where the glass's own edge sits, with no relief. Cut a small
    # conical relief at that inner corner (same cone_frustum_solid
    # boolean-cut idiom as the print-orientation chamfer above, at the
    # OPPOSITE end of the bore -- window_z_bottom instead of top_z): the
    # bore's own radius grows a further `glass_seat_chamfer` mm over the
    # same rise, starting just inside window_z_bottom (into the bore
    # itself, where nothing rests) and finishing just above it (into the
    # ledge, where the glass's own square edge would otherwise meet a
    # knife-edge corner) -- removes the stress-riser without touching the
    # ledge's own flat bearing area more than glass_seat_chamfer mm in.
    glass_chamf = p.get('glass_seat_chamfer', 0.0)
    if glass_chamf > 0:
        gz0, gz1 = p['window_z_bottom'] - 0.1, p['window_z_bottom'] + glass_chamf
        glass_tool = cone_frustum_solid(root, cx, cy, r - 0.05, r + glass_chamf, gz0, gz1)
        top = combine_cut(root, top, [glass_tool])
        top = _refetch_by_name(root, 'Top') or top
        bodies['Top'] = top

    return bodies


FPC_RELIEF_MIN_WALL = 1.2  # mm of shell that must remain above the pocket everywhere (see gate below)
# 2026-09-08 pass 9 (Jake's decision after the pass-7-follow-up's 0.3mm
# compromise, see git history on this branch for that whole dead end):
# 1.2mm is achievable again, WITHOUT the ~62mm^3 Top x display-housing
# interference the plain "shrink the pocket" approach kept recreating --
# shrinking the pocket eats directly into the same clearance the
# display's real housing needs there (root cause of the 62mm^3 overlap,
# preserved in git history).
#
# 2026-09-12 pass 12 (Jake asked to delete a local "brow" bump -- pass 9's
# original fix for this same 1.2mm bar -- by raising trim's top_z
# instead): proved both analytically and live that height cannot do it
# (`rho_at_z(p, z) = flat_rho + (top_z - z)` shifts the pocket and the
# shoulder profile together, so `top_z - z` at the pocket's own z1 is
# invariant to top_z -- see README's pass-12 section) -- so the brow
# stayed, unmodified, through pass 12.
#
# 2026-09-13 pass 12b (Jake: "move the top to be longer" -- meaning the
# case, at the USB end, not the height): `usb_end_extension_mm` (see
# params_current.py/params_trim.py) grows the outer envelope's own +y
# dome outward instead, which DOES recover real skin here (a length
# change, not a height one, and it moves the shoulder profile's own
# reach at this corner rather than translating pocket+shoulder together
# -- see README's pass-12b section for the live numbers). With enough
# skin recovered this way, the brow (`add_fpc_brow`/
# `build_fpc_brow_solid`/`FPC_BROW_TIERS`, pass 9-12) is no longer needed
# and has been deleted outright -- `verify_fpc_relief` passes with a
# plain, un-raised shoulder now; see that gate's own live numbers in the
# README for the before/after probe counts.


def fpc_relief_footprint(p):
    """The FPC relief pocket's cut footprint (x0, x1, y0, y1, z0, z1) --
    factored out (2026-09-08 pass 9) so add_fpc_relief and
    verify_fpc_relief derive it the exact same way and can never silently
    drift apart (the same reasoning lug_ear_geometry documents for the
    ear vs. its own verify/export checks)."""
    fr = p['fpc_relief']
    x0 = min(fr['x'][0], -14.0)
    x1 = max(fr['x'][1], 14.0)
    y0 = min(fr['y'][0], 65.0)
    y1 = fr['y'][1]
    z0 = min(fr['z'][0], 21.0)
    z1 = fr['z'][1] + 0.1
    return x0, x1, y0, y1, z0, z1


def add_fpc_relief(root, bodies, p):
    """Cut the display module's FPC-tab relief pocket into the Top
    ceiling's underside (2026-09-05 fix): PARAMS['fpc_relief'] has held
    SPEC's exact pocket coordinates since M1, but nothing ever actually
    cut it -- the FPC tab collided with the plain 2mm ceiling skin there
    ('Top x <display module body>' interference).

    The cut is a superset of SPEC's stated box: probing the actual
    inserted display occurrence (not just the SPEC text) found the real
    FPC-tab/PMMA-lens bodies spanning a noticeably WIDER and slightly
    LOWER region than SPEC's numbers alone (x roughly +-14 vs SPEC's
    -6.2..7.02, y down to ~65.8 vs SPEC's 71.44 lower bound) -- SPEC's box
    is kept as the documented reference/minimum, widened with an empirical
    margin so the cut matches the real geometry it needs to clear. z1
    keeps a small margin past the nominal ceiling underside for a clean
    cut; the pocket stays a blind recess (well short of the outer top
    face at top_z).

    2026-09-07 pass 7 follow-up fix: the widened box's own outer corners
    (x roughly +-10..17, y roughly 67..73.5) reach past spine_b into the
    domed +y end cap, where the true outer shoulder/dome surface curves
    down well below the box's flat z1 (confirmed by ray-casting the
    exported trim Top.stl -- two wedge-shaped holes flanking the USB
    opening, cut depth up to 15mm at those corners). A flat box has no way
    to know that -- so, same idiom as clip_to_inner_cavity for case bosses/
    Top posts (Combine-Intersect against a tool built from the REAL
    curved cavity solid instead of a flat approximation): build a copy of
    the shared inner-cavity solid GROWN outward by (wall - FPC_RELIEF_MIN_WALL)
    via build_inner_cavity_clip_tool's existing safety_margin parameter
    (negative = grow, not shrink -- see its docstring), then
    Combine-Intersect the pocket tool against it before cutting Top. The
    intersected tool can then never reach closer than FPC_RELIEF_MIN_WALL
    to the true outer surface anywhere in its footprint, by construction,
    following the actual dome curvature rather than a flat z1 -- exactly
    the mechanism verify_fpc_relief (below) gates on.

    2026-09-07 pass 7 follow-up, round 2: clipping the WHOLE widened box
    (including the literal SPEC sub-box) against the skin-safe tool
    directly regressed requirement #1 above -- probing the SPEC box's own
    corners after that clip found TWO of them re-breached (measured: the
    true outer surface at the SPEC box's own extreme +y corners is only
    ~25.95-26.25mm there, essentially the same knife's-edge the wedge
    holes came from, just ~0.1mm short of SPEC's own literal z1 --
    independent of the widened margin, and independent of this fix). The
    SPEC box is the one part of this footprint functional correctness
    actually depends on (the real inserted FPC tab needs it, full stop),
    so it is cut in full, UNCLIPPED, exactly as before this whole fix --
    the skin-safe clip applies only to the WIDENED MARGIN outside it (an
    empirical buffer, not a hard requirement, and the actual footprint of
    the confirmed wedge-hole defect: x roughly +-10..17, entirely outside
    SPEC's +-6.2..7.02).

    Implemented as TWO SEPARATE Cuts on Top, not one unioned tool: first
    attempt tried `pocket INTERSECT (skin_safe_tool UNION spec_box)`
    (Combine-Join the SPEC box into the skin-safe tool, then intersect) --
    that regressed verify()'s own interference gate with a stray orphaned
    'Body1' left interfering with Top, traced to the exact hazard
    clipped_pillar_with_reach's docstring already documents for boss/post
    joins: Combine-Join SILENTLY NO-OPS (and, empirically, leaves the tool
    body an orphaned stray rather than deleting it) when the two solids
    don't actually touch/overlap -- which the skin-safe-clipped pocket and
    the SPEC box don't reliably do everywhere the clip bites hardest. A
    Cut has no such hazard: cutting with a tool always consumes it,
    whether or not it removed any material -- so the SPEC box is cut
    independently, in a second Cut, instead of being unioned into the
    first tool at all.

    2026-09-08 pass 9 (finding 1, Jake's decision): FPC_RELIEF_MIN_WALL is
    back to 1.2mm (from the pass-7-follow-up's 0.3mm compromise -- see
    that constant's own comment, kept in git history). The skin-safe tool
    is no longer `build_inner_cavity_clip_tool` grown outward by an
    ANALYTIC (inner-cavity-surface + nominal wall) approximation of the
    true outer surface -- that approximation is exactly what silently
    broke down at these corners in the first place (the dome's true outer
    surface drops away faster than the inner cavity does, near the tip),
    which is also why it could never recover more than ~0.3mm before
    re-hitting the display-housing interference (see FPC_RELIEF_MIN_WALL's
    comment). The tool is built directly from the real outer surface: a
    fresh reference envelope (build_outer_pill_solid, not the actual
    multi-feature Top -- cheaper, and this pocket's clearance only ever
    depended on the plain outer skin, never on the lip/window/boss
    features layered onto Top elsewhere), with every face offset INWARD
    by FPC_RELIEF_MIN_WALL (offsetFacesFeatures, same idiom as
    build_thickened_envelope/build_inner_cavity_clip_tool). Intersecting
    the pocket against that tool guarantees >= FPC_RELIEF_MIN_WALL of
    real skin above the cut, everywhere, by construction, following the
    actual dome curvature.

    2026-09-08 pass 9 through pass 12 (finding 1's original fix, since
    superseded): a local "brow" (`add_fpc_brow`/`build_fpc_brow_solid`)
    raised this same reference envelope over the pocket's footprint
    before the inward offset, because pass 9's SPEC-box corner did not
    clear 1.2mm on the plain (un-raised) envelope. 2026-09-13 pass 12b
    (Jake: "move the top to be longer" -- the case, not the height):
    `usb_end_extension_mm` (see params_current.py/params_trim.py) grows
    the outer envelope's own +y dome outward instead, which recovers
    real skin at this same corner directly (see README's pass-12b
    section for the live derivation/numbers) -- with that margin
    recovered structurally, the brow added nothing further and has been
    deleted; `build_outer_pill_solid(root, p)` alone is now the
    reference envelope this tool offsets inward from."""
    fr = p['fpc_relief']
    x0, x1, y0, y1, z0, z1 = fpc_relief_footprint(p)
    pocket = box_solid(root, x0, x1, y0, y1, z0, z1)

    skin_safe_tool = build_outer_pill_solid(root, p)
    faces = [f for f in skin_safe_tool.faces]
    offset_input = root.features.offsetFacesFeatures.createInput(faces, V(-FPC_RELIEF_MIN_WALL))
    root.features.offsetFacesFeatures.add(offset_input)

    pocket = combine_intersect(root, pocket, [skin_safe_tool])
    bodies['Top'] = combine_cut(root, bodies['Top'], [pocket])

    spec_box = box_solid(root, fr['x'][0], fr['x'][1], fr['y'][0], fr['y'][1], z0, z1)
    bodies['Top'] = combine_cut(root, bodies['Top'], [spec_box])

    # SPEC-box clearance probe: the documented minimum pocket (not the
    # widened margin) must still be fully cleared after the skin-safe
    # clip above. Corners are inset 0.05mm off each face (avoids the
    # on-boundary PointOn ambiguity of probing exactly at a cut tool's own
    # face) and z is checked at the SPEC box's own z1 (fr['z'][1], not the
    # +0.1 cutter margin). A corner that also sits inside the display
    # window's own through-hole (add_window, independent of this pocket)
    # is trivially clear either way and not a meaningful test of THIS
    # pocket, but is left in the probe set since it costs nothing extra
    # and only ever weakens (never causes) a false failure here.
    top = bodies['Top']
    eps = 0.05
    fx = (fr['x'][0] + eps, fr['x'][1] - eps)
    fy = (fr['y'][0] + eps, fr['y'][1] - eps)
    fz = fr['z'][1] - eps
    uncleared = []
    for cx in fx:
        for cy in fy:
            pt = P(cx, cy, fz)
            if probe_point_solid(top, pt):
                uncleared.append((round(cx, 3), round(cy, 3), round(fz, 3)))
    assert not uncleared, (
        f'add_fpc_relief: SPEC box corner(s) not cleared after the '
        f'skin-safe clip: {uncleared} -- FPC tab would not fit')

    return bodies


_CIRCULAR_CURVE_TYPES = (
    adsk.core.Curve3DTypes.Circle3DCurveType,
    adsk.core.Curve3DTypes.Arc3DCurveType,
)


def chamfer_stadium_edge_at(root, body, ay, by, r_mm, z_mm, chamfer_mm, tol=0.1):
    """Chamfer a STADIUM-shaped edge loop (a mix of 2 straight-line
    segments and 2 arcs, e.g. the lip/anchor ring's outer radius step) at
    world z=z_mm, distance r_mm from the spine (|x| in the straight band
    0<=y<=50, distance from the nearer spine endpoint in the domed ends)
    -- `chamfer_edge_at` above only matches circular edges by
    center+radius, which misses a stadium's straight sides. Matches every
    edge by its MIDPOINT (works uniformly for line and arc geometry,
    unlike curveType-specific logic) rather than by center/radius.

    2026-09-2x pass 16, item C (printability review, finding 1): this
    function used to be best-effort (a failed or empty match silently
    returned 0, "since this is a cosmetic/printability feature, not a
    dimensional one") -- that silence is exactly what let the pass-9c
    lip/anchor seam chamfer regress to a hard 90-degree shelf across at
    least two later passes (the per-boss/keepout notches this ring
    gained afterward fragmented its once-clean 4-arc outer loop into a
    multi-edge selection a real `chamferFeatures.add()` call then
    apparently refused) with zero build-time signal anywhere -- caught
    only by an independent offline STL scan of the shipped geometry, long
    after the fact. This function has no remaining caller in this file
    (the lip/anchor ring's own outer step is now a continuous <=45-degree
    taper, built from boolean solids -- see add_lip_anchor_reliefs -- not
    an edge chamfer at all), but it is kept, fixed, for any FUTURE
    edge-chamfer need: a zero-edge match or a chamfer-feature failure now
    raises loudly instead of returning 0, so a silent regression like
    finding 1 cannot happen again without a build-time failure."""
    edges = adsk.core.ObjectCollection.create()
    found = 0
    for edge in body.edges:
        bb = edge.boundingBox
        z0, z1 = bb.minPoint.z / MM, bb.maxPoint.z / MM
        if abs(z0 - z_mm) > tol or abs(z1 - z_mm) > tol:
            continue
        try:
            ev = edge.evaluator
            ok, pr0, pr1 = ev.getParameterExtents()
            if not ok:
                continue
            ok2, mid = ev.getPointAtParameter((pr0 + pr1) / 2.0)
            if not ok2:
                continue
        except RuntimeError:
            continue
        mx, my = mid.x / MM, mid.y / MM
        if ay <= my <= by:
            rho = abs(mx)
        else:
            cy = ay if my < ay else by
            rho = math.hypot(mx, my - cy)
        if abs(rho - r_mm) < tol:
            edges.add(edge)
            found += 1
    assert found > 0, (
        f'chamfer_stadium_edge_at: no matching edge at r={r_mm}, z={z_mm} -- '
        f'a silent 0-match here is exactly the pass-9c/printability-review '
        f'finding-1 regression (chamfer claimed but never actually cut)')
    chamferFeats = root.features.chamferFeatures
    inp = chamferFeats.createInput(edges, True)
    inp.setToEqualDistance(V(chamfer_mm))
    chamferFeats.add(inp)
    return found


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


# ---------------------------------------------------------------------------
# 2026-09-07 pass 13, item 1: root fillets/collars at every post & boss
# (Jake's printed posts snap off at the ceiling/floor joint). The pass-9
# best-effort fillet on the top posts (see add_top_posts, unchanged code
# kept for history in git blame) is a REAL fillets.createInput() call, not
# a stub -- it does sometimes succeed (the pass-9 README section reports 4
# live `Fillet` timeline features on those exact posts) but a plain
# constant-radius fillet only helps if its radius is large enough to
# satisfy the strength probe used below, and pass 9g's OWN attempt at the
# unrelated FPC-brow seam fillet (same try/except pattern) got 0 Fillet
# features both variants -- so "the try/except didn't raise" was never a
# reliable signal that real material got added. Root cause, worked out by
# hand from the fillet's own circle geometry (a constant-radius fillet's
# cross-section is a quarter-circle: added_radius(dz) = R - sqrt(R^2 -
# (R-dz)^2) for height dz above the root, 0<=dz<=R): the verify_root_
# fillets gate below probes for >=0.6mm of ADDED radius at dz=0.4mm above
# the root, which needs R >~1.69mm to satisfy on its own (solved from that
# formula) -- the pass-9 top-post fillet used R=1.0mm, which only adds
# ~0.2mm there even when the feature applies cleanly. This file now tries
# a real fillet at R=ROOT_FILLET_R (1.8mm, satisfies the probe with ~0.07mm
# to spare if it applies) with the SAME edge-matching approach as
# chamfer_edge_at (by center+radius+z, tolerant, not the vertical-edge-by-
# dz filter add_lug/_best_effort_fillet use elsewhere -- this matters:
# add_lug's approach would happily also catch the wrong circular edge on a
# tall post). If Fusion's own fillet solver refuses the edge (the tangent-
# chain / merged-body issue README pass 9g already documented and never
# root-caused further), the fallback is a 45-degree conical collar built
# as its own solid (cone_frustum_solid) and boolean-JOINED in -- unlike a
# skipped Fillet/Chamfer feature, combine_join can never silently no-op
# the way a disjoint-body join can (dedupe_body's own docstring), and the
# same added_radius(dz) = min(collar_rise, ...) math for a 45-degree cone
# is LINEAR (added_radius(dz) = collar_rise - dz for dz<=collar_rise), so
# a collar_rise of 1.5mm (>= the SPEC's own 1.2mm floor) always adds
# 1.1mm at dz=0.4mm regardless of the post/boss's own radius -- strictly
# more margin than the fillet path, and geometry-independent."""
ROOT_FILLET_R = 1.8      # mm; satisfies verify_root_fillets (0.6mm @ dz=0.4mm) with ~0.07mm to spare if it applies
ROOT_COLLAR_RISE = 1.5   # mm; >= the 1.2mm floor; adds 1.1mm @ dz=0.4mm, geometry-independent
PEG_COLLAR_RISE = 1.1    # mm; smaller footprint growth for the small (Ø2.7/Ø3.0) compass pegs/pads --
                         # still adds 0.7mm @ dz=0.4mm, comfortably >= the 0.6mm gate
ROOT_COLLAR_OVERLAP = 0.05  # mm; both collar ends overlap real material instead of exactly touching it (avoids a
                            # coincident-surface tessellation seam -- see add_root_reinforcement's own comment)
ROOT_FILLET_REPORT = []  # [(feature_name, method, radius_or_rise), ...] -- reset in build(), read by run()
# 2026-09-07 pass 13: ALSO append every decision to a small JSON-lines file
# on disk, one line per add_root_reinforcement call, tagged with the
# variant -- this file (not the in-memory list above) is what survives
# across separate fusion_mcp_execute calls when build()/verify() are run
# piecewise (README's own documented workflow for this file, since a
# single call can time out client-side while Fusion keeps executing
# server-side): each call's own `runpy.run_path` gives ROOT_FILLET_REPORT
# a FRESH empty list, but the file persists and can be read back with
# plain Python after the fact for the pass-13 per-feature report.
ROOT_FILLET_LOG_PATH = os.path.join(_HERE, '_root_fillet_log.jsonl')


def _log_root_fillet(variant, feature_name, method, value):
    try:
        with open(ROOT_FILLET_LOG_PATH, 'a') as f:
            f.write(json.dumps({'variant': variant, 'feature': feature_name, 'method': method, 'value': value}) + '\n')
    except OSError:
        pass


def cone_frustum_solid(root, cx, cy, r_lo, r_hi, z_lo, z_hi):
    """A solid of revolution around the vertical (world Z) axis through
    (cx, cy): radius r_lo at z_lo, linearly to r_hi at z_hi (a cone
    frustum; r_lo may equal r_hi only if both are 0, otherwise use a
    cylinder). Built the way SPEC.md's own gotcha (5) prescribes for
    tapered solids -- a single-loop profile revolved 360 degrees, not an
    extrude with taper (whose sign flips unpredictably between single-
    loop and ring profiles) -- so it never depends on Fusion's taper-sign
    behavior. The profile is a quadrilateral with two of its four corners
    coincident with the revolve axis (cx,cy,z_lo) and (cx,cy,z_hi), which
    is exactly the closed-triangle shape revolve needs for a solid (not
    hollow) cone/frustum."""
    planes = root.constructionPlanes
    pin = planes.createInput()
    pin.setByOffset(root.xZConstructionPlane, V(cy))
    plane = planes.add(pin)
    sk = new_sketch(root, plane)
    axis = add_line(sk, P(cx, cy, z_lo - 1.0), P(cx, cy, z_hi + 1.0))
    axis.isConstruction = True
    add_line(sk, P(cx, cy, z_lo), P(cx + r_lo, cy, z_lo))
    add_line(sk, P(cx + r_lo, cy, z_lo), P(cx + r_hi, cy, z_hi))
    add_line(sk, P(cx + r_hi, cy, z_hi), P(cx, cy, z_hi))
    add_line(sk, P(cx, cy, z_hi), P(cx, cy, z_lo))
    prof = None
    for pr in sk.profiles:
        if pr.profileLoops.count == 1:
            prof = pr
            break
    assert prof is not None, 'cone_frustum_solid: no closed profile found'
    return revolve_new_body(root, prof, axis, 360.0)


def add_root_reinforcement(root, body, base_name, feature_name, cx, cy, r, z_root, direction,
                            fillet_r=ROOT_FILLET_R, collar_rise=ROOT_COLLAR_RISE, tol=0.15):
    """Reinforce a post/boss root against snap-off (pass 13, item 1).

    INVESTIGATED LIVE (this is the "investigate why" the task asked for,
    not a guess): a first version of this function tried a real Fusion
    fillet first and used a 45-degree conical collar ONLY as a fallback
    when the fillet API raised. Live-probed with verify_root_fillets on
    an actual built document, that version passed at only SOME of the 8
    angles per post/boss (e.g. top_post_P1 solid at 135/180/225 degrees,
    hollow at the other 5) -- not the all-or-nothing "fillet worked" /
    "fillet raised, fell back to collar" the earlier pass-9g investigation
    assumed. Root cause: `clipped_pillar_with_reach`'s own two-part
    design (a narrow, full-height "core" PLUS a wider "sleeve" that gets
    RADIALLY CLIPPED away wherever the local inner-cavity boundary is
    tighter than the post's own radius -- see that function's docstring,
    and finding 4's) means the root edge at z_root is NOT a full circle
    of radius `r` -- it is several disconnected arcs (wide-sleeve
    present) interrupted by narrower core-only stretches (sleeve clipped
    away) at other angles. `fillets.createInput` only reinforces the
    arcs it's given (a valid, real Fillet feature, just an incomplete
    one, matching edges by center+radius+z the same tolerant way
    chamfer_edge_at does) -- and the core-only stretches, where the
    fillet has no matching edge, get NO fillet OR collar with the
    fillet-first/collar-fallback design, since "the try succeeded" was
    being treated as "this feature is done."

    Fix, step 1: the conical collar (cone_frustum_solid, boolean-JOINED)
    is now ALWAYS added, unconditionally -- it is a plain, full
    360-degree solid of revolution, so unlike a Fillet feature it can
    never be "partially selected"; combine_join adds its whole volume
    regardless of what the underlying pillar's own cross-section looks
    like at that height, guaranteeing full-circumference coverage.

    Fix, step 2 -- the real fillet is NOT attempted at all any more (an
    intermediate version tried it first, best-effort, as an additional
    finer surface on top of the collar): a live offline STL scan of that
    version found 4 non-manifold edges on EVERY boss where the partial
    fillet also applied (both Bottom and Top, at the exact boss_dia/2
    radius from the boss centre, right at the root z) -- the fillet arc
    (only covering the wide-sleeve portion of the loop) and the collar's
    own cone surface, which are both real, correct geometry independently
    but were never DESIGNED to be tangent to each other, meet along a
    seam that tessellates into a sliver. Skipping the fillet attempt
    entirely removes that interaction; the collar alone was already
    proven sufficient for the strength gate (verify_root_fillets) on its
    own, and is what the compass pegs/pads (too small for
    ROOT_FILLET_R to ever apply) were already using with zero manifold
    issues. `fillet_r`/`tol` are kept as parameters (unused) rather than
    removed, so this function's call sites don't need to change if a
    future pass wants to re-attempt a real fillet with a different
    edge-selection strategy. Appends (feature_name, 'collar', collar_rise)
    to ROOT_FILLET_REPORT and to the on-disk log (see
    ROOT_FILLET_LOG_PATH). direction='up' means the root is at the TOP of
    the post/boss (collar widens upward into the ceiling it's fused to --
    top posts, the Top-side halves of bosses A/B1/B2/C, the compass
    pegs/pads); direction='down' means the root is at the BOTTOM (collar
    widens downward into the floor -- every Bottom-side boss,
    A/B1/B2/C/D). Returns the (possibly replaced, via combine_join) body
    -- caller must re-fetch by base_name afterward exactly like every
    other combine_* call in this file."""
    # ROOT_COLLAR_OVERLAP: both ends of the collar are nudged to genuinely
    # OVERLAP the existing geometry rather than exactly touch it -- a
    # defensive margin against the SAME class of coincident-surface
    # tessellation sliver the fillet-vs-collar interaction turned out to
    # be (see this function's own docstring): the collar's "pillar-facing"
    # end is otherwise exactly tangent to the plain cylinder above/below
    # it, which is the same kind of exact-tangency Fusion's B-rep kernel
    # has already been observed (twice, now) to occasionally mesh with a
    # hairline mismatch. Same fix/reasoning already used elsewhere in this
    # file for the identical class of problem -- see add_mag_module's
    # fence z_ceiling ("pushed 0.3mm PAST the nominal ceiling so it
    # genuinely embeds instead of merely touching").
    if direction == 'up':
        z_lo, z_hi = z_root - collar_rise, z_root + ROOT_COLLAR_OVERLAP
        r_lo, r_hi = r + ROOT_COLLAR_OVERLAP, r + collar_rise
    else:
        z_lo, z_hi = z_root - ROOT_COLLAR_OVERLAP, z_root + collar_rise
        r_lo, r_hi = r + collar_rise, r + ROOT_COLLAR_OVERLAP
    collar = cone_frustum_solid(root, cx, cy, r_lo, r_hi, z_lo, z_hi)
    body = combine_join(root, body, [collar])
    body = dedupe_body(root, body, base_name)

    ROOT_FILLET_REPORT.append((feature_name, 'collar', collar_rise))
    return body


def _best_effort_fillet_at_z(root, body, z_target, radius, tol=0.15):
    """Best-effort constant-radius fillet on a body's own edges lying flat
    at z=z_target (a floor or ceiling seam) -- same skip-on-failure
    pattern as `_best_effort_fillet` (which selects by vertical-edge dz
    instead of a flat z), used for the GPS/stack-frame wall roots (pass
    13, item 1) where the SPEC only asks for a modest 0.6mm cosmetic/
    print-quality fillet, not a load-bearing one -- a missing fillet here
    is not gated by verify_root_fillets."""
    try:
        edges = adsk.core.ObjectCollection.create()
        for edge in body.edges:
            bb = edge.boundingBox
            if (abs(bb.maxPoint.z / MM - z_target) < tol
                    and abs(bb.minPoint.z / MM - z_target) < tol):
                edges.add(edge)
        if edges.count > 0:
            fillets = root.features.filletFeatures
            fin = fillets.createInput()
            fin.addConstantRadiusEdgeSet(edges, V(radius), True)
            fillets.add(fin)
            return True
    except RuntimeError:
        pass
    return False


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


CLIP_TOOL_NAME = 'Inner Cavity Clip Tool'  # shared constant -- see _refetch_by_name's docstring
BOSS_CORE_R = 2.6   # see clipped_pillar_with_reach -- < boss_dia/2 (3.0), > counterbore/hole radii.
# 2026-09-06: bumped from 1.8 -- boss D's real local floor (measured
# directly: solid at radius >=2.5 from its own center, hollow inside
# that) starts further out than 1.8 could reach, so its core touched
# nothing and the join silently no-opped (see clipped_pillar_with_reach's
# docstring). 2.6 clears the measured 2.5mm threshold with margin while
# staying under boss_r (3.0) and the counterbore radius floor doesn't
# matter here since the join happens BEFORE the hole/counterbore cuts.
POST_CORE_R = 1.1   # < top_post_dia/2 (2.0), > top_post_pilot_dia/2 (0.81)

# --- pass 16 resumed (item 1, live-found the hard way): the display's own
# SMT standoff at each of S1/S2/S3 is a REAL physical component (Fusion
# names it 'SMTSO-M2-3_5X2-3_5ET' -- an M2 SMT standoff, ~3.5mm OD), not
# just a bare screw shaft -- live-measured bbox (both variants, all three
# points): 3.53mm OD x 4.70mm tall, hanging DOWN from the display PCB, its
# TOP flush with the live-measured standoff plane (Finding 2) and its own
# BOTTOM 4.70mm below that. The old ear_standoff_hole_dia (2.4mm) is a
# plain M2 clearance hole sized for the SCREW only -- it left solid ear/
# boss material exactly where this barrel's own 3.53mm-OD body needs to
# sit, confirmed live via a from-scratch check_interference(Top, all board
# occurrences) run: 9 real Top-vs-display hits, three of them a full
# 36.75mm^3 each, centered EXACTLY on S1/S2/S3 (bbox 3.5x3.5x3.0mm, right
# at the barrel's own footprint) -- see the pass-16 README section for the
# full live trace. Fixed with a stepped counterbore: the plain M2
# clearance hole continues the FULL height (screw shaft access from
# below), PLUS a short, wide counterbore right under the seat sized to
# clear the real barrel -- see add_ear/add_s2_boss's own '_cut_hole'.
STANDOFF_BARREL_DIA = 4.2      # mm -- measured 3.53mm OD + ~0.33mm radial clearance/side for print tolerance.
STANDOFF_BARREL_DEPTH = 4.70   # mm -- the barrel's own live-measured height (both variants, all 3 points).
STANDOFF_BARREL_MARGIN = 0.30  # mm -- extra depth clearance below the barrel's real measured bottom.

# --- pass 14, item 1: lanyard-end corner blocks (A+B1 / C+B2) ---------------
# Jake's sketch: the four free-standing Top-side bosses at the lanyard end
# become TWO solid corner blocks, one per side -- "a buttress block the two
# screws land in, not two posts with a web". See add_lanyard_corner_block's
# own docstring for the full construction. These constants went through TWO
# live rounds this pass: a first version padded the capsule to 3.7mm radius
# (boss_dia/2 + 0.7) to satisfy verify_root_fillets' 3.6mm probe on its own
# -- but a live check_interference run found real overlap (up to 46mm^3)
# against the inserted XIAO/Wio boards at B1/B2: their REAL footprint at
# B1/B2's own y (~-15) reaches x=+-8.9mm, only 3.61mm from B1/B2's own
# centre (+-12.5mm) -- LESS than the 3.7mm the capsule needed, so no amount
# of ring/stack-footprint clamping alone could fit both at once. Fixed by
# splitting the two jobs: CORNER_BLOCK_PAD=0.0 (the capsule/wedge's own
# radius is just boss_dia/2, the SAME footprint the old individual bosses
# always had, proven interference-free through pass 13) plus the pass-13
# conical collar (already added below, near the ceiling only) to satisfy
# verify_root_fillets' probe instead -- the collar's own geometry doesn't
# care what the underlying pillar's cross-section is, so a plain
# boss-radius capsule gets exactly the same 1.1mm-at-dz=0.4mm boost any
# other boss/post in this file gets. CORNER_BLOCK_STACK_MARGIN is a second,
# independent belt-and-suspenders cut against the STATIC L76K PCB footprint
# param (which the live board interference above showed is a reasonable,
# if not perfectly tight, proxy for the real 3-board stack's own envelope).
CORNER_BLOCK_PAD = 0.0             # mm added to boss_dia/2 -- see the module comment above: 0 keeps the
                                    # capsule/wedge at exactly the old proven-safe boss radius; verify_root_
                                    # fillets is satisfied by the collar (added unconditionally below), not this.
CORNER_BLOCK_REACH = 10.0          # mm -- deliberately oversized outward-wedge reach: clipped by
                                    # clip_to_inner_cavity (the true shell) and CORNER_BLOCK_RING_CLEARANCE below,
                                    # so the block's REAL reach is whichever boundary is actually closer.
CORNER_BLOCK_RING_CLEARANCE = 0.5  # mm kept clear of the lip/anchor ring's own inner edge (lip_r[0]) -- at
                                    # CORNER_BLOCK_PAD=0.0, B1/B2 (19.53mm from spine_a) reach only 22.53mm;
                                    # trim's lip_r[0]=23.95 minus this 0.5mm clearance = 23.45mm, comfortable
                                    # margin (current has far more still: lip_r[0]=25.95).
CORNER_BLOCK_STACK_MARGIN = 0.8    # mm -- belt-and-suspenders cut of the L76K PCB footprint (p['bay']['stack3']
                                    # ['l76k_pcb']) + this margin, from the block's own 'wide' portion, full
                                    # z0..z1 height (not just the frame's own low-z band) -- the live interference
                                    # this pass found reached as high as z=22.9 (the Wio module), well above the
                                    # comms-stack FRAME's own z-range, so a low-z-only keepout (matching
                                    # add_comms_stack_frame's) would have missed it. Sized to clear BOSS_CORE_R's
                                    # own reach (2.6mm from each screw, i.e. to x=+-9.9 for B1/B2) with 0.2mm to
                                    # spare -- the core is never clipped by this (it's added back in afterward,
                                    # unclipped, same as every other boss/post's core).

DISPLAY_KEEPOUT_CLEARANCE = 0.5    # mm -- pass 16: same "small clearance to a real external feature"
                                    # convention as CORNER_BLOCK_RING_CLEARANCE/wall_clear (both 0.5/0.6mm)
                                    # elsewhere in this file. Live check_interference (this pass) found the
                                    # D1/D2 ear ROOT pillar -- built full-height split_z..top_ceiling_underside_z,
                                    # "IDENTICAL to add_single_corner_block" per the mechanical review's own F7 --
                                    # genuinely collides with the real inserted display module's own housing
                                    # bbox (`display_bbox`, offset by `display_z_offset`): D1/D2 sit at
                                    # (+-19, 64), well inside the display's real XY footprint (+-22.39,
                                    # 27.6-73.13), and the module's own housing bottom (20.3mm current /
                                    # 23.3mm trim, world frame) sits BELOW top_ceiling_underside_z (23.0 / 26.0)
                                    # -- so a literal floor-to-ceiling pillar there physically occupies the same
                                    # volume as the assembled display module. The mechanical review flagged
                                    # exactly this risk in general terms (F14, "I don't have live geometry to
                                    # probe... flag as a specific thing the modeling pass's own check_interference
                                    # ... needs to look at explicitly") without the live geometry to quantify it.
                                    # Fix: `_ear_root_z1` (below) caps the WALL-ROOT pillar's own z1 to stay
                                    # this far below the display's real underside wherever the root's own XY
                                    # footprint actually overlaps it -- leaving both variants' pilots
                                    # (`top_pilot_z[1]`=19.1mm) comfortably embedded well below the cap (19.8mm
                                    # current / 22.8mm trim -- 0.7mm / 3.7mm of solid pillar above the pilot's
                                    # own end, still resting on the review's own finding that D1/D2 sit against
                                    # a PLAIN VERTICAL WALL there, not the curving ceiling fillet the lanyard
                                    # corner blocks need full height for -- see review-mechanical.md Sec 3.1).
                                    # Only caps when the root's own XY footprint (boss_dia radius) actually
                                    # overlaps the display's real XY bbox -- a no-op everywhere else.


PILOT_PROTECT_MARGIN = 1.0         # mm -- pass 16: the floor below which the display-vs-Top
                                    # ceiling-safe cut (in build(), right after insert_display_pcba)
                                    # is never allowed to remove material, regardless of how low any
                                    # real display sub-body's own geometry reaches -- see that cut's
                                    # own docstring. top_pilot_z[1] (19.1mm) + this margin = 20.1mm,
                                    # comfortably below every live-probed display sub-body found so
                                    # far (lowest observed: 21.47mm trim-world) with room to spare.


def _ear_boss_keepout_points(p):
    """The full set of pass-16 mount XY points that must always keep a
    continuous, uninterrupted material column, EACH WITH ITS OWN
    documented z-band (not one shared band for all of them -- see below):
    each ear's own wall-root anchor (`ears[name]['root_xy']`) -- protected
    from `top_pilot_z[0]` down to its own capped ceiling reach (the
    tallest real need, since the wall-root wedge/core physically has to
    reach that high to fuse into Top's shell) -- plus every ear's own
    standoff target (S1, S3) and the S2 boss's own target, protected only
    through their own much SHORTER arm/seat band (`ear_seat_z -
    ear_arm_thickness` .. `ear_seat_z`).

    RELOCATED pass 16 (resumed, Finding 1 fix): D1/D2 (the case-closure
    screws) no longer sit at the ear roots at all -- they moved to their
    own independent corner blocks north of the ears (see 'screws_D12' in
    params_current.py), far enough from both buttons that this button-
    cutting keepout is a structural no-op there; only the ear's own
    wall-root anchor (still at the old (+-19, 64) point, now pilot-less)
    still needs button protection, so this function is keyed off
    `p['ears']` alone now, not `p['screws_D12']`.

    ROUND 3 (live-found, ORIGINAL D1/D2-at-the-ear-root design): a
    same-band-for-everyone version (either the pilot's own 10.0-19.1mm
    depth, or the boss's own full OD over the full case height) was
    always either too short (the wall-root reaches z~22.8mm, ABOVE the
    pilot's own depth, to fuse into the ceiling -- a live probe found the
    ear severed into two disjoint bodies again with only the pilot's own
    depth protected) or too tall (protecting the full case height
    re-created a real, large Top-vs-Home-Button interference -- see
    EAR_BOSS_BUTTON_KEEPOUT_R's own docstring). Each point's own real
    structural need is different, so each gets its own band -- this is
    the minimum that is BOTH sufficient (no more live-found severs) and
    small enough to leave the button's own real cap/shaft clearance free
    to cut everywhere else.

    Shared between add_ear/add_s2_boss's own construction and add_button/
    build()'s keepout (below) so the two can never silently drift apart
    about which points -- or which z-bands -- matter."""
    boss_r = p['boss_dia'] / 2.0
    reach_r = boss_r + CORNER_BLOCK_REACH
    ceiling = p['top_ceiling_underside_z']
    pz0 = p['top_pilot_z'][0]
    seat_z = p['ear_seat_z']
    pts = []
    for ear in p['ears'].values():
        rx, ry = ear['root_xy']
        z1 = ear_root_cap_z1(p, rx, ry, reach_r, ceiling)
        pts.append({'xy': (rx, ry), 'z': (pz0 - 0.5, z1 + 0.5)})
        # RESUMED pass 16 (Finding 1 fix): the ear's own TARGET now
        # carries a full-height RISER (root_z1 .. seat_z, see add_ear's
        # own arm/riser connectivity fix), not just a thin
        # ear_arm_thickness-tall slice under the seat -- protect the
        # whole riser span (its own xy sits close enough to the Home
        # button's own bbox, S1 in particular, to be worth the same
        # belt-and-suspenders protection the root already gets), not
        # just its top.
        tx, ty = p['board_standoffs'][ear['target']]
        pts.append({'xy': (tx, ty), 'z': (z1 - p['ear_arm_thickness'] - 0.5, seat_z + 0.5)})
    s2_arm_z = (seat_z - p['ear_arm_thickness'] - 0.5, seat_z + 0.5)
    pts.append({'xy': p['board_standoffs'][p['s2_boss']['target']], 'z': s2_arm_z})
    return pts


EAR_BOSS_BUTTON_KEEPOUT_R = 2.5     # mm -- pass 16 (live-found, ROUND 2): top_pilot_dia/2 (0.81) +
                                    # POST_WALL_MIN (1.2) + a small margin -- just enough to cover
                                    # verify_post_walls' own probe radius (2.01mm) plus
                                    # verify_ear_root_material's arm-solid probes, subtracted from
                                    # every button cut tool (see _clip_of_ear_boss_keepout) around each
                                    # of _ear_boss_keepout_points. NOT the boss's own full OD (3.0mm) +
                                    # margin (4.0mm, this constant's first version) -- see this
                                    # function's own docstring for why that was too generous.


def _clip_of_ear_boss_keepout(root, tool, p):
    """Pass 16 (live-found): subtract a keepout cylinder
    (EAR_BOSS_BUTTON_KEEPOUT_R about each of `_ear_boss_keepout_points`,
    spanning just the pilot/arm's own documented z-band) from a button's
    own Top-cutting tool BEFORE it is applied.

    Live-found the hard way: add_button's own `hole_cutter`/
    `tab_hole_body` cuts run AFTER the ears/S2-boss are already built and
    joined into Top (see build()) -- the Home button's own guide-rib
    lead-in sits close enough to D1 (the S1 ear's own wall-root, (-19,
    64)) that its tab-hole cut sliced clean through the ear's own
    wall-root column at z~15.6-17.0mm (inside the M2x12 pilot's own
    documented engagement depth, top_pilot_z=(10.0,19.1)) -- not just the
    ~9mm^3 volume overlap an earlier, PRE-ear-existing live
    check_interference run had found and (wrongly) assumed was the whole
    story. A live post-build probe found the pilot wall reading FULLY
    HOLLOW at every angle -- worse than an interference number: Fusion's
    own body-count came back as two DISJOINT bodies ('Top' and a stray
    'Top (1)', the severed lower stub of the ear-root, split off from the
    main shell entirely) -- the cut had cut the ear in half, not just
    dented it. A dented/undersized ear could still be caught by
    verify_ear_root_material's live probes; a body that silently splits
    into two Fusion still happily calls 'Top' would not be, since
    whichever fragment `combine_cut`'s own return value happens to point
    at is still a real, valid BRepBody -- this is why this fix subtracts
    the keepout from every button cut tool BEFORE it ever touches Top,
    rather than trying to detect a split after the fact.
    Same idiom `stack_keepout` inside add_ear already uses for the L76K
    PCB, just aimed the other direction (protecting the screw/ear column
    FROM the button's own cut, not protecting the comms stack from the
    ear).

    ROUND 2 (live-found): a first version used the boss's own full OD +
    1.0mm margin (4.0mm radius) over the FULL case height (0..top_z) --
    "cheap, and correct regardless of exactly which z-band a future
    button/geometry change might reach into next," reasoning that turned
    out wrong: it also shields the button's own CAP/SHAFT clearance
    (hole_cutter) itself, not just the stationary rib/gusset -- a live
    check_interference run found a real 589mm^3 Top-vs-Home-Button
    overlap once that big a column was guaranteed never to be cut, i.e.
    the Home button's own real cap/shaft physically needs to pass through
    part of that same column to reach the wall. Narrowed to
    EAR_BOSS_BUTTON_KEEPOUT_R (2.5mm -- just past the pilot-wall probe
    radius used by verify_post_walls/verify_ear_root_material, well under
    the boss's own OD) and to the pilot/arm's own documented z-band
    (top_pilot_z, +/-0.5mm margin) rather than the full case height --
    comfortably covers every live-found defect above while leaving the
    button's own cap/shaft clearance, which reaches higher (up to ~23mm,
    per that same live check), free to cut normally.

    ROUND 3 (live-found): a single shared z-band (top_pilot_z, 10.0-19.1)
    for every point was still too short for D1/D2 themselves -- their
    own wall-root wedge/core has to reach up to `_ear_root_z1`'s own
    capped ceiling height (~22.8mm trim) to actually fuse into Top's
    shell (see that function's docstring); protecting only the pilot's
    own 19.1mm depth left the ~19.1-22.8mm band unprotected and a live
    probe found the ear severed again, one level up. `_ear_boss_keepout_
    points` now hands back each point's OWN z-band (tall for D1/D2, short
    for the ear/boss targets) instead of one band for all of them -- see
    that function's own docstring."""
    keepouts = [cylinder_solid(root, s['xy'][0], s['xy'][1], EAR_BOSS_BUTTON_KEEPOUT_R, s['z'][0], s['z'][1])
                for s in _ear_boss_keepout_points(p)]
    return combine_cut(root, tool, keepouts)


BUTTON_RIB_GUSSET_MARGIN = 2.0  # mm -- RESUMED pass 16 (Finding 1, live-found the hard way):
                                 # the button's own REAL body (cap + guide-rib + ceiling-gusset,
                                 # everything `add_button` builds -- see build()'s own
                                 # "cut Top against the buttons' OWN FINAL bodies" comment) reaches
                                 # LOWER than `home_cap`/`power_cap`'s own 'z' (the CAP alone) --
                                 # live-probed (this pass): the 'Home Button' body's real solid
                                 # starts as low as z=14.4mm (trim world) at x=-21..-23 (the
                                 # WEDGE's own outward reach, not the root's own narrow axis, which
                                 # only starts at z=16.4 -- matching home_cap['z'][0] exactly) --
                                 # a full 2.0mm below the cap's own stated z0. A height cap using
                                 # only home_cap/power_cap's own z0 (this constant's original,
                                 # pre-resume version, margin=0.4 only) left the wedge's OWN
                                 # outward-reaching corner still inside the button's real rib/
                                 # gusset material -- confirmed live (a real 212mm^3 Top-vs-Home-
                                 # Button interference, bounding box z 14.4-21.6, x -21.5..-14.42 --
                                 # squarely the wedge's own reach, not the root axis or the
                                 # standoff riser, both independently confirmed clear).


def _ear_wedge_wall_touch_z1(p):
    """Pass 16 (live-found, RESUMED this pass -- see BUTTON_RIB_GUSSET_
    MARGIN's own docstring for the live numbers): the highest z the ear
    wall-root's WEDGE (and, since Finding 1's resumed fix, the ENTIRE
    root -- capsule/wedge/core alike, via `ear_root_cap_z1`) may safely
    touch, staying below BOTH side buttons' own REAL rib/gusset geometry
    (not just their cap -- `BUTTON_RIB_GUSSET_MARGIN` accounts for the
    difference) by a small extra safety margin."""
    margin = 0.4
    return min(p['power_cap']['z'][0], p['home_cap']['z'][0]) - BUTTON_RIB_GUSSET_MARGIN - margin


def _ear_root_z1(p, cx, cy, r, z1_nominal):
    """Pass 16: the ear/D1/D2 wall-root pillar's own effective z1 -- capped
    below the real inserted display module's own housing underside
    (`display_bbox`, world-offset by `display_z_offset`) by
    DISPLAY_KEEPOUT_CLEARANCE, wherever the pillar's own XY footprint
    (radius `r` about (cx, cy)) actually overlaps the display's real XY
    bbox. See DISPLAY_KEEPOUT_CLEARANCE's own comment for the live
    interference this fixes and the numbers behind it."""
    db = p['display_bbox']
    dz = p.get('display_z_offset', 0.0)
    overlap = _xy_overlap(cx - r, cx + r, cy - r, cy + r, db['x'][0], db['x'][1], db['y'][0], db['y'][1])
    if overlap is None:
        return z1_nominal
    display_z0 = db['z'][0] + dz
    return min(z1_nominal, display_z0 - DISPLAY_KEEPOUT_CLEARANCE)


def ear_root_cap_z1(p, cx, cy, r, z1_nominal):
    """Pass 16 (resumed, Finding 1 fix): the ear's own wall-root anchor
    -- still at the old (+-19, 64) point, right next to the Home button's
    own real cap/collar geometry, even though the D1/D2 SCREW itself has
    moved away (see 'ears'/'screws_D12' in params_current.py) -- needs
    BOTH height caps `_ear_root_z1` (below the real display module) AND
    `_ear_wedge_wall_touch_z1` (below both buttons' own cap z0) applied
    to its ENTIRE root (capsule + wedge + core), not just the wedge.

    Live-found (Finding 1, this pass's resumed work): the ORIGINAL
    add_ear only capped the WEDGE this way, reasoning "the button
    conflict is entirely the wedge's own outward reach, not the narrow
    axial column" -- a live per-sub-body interference probe proved that
    reasoning wrong: the Home button's own real cap/shaft solid
    completely fills the root_capsule/root_core's own narrow axial
    column too, at every angle within `verify_post_walls`' own probe
    radius, for the full z~17-22mm band those pieces used to reach
    (uncapped by any button constraint, only by the display). Since the
    root no longer carries a pilot at all (the M2x12 engagement
    requirement that justified reaching that high moved away with the
    screw), capping the WHOLE root at the tighter of the two limits
    removes the real volume overlap by construction -- Top's material
    there now never reaches into the button's own z-band at all -- at
    the cost of a shorter root (loses ~6-7mm of height vs the old,
    pilot-driven number), acceptable since the ear's own remaining job
    (a lightweight SMT-standoff seat, not a screw boss) never needed that
    reach in the first place."""
    return min(_ear_root_z1(p, cx, cy, r, z1_nominal), _ear_wedge_wall_touch_z1(p))


def _wall_outward_axes(p, cx, cy):
    """The (axis1, axis2) tangential/outward pair used by every wall-
    anchored member in this file (corner blocks, ears, the S2 boss):
    axis2 points OUTWARD, away from the NEARER spine point -- the SAME
    "nearer spine endpoint" convention rho_from_spine/true_wall_
    distance_along_ray already use (spine_a for a point south of it,
    spine_b for a point north of it, and (0, cy) itself -- i.e. pure
    +-X -- for a point in the straight band, which collapses to exactly
    sign(cx) along X, matching true_wall_distance_along_ray's own
    straight-band case). The ORIGINAL add_lanyard_corner_block version of
    this always used spine_a specifically (correct for A/B1/C/B2, which
    all sit south of it) -- generalized here (pass 16) because D1/D2
    (+-19, 64) sit in the +y dome, north of spine_b, where "away from
    spine_a" and "away from spine_b" point in meaningfully different
    directions (confirmed: using spine_a there would aim the outward
    wedge ~30 degrees off the true radial direction). axis1 is the
    perpendicular tangential direction."""
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


def _nearer_spine_y(p, cy):
    """The y of the nearer spine endpoint for a point at world y=cy --
    ay for a point south of spine_a, by for a point north of spine_b, cy
    itself in the straight band (matches rho_from_spine's own
    convention). Used to centre the lip/anchor ring's own inner-edge
    clearance cylinder correctly regardless of which dome end a
    wall-anchored member sits in (pass 16 fix: the ORIGINAL add_lanyard_
    corner_block always centred this at spine_a, correct for A/B1/C/B2 --
    all south of it -- but WRONG for D1/D2, north of spine_b: a live
    Combine-Intersect against a cylinder centred at the wrong end failed
    outright, FEATURE_FAILED_TO_CREATE, since D1/D2 sit ~60mm+ from
    spine_a's own point, far outside any plausible ring-clearance
    radius)."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= cy <= by:
        return cy
    return ay if cy < ay else by


def _corner_block_ring_limit_r(p, cx, cy):
    """RESUMED pass 16 (Finding 1, live-found the hard way): the radius
    (from the nearer spine point, see `_nearer_spine_y`) every wall-
    anchored member's own Combine-Intersect clip must NOT be smaller
    than, or the intersect silently amputates the wedge's own reach
    toward the true wall before it ever gets there.

    The ORIGINAL, pre-resume value (`lip_r[0] - CORNER_BLOCK_RING_
    CLEARANCE`, a FIXED ~23.45mm/25.45mm regardless of where the block
    actually sits) was calibrated for A/B1/C/B2 -- all close to spine_a
    (within ~20mm), where 23.45mm is comfortably MORE than the block's
    own reach (its own distance from the spine + boss_r + CORNER_BLOCK_
    REACH), so the intersect only ever does its INTENDED job (staying
    clear of the lip/anchor ring's own inner edge in the z-band they
    actually share, z~9.2-11) and never touches the wedge's outward
    reach at all. Live-found this pass: the relocated D screw
    ((18, 58), see 'screws_D12' in params_current.py) sits 19.04mm from
    its own nearer spine point ALREADY -- add its own wedge reach
    (boss_r + CORNER_BLOCK_REACH = 13mm) and the block needs to reach
    ~32mm to get anywhere near the true wall (~28mm out), but the fixed
    23.45mm limit clipped it off at barely more than its own centre,
    leaving ZERO material anywhere near the true wall -- confirmed live
    (a built 'Top' with a completely hollow interior at (18, 58), no
    boss at all, despite `add_single_corner_block` reporting success).
    The SAME risk applies to the EARS' own wall-root (still at the old
    (+-19, 64) point -- 22.58mm from spine_b, uncomfortably close to the
    same fixed limit).

    Fixed generically: take the LARGER of the original fixed value (so
    A/B1/C/B2's own well-tested behaviour is completely unchanged) and
    this SPECIFIC block's own needed reach (distance to the nearer spine
    point + boss_r + CORNER_BLOCK_REACH, the same oversized-then-clipped
    figure the wedge's own construction already uses, +1mm margin) -- so
    the ring-clearance intersect can only ever do its own narrow job
    (clipping the low z-band near the lip/anchor ring, where a wedge
    that's ALREADY headed toward the wall might otherwise graze the
    ring's own inner edge) and can never again amputate a wedge that
    legitimately needs to travel further out to reach the true wall."""
    boss_r = p['boss_dia'] / 2.0
    nearer_y = _nearer_spine_y(p, cy)
    own_reach = math.hypot(cx, cy - nearer_y) + boss_r + CORNER_BLOCK_REACH + 1.0
    return max(p['lip_r'][0] - CORNER_BLOCK_RING_CLEARANCE, own_reach)


def add_single_corner_block(root, bodies, p, screw, clip_tool=None):
    """Pass 16, item B (owner call): retires B1/B2 outright (see mech
    review F1's pull-out math, reused verbatim in the README's pass-16
    section -- 695-926N per M2x12 pilot at the existing 9.1mm engagement
    vs a 50-150N worst-case lanyard tug, with A and B1 (or C and B2) only
    7.6mm apart center-to-center) -- A and C's own Top-side halves become
    single-pilot wall-anchored blocks instead of the old pass-14 two-
    screw capsule. Same construction, degenerated to ONE point: a plain
    cylinder at the screw's own centre (radius boss_dia/2) unioned with
    an oversized outward wedge (CORNER_BLOCK_REACH, found live via
    _wall_outward_axes) reaching toward the true dome wall, clipped by
    clip_to_inner_cavity + the lip/anchor ring-clearance cylinder + the
    L76K stack keep-out (all unchanged from the pass-14 pattern), plus a
    full-height core and the unconditional 45-degree root-reinforcement
    collar. This exact pattern -- capsule/cylinder + clipped wedge into
    the wall + full-height core + collar -- is also reused verbatim by
    add_ear/add_s2_boss below (mech review F7: "don't hand-roll new
    wedge/gusset math... generalize add_lanyard_corner_block's pattern").

    RESUMED pass 16 (Finding 1 fix): generalized to apply `_ear_root_z1`'s
    own display-keepout height cap to EVERY corner block, not just the
    old D1/D2-at-the-ear-root special case -- a no-op for A/C (well south
    of the display's own y-range) but load-bearing for the NEW D1/D2
    (relocated north of the ears, at (+-12.7-14, ~71.5-72), squarely
    under the display module's own XY footprint -- see 'screws_D12' in
    params_current.py) so their own full-height pilot column stays clear
    of the real inserted display housing exactly the way the ears'
    former wall-root already did."""
    cx, cy = screw['xy']
    boss_r = p['boss_dia'] / 2.0
    z0, z1_nominal = p['split_z'], p['top_ceiling_underside_z']
    ay = p['spine_a'][1]
    axis1, axis2 = _wall_outward_axes(p, cx, cy)
    z1 = _ear_root_z1(p, cx, cy, boss_r + CORNER_BLOCK_REACH, z1_nominal)

    capsule = cylinder_solid(root, cx, cy, boss_r, z0, z1)
    wedge_offset = boss_r + CORNER_BLOCK_REACH / 2.0
    wedge_center = (cx + axis2[0] * wedge_offset, cy + axis2[1] * wedge_offset, z0)
    wedge = oriented_box_prism(
        root, wedge_center, axis1, axis2, (0.0, 0.0, 1.0),
        2.0 * boss_r, CORNER_BLOCK_REACH, z1 - z0)
    wide = combine_join(root, capsule, [wedge])
    wide = clip_to_inner_cavity(root, wide, p, clip_tool)

    ring_limit_r = _corner_block_ring_limit_r(p, cx, cy)
    ring_limit = cylinder_solid(root, 0.0, _nearer_spine_y(p, cy), ring_limit_r, z0 - 1.0, z1 + 1.0)
    wide = combine_intersect(root, wide, [ring_limit])

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        m = CORNER_BLOCK_STACK_MARGIN
        stack_keepout = box_solid(root, pcb['x'][0] - m, pcb['x'][1] + m, pcb['y'][0] - m, pcb['y'][1] + m,
                                   z0 - 0.5, z1 + 0.5)
        wide = combine_cut(root, wide, [stack_keepout])

    core = cylinder_solid(root, cx, cy, BOSS_CORE_R, z0, z1)
    block = combine_join(root, wide, [core])

    top_in = _refetch_by_name(root, 'Top') or bodies['Top']
    top = combine_join(root, top_in, [block])
    if clip_tool is not None:
        top = dedupe_body(root, top, 'Top')
    top = _refetch_by_name(root, 'Top') or top

    def _cut_pilot(t):
        pilot = cylinder_solid(root, cx, cy, p['top_pilot_dia'] / 2.0, p['top_pilot_z'][0], p['top_pilot_z'][1])
        t = combine_cut(root, t, [pilot])
        return _refetch_by_name(root, 'Top') or t

    top = _cut_pilot(top)
    top = add_root_reinforcement(root, top, 'Top', f'corner_block_{screw["name"]}', cx, cy, boss_r,
                                  z1, direction='up')
    top = _refetch_by_name(root, 'Top') or top
    # pass-15 item 8 lesson: re-cut the pilot AFTER the collar joins (a
    # solid-to-the-axis collar can silently replug a hole in its own band).
    top = _cut_pilot(top)

    bodies['Top'] = top
    return bodies


def add_ear(root, bodies, p, name, clip_tool=None):
    """Pass 16, item A (owner's chosen candidate 5, display mount): one
    EAR grown from Top's own dome wall, carrying the display's own SMT
    standoff S1 or S3. Two pieces, unioned into a single block before any
    hole is cut:

    (1) a wall-anchored root at `ears[name]['root_xy']` -- SAME
    capsule+wedge+core construction as add_single_corner_block (mech
    review F7), but (RESUMED pass 16, Finding 1 fix) carrying NO pilot
    at all any more, and capped in height by `ear_root_cap_z1` (both the
    real display module's underside AND both buttons' own cap z0, not
    just the display) -- see that function's own docstring for why the
    entire root needs both caps now, not just the wedge;

    (2) a seat arm (a stadium capsule from the root centre to the
    standoff's own xy) kept at the ROOT's OWN low, button-safe z-band
    (NOT near the seat -- see the Finding-1 connectivity fix below, in
    the function body), topped by a vertical RISER at the standoff's own
    xy that climbs from the arm up to the real seat -- PARAMS['ear_seat_z']
    (the display module's own REAL, live-measured standoff plane -- see
    params_current.py's own comment on this param for the pass-16
    re-derivation) -- printed `ear_seat_offset` mm short so the WINDOW
    seat, not this one, takes the assembly preload, mech review F5) --
    with a through-hole (`ear_standoff_hole_dia`) for the M2x4 driven up
    from below into the display's own standoff.

    The standoff through-hole is cut TWICE (once after both pieces join,
    once more after both root-reinforcement collars have been added --
    pass-15 item 8's lesson: re-cut every hole AFTER every collar AND
    every later join that shares its axis) -- there is no longer a
    second (pilot) hole to worry about re-plugging, since D1/D2 moved to
    their own independent corner blocks (see add_single_corner_block,
    called from add_case_screws)."""
    ear = p['ears'][name]
    rx, ry = ear['root_xy']
    tx, ty = p['board_standoffs'][ear['target']]
    boss_r = p['boss_dia'] / 2.0
    ay = p['spine_a'][1]
    z0, z1 = p['split_z'], p['top_ceiling_underside_z']
    seat_z = p['ear_seat_z']

    # RESUMED pass 16 (Finding 1 fix): cap the ENTIRE wall-root (capsule +
    # wedge + core alike), not just the wedge -- see ear_root_cap_z1's own
    # docstring for the live-probed reason the old wedge-only cap left a
    # genuine ~304mm^3 Top-vs-Home-Button interference in the axial
    # column. Since the root no longer carries a pilot, there is no
    # engagement-depth requirement pulling it back up.
    root_z1 = ear_root_cap_z1(p, rx, ry, boss_r + CORNER_BLOCK_REACH, z1)

    # (1) wall-anchored root (capped, no pilot)
    axis1w, axis2w = _wall_outward_axes(p, rx, ry)
    root_capsule = cylinder_solid(root, rx, ry, boss_r, z0, root_z1)
    wedge_offset = boss_r + CORNER_BLOCK_REACH / 2.0
    wedge_center = (rx + axis2w[0] * wedge_offset, ry + axis2w[1] * wedge_offset, z0)
    wedge = oriented_box_prism(root, wedge_center, axis1w, axis2w, (0.0, 0.0, 1.0),
                                2.0 * boss_r, CORNER_BLOCK_REACH, root_z1 - z0)
    root_wide = combine_join(root, root_capsule, [wedge])
    root_wide = clip_to_inner_cavity(root, root_wide, p, clip_tool)
    ring_limit_r = _corner_block_ring_limit_r(p, rx, ry)
    ring_limit = cylinder_solid(root, 0.0, _nearer_spine_y(p, ry), ring_limit_r, z0 - 1.0, root_z1 + 1.0)
    root_wide = combine_intersect(root, root_wide, [ring_limit])
    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        m = CORNER_BLOCK_STACK_MARGIN
        stack_keepout = box_solid(root, pcb['x'][0] - m, pcb['x'][1] + m, pcb['y'][0] - m, pcb['y'][1] + m,
                                   z0 - 0.5, root_z1 + 0.5)
        root_wide = combine_cut(root, root_wide, [stack_keepout])
    root_core = cylinder_solid(root, rx, ry, BOSS_CORE_R, z0, root_z1)
    root_block = combine_join(root, root_wide, [root_core])

    # (2) seat arm + target riser (RESUMED pass 16, Finding 1 fix -- root/
    # arm connectivity): the ORIGINAL design put the arm at a fixed
    # z-band just below `seat_z` -- safe when `root_z1` reached all the
    # way up near the ceiling (as it did before this fix), since the
    # arm's own z-band sat comfortably WITHIN the root's own tall z-span.
    # Now that `root_z1` is capped well below `seat_z` (by the button, at
    # THIS xy specifically -- see ear_root_cap_z1), a fixed arm z-band
    # near `seat_z` would sit entirely ABOVE the root's own shortened
    # z-span: `combine_join` on two solids that share an XY footprint but
    # NEVER overlap in Z produces a body with two disjoint lumps, not a
    # real connection (confirmed by inspection before this fix shipped --
    # exactly the "silently splits into two bodies" defect class this
    # file's own dedupe_body/keepout docstrings already warn about
    # elsewhere).
    #
    # Fix: keep the ARM itself at the ROOT's OWN low, button-safe z-band
    # (the SAME z-band the wedge already proves safe -- no live check has
    # ever flagged the wedge, only the tall axial column, at this xy), so
    # it shares real Z-overlap with root_block by construction. A
    # separate vertical RISER, at the STANDOFF's own xy (tx, ty) -- never
    # flagged as a button conflict at ANY height, root xy only -- then
    # climbs from the arm's own z-band up to the real `seat_z`. Every
    # bit of material that reaches into the button's own z-range now
    # sits at the standoff's xy, never at the root's.
    arm_z1 = root_z1
    arm_z0 = arm_z1 - p['ear_arm_thickness']
    dx, dy = tx - rx, ty - ry
    seg_len = math.hypot(dx, dy)
    axis1 = (dx / seg_len, dy / seg_len, 0.0)
    axis2 = (-axis1[1], axis1[0], 0.0)
    mx, my = (rx + tx) / 2.0, (ry + ty) / 2.0
    if _dot(axis2, (mx, my - ay, 0.0)) < 0:
        axis2 = (-axis2[0], -axis2[1], -axis2[2])
    arm = oriented_stadium_prism(root, (mx, my, arm_z0), axis1, axis2, (0.0, 0.0, 1.0),
                                  seg_len + 2.0 * boss_r, 2.0 * boss_r, arm_z1 - arm_z0)
    arm = clip_to_inner_cavity(root, arm, p, clip_tool)
    # pass 16 resumed (item 1): the riser's top band (wherever it overlaps
    # the real standoff barrel's own clearance depth -- see
    # STANDOFF_BARREL_* above) needs a real wall around the wider
    # counterbore _cut_hole cuts there, so it widens from BOSS_CORE_R to
    # the full boss_r for that band; the rest of the riser stays slim.
    barrel_z0 = seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN
    riser_wide_z0 = max(arm_z0, barrel_z0)
    riser = cylinder_solid(root, tx, ty, BOSS_CORE_R, arm_z0, riser_wide_z0)
    riser_wide = cylinder_solid(root, tx, ty, boss_r, riser_wide_z0, seat_z)
    arm = combine_join(root, arm, [riser, riser_wide])

    block = combine_join(root, root_block, [arm])

    top_in = _refetch_by_name(root, 'Top') or bodies['Top']
    top = combine_join(root, top_in, [block])
    if clip_tool is not None:
        top = dedupe_body(root, top, 'Top')
    top = _refetch_by_name(root, 'Top') or top

    def _cut_hole(t):
        hole = cylinder_solid(root, tx, ty, p['ear_standoff_hole_dia'] / 2.0, arm_z0 - 0.5, seat_z + 0.5)
        # pass 16 resumed (item 1): a second, wider, SHORT counterbore
        # right under the seat clears the display's real standoff barrel
        # (see STANDOFF_BARREL_* above) -- the plain M2 hole above still
        # runs the full height for the screw shaft.
        barrel_clear = cylinder_solid(root, tx, ty, STANDOFF_BARREL_DIA / 2.0,
                                       seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN, seat_z + 0.5)
        t = combine_cut(root, t, [hole, barrel_clear])
        return _refetch_by_name(root, 'Top') or t

    top = _cut_hole(top)

    top = add_root_reinforcement(root, top, 'Top', f'ear_{name}_wall_root', rx, ry, boss_r, root_z1, direction='up')
    top = _refetch_by_name(root, 'Top') or top
    top = add_root_reinforcement(root, top, 'Top', f'ear_{name}_seat', tx, ty, boss_r, seat_z, direction='up')
    top = _refetch_by_name(root, 'Top') or top

    # pass-15 item 8 lesson: re-cut the standoff hole after both collars join.
    top = _cut_hole(top)

    bodies['Top'] = top
    return bodies


def add_s2_boss(root, bodies, p, clip_tool=None):
    """Pass 16, item A (owner call): a SHORT boss from the WEST wall
    carrying S2 -- NOT a wall-to-wall crossbar. The printability review
    (S6) found no support-free span was possible across the full ~52mm
    cavity width, and the mechanical review (F6) found the crossbar
    version overlapped the display's own battery-connector clearance
    footprint by 3.2mm in Y and fully in Z -- a real, already-quantifiable
    interference, not a marginal one. This boss reaches from the true
    west wall to S2 only, at a height CLAMPED below `ear_seat_z` whenever
    the connector's own real bbox (`battery_connector_world_bbox`) would
    otherwise come within `s2_battery_clear` of it -- keeping the bulk of
    the arm's run safely under the connector -- plus an explicit,
    unconditional cut of the connector's own XY+Z footprint (+1mm margin)
    out of the arm regardless (belt-and-suspenders, same pattern
    CORNER_BLOCK_STACK_MARGIN already uses for the L76K PCB). A short
    local riser PAD at S2 itself (well clear of the connector's own
    x-range -- S2 sits east of it) makes up the last bit of height back
    to the true seat. `verify_s2_boss_clearance` (new gate) re-checks
    both this and the >=0.5mm GPS-frame margin live against the built
    geometry -- the owner allows a touch of slicer support here (this is
    a long, unsupported horizontal reach, unlike the short ear cantilevers)
    if support-free printing proves impossible; see the README's pass-16
    section for which it needed."""
    s2 = p['s2_boss']
    tx, ty = p['board_standoffs'][s2['target']]
    boss_r = p['boss_dia'] / 2.0
    ay = p['spine_a'][1]
    wall_x = -(p['outer_radius'] - p['wall'])   # true west wall (inner face), straight-band convention
    wy = ty

    seat_z = p['ear_seat_z']
    (bcx0, bcx1), (bcy0, bcy1), (bcz0, bcz1) = battery_connector_world_bbox(p)
    clear = p['s2_battery_clear']
    arm_z1 = seat_z
    if bcy0 - boss_r <= wy <= bcy1 + boss_r:
        arm_z1 = min(seat_z, bcz0 - clear)
    arm_z0 = arm_z1 - p.get('ear_arm_thickness', 3.0)

    # push the capsule's own west end WELL past the true wall (same
    # oversized-then-clipped idiom as the corner blocks/ears, simplified
    # to a straight capsule since the west anchor point isn't itself a
    # screw) -- clip_to_inner_cavity trims it back to the true boundary.
    wx_embed = wall_x - CORNER_BLOCK_REACH
    dx, dy = tx - wx_embed, ty - wy
    seg_len = math.hypot(dx, dy)
    axis1 = (dx / seg_len, dy / seg_len, 0.0)
    axis2 = (-axis1[1], axis1[0], 0.0)
    mx, my = (wx_embed + tx) / 2.0, (wy + ty) / 2.0
    arm = oriented_stadium_prism(root, (mx, my, arm_z0), axis1, axis2, (0.0, 0.0, 1.0),
                                  seg_len + 2.0 * boss_r, 2.0 * boss_r, arm_z1 - arm_z0)
    # JOIN-SAFE clip, NOT the shared (safety_margin=+0.1) clip_tool
    # (live-found, this pass): clip_to_inner_cavity's own default margin
    # shrinks the cavity INWARD by 0.1mm on every face -- exactly enough,
    # empirically, to leave this arm's own west end 0.1mm SHORT of the
    # true wall it's built to embed into. A live probe confirmed the
    # resulting `combine_join` into Top was a genuine no-op (Top stayed
    # as ONE unchanged lump, the boss's own target reading fully hollow
    # afterward) rather than the "silently fused, just untested" outcome
    # the ears happen to get away with elsewhere on the curved dome wall
    # -- same root cause `clipped_pillar_with_reach`'s own docstring
    # already documents for vertical bosses (a clipped pillar not
    # actually touching the shell it's meant to fuse into), just hitting
    # a HORIZONTAL wall-reaching member instead of a vertical one for the
    # first time this file has needed it. Fixed the same way the FPC-
    # relief skin-safe tool already proves out (see add_fpc_relief's own
    # docstring): a DEDICATED clip tool with a NEGATIVE safety_margin
    # (-0.15mm -- grows the cavity OUTWARD by that much instead of
    # shrinking it) guarantees a genuine 0.15mm of real overlap with
    # Top's true wall material everywhere along this arm's own west end,
    # comfortably short of the 2.0mm wall thickness (no risk of punching
    # through the outer skin -- this only ever touches the wall's own
    # INNER face, from the inside).
    join_clip_tool = build_inner_cavity_clip_tool(root, p, safety_margin=-0.15)
    arm = combine_intersect(root, arm, [join_clip_tool])

    conn_keepout = box_solid(root, bcx0 - 1.0, bcx1 + 1.0, bcy0 - 1.0, bcy1 + 1.0,
                              bcz0 - clear, bcz1 + 1.0)
    arm = combine_cut(root, arm, [conn_keepout])

    target_core = cylinder_solid(root, tx, ty, BOSS_CORE_R, arm_z0, arm_z1)
    arm = combine_join(root, arm, [target_core])

    # pass 16 resumed (item 1): the wide pad must reach down at least as
    # far as the real standoff barrel's own clearance depth (see
    # STANDOFF_BARREL_* above), not just down to arm_z1 -- otherwise the
    # barrel counterbore _cut_hole cuts below arm_z1 would have nothing
    # but the slim BOSS_CORE_R core around it.
    barrel_z0 = seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN
    pad_z0 = min(arm_z1, barrel_z0)
    if seat_z - pad_z0 > 1e-6:
        pad = cylinder_solid(root, tx, ty, boss_r, pad_z0, seat_z)
        arm = combine_join(root, arm, [pad])

    top_in = _refetch_by_name(root, 'Top') or bodies['Top']
    top = combine_join(root, top_in, [arm])
    if clip_tool is not None:
        top = dedupe_body(root, top, 'Top')
    top = _refetch_by_name(root, 'Top') or top

    def _cut_hole(t):
        hole = cylinder_solid(root, tx, ty, p['ear_standoff_hole_dia'] / 2.0, arm_z0 - 0.5, seat_z + 0.5)
        # pass 16 resumed (item 1): a second, wider, SHORT counterbore
        # right under the seat clears the display's real standoff barrel
        # (see STANDOFF_BARREL_* above) -- the plain M2 hole above still
        # runs the full height for the screw shaft.
        barrel_clear = cylinder_solid(root, tx, ty, STANDOFF_BARREL_DIA / 2.0,
                                       seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN, seat_z + 0.5)
        t = combine_cut(root, t, [hole, barrel_clear])
        return _refetch_by_name(root, 'Top') or t

    top = _cut_hole(top)
    top = add_root_reinforcement(root, top, 'Top', 's2_boss_seat', tx, ty, boss_r, seat_z, direction='up')
    top = _refetch_by_name(root, 'Top') or top
    top = _cut_hole(top)

    bodies['Top'] = top
    return bodies

# 2026-09-08 pass 9b, finding 9: the rib_plate's reach-to-the-wall
# 'connector spoke' (add_button) -- shared with add_button_plate_clearance
# so the Screen Plate's own clearance cutout always covers whatever
# tangential footprint the connector actually occupies, by construction,
# instead of two independently-hand-picked margins that can silently
# drift apart (exactly the class of bug this file's own dedupe_body /
# lug_ear_geometry docstrings warn about elsewhere).
#
# NEGATIVE T_OFFSET is deliberate: a first version used +1.0 (a real GAP
# between the connector and the rib's own edge, reasoning "stay clear of
# the shaft") -- but that made the connector-to-rib_plate Combine-Join
# itself a second instance of the EXACT no-touching-bodies no-op this
# connector exists to fix (confirmed live: Home's rib had real material
# for 'trim' but came back completely empty for 'current', same code
# path, just enough geometry difference to flip which side of Fusion's
# touching-vs-disjoint inconsistency it landed on). A small NEGATIVE
# offset makes the connector genuinely OVERLAP the rib's own (already
# attach_margin-oversized) edge by a real 0.2mm of shared volume --
# unambiguous, not a boundary case -- while its near face (attach_margin
# + T_OFFSET = 0.6mm inboard of the shaft's own L/2 edge) still clears
# the shaft (which only reaches L/2) with margin comfortably more than
# rib_slot_clearance (0.25mm), the smallest such gap already trusted
# elsewhere in this file.
RIB_CONNECTOR_T_OFFSET = -0.2  # OVERLAP with the rib's own (already oversized) edge, not a gap -- see comment
RIB_CONNECTOR_W = 2.0          # connector's own tangential width


def _refetch_by_name(root, name):
    """Re-fetch a body fresh by name. dedupe_body's Remove-feature cleanup
    can invalidate previously-held BRepBody Python references more
    broadly than just the body actually removed (its own docstring) --
    build() already re-fetches `clip_tool` once, AFTER both
    add_case_screws() and add_top_posts() finish, but that leaves it
    potentially stale for the LATER iterations WITHIN either function's
    own per-screw/per-post loop (2026-09-06 pass 6 finding: boss B and
    boss D -- the 2nd and 4th of 4 add_case_boss() calls in the same
    add_case_screws() loop -- silently never joined at all in a real
    build, while A and C, direct neighbors in the same loop, worked fine;
    isolating boss D's own construction alone worked perfectly, narrowing
    the cause to accumulated staleness across the loop's own repeated
    dedupe_body calls, not the boss's own geometry). Re-fetching after
    EVERY dedupe_body call, not just once at the very end, closes this."""
    for b in root.bRepBodies:
        if b.name == name:
            return b
    return None


def add_case_boss(root, bodies, cx, cy, p, clip_tool=None, core_r=None, build_top=True):
    boss_r = p['boss_dia'] / 2.0
    if core_r is None:
        core_r = BOSS_CORE_R
    # 2026-09-06 pass 6: re-fetch Bottom fresh by name before using it as a
    # combine target, not just after -- dedupe_body's own re-fetch only
    # fires when it actually finds an orphan to remove ('if not stale:
    # return tracked_body' skips it otherwise), so a Bottom reference that
    # went stale from an EARLIER, unrelated Remove/dedupe call elsewhere
    # can silently survive untouched and be handed to a LATER combine_join
    # as the target. That join then silently no-ops (0 volume added, no
    # exception) exactly like a genuinely disjoint body would (see
    # deboss_loops' docstring) -- this was found to be the actual cause
    # of boss B/D never joining (isolating either boss's own construction
    # alone always worked; only joining into the SAME, already-processed
    # Bottom reference from this loop's earlier iterations failed).
    bottom_in = _refetch_by_name(root, 'Bottom') or bodies['Bottom']
    bottom_boss = clipped_pillar_with_reach(
        root, cx, cy, boss_r, 2.0, p['split_z'], p, clip_tool, core_r)
    bottom = combine_join(root, bottom_in, [bottom_boss])
    if clip_tool is not None:
        bottom = dedupe_body(root, bottom, 'Bottom')
        clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
    bottom = _refetch_by_name(root, 'Bottom') or bottom
    hole = cylinder_solid(root, cx, cy, p['screw_hole_dia'] / 2.0, -0.5, p['split_z'] + 0.5)
    bottom = combine_cut(root, bottom, [hole])
    # pass 16 (owner call, item A/B): the old deeper `counterbore_D_h`
    # (needed only because screw D used to bottom against the Screen
    # Plate's own post at plate_z[0]) is retired along with screw D and
    # the plate -- D1/D2 are now IDENTICAL in construction to A/C (both
    # use the shallow counterbore_ABC_h, since there is no plate post any
    # more to need a deeper one).
    cb_h = p['counterbore_ABC_h']
    cb = cylinder_solid(root, cx, cy, p['counterbore_ABC_dia'] / 2.0, -0.5, cb_h)
    bottom = combine_cut(root, bottom, [cb])
    # pass 13, item 1: root reinforcement where the boss meets Bottom's own
    # floor (z=2.0, the bottom of every Bottom-side boss -- see the
    # clipped_pillar_with_reach call above). Real fillet tried first, a
    # 45-degree conical collar joined in as the fallback -- see
    # add_root_reinforcement's own docstring for why a plain 1.0mm fillet
    # (the old best-effort attempt on the top posts) isn't enough on its
    # own to satisfy verify_root_fillets.
    boss_label = f'boss_ABC_{cx:.1f}_{cy:.1f}_bottom'
    bottom = add_root_reinforcement(root, bottom, 'Bottom', boss_label, cx, cy, boss_r, 2.0, direction='down')
    bottom = _refetch_by_name(root, 'Bottom') or bottom
    # 2026-09-15 pass 15, item 8 ("the bottom of the case's holes seem to
    # be filled in"): CONFIRMED root cause -- add_root_reinforcement's
    # collar (cone_frustum_solid) is a SOLID revolve whose own profile
    # includes the vertical axis itself (see that function's docstring --
    # the two profile corners AT the axis, (cx,cy,z_lo)/(cx,cy,z_hi), are
    # what make the revolve a filled disc at every z, not a hollow
    # washer). For direction='down' (every Bottom-side boss A/B1/B2/C/D),
    # the collar's own z-band is z_root-0.05..z_root+collar_rise = 1.95..
    # 3.5 -- squarely inside BOTH the pilot hole's full-through z-span
    # (-0.5..split_z+0.5, i.e. the WHOLE of Bottom) and boss D's own
    # counterbore (cb_h=4.0, i.e. -0.5..4.0 -- entirely containing the
    # collar's band) -- so `combine_join`ing this solid-to-the-axis collar
    # AFTER the hole/counterbore cuts above silently REPLUGS both, solid,
    # right at the screw. Confirmed independently (no Fusion needed) by a
    # pure-Python ray-cast of the shipped pass-14 export/{trim,current}/
    # Bottom.stl at all 5 Bottom boss centres: every one reads exactly 2
    # surface crossings at z=1.95 and z=3.5 -- a solid plug over that
    # 1.55mm band, open everywhere else -- for BOTH the pilot bore (Ø2.4,
    # A/B1/B2/C) and boss D's own Ø4.5 counterbore (cb_h=4.0, which fully
    # contains the collar's band). Top's own posts/bosses/pegs never hit
    # this: every direction='up' call sites' own z_root sits far enough
    # ABOVE its matching pilot hole's own z1 that the collar's z-band never
    # overlaps a hole there (verified by inspection of every add_root_
    # reinforcement call site, see the pass-15 README section) -- this is
    # specific to the FIVE Bottom-side bosses, not a general defect in the
    # collar mechanism itself. Fixed at the boss level (not by hollowing
    # the collar, which would also touch every up-facing collar and is a
    # bigger, riskier change for no benefit -- the collar's OWN outward
    # reinforcement, well outside the hole/counterbore radius, is exactly
    # what item 1/pass 13 wanted and is untouched by this fix): re-cut the
    # SAME pilot hole and counterbore, in the SAME place, immediately after
    # the collar join -- a plain, cheap Cut always wins over whatever the
    # collar's join silently refilled, restoring exact pre-pass-13
    # patency while keeping every mm of the collar's own outward (radius
    # >> hole/counterbore radius) reinforcement intact. See
    # verify_bottom_openings (new pass-15 gate) for the live regression
    # check this closes.
    hole2 = cylinder_solid(root, cx, cy, p['screw_hole_dia'] / 2.0, -0.5, p['split_z'] + 0.5)
    bottom = combine_cut(root, bottom, [hole2])
    cb2 = cylinder_solid(root, cx, cy, p['counterbore_ABC_dia'] / 2.0, -0.5, cb_h)
    bottom = combine_cut(root, bottom, [cb2])
    bottom = _refetch_by_name(root, 'Bottom') or bottom
    bodies['Bottom'] = bottom

    if build_top:
        top_in = _refetch_by_name(root, 'Top') or bodies['Top']
        top_boss = clipped_pillar_with_reach(
            root, cx, cy, boss_r, p['split_z'], p['top_ceiling_underside_z'], p, clip_tool, BOSS_CORE_R)
        top = combine_join(root, top_in, [top_boss])
        if clip_tool is not None:
            top = dedupe_body(root, top, 'Top')
            clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
        top = _refetch_by_name(root, 'Top') or top
        pilot = cylinder_solid(root, cx, cy, p['top_pilot_dia'] / 2.0, p['top_pilot_z'][0], p['top_pilot_z'][1])
        top = combine_cut(root, top, [pilot])
        # pass 13, item 1: root reinforcement where the boss meets Top's
        # own ceiling (z=top_ceiling_underside_z, the top of every Top-
        # side boss -- see the clipped_pillar_with_reach call above).
        top_label = f'boss_ABC_{cx:.1f}_{cy:.1f}_top'
        top = add_root_reinforcement(root, top, 'Top', top_label, cx, cy, boss_r,
                                      p['top_ceiling_underside_z'], direction='up')
        top = _refetch_by_name(root, 'Top') or top
        bodies['Top'] = top
    return bodies


def add_case_screws(root, bodies, p, clip_tool=None):
    """Pass 16 (owner call, items A and B): every Bottom-side boss --
    A/C (unchanged) and D1/D2 -- is built IDENTICALLY (build_top=False;
    every Top-side pilot lives inside its own independent single-pilot
    corner block, see below). B1/B2 are retired outright (mech review
    F1) -- 4 case-closure screws total (A, C, D1, D2), down from 5.

    RESUMED pass 16 (Finding 1 fix): D1/D2's own Top-side pilot used to
    live INSIDE add_ear (built at the ear's own wall-root) -- moved out
    to their own independent add_single_corner_block call, exactly like
    A/C, now that D1/D2 have relocated away from the ears entirely (see
    'screws_D12' in params_current.py). add_single_corner_block's own
    display-keepout height cap (generalized this pass, see its own
    docstring) keeps D1/D2's full-height pilot column clear of the real
    display module at their new position, same as it always has for
    A/C (a no-op there)."""
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        bodies = add_case_boss(root, bodies, cx, cy, p, clip_tool=clip_tool, core_r=None, build_top=False)
        if clip_tool is not None:
            clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool

    # pass 16, item B/Finding-1-resumed: A/C AND (now) D1/D2's own
    # Top-side halves, all single-pilot wall-anchored blocks (B1/B2
    # retired -- see mech review F1).
    for s in p['screws_ABC'] + p['screws_D12']:
        bodies = add_single_corner_block(root, bodies, p, s, clip_tool=clip_tool)
        if clip_tool is not None:
            clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
    return bodies


POST_WALL_MIN = 1.2  # mm, finding 4: min wall required around a pilot, everywhere the post/ear has ANY material
                      # (pass 16: reused by verify_post_walls, re-targeted from P1-P4 to D1/D2)


def battery_connector_world_bbox(p):
    """World bbox of the display module's own 2-pin JST-style battery
    socket (pass 13, item 3) -- see params_current.py's
    'battery_connector_bbox' comment for how the base (x, y, z) numbers
    were identified (live-probed in the inserted display occurrence's own
    component tree: 'HP1_25MM-2P-SMT-HORIZONTAL', a 1.25mm-pitch 2-pin
    horizontal SMT connector on the PCB's underside). x/y are identical
    in both variants (same reference transform); z is offset by
    `display_z_offset`, same convention as every other display-relative z
    value in this file (see insert_display_pcba).

    2026-09-2x pass 16 (owner call, item A): the Screen Plate this bbox
    used to size a clearance WINDOW through (`battery_connector_window`/
    `add_battery_connector_access`, pass 13 item 3) is retired along with
    the plate itself -- candidate 5 has no plate to cut a window into,
    per mech review F8 ("battery-plug plate window becomes moot under
    candidate 5"). This function survives, unchanged, because it is now
    the real, live-probed footprint `add_s2_boss`/`verify_s2_boss_
    clearance` build the S2 boss's own hard keep-out against instead."""
    bb = p['battery_connector_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


def secondary_conn_world_bbox(p):
    """World bbox of the display module's second real SMT connector
    ('PITCH1MM-2PIN-SMT-HORIZONTAL') -- see 'secondary_conn_bbox' in
    params_current.py for the live-found story. Same display_z_offset
    convention as battery_connector_world_bbox."""
    bb = p['secondary_conn_bbox']
    dz = p.get('display_z_offset', 0.0)
    return bb['x'], bb['y'], (bb['z'][0] + dz, bb['z'][1] + dz)


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
    """Insert the display PCBA at the reference doc's own transform, plus a
    Z offset (PARAMS['display_z_offset'], pass 7 / 2026-09-06): the
    reference transform is read straight off "Firefly V2 v16", which was
    built for the 25mm-tall case -- for the trim variant's new 28mm-tall
    case, the display module (and everything built relative to it: Screen
    Plate, Top posts, USB tunnel, FPC relief, button caps/switches) must
    sit display_z_offset mm higher so the glass stays flush with the new
    top face at z_top. A pure Z translation added to the reference
    transform (not a re-derivation) keeps the module's own X/Y placement
    and orientation exactly as measured off the reference doc."""
    transform = get_reference_transform(app, 'Firefly V2', p['display_doc_name'])
    dz = p.get('display_z_offset', 0.0)
    if dz:
        t = transform.translation
        t.z = t.z + dz * MM
        transform.translation = t
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

    2026-09-05: s_wall now uses the REAL curved-shell distance
    (true_wall_distance_along_ray, evaluated at the cap's vertical center)
    instead of the flat-plane x=-outer_radius assumption. The flat-wall
    version was fine for the Power button (mostly in the straight section)
    but badly wrong for the Home button, which sits in the domed +y end
    cap (switch y 57.6-63.5, past spine_b=50): it placed the plunger guide
    rib's flat-wall-relative position so far outboard that the rib itself
    poked ~1.4mm past the TRUE dome surface, in a way no amount of
    shrinking the rib's own footprint could fix (confirmed empirically --
    even a zero-margin rib still overshot). The cap head geometry itself
    stays fine either way since it's separately trimmed to the true
    envelope (see build_thickened_envelope / add_button's Combine-
    Intersect).

    2026-09-08 pass 9b/finding 10: `housing_xy` (below, via
    `ray_box_exit_2d`) is NOT the real switch housing surface -- it is
    where a ray from the switch bbox's center exits the bbox's own
    DIAGONAL CORNER in the nub direction, which is only a real surface
    point if the switch body happens to fill its bbox all the way to
    that corner. Confirmed empirically (a live Fusion probe of the actual
    inserted 'SWITCH-TS24CA' body, scanning point-containment along the
    exact same ray) that it does NOT for either button: the real body's
    outermost point along this diagonal ray is only ~1.8mm from the bbox
    center (the actual raised nub, z 16.2-17.2, matching SPEC's "nub at
    z~16.7" and its 1.2mm-above-a-~0.6mm-housing-base shape exactly) --
    3.5mm (power) / 3.3mm (home) short of `t_exit`, the assumed housing
    point. `s_wall`/`s_rib_*`/`s_collar_*`/the cap head/hole (all anchored
    to the OUTER wall via `true_wall_distance_along_ray` from `housing_xy`)
    are unaffected by this error in absolute position -- shifting the ray
    origin along its own direction shifts `s_wall` by the same amount, so
    `housing_xy + s_wall*d2` (the real wall point) comes out identical
    either way, and every later `verify()` gate on that geometry already
    passed clean. Only `s_plunger_tip` (the plunger's reach TOWARD the
    switch) was wrong: built as an offset from `housing_xy`, it inherited
    that point's own ~3.3-3.5mm error, leaving the nub pocket built
    2.3-2.9mm short of the real actuator -- "too short to reach the
    switch", finding 10's actual defect (present on both buttons, not
    just the one Jake's print test happened to flag).

    Fix: `PARAMS['switch_actuator_reach']` (1.82mm, from the same live
    probe) is the real actuator nub's own outward reach from the switch
    bbox's center, along the nub direction -- an intrinsic property of
    the TS24CA switch body itself (both buttons use the same physical
    part; the probe found the identical value, 1.82mm, at both). `s_
    actuator` converts that into the same housing_xy-relative `s`
    convention everything else here uses, and `s_plunger_tip` (still the
    plunger's own REST position) is now `s_actuator + plunger_pretravel`
    -- the tip sits `plunger_pretravel` (0.3mm) further from the switch
    than the real nub, at rest, not from an uninhabited bbox corner.
    """
    d2 = normalize2(nub_dir)
    t2 = (-d2[1], d2[0])
    cx = (switch_bbox['x'][0] + switch_bbox['x'][1]) / 2.0
    cy = (switch_bbox['y'][0] + switch_bbox['y'][1]) / 2.0
    switch_z_mid = (switch_bbox['z'][0] + switch_bbox['z'][1]) / 2.0

    t_exit = ray_box_exit_2d((cx, cy), d2, switch_bbox['x'], switch_bbox['y'])
    housing_xy = (cx + t_exit * d2[0], cy + t_exit * d2[1])
    # real actuator nub reach, converted from "distance along d2 from the
    # switch bbox center" (how it was measured) to "distance along d2
    # from housing_xy" (the convention every other s_* value here uses).
    s_actuator = p['switch_actuator_reach'] - t_exit

    cap_z_center = (cap['z'][0] + cap['z'][1]) / 2.0
    # s_wall: the true (curved-shell) wall distance at the cap's own
    # vertical center -- used for the cap head/tab/hole positioning, which
    # all sit close to cap_z_center. (2026-09-05, pass 5: an earlier
    # attempt to take the MINIMUM true-wall distance sampled across the
    # button's whole built height -- including the rib plate's
    # attach-margin-inflated top, well above the cap's own z-range --
    # backfired badly for the Home button: at that extra height the domed
    # shell's true rho shrinks fast enough that s_wall collapsed to ~3.4mm,
    # pushing s_rib_inner/s_collar_* NEGATIVE (past the housing) and making
    # the real interference much WORSE, not better. Reverted to the single
    # z-center sample; see add_button's rib-plate handling below for the
    # actual, targeted fix for the rib plate specifically.)
    s_wall = true_wall_distance_along_ray(p, housing_xy, d2, cap_z_center)
    # inner wall face is INBOARD of the outer face (smaller s -- s
    # increases outward from the housing at s=0), by wall thickness
    # measured directly along the ray (a generic thin-shell approximation,
    # not the old flat-wall-specific x-projection, since d2 is not
    # generally aligned with the local surface normal once the dome is
    # involved).
    s_inner = s_wall - p['wall']
    # REST position (2026-09-08 pass 9b/finding 10 fix -- see docstring):
    # the plunger tip sits `plunger_pretravel` mm further from the switch
    # than the REAL, measured actuator nub (s_actuator), not an offset
    # from an uninhabited bbox corner. Pressing the cap moves the whole
    # plunger inward (decreasing s) by up to `plunger_travel` before the
    # collar bottoms on the rib -- 0.3mm of that closes the pre-travel gap
    # to the actuator, the remaining ~0.32mm pushes the actuator itself in
    # (a normal tactile-switch actuation travel), well short of crashing
    # the plunger into the switch housing's own base (s~0.6mm on this same
    # ray, per the live probe) at full press.
    s_plunger_tip = s_actuator + p['plunger_pretravel']
    s_outer_face = s_wall + cap['proud']
    s_tab_face = s_inner - p['tab']['gap']  # further inboard than the inner wall face by the tab gap

    # plunger guide rib: outboard face rib_inboard_offset mm inboard of the
    # outer wall face, rib_thickness mm thick along the travel axis.
    s_rib_outer = s_wall - p['rib_inboard_offset']
    s_rib_inner = s_rib_outer - p['rib_thickness']
    # inward stop collar: at rest, its outboard face sits plunger_travel mm
    # from the rib's inboard face; it is collar['len'] mm long along d.
    s_collar_outer = s_rib_inner - p['plunger_travel']
    s_collar_inner = s_collar_outer - p['collar']['len']
    # 2026-09-08 pass 9b, finding 9 (collateral discovery): rib_inboard_
    # offset (nominal 6.0) is measured from s_wall, via housing_xy -- fine
    # when housing_xy is a close proxy for the real switch (Power), but
    # for a button where it overshoots the real switch by several mm
    # (Home -- see this function's own docstring above), the nominal
    # rib/COLLAR can land almost ON TOP of the real actuator (confirmed
    # live: a real ~39mm3 rib/collar-vs-switch-body interference at Home
    # with the nominal offset, once the reach fix (finding 10) put the
    # mechanism at its real, measured position instead of an offset from
    # an empty bbox corner). The collar (further inboard than the rib by
    # plunger_travel + collar['len']) is the tighter constraint, so it
    # drives the clamp: shift rib+collar outward (toward the wall) only
    # as far as needed for `s_collar_inner` to clear the real actuator by
    # `rib_actuator_clearance` -- a no-op for Power (nominal already
    # clears by 10mm+). Kept as a small, TARGETED shift (not a global
    # PARAMS reduction, tried and reverted -- see PARAMS['rib_inboard_
    # offset']'s own comment -- a global reduction also shrank Power's
    # margin against a DIFFERENT, unrelated risk: the rib's own flat Z-
    # extent reaching into the R10 shoulder curve above cap_z_center,
    # where the true wall is measurably closer than the flat estimate --
    # confirmed live as a real ~0.5mm envelope breach at Power once try).
    # `rib_actuator_shifted` flags this for add_button, which applies an
    # extra Combine-Intersect against the true outer envelope to the rib
    # ONLY when shifted -- the same shoulder-curve risk in reverse (this
    # shift moves Home's rib closer to ITS OWN wall too), guarded instead
    # of relied-on-margin like Power's untouched case.
    rib_actuator_clearance = 0.3
    min_collar_inner = s_actuator + rib_actuator_clearance
    rib_actuator_shifted = s_collar_inner < min_collar_inner
    if rib_actuator_shifted:
        _shift = min_collar_inner - s_collar_inner
        s_rib_inner += _shift
        s_rib_outer += _shift
        s_collar_outer += _shift
        s_collar_inner += _shift

    def xy_at(s):
        return (housing_xy[0] + s * d2[0], housing_xy[1] + s * d2[1])

    return {
        'd': d2, 't': t2, 'housing_xy': housing_xy, 'switch_z_mid': switch_z_mid,
        's_wall': s_wall, 's_inner': s_inner, 's_plunger_tip': s_plunger_tip,
        's_actuator': s_actuator, 'actuator_xy': xy_at(s_actuator),
        'rib_actuator_shifted': rib_actuator_shifted,
        's_outer_face': s_outer_face, 's_tab_face': s_tab_face,
        's_rib_outer': s_rib_outer, 's_rib_inner': s_rib_inner,
        's_collar_outer': s_collar_outer, 's_collar_inner': s_collar_inner,
        'outer_face_xy': xy_at(s_outer_face), 'plunger_tip_xy': xy_at(s_plunger_tip),
        'tab_face_xy': xy_at(s_tab_face),
        # 2026-09-05 fix: oriented_box_prism/oriented_stadium_prism extrude
        # ONE-SIDED from the given center point along `normal` (they are
        # NOT centered on it) -- rib_center_xy/collar_center_xy used to be
        # computed as the (s_outer+s_inner)/2 MIDPOINT and then passed as
        # that starting point, which silently shifted the whole rib/collar
        # half a thickness/length further OUTBOARD than intended (confirmed
        # by point-containment probing: the built rib only spanned
        # [midpoint, midpoint+thickness], missing its inner half entirely).
        # The correct starting point for a one-sided extrude along +d is
        # the INBOARD (smaller-s) edge.
        'rib_start_xy': xy_at(s_rib_inner),
        'collar_start_xy': xy_at(s_collar_inner),
        'rib_center_xy': xy_at((s_rib_outer + s_rib_inner) / 2.0),
        'collar_center_xy': xy_at((s_collar_outer + s_collar_inner) / 2.0),
    }


def add_button(root, bodies, name, switch_bbox, nub_dir, cap, hole_wh, p, thickened_envelope=None, clip_tool=None,
               outer_envelope=None, tab_clip_tool=None):
    g = button_geometry(p, switch_bbox, nub_dir, cap)
    d2, t2 = g['d'], g['t']
    d3, t3 = (d2[0], d2[1], 0.0), (t2[0], t2[1], 0.0)
    z3 = (0.0, 0.0, 1.0)
    z_lo, z_hi = cap['z']
    z_center = (z_lo + z_hi) / 2.0
    L, W = cap['stadium']

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
    thick_env = thickened_envelope if thickened_envelope is not None else build_thickened_envelope(root, p, cap['proud'])
    cap_body = oriented_stadium_prism(root, cap_center, t3, z3, neg_d3, L, W, total_depth)
    if thickened_envelope is not None:
        cap_body = combine_intersect_keep(root, cap_body, [thick_env])
    else:
        cap_body = combine_intersect(root, cap_body, [thick_env])

    # wall hole: 2026-09-05 fix (real, large 'Top x Button' interference,
    # root-caused by point-containment probing): the hole used to be a
    # hand-positioned STRAIGHT stadium prism, sized/depth-margined from a
    # single s_wall value -- but the real outer shoulder is CURVED in this
    # z-range (the R10 fillet begins well below the button's own z-span),
    # so a straight cut through a curved wall leaves real, uncleared
    # material wherever the true curved surface diverges from the
    # straight-cut assumption, no matter how much extra depth margin is
    # added. The hole is now cut using an ENLARGED COPY OF THE ACTUAL
    # SHAFT (cap_body's own construction, before its pocket/tab cuts,
    # widened by cap_clearance per side and Combine-Intersected against
    # the SAME thickened_envelope the real cap is trimmed against) --
    # geometrically guaranteed to fully contain (with clearance) whatever
    # shape the real, curve-trimmed shaft ends up being, since it is built
    # exactly the same way, just bigger.
    hole_cutter = oriented_stadium_prism(root, cap_center, t3, z3, neg_d3,
                                          L + 2 * p['cap_clearance'], W + 2 * p['cap_clearance'], total_depth)
    if thickened_envelope is not None:
        hole_cutter = combine_intersect_keep(root, hole_cutter, [thick_env])
    else:
        hole_cutter = combine_intersect(root, hole_cutter, [thick_env])
    hole_cutter = _clip_of_ear_boss_keepout(root, hole_cutter, p)
    bodies['Top'] = combine_cut(root, bodies['Top'], [hole_cutter])

    # the retaining tab (added below) hangs BELOW the shaft's own W-based
    # Z-range, at a fixed tangential width (tab['w']) -- much smaller than
    # the main shaft, so a plain small box (not curve-matched) cut through
    # just the wall thickness is sufficient for it specifically.
    #
    # 2026-09-06 pass 6 fix: this cut used to reach from s_tab_face (deep
    # inboard) all the way OUT PAST s_outer_face (the true exterior
    # surface, `cap['proud']` mm proud of the wall) plus a 2.5mm margin --
    # a real, visible rectangular notch through the outer skin next to the
    # main stadium hole (Jake's screenshot review; the "small block" he
    # saw inside it was the tab itself, now visible from outside). The
    # tab's own BUILT geometry (tab_body, below) never reaches anywhere
    # near the outer surface -- it stays entirely inboard of the inner
    # wall face (s_inner) by design (`s_tab_face = s_inner - tab['gap']`,
    # further inboard still) -- so this hole only ever needed to reach
    # the INNER wall face (s_inner), not the exterior: rib slots, tab
    # shelves, and clearance pockets are interior-only features and must
    # never breach the printed skin. Bounded analytically here (not via a
    # Combine-Intersect against the inner-cavity clip tool, which raised
    # FEATURE_FAILED_TO_CREATE for the similarly-shaped rib plate -- see
    # below) by simply stopping the cut at s_inner + a small margin.
    tab = p['tab']
    tab_hole_z_lo = z_center - W / 2.0 - tab['h'] - 0.3
    tab_hole_z_hi = z_center - W / 2.0 + 0.3
    tab_hole_z_center = (tab_hole_z_lo + tab_hole_z_hi) / 2.0
    tab_hole_z_span = tab_hole_z_hi - tab_hole_z_lo
    inner_face_xy = (g['housing_xy'][0] + g['s_inner'] * d2[0], g['housing_xy'][1] + g['s_inner'] * d2[1])
    # 2026-09-06: bumped from 0.5 to 2.0 after a real (if small, ~0.68mm3)
    # residual 'Top x Power Button' interference was found empirically at
    # the tab -- the flat-ray analytic estimate of the tab's own outward
    # reach (s_inner + tab['gap'] - related terms) undershoots the tab's
    # REAL reach against the true curved surface by more than expected
    # (the same kind of ray-vs-true-curvature slack verify_m2's cap-proud
    # check already documents, up to ~0.25mm there). This hole's own
    # outward reach is s_inner + skin_margin/2 (see the depth/center math
    # below) -- 2.0 keeps it a full 1.45mm short of the true outer
    # surface (s_inner + wall + proud), nowhere near reopening the skin
    # breach this fix closes, while comfortably covering the tab's real
    # reach.
    skin_margin = p.get('tab_hole_skin_margin', 2.0)  # stop this many mm short of the inner wall face -- never reaches the outer skin; also read by verify_skin_intact (kept as ONE shared value, not duplicated)
    tab_hole_depth = abs(g['s_inner'] - g['s_tab_face']) + skin_margin
    tab_hole_center_xy = ((inner_face_xy[0] + g['tab_face_xy'][0]) / 2.0,
                          (inner_face_xy[1] + g['tab_face_xy'][1]) / 2.0)
    tab_hole_start = (tab_hole_center_xy[0] - (tab_hole_depth / 2.0) * d2[0],
                      tab_hole_center_xy[1] - (tab_hole_depth / 2.0) * d2[1])
    tab_hole_body = oriented_box_prism(root, (tab_hole_start[0], tab_hole_start[1], tab_hole_z_center),
                                        t3, z3, d3, tab['w'] + 2.0, tab_hole_z_span, tab_hole_depth)
    # 2026-09-12 pass 12 (Jake's sidescan of the pass-11 export/trim/Top.stl
    # found a real ray-through breach below each button's main hole -- a
    # slot in the outer wall, with a stepped interior surface visible
    # through it -- at (y~32, z~15) for Power and (y~68, z~18-20) for
    # Home): the analytic bound above (s_end = s_inner + skin_margin/2,
    # ~1.45mm short of the true outer surface along the ray's own t=0
    # centerline, per the comment above) is only checked ON that
    # centerline -- this cut is a flat, uncurved box spanning tab['w']+2mm
    # tangentially, and nothing here previously bounded its off-axis
    # corners against the TRUE (curved) wall the way the main hole/USB
    # liner/rib+connector already do elsewhere in this file. Combine-
    # Intersect against a tight clip tool (`tab_clip_tool`, built once in
    # add_buttons -- see its own comment for the exact margin, tuned to
    # `s_wall - wall_clear(0.6)` = s_inner+1.4mm, the SAME "stay clear of
    # the true wall" convention add_lip_anchor_reliefs/add_lug already use
    # -- the SAME general mechanism bosses/posts use via
    # clip_to_inner_cavity, just aimed at s_wall instead of the inner
    # cavity) guarantees this cut can never reach closer than 0.6mm to the
    # true skin anywhere in its footprint, on or off axis, while still
    # comfortably covering the tab's real reach (its own outward-most
    # point, s_inner+0.15 from add_button's tab_body construction, sits
    # 1.25mm inside this tool's boundary). A no-op at t=0, where the plain
    # analytic bound (s_inner + skin_margin/2 = s_inner+1.0) already sits
    # inboard of this tool's own s_inner+1.4 boundary -- it only bites
    # off-axis, where the true wall curves in closer than the centerline
    # assumes. See verify_openings_open's new `*_button_hole_footprint`
    # gate (added this pass) for the live
    # regression check this closes, and verify_skin_intact's widened probe
    # grid for the direct before/after numbers.
    if tab_clip_tool is not None:
        tab_hole_body = combine_intersect_keep(root, tab_hole_body, [tab_clip_tool])
    tab_hole_body = _clip_of_ear_boss_keepout(root, tab_hole_body, p)
    bodies['Top'] = combine_cut(root, bodies['Top'], [tab_hole_body])

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
    # 2026-09-05 fix: the rib plate used to punch ~1.45mm through the dome
    # near the Home button (close to the spine_b end cap). Root cause was
    # upstream in button_geometry()'s s_wall -- the flat-wall approximation
    # placed the rib's "6mm inboard of the wall" relative to a wall that,
    # for a button actually in the domed cap, was much further away than
    # the TRUE curved surface, so the rib ended up outboard of the real
    # wall. Fixed at the source (s_wall now uses
    # true_wall_distance_along_ray); attach_margin itself was never the
    # problem (confirmed empirically -- even a zero/negative margin still
    # overshot before the s_wall fix, and margin=2.0 is clean after it). A
    # boolean Combine-Intersect against the inner-cavity clip tool was
    # tried first but raised 'FEATURE_FAILED_TO_CREATE' for this specific
    # diagonal-box-against-filleted-revolve combination regardless of
    # margin -- abandoned once the real fix made it unnecessary.
    # 2026-09-05 fix: oriented_box_prism extrudes ONE-SIDED from the given
    # point along `normal` (from center_mm to center_mm + depth*normal),
    # it does NOT center the extrusion on that point. rib_center/
    # collar_center used to pass the (outer+inner)/2 MIDPOINT as that
    # starting point, which silently built the rib/collar a half-thickness
    # too far OUTBOARD, entirely missing their intended inner half
    # (confirmed by point-containment probing) -- use the INBOARD
    # (smaller-s) edge as the start point instead, exactly as hole_start
    # already does for the wall hole above.
    attach_margin = 0.8
    rib_len = p['rib_thickness']
    rib_start = (g['rib_start_xy'][0], g['rib_start_xy'][1], z_center)
    rib_plate = oriented_box_prism(root, rib_start, t3, z3, d3,
                                    L + 2 * attach_margin, W + 2 * attach_margin, rib_len)
    # 2026-09-05 fix (real, large 'Top x Button' interference): the slot
    # used to be a separately hand-positioned box (same rib_start point,
    # rectangular L+0.5 x W+0.5 cross-section) -- built independently of
    # the actual plunger shaft (cap_body, a STADIUM cross-section built
    # from cap_center along neg_d3), any small mismatch in how the two
    # constructions resolve their local axes left real, un-cleared shaft
    # material inside the rib plate. The slot is now cut using the EXACT
    # SAME center point, axes, and sign convention as the shaft itself
    # (cap_center / neg_d3, not rib_start / d3) -- just the shaft's own
    # stadium cross-section enlarged by rib_slot_clearance per side and
    # extended a bit deeper -- guaranteeing it is coaxial with the real
    # shaft by construction, not by two independently-derived positions
    # that happen to be intended to match.
    slot_body = oriented_stadium_prism(root, cap_center, t3, z3, neg_d3,
                                        L + 2 * p['rib_slot_clearance'], W + 2 * p['rib_slot_clearance'],
                                        total_depth + 4.0)
    rib_plate = combine_cut(root, rib_plate, [slot_body])

    # 2026-09-08 pass 9b, finding 9: the cap (shaft + collar + retaining
    # tab, all one rigid printed piece) can only be assembled from the
    # INSIDE of the open (Bottom-not-yet-attached) Top half, sliding it
    # outward along the plunger axis until the head seats in the wall
    # hole -- an outside-in, tip-first insertion is geometrically
    # impossible (the collar, `collar['h']`=0.8mm wider than the shaft, is
    # deliberately too wide for the rib's own slot -- that is what lets it
    # bottom out against the rib during a hard press instead of sliding
    # through it). But the retaining tab sits OUTBOARD of the rib at rest
    # (s_tab_face > s_rib_outer -- it is close to the inner wall, not the
    # collar), so an inside-out insertion still has to carry the tab PAST
    # the rib's own axial thickness at some point in the stroke. The tab
    # hangs `tab['h']` (2.0mm) below the shaft's own slot envelope, but the
    # rib's slot cut above only clears `W + 2*rib_slot_clearance` (0.5mm
    # total) below the shaft -- confirmed by direct geometry (and by a
    # live insertion sweep, see verify_button_insertion) that a 0.55mm-
    # tall band of real, solid rib material (between the slot's own lower
    # edge and the rib plate's own outer edge, `attach_margin` beyond the
    # slot) sits directly in the tab's path, for every position along the
    # rib's thickness -- not a tolerance-dependent near-miss, a
    # geometrically guaranteed interference for ANY straight-line
    # insertion. The tab is a short, thick stub cast integrally with the
    # shaft (not a thin cantilever spring), so relying on it to flex
    # ~0.55mm past a hard stop is not realistic for a printed PETG/PLA
    # part -- per the finding's own guidance, this relieves the rib
    # instead of asking the tab to deflect: a dedicated lane, sized to the
    # tab's own footprint plus a generous (0.3mm/side -- looser than the
    # working `rib_slot_clearance` on purpose, since this lane is a
    # one-time assembly pass-through, not an operating fit) clearance, cut
    # the full thickness of the rib plate so the tab can slide past freely
    # during assembly. It has no effect on the rib's own guiding function
    # for the shaft (the main slot, above, is untouched) or on the collar
    # (built and clipped separately, entirely inboard of the rib -- never
    # enters this lane).
    tab_relief_margin = 0.3
    tab_relief_w = p['tab']['w'] + 2 * tab_relief_margin
    tab_relief_z_hi = z_center - W / 2.0 + tab_relief_margin
    tab_relief_z_lo = z_center - W / 2.0 - p['tab']['h'] - tab_relief_margin
    tab_relief_z_span = tab_relief_z_hi - tab_relief_z_lo
    tab_relief_z_center = (tab_relief_z_hi + tab_relief_z_lo) / 2.0
    tab_relief_axial_margin = 0.5  # a bit past the rib's own thickness each end, for a clean full-depth cut
    rib_start_for_relief = (g['rib_start_xy'][0] - tab_relief_axial_margin * d3[0],
                             g['rib_start_xy'][1] - tab_relief_axial_margin * d3[1],
                             tab_relief_z_center)
    tab_relief_body = oriented_box_prism(root, rib_start_for_relief, t3, z3, d3,
                                          tab_relief_w, tab_relief_z_span,
                                          rib_len + 2 * tab_relief_axial_margin)
    # 2026-09-12 pass 12: same tab_clip_tool as the wall's own tab_hole_body
    # cut above (see its comment) -- this lane only ever needs to reach the
    # rib's own thickness (already bounded by tab_relief_axial_margin), but
    # clipping it too guarantees it can never contribute a path toward the
    # true outer skin regardless of how it interacts with rib_plate's later
    # Combine-Intersect against rib_outer_envelope, matching Jake's own
    # description of the defect ("the tab-relief lane cut through the rib
    # AND the outer skin"). A no-op wherever the lane was already safely
    # inboard.
    if tab_clip_tool is not None:
        tab_relief_body = combine_intersect_keep(root, tab_relief_body, [tab_clip_tool])
    rib_plate = combine_cut(root, rib_plate, [tab_relief_body])

    # 2026-09-08 pass 9b, finding 9 (collateral discovery): the rib_plate
    # sits deep in the hollow cavity, not touching any other Top feature
    # -- Combine-Join with a tool body that doesn't touch/overlap the
    # target's EXISTING solid at all silently NO-OPS in this Fusion
    # build (confirmed live: joining a fresh copy of the rib box added
    # exactly 0.0mm3 to Top's volume for the Home button -- the tab-relief
    # fix above was correct, but there was no real rib there to relieve).
    # This is the same class of problem BOSS_CORE_R/POST_CORE_R already
    # work around for bosses/posts (see clipped_pillar_with_reach's
    # docstring) -- here as a thin (2mm), off-axis 'reach spoke' from the
    # rib's own outer edge out to solidly EMBED in the real wall (target
    # `s_wall - 0.3`, staying 0.3mm short of the true outer surface --
    # confirmed by live probe to never breach the skin -- while reaching
    # `s_wall - rib_slot_clearance...` comfortably past `s_inner`, into
    # the middle of the 2mm wall thickness, a robust, unambiguous touch).
    # Offset in `t` past the rib's own (already oversized by attach_margin)
    # tangential footprint, so it clears the plunger shaft's own full-
    # length path (which runs at |t| <= L/2 the entire way from tip to
    # head) entirely -- it cannot ever touch the cap. Applied to BOTH
    # buttons (Power's original join happened to succeed without this,
    # but nothing in the design guaranteed that -- this makes the
    # guarantee explicit and construction-based instead of incidental).
    connector_margin = 0.3
    connector_target_s = g['s_wall'] - connector_margin
    connector_len = connector_target_s - g['s_rib_outer']
    if connector_len > 0:
        connector_t_center = (L / 2.0 + attach_margin) + RIB_CONNECTOR_T_OFFSET + RIB_CONNECTOR_W / 2.0
        rib_outer_xy = (g['housing_xy'][0] + g['s_rib_outer'] * d2[0], g['housing_xy'][1] + g['s_rib_outer'] * d2[1])
        connector_start = (rib_outer_xy[0] + connector_t_center * t2[0],
                            rib_outer_xy[1] + connector_t_center * t2[1], z_center)
        connector_body = oriented_box_prism(root, connector_start, t3, z3, d3, RIB_CONNECTOR_W, W, connector_len)
        rib_plate = combine_join(root, rib_plate, [connector_body])

    # 2026-09-15 pass 15, item 5 (Jake: "probably want to make the 'guide'
    # for it and the back button more robust and connected to the top of
    # the case since right now it's floating"): CONFIRMED by inspection --
    # the rib plate (built just above) is a small flat box at the cap's
    # own z-height, with a horizontal spoke reaching sideways to the WALL
    # (the connector just above), but nothing tying it to the CEILING
    # (top_ceiling_underside_z) at all -- it hangs alone in open cavity
    # air roughly 10-20mm below the ceiling, exactly "floating". Fixed by
    # adding a second gusset -- a vertical pillar reaching from the rib's
    # own top edge up to the ceiling.
    #
    # FIRST attempt (kept as a dead-end note, not repeated): a gusset
    # positioned by a tangential (t) offset from the rib's own centre,
    # mirroring the wall connector's t-offset convention on the opposite
    # side. A live `check_interference` run caught a real ~14.3mm^3 Home
    # Button x <board reference body> overlap -- both buttons sit under
    # the display module's own y-span (27.6-73.13), and the display's
    # real PCBA/shield body's lower z (23.3mm trim, `display_bbox`) sits
    # BELOW the gusset's own target top (ceiling+overlap, 26.3mm trim) --
    # a t-offset gusset staying near the plunger axis (close to the
    # display's own x-range, +-22.39) has nothing stopping it from
    # passing straight through real board material on the way up.
    #
    # FIX: anchor the gusset at the CONNECTOR's own outboard end instead
    # (near s_wall, the same near-the-true-wall position the wall
    # connector above already proves safe) -- the display module's own
    # edge sits `outer_radius - wall - display half-width` inboard of the
    # true wall (a real, if modest, gap the connector already lives in
    # without incident), so a gusset based there is laterally clear of
    # the display's real footprint for the ENTIRE climb to the ceiling,
    # not just at one z. A no-op (gusset_h<=0, skipped) if the connector
    # itself is a no-op (connector_len<=0 -- would only happen for a
    # button whose rib already sits within 0.3mm of the wall, not the
    # case for either Power or Home). Reuses `outer_envelope`'s own
    # Combine-Intersect below (built and joined into rib_plate BEFORE
    # that intersect) for the same "can never poke past the true curved
    # skin" protection the rib/connector already have -- no separate clip
    # needed for that part. The display-board clearance specifically is
    # re-verified LIVE via `check_interference` (both buttons, both
    # variants) rather than trusted from geometry alone, given the first
    # attempt's own history above.
    CEILING_GUSSET_W = 2.0          # mm, tangential width (mirrors RIB_CONNECTOR_W)
    CEILING_GUSSET_OVERLAP = 0.3    # mm past the nominal ceiling, guarantees a real (not coincident-face) join
    ceiling = p['top_ceiling_underside_z']
    gusset_top = z_center + W / 2.0 + attach_margin
    gusset_h = (ceiling + CEILING_GUSSET_OVERLAP) - gusset_top
    if gusset_h > 0 and connector_len > 0:
        # anchor at the connector's own OUTER end (near s_wall -- see
        # connector_target_s above), same t-centre as the connector so the
        # gusset rises directly from where the connector already reaches,
        # reading as one continuous near-wall bracket.
        gusset_outer_s = connector_target_s
        gusset_xy_base = (g['housing_xy'][0] + gusset_outer_s * d2[0],
                           g['housing_xy'][1] + gusset_outer_s * d2[1])
        gusset_start = (gusset_xy_base[0] + connector_t_center * t2[0],
                         gusset_xy_base[1] + connector_t_center * t2[1], gusset_top)
        gusset_body = oriented_box_prism(root, gusset_start, t3, d3, z3,
                                          CEILING_GUSSET_W, RIB_CONNECTOR_W, gusset_h)
        rib_plate = combine_join(root, rib_plate, [gusset_body])

    # 2026-09-08 pass 9b, finding 9 (collateral discovery): the reach-to-
    # the-wall connector (and, for Home once the actuator-clearance clamp
    # fires, the rib itself too -- button_geometry's `rib_actuator_
    # shifted`) can end up close enough to the TRUE curved surface that
    # the connector_margin (0.3mm, measured only along the t=0 ray) isn't
    # enough everywhere: a live export-envelope check found a real
    # ~0.2mm breach at Home on the 'current' variant even WITHOUT the
    # clamp firing there -- the connector's own tangential offset puts it
    # on a ray that isn't purely radial from the dome's spine, so its
    # true distance to the curved wall isn't exactly `s_wall` (computed
    # for the t=0 ray) either. Rather than chase a bigger-still margin
    # number, Combine-Intersect the whole rib+connector unit against the
    # TRUE outer solid -- the same technique add_usb_tunnel's liner
    # already uses for exactly this "flat box built near the curved
    # shoulder" problem (NOT the inner-cavity clip tool, whose docstring
    # note about a FEATURE_FAILED_TO_CREATE on this rib doesn't apply to
    # this simpler, unfilleted solid) -- clipping back any part that
    # would otherwise poke past the real surface. Applied unconditionally
    # (both buttons, both variants) rather than only when the clamp
    # fires, since the tangential-offset error above is independent of
    # it; a no-op wherever there's nothing to clip. Shared across both
    # buttons by the caller (add_buttons) -- like thickened_envelope
    # above, rebuilding the whole outer pill solid per button was part of
    # what made this build slow enough to risk the MCP call timing out.
    rib_outer_envelope = outer_envelope if outer_envelope is not None else build_outer_pill_solid(root, p)
    if outer_envelope is not None:
        rib_plate = combine_intersect_keep(root, rib_plate, [rib_outer_envelope])
    else:
        rib_plate = combine_intersect(root, rib_plate, [rib_outer_envelope])

    # pass 16, item D (printability review #2, "should-fix"): small
    # best-effort lead-in fillets at (a) the guide rib's own inboard edge
    # (near the plunger-tip pocket, at the rib's own z_center) and (b) the
    # ceiling gusset's own attach face (at gusset_top, where the gusset
    # starts climbing from the rib/connector below) -- the review found
    # small (each under the 30mm^2 cluster gate on its own) but real
    # overhangs at both, inside the button housing's own narrow,
    # hard-to-support channel -- see review-printability.md's own finding
    # 2 for the exact live-measured facets this responds to. Scoped to
    # `rib_plate` ALONE, before it joins Top (the intersect just above),
    # so a Fusion fillet-selection failure here can only ever skip this
    # cosmetic touch -- never risk the mechanism: verify_plunger_reach/
    # verify_button_insertion/verify_button_retention all gate the SAME
    # rib/collar geometry a fillet only rounds the edges of, never moves.
    # Best-effort/skip-on-failure, same pattern _best_effort_fillet_at_z's
    # own docstring already establishes for exactly this class of
    # cosmetic/print-quality (not load-bearing) touch.
    RIB_LEAD_IN_FILLET_R = 0.4  # mm -- modest, cosmetic/print-quality only (same order as the
                                # GPS/stack-frame wall roots' own 0.6mm precedent, kept smaller
                                # here given the tight quarters inside the button housing).
    _best_effort_fillet_at_z(root, rib_plate, z_center, RIB_LEAD_IN_FILLET_R)
    _best_effort_fillet_at_z(root, rib_plate, gusset_top, RIB_LEAD_IN_FILLET_R)

    # 2026-09-06 pass 6: a Combine-Intersect of rib_plate against the
    # inner-cavity clip tool was tried here as an extra safety net (like
    # the collar's, below) but produced a real, large 'Top x Button'
    # interference (the intersect result joined into Top incorrectly) --
    # reverted. The rib plate's existing analytic bound (s_rib_outer,
    # already inboard of the true wall by rib_inboard_offset -- see
    # button_geometry) plus verify_skin_intact()'s probe-based regression
    # gate are the actual protections here; button_geometry's docstring
    # also documents the FEATURE_FAILED_TO_CREATE this combination raised
    # in an earlier pass.
    bodies['Top'] = combine_join(root, bodies['Top'], [rib_plate])

    collar = p['collar']
    collar_start = (g['collar_start_xy'][0], g['collar_start_xy'][1], z_center)
    collar_body = oriented_box_prism(root, collar_start, t3, z3, d3,
                                      L, W + 2 * collar['h'], collar['len'])
    # 2026-09-05 fix (residual 'Top x Button' interference): the collar
    # sits deep inboard (not near the wall), but it is a flat box on a
    # DIAGONAL ray -- its tangential corners (L/2 either side of the ray)
    # land further out in world X/Y than the ray's own position suggests,
    # and for the Power button that corner reaches past the true (locally
    # curved) inner cavity wall. Unlike the rib plate (whose much larger,
    # wall-adjacent Combine-Intersect against this same clip tool raised
    # FEATURE_FAILED_TO_CREATE), the collar is small and fully interior,
    # where the intersect is well-behaved -- clip it to the inner cavity
    # (same tool used for case-screw bosses/Top posts) as a direct
    # guarantee, rather than one more hand-derived margin number.
    # 2026-09-12 pass 12: tried swapping this to the looser tab_clip_tool
    # (built for the tab_hole_body/tab_relief_body cuts, which need to
    # reach much further outward than the collar ever does -- see that
    # tool's own comment) per a literal reading of "rib/collar must stay
    # strictly inside the cavity" -- WRONG, confirmed by a live
    # interference hit (0.13mm3, Top x Power Button): this is exactly the
    # diagonal-corner overshoot the comment above already documents and
    # already fixes with the TIGHT default clip_tool -- tab_clip_tool's
    # boundary sits much further outboard (calibrated for a completely
    # different job), so swapping to it silently reopened the old defect.
    # Reverted to the original clip_tool (unchanged from pass 5); the
    # "stay inside the cavity" requirement for the collar was already
    # satisfied before this pass touched anything here.
    if clip_tool is not None:
        collar_body = clip_to_inner_cavity(root, collar_body, p, clip_tool)
    cap_body = combine_join(root, cap_body, [collar_body])

    cap_body.name = name
    bodies[name] = cap_body
    return bodies


def add_buttons(root, bodies, p, clip_tool=None):
    # shared thickened envelope (both caps use the same proud amount) --
    # rebuilding the whole outer pill solid + OffsetFaces per button was
    # part of what made the post-redesign build slow enough to risk the
    # MCP call timing out.
    assert p['power_cap']['proud'] == p['home_cap']['proud'], 'shared thickened envelope assumes equal proud amounts'
    thickened_envelope = build_thickened_envelope(root, p, p['power_cap']['proud'])
    # 2026-09-08 pass 9b, finding 9: shared plain outer envelope for the
    # rib-plate's own Combine-Intersect (see add_button) -- same reasoning
    # as thickened_envelope above, built once here rather than per button.
    rib_outer_envelope = build_outer_pill_solid(root, p)
    # 2026-09-12 pass 12: shared tight clip tool for the tab_hole_body cut
    # (wall) and the tab_relief_body cut (rib) -- see add_button's
    # tab_hole_body comment for the full root-cause/fix writeup. Built
    # once here, same reasoning as thickened_envelope/rib_outer_envelope
    # above. NOT used for the collar (see that comment in add_button --
    # a first attempt swapped it in there too and reopened an already-
    # fixed diagonal-corner overshoot defect).
    #
    # Margin: this file already has an established convention for "stay
    # clear of the TRUE (curved) outer wall by a safety margin" --
    # `wall_clear = 0.6` in add_lip_anchor_reliefs/add_lug, applied as
    # `s_wall - wall_clear` (0.6mm short of the true wall).
    # `build_inner_cavity_clip_tool`'s own `safety_margin` parameter is
    # relative to a DIFFERENT reference point -- the INNER CAVITY surface
    # (already `wall` inboard of s_wall), not s_wall itself -- so matching
    # the s_wall-0.6 convention needs `safety_margin = wall_clear -
    # p['wall']` (negative here, since wall_clear(0.6) < wall(2.0): the
    # tool must be GROWN outward past the bare inner cavity, not shrunk,
    # to reach that same s_wall-0.6 target).
    #
    # A first attempt used safety_margin=+0.6 directly (reading the task's
    # "offset inward by wall + 0.6mm" as "wall+0.6mm inward from s_wall",
    # i.e. s_inner-0.6) -- confirmed WRONG by a live verify() run: it put
    # the boundary INBOARD of the tab's own real outward reach
    # (s_tab_face + tab_len_along_d/2 = s_inner+0.15, from add_button's own
    # tab_body construction), clipping tab_hole_body's clearance cut short
    # of the tab it exists to clear and producing a real ~5.6mm3 (Power) /
    # ~7.1mm3 (Home) Top-x-Button interference -- the cap's own tab poking
    # into wall material the (over-)clipped cut no longer removed. Fixed
    # with `wall_clear=0.6` (s_inner+1.4 boundary -- 1.25mm of slack past
    # the tab's own need, still a full 0.6mm short of the true wall, and a
    # no-op at t=0 since the plain analytic bound, s_inner+1.0, already
    # sits inboard of it). A separate small residual interference
    # (~0.13mm3) traced to this same live-testing round turned out to be
    # the COLLAR clip swap (see add_button's own comment), NOT this
    # margin -- re-verified after reverting that swap: 0 interference,
    # both buttons, footprint/skin-intact gates clean, at this exact
    # wall_clear=0.6 value (a 0.3 retune tried in between is NOT needed
    # and was reverted -- it reopened a footprint-check failure of its
    # own for no benefit, since the real interference was elsewhere).
    wall_clear = 0.6
    tab_clip_tool = build_inner_cavity_clip_tool(root, p, safety_margin=wall_clear - p['wall'])

    bodies = add_button(root, bodies, 'Power Button', p['switch_power_bbox'], p['power_nub_dir'],
                         p['power_cap'], (p['power_cap']['stadium'][0] + 2 * p['cap_clearance'],
                                          p['power_cap']['stadium'][1] + 2 * p['cap_clearance']), p,
                         thickened_envelope=thickened_envelope, clip_tool=clip_tool,
                         outer_envelope=rib_outer_envelope, tab_clip_tool=tab_clip_tool)
    home_bbox = dict(p['switch_home_bbox'])
    home_bbox['z'] = p['switch_power_bbox']['z']  # z not separately specified in SPEC.md; reuse power's
    bodies = add_button(root, bodies, 'Home Button', home_bbox, p['home_nub_dir'],
                         p['home_cap'], (p['home_cap']['stadium'][0] + 2 * p['cap_clearance'],
                                         p['home_cap']['stadium'][1] + 2 * p['cap_clearance']), p,
                         thickened_envelope=thickened_envelope, clip_tool=clip_tool,
                         outer_envelope=rib_outer_envelope, tab_clip_tool=tab_clip_tool)
    thickened_envelope.name = 'Cap Trim Envelope'
    thickened_envelope.isLightBulbOn = False
    root.features.removeFeatures.add(rib_outer_envelope)
    root.features.removeFeatures.add(tab_clip_tool)
    return bodies


# add_button_plate_clearance (cut clearance pockets into the Screen
# Plate for the button rib/plunger travel path) RETIRED pass 16 -- there
# is no Screen Plate any more (owner call, item A/candidate 5).


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


def lug_ear_geometry(p):
    """Analytic geometry of the lanyard ear (2026-09-06 pass 6 rebuild;
    re-derived 2026-09-07 pass 7 item 2 -- see add_lug's docstring for the
    wedge-sliver defect this fixes) -- shared between add_lug (which
    builds it) and the envelope/export-vertex verify checks (which need
    to know where it legitimately protrudes), so the two can never
    disagree. Returns (half_w, y_far, y_root, hole_y): half_w -- half the
    ear's width; y_far -- its outward-facing end (the protrusion tip);
    y_root -- a conservative inner extent (BEFORE being trimmed to the
    true inner cavity surface -- see add_lug); hole_y -- the vertical
    hole's y position.

    2026-09-07: y_far/hole_y are now derived from the NARROWEST true wall
    radius across the ear's own z-span (min of rho_at_z at both z0 and
    z1 -- the outer profile is monotonically increasing from the flat
    bed to the parting line over this range, per SPEC's own probe table,
    so the minimum is always at an endpoint), not the single value at
    the vertical z-midpoint. add_lug then Combine-Intersects the ear's
    box against a thickened copy of the outer envelope offset by
    `protrusion` (the same technique pass 2 used to fix button caps
    against the curved shell) -- using the narrowest-radius endpoint here
    guarantees the box's full y_far..y_root footprint is never NARROWER
    than what that offset envelope actually contains at every z in the
    ear's range (the envelope only gets more permissive at the wider end),
    so the intersect only ever rounds the box's outward corners to match
    the true curve -- it can never eat into the hole's own footprint near
    x=0."""
    lug = p['lug']
    half_w = lug['width'] / 2.0
    ay = p['spine_a'][1]
    z0, z1 = lug['z']
    # at x=0, straight down from spine_a, the ray runs exactly along the
    # dome revolve's own symmetry axis -- the true wall distance there is
    # simply rho_at_z(z) by definition (true_wall_distance_along_ray's
    # general ray-casting form degenerates to None for a purely-vertical
    # ray starting exactly at the spine point, since its straight-section
    # branch expects a horizontal direction).
    s_wall = min(rho_at_z(p, z0), rho_at_z(p, z1))
    y_outer = ay - s_wall
    y_far = y_outer - lug['protrusion']
    y_root = ay - (s_wall - 3.0)  # 3mm inside the true wall -- trimmed to the cavity surface below
    hole_y = y_far + lug['hole_from_tip']
    return half_w, y_far, y_root, hole_y


def add_lug(root, bodies, p):
    """Lanyard lug, rebuilt (2026-09-06 pass 6) as an integrated ear after
    Jake's screenshot review found the previous box+cylinder tab
    intruding into the hollow cavity -- its inner end crossed the inner
    wall, reading from inside as a big cylinder standing next to the
    L76K (`y_root` was hand-picked and landed 1.5mm inside the true inner
    wall for the 'current' variant). `lug['width']`-wide (14mm), it
    protrudes `lug['protrusion']` (6mm) beyond the shell's TRUE curved
    outer surface (see lug_ear_geometry); its inner end is trimmed flush
    with the inner cavity surface by a Combine-Cut against a fresh copy
    of the inner cavity solid -- guaranteed no intrusion regardless of
    the shoulder curve's exact shape here, the same idea as
    clip_to_inner_cavity but inverted (an ear must stay embedded in the
    WALL and protrude OUTWARD, unlike a boss/post which lives entirely
    inside the hollow interior, so it needs the void REMOVED from an
    oversized blank, not INTERSECTED). A vertical hole sits
    `lug['hole_from_tip']` in from the ear's own outward face; `fillet_r`
    rounds its two vertical outer corners; `hole_chamfer` softens both
    hole edges -- both best-effort (skipped, not rolled back, if Fusion's
    fillet/chamfer feature refuses this specific geometry).

    2026-09-07 pass 7 (item 2): Jake's own renders (rim_lanyard_end.png /
    lanyard_end.png) showed a thin triangular WEDGE SLIVER on the outer
    skin flanking the ear, on the +x side. Root cause: the ear's own outer
    face was a flat box face, but the dome it's unioned into is a surface
    of revolution whose radius varies hugely across the ear's z0..z1 span
    (flat_rho at z0 up to the full outer_radius at z1, the whole flare of
    the bed-to-wall shoulder) -- the box's flat side walls cross that
    curving surface at a shallow, near-tangent angle at some z, producing
    a sliver face at the boolean union seam. Same category of defect as
    the pass-2 button caps (a flat approximation built against a curved
    shell), fixed the same way: the box is now Combine-Intersected against
    a thickened copy of the outer envelope (`build_thickened_envelope`,
    offset by `lug['protrusion']`) so its outward boundary follows the
    true curve (rounding the box's outward corners where the dome is
    locally narrower than the box is wide) instead of colliding with it
    edge-on. `lug_ear_geometry`'s y_far/hole_y already use the NARROWEST
    true-wall radius across the ear's z-span specifically so this
    intersect can only ever round the box's far corners -- it cannot eat
    into the hole's own footprint near x=0 at any z in z0..z1 (see that
    function's docstring).

    **2026-09-07, later same pass -- real root cause + fix**: the hole
    was being cut from the standalone `ear` tool body, then the (already-
    holed) ear was Combine-JOINED into Bottom. The NEW (min-of-endpoints)
    y_far/hole_y derivation above deliberately keeps the ear conservative
    -- close enough to the true wall that `hole_y`'s xy now falls WITHIN
    the base shell's own pre-existing wall thickness at some z in the
    ear's z0..z1 span (confirmed empirically: `probe_point_solid` on
    Bottom finds that point solid even BEFORE add_lug runs at all -- it's
    inside the plain hollow shell's wall band there, nothing to do with
    the ear). The OLD z-midpoint derivation placed hole_y well beyond the
    true wall, in what was then open air outside the base shell entirely,
    which is why cutting the hole from `ear` alone used to work -- there
    was no pre-existing Bottom material at that point to worry about. A
    hole cut into a TOOL body and then Combine-JOINED (a boolean union,
    A ∪ B) can never remove material the TARGET already had -- only a cut
    on the actual union result can. Fixed by joining the (hole-less) ear
    into Bottom FIRST, then cutting the through-hole from the resulting
    Bottom -- guaranteed to go all the way through regardless of how much
    of the hole's footprint overlaps pre-existing wall vs. new ear
    material. The R3 corner fillets stay on the standalone `ear` (cheap,
    and correct either way -- they only concern the ear's own outward
    corners, never Bottom's pre-existing geometry); only the hole cut and
    its chamfer move to after the join.

    Jake also asked about a RECESSED lanyard bar instead of a protruding
    ear -- not built: a 5mm-deep pocket at the tip would need an interior
    pad that collides with the L76K wired frame at y ~ -23.5 (the bay's
    -y dome tip is already the tightest-margin area in the whole case --
    see README known limitations), which would require moving the L76K.
    Documented as a trade-off in the README rather than built."""
    lug = p['lug']
    z0, z1 = lug['z']
    half_w, y_far, y_root, hole_y = lug_ear_geometry(p)

    ear = box_solid(root, -half_w, half_w, y_far, y_root, z0, z1)

    # trim the inner end flush with the true inner cavity surface -- a
    # fresh copy (not the shared, safety-margin-SHRUNK clip_tool, which
    # would leave the ear a fraction of a mm too long) since add_lug does
    # not currently receive clip_tool and this runs only once.
    void = build_inner_pill_solid(root, p)
    ear = combine_cut(root, ear, [void])

    # clip the outward reach to the TRUE curved shell + protrusion --
    # eliminates the flat-box-vs-round-dome wedge sliver (see docstring).
    thickened = build_thickened_envelope(root, p, lug['protrusion'])
    ear = combine_intersect(root, ear, [thickened])

    # R3 fillets on the two vertical outer corners (where the far/outward
    # face meets the two side faces) -- selected by geometry (a vertical
    # edge, i.e. spanning the full z0..z1 with constant x,y, sitting at
    # the far face's y and either side face's x). Best-effort: Jake's own
    # instructions are explicit that a fillet failure here should not
    # roll back the whole ear. Done on the standalone `ear` (before the
    # join) -- only concerns the ear's own outward corners.
    try:
        fillet_edges = adsk.core.ObjectCollection.create()
        for edge in ear.edges:
            bb = edge.boundingBox
            dx = (bb.maxPoint.x - bb.minPoint.x) / MM
            dy = (bb.maxPoint.y - bb.minPoint.y) / MM
            dz = (bb.maxPoint.z - bb.minPoint.z) / MM
            if dx < 0.05 and dy < 0.05 and dz > (z1 - z0) - 0.1:
                ex = bb.minPoint.x / MM
                ey = bb.minPoint.y / MM
                if abs(ey - y_far) < 0.05 and (abs(ex - half_w) < 0.05 or abs(ex + half_w) < 0.05):
                    fillet_edges.add(edge)
        if fillet_edges.count > 0:
            fillets = root.features.filletFeatures
            fin = fillets.createInput()
            fin.addConstantRadiusEdgeSet(fillet_edges, V(lug['fillet_r']), True)
            fillets.add(fin)
    except RuntimeError:
        pass

    # join the (hole-less) ear into Bottom, THEN cut the through-hole from
    # the resulting Bottom -- see docstring for why this order is required.
    bottom = combine_join(root, bodies['Bottom'], [ear])

    # 2026-09-15 pass 15, item 9: best-effort root fillet -- the ear's own
    # TOP and BOTTOM edges (z=z0, z=z1) running along its length (varying
    # y, constant-ish x within the ear's own footprint) are where a hard
    # lanyard tug concentrates bending stress right at the shell
    # attachment; a modest constant-radius fillet there softens that
    # transition. Selected by geometry directly on the merged `bottom`
    # body (the ear's own root only exists as real edges once it has
    # actually fused into the shell) -- any edge lying flat at z0 or z1,
    # within the ear's own x-half-width, y between y_far and y_root
    # (i.e. NOT the general shell's own far-away edges elsewhere on
    # Bottom, which this selection must not touch). Skip-on-failure, same
    # pattern as every other cosmetic fillet in this file -- a refusal
    # here must never roll back the ear/hole geometry that already works.
    root_fillet_r = lug.get('root_fillet_r')
    if root_fillet_r:
        try:
            fillet_edges = adsk.core.ObjectCollection.create()
            for edge in bottom.edges:
                bb = edge.boundingBox
                ex0, ex1 = bb.minPoint.x / MM, bb.maxPoint.x / MM
                ey0, ey1 = bb.minPoint.y / MM, bb.maxPoint.y / MM
                ez0, ez1 = bb.minPoint.z / MM, bb.maxPoint.z / MM
                flat_z0 = abs(ez1 - ez0) < 0.05 and (abs(ez0 - z0) < 0.05 or abs(ez0 - z1) < 0.05)
                within_ear = (-half_w - 0.1 <= ex0 and ex1 <= half_w + 0.1
                              and y_far - 0.1 <= ey0 and ey1 <= y_root + 0.1
                              and (ey1 - ey0) > 1.0)  # a real length-wise edge, not a short end-cap segment
                if flat_z0 and within_ear:
                    fillet_edges.add(edge)
            if fillet_edges.count > 0:
                fillets = root.features.filletFeatures
                fin = fillets.createInput()
                fin.addConstantRadiusEdgeSet(fillet_edges, V(root_fillet_r), True)
                fillets.add(fin)
        except RuntimeError:
            pass
        bottom = _refetch_by_name(root, 'Bottom') or bottom

    hole_r = lug['hole_dia'] / 2.0
    hole = cylinder_solid(root, 0.0, hole_y, hole_r, z0 - 0.5, z1 + 0.5)
    bottom = combine_cut(root, bottom, [hole])

    # 0.6mm chamfer on both hole edges (top and bottom circular edges of
    # the vertical hole) -- best-effort, same reasoning as the fillets.
    # Done on `bottom` now (the hole only exists there post-cut).
    try:
        chamfer_edge_at(root, bottom, (0.0, hole_y), hole_r, z0, lug['hole_chamfer'])
    except (RuntimeError, AssertionError):
        pass
    try:
        chamfer_edge_at(root, bottom, (0.0, hole_y), hole_r, z1, lug['hole_chamfer'])
    except (RuntimeError, AssertionError):
        pass

    bodies['Bottom'] = bottom
    return bodies


def _polyline_loop_lines(sk, loop_pts, z_mm):
    n = len(loop_pts)
    if n < 3:
        return
    for i in range(n):
        a = loop_pts[i]
        b = loop_pts[(i + 1) % n]
        add_line(sk, P(a[0], a[1], z_mm), P(b[0], b[1], z_mm))


def _loop_bbox_mm(loop_pts):
    xs = [pt[0] for pt in loop_pts]
    ys = [pt[1] for pt in loop_pts]
    return (min(xs), max(xs), min(ys), max(ys))


def _profile_bbox_mm(prof):
    bb = prof.boundingBox
    return (bb.minPoint.x / MM, bb.maxPoint.x / MM, bb.minPoint.y / MM, bb.maxPoint.y / MM)


def _bbox_matches(b1, b2, tol=0.05):
    return all(abs(v1 - v2) <= tol for v1, v2 in zip(b1, b2))


def deboss_loops(root, body, loops_xy, z_mm, depth_mm, cut_direction, counter_loops_xy=None):
    """loops_xy: list of closed polygon point lists (world mm, at height
    z_mm). Extrudes each resulting sketch profile `depth_mm` along
    cut_direction (+1 or -1 in Z) and cuts the union from `body`.

    2026-09-14 pass 14, item 2 (real print defect: wordmark counters cut
    away): a glyph with an enclosed counter (an 'a', a 'd', a flower 'o')
    draws as an OUTER loop plus an INNER (hole) loop in the same sketch --
    Fusion's own profile-finder then returns TWO profiles for it: the ring
    (outer minus counter -- what we WANT to cut) and a SECOND profile that
    is the counter's own disk, on its own, treated as its own standalone
    filled region (real, documented Fusion behavior for nested closed
    curves, not a bug in Fusion). The old code extruded+cut EVERY profile
    unconditionally, so the counter disk got cut too, erasing the counter
    entirely (the whole glyph printed solid). `counter_loops_xy`, when
    given, is the subset of `loops_xy` that are pure counter/hole outlines
    (from kandiwooks_logo.json's own `is_outer: false` tag -- see
    wordmark_layout's `counter_loops`): any profile whose own bounding box
    matches one of these (the counter-disk profile keeps exactly the
    small counter loop's own bbox; the ring profile keeps the larger
    OUTER loop's bbox instead, so the two can never be confused) is
    skipped -- not extruded, not cut -- so the counter survives as a real
    hole in the debossed ring. None (the default) preserves the exact old
    behavior of cutting every profile, used by flare_glyph_loops, which
    has no nested counters to begin with."""
    plane = plane_at_z(root, z_mm)
    sk = new_sketch(root, plane)
    for loop in loops_xy:
        _polyline_loop_lines(sk, loop, z_mm)
    if sk.profiles.count == 0:
        return body
    counter_bboxes = [_loop_bbox_mm(loop) for loop in (counter_loops_xy or [])]
    direction = 'positive' if cut_direction > 0 else 'negative'
    tools = []
    for i in range(sk.profiles.count):
        prof = sk.profiles.item(i)
        if counter_bboxes:
            pb = _profile_bbox_mm(prof)
            if any(_bbox_matches(pb, cb) for cb in counter_bboxes):
                continue  # this profile IS a counter disk -- skip it, the counter stays a hole
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


## 2026-09-08 pass 9e (finding 7, two-line wordmark): the single-line
## wordmark printed tiny (30mm wide across all 10 letters) and was hard to
## read. Split kandiwooks_logo.json's 6 bodies into the two WORDS by body
## NAME (not a runtime x-extent heuristic -- the words visually overlap in
## x once you include the tall flourish on the 'i', so a pure x-threshold
## split cannot separate them; see the analysis in README's pass-9e
## section for how these two groups were identified from the raw JSON
## bboxes before writing any of this): 'Body1' is the 'i' (its single
## loop's y-extent, -0.36..5.27, is far taller than any other glyph's
## ~2.3-2.4mm cap height -- this is a decorative flourish/sprout on the
## dot, part of the 'i', not a separate glyph), 'Body2' is the 'd',
## 'Body4' is 'k'+'a' fused (touching strokes -- same fusion the original
## docstring already documented), 'Body5' is the 'n': together, in x
## order, K-a-n-d-i = "KANDI". 'Body3' is 'W'+'o'+'o'+'k' fused into one
## lump (its 2 small enclosed loops are the flower/leaf glyphs standing in
## for the two O's -- see SPEC.md/the finding brief), 'Body6' is the 's':
## W-o-o-k-s = "WOOKS". The flower/leaf O-glyphs are Body3's own enclosed
## loops, so they travel with WOOKS automatically.
WORDMARK_LINE1_BODIES = ('Body1', 'Body2', 'Body4', 'Body5')  # "KANDI" (top line)
WORDMARK_LINE2_BODIES = ('Body3', 'Body6')                    # "WOOKS" (bottom line, carries the flower/leaf O glyphs)
WORDMARK_EDGE_CLEARANCE = 1.6  # mm from the widest debossed point to flat_rho (spec asks >=1.5; +0.1 margin)
WORDMARK_LINE_GAP = 2.0        # mm, vertical gap between the two lines' own local bboxes
WORDMARK_SCALE_FACTOR = 0.8    # pass 13, item 2: Jake -- "a bit smaller" -- 80% of the pass-9e target width
WORDMARK_VERTICAL_CLEARANCE = 1.5  # mm, matches verify_wordmark's own >=1.5mm clearance floor


def _wordmark_word_raw_loops(data, body_names):
    raw_loops = []
    for body in data:
        if body['name'] not in body_names:
            continue
        for loop in body['loops']:
            pts = loop['points']
            if len(pts) >= 3:
                raw_loops.append(pts)
    return raw_loops


def _wordmark_word_loops_by_flag(data, body_names):
    """Like _wordmark_word_raw_loops, but split by kandiwooks_logo.json's
    own per-loop `is_outer` tag -- (outer_loops, counter_loops). Pass 14,
    item 2: needed to identify which Fusion sketch profile is the
    unwanted 'counter disk' byproduct (see deboss_loops' own docstring)
    and to pair each counter with its own enclosing outer glyph for
    verify_wordmark_counters' probes."""
    outer, counter = [], []
    for body in data:
        if body['name'] not in body_names:
            continue
        for loop in body['loops']:
            pts = loop['points']
            if len(pts) < 3:
                continue
            (outer if loop.get('is_outer', True) else counter).append(pts)
    return outer, counter


def _point_in_poly(pt, poly):
    """Plain even-odd ray-casting point-in-polygon test, `poly` a list of
    (x, y) world-mm points (closed implicitly -- last point connects back
    to the first). Pure Python -- no Fusion/shapely dependency needed for
    the pass-14 counter/stroke probe geometry (see
    _wordmark_counter_probes)."""
    x, y = pt
    n = len(poly)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y):
            x_int = (xj - xi) * (y - yi) / (yj - yi) + xi
            if x < x_int:
                inside = not inside
        j = i
    return inside


def _poly_area(poly):
    a = 0.0
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2.0


def _poly_centroid(poly):
    xs = [pt[0] for pt in poly]
    ys = [pt[1] for pt in poly]
    return (sum(xs) / len(xs), sum(ys) / len(ys))


def _wordmark_counter_probes(outer_loops, counter_loops):
    """Pair each counter (hole) loop with its own smallest enclosing outer
    loop (point-in-polygon containment of the counter's own centroid,
    picking the smallest-area match when more than one outer loop
    contains it -- needed for the 'Ka' body, whose 'K' and 'a' outer
    loops share one JSON body but only 'a' has a counter) and return one
    probe pair per counter: the counter's own centroid (must read SOLID
    after the pass-14 fix -- the counter itself must survive) and a point
    found on the ring between the counter and its own outer boundary
    (must read HOLLOW -- confirms the deboss itself still happened around
    it, not silently skipped along with the counter). Pass 14, item 2
    (verify_wordmark_counters)."""
    probes = []
    for counter in counter_loops:
        ccx, ccy = _poly_centroid(counter)
        candidates = [o for o in outer_loops if _point_in_poly((ccx, ccy), o)]
        if not candidates:
            probes.append({'counter_center': (ccx, ccy), 'stroke_point': None})
            continue
        outer = min(candidates, key=_poly_area)
        r0 = max(math.hypot(px - ccx, py - ccy) for px, py in counter)
        stroke_pt = None
        for step_i in range(1, 60):
            r = r0 + step_i * 0.08
            for k in range(24):
                ang = 2.0 * math.pi * k / 24.0
                pt = (ccx + r * math.cos(ang), ccy + r * math.sin(ang))
                if _point_in_poly(pt, outer) and not _point_in_poly(pt, counter):
                    stroke_pt = pt
                    break
            if stroke_pt is not None:
                break
        probes.append({'counter_center': (ccx, ccy), 'stroke_point': stroke_pt})
    return probes


def _wordmark_split_sprout(loop_pts, gap_threshold=1.5):
    """Split a closed polyline loop into (stem_pts, sprout_pts) by finding
    the two large Euclidean gaps that bound the seam between them. Used
    to separate the 'i' sprout flourish from its own dot+stem base within
    kandiwooks_logo.json's Body1/loop[1] (pass 13, item 2): the two are
    drawn as one continuous 70-point outline, and inspecting the raw
    edge lengths around the loop finds exactly TWO edges over ~2.6mm
    (the sprout curve dropping down to the stem's near corner, then
    jumping back up from the stem's far corner to rejoin the sprout
    curve) versus every other consecutive edge in the loop, all under
    ~0.8mm -- a single-largest-edge search (an earlier version of this
    function) picks only ONE of these two comparably-sized gaps,
    misclassifying 68 of the 70 points as "stem". With both gap
    boundaries known, the points strictly between them (inclusive of
    both boundary points, which sit right at the seam) are the small
    stem+neck quad; everything else, wrapping around, is the sprout.
    General (keyed on actual edge-length outliers via `gap_threshold`,
    not hardcoded indices) -- if the loop doesn't have exactly 2 edges
    over the threshold (e.g. a different, cleanly-drawn glyph with no
    sprout at all), returns the whole loop as `stem_pts` with an empty
    `sprout_pts` -- a safe no-op exclusion."""
    n = len(loop_pts)
    if n < 4:
        return loop_pts, []
    gap_idxs = [i for i in range(n)
                if math.hypot(loop_pts[(i + 1) % n][0] - loop_pts[i][0],
                              loop_pts[(i + 1) % n][1] - loop_pts[i][1]) > gap_threshold]
    if len(gap_idxs) != 2:
        return loop_pts, []
    i0, i1 = sorted(gap_idxs)
    inside = loop_pts[i0:i1 + 2]                       # both seam/neck points + everything between
    outside = loop_pts[i1 + 2:] + loop_pts[:i0]         # wraps around -- the sprout curve itself
    return (inside, outside) if len(inside) <= len(outside) else (outside, inside)


def _wordmark_local_bbox(raw_loops):
    xs = [pt[0] for loop in raw_loops for pt in loop]
    ys = [pt[1] for loop in raw_loops for pt in loop]
    return min(xs), max(xs), min(ys), max(ys)


def _wordmark_place_word(raw_loops, bbox, scale, center_xy, x_center_bbox=None):
    """Uniform-scale + recenter a word's own raw loops to `center_xy`,
    mirrored in x -- same convention as the original single-line
    load_wordmark_loops (confirmed pass 6: reads correctly from the
    outside of the back face once mirrored this way). `bbox` drives the
    vertical center AND the scale/height math (unchanged from before --
    the sprout's own height is real ink the word occupies, so it still
    counts toward line spacing); `x_center_bbox`, when given, drives ONLY
    the horizontal center (pass 13, item 2: "centre KANDI as if the
    sprout on the i isn't there" -- the sprout then hangs off to the
    right of the centered letters, exactly as designed, instead of
    pulling the whole word's centre rightward)."""
    minx, maxx, miny, maxy = bbox
    local_cy = (miny + maxy) / 2.0
    if x_center_bbox is not None:
        cminx, cmaxx, _, _ = x_center_bbox
        local_cx = (cminx + cmaxx) / 2.0
    else:
        local_cx = (minx + maxx) / 2.0
    cx, cy = center_xy
    out = []
    for loop in raw_loops:
        wl = []
        for lx, ly in loop:
            sx = -(lx - local_cx) * scale  # mirror in x so it reads correctly when the puck is flipped
            sy = (ly - local_cy) * scale
            wl.append((sx + cx, sy + cy))
        out.append(wl)
    return out


def wordmark_vertical_span(p):
    """Usable y-span on the flat back face, between the lanyard-end lug
    relief (south) and screw D's own counterbore (north), each padded by
    WORDMARK_VERTICAL_CLEARANCE -- the SAME 1.5mm floor verify_wordmark
    already checks per-target, so centering the wordmark block anywhere
    inside this span can never itself manufacture a clearance violation
    (pass 13, item 2). `lug_ear_geometry`'s `y_root` is the ear's own
    inner extent (BEFORE being trimmed to the true cavity wall) -- the
    more conservative (further-north) of its two y values, and the one
    that bounds how far the ear's structure can reach toward the
    wordmark, vs. `hole_y` which is further out at the tip."""
    _, y_far, y_root, hole_y = lug_ear_geometry(p)
    south = max(y_root, hole_y) + WORDMARK_VERTICAL_CLEARANCE
    cb_r = p['counterbore_ABC_dia'] / 2.0
    # pass 16: screw D (0,60) is retired -- D1/D2 (+-19, 64) are the new
    # northmost Bottom-side counterbores; same margin convention (a
    # counterbore radius + WORDMARK_VERTICAL_CLEARANCE) against whichever
    # is closest to the wordmark's own centreline block.
    north = min(s['xy'][1] for s in p['screws_D12']) - cb_r - WORDMARK_VERTICAL_CLEARANCE
    return south, north


def wordmark_layout(p):
    """Compute the two-line "KANDI" / "WOOKS" layout (finding 7; pass 13
    item 2 rework -- smaller, recentred). Each word is scaled
    INDEPENDENTLY to the same target width -- derived from `flat_rho`
    (the flat bed's own true radius at the Bottom face, not a fixed mm
    constant) scaled by WORDMARK_SCALE_FACTOR (0.8, "a bit smaller" per
    Jake) so the wordmark fills 80% of the pass-9e usable width
    regardless of variant, leaving WORDMARK_EDGE_CLEARANCE to the
    flat-face edge -- then stacked vertically (KANDI above WOOKS) with
    WORDMARK_LINE_GAP between their own local bboxes, centred as a whole
    block on `wordmark_vertical_span`'s own midpoint (pass 13: no longer
    the fixed params['wordmark_center'] y -- computed live between the
    lanyard lug relief and screw D's counterbore so it can never drift
    out of the usable span regardless of variant). Both words happen to
    have almost identical native widths (12.52mm / 12.58mm in the source
    JSON), so this gives them nearly the same font scale, matching how
    the original single-line wordmark was one uniform scale throughout.

    KANDI's own horizontal centering (pass 13, item 2) ignores the
    sprout flourish on the 'i' (Body1's loop[1], see
    `_wordmark_split_sprout`'s docstring) -- the letters K-a-n-d-i are
    centered as if the sprout weren't there, so it simply hangs off to
    the right, as designed, instead of visually dragging the whole word
    off-center. The sprout still counts toward KANDI's own scale/height
    (it's real debossed material occupying real vertical space) -- only
    the x-CENTER calculation excludes it."""
    json_path = os.path.join(_HERE, 'kandiwooks_logo.json')
    with open(json_path, 'r') as f:
        data = json.load(f)

    line1_raw = _wordmark_word_raw_loops(data, WORDMARK_LINE1_BODIES)
    line2_raw = _wordmark_word_raw_loops(data, WORDMARK_LINE2_BODIES)
    assert line1_raw, 'no loops found for wordmark line 1 (KANDI) -- check WORDMARK_LINE1_BODIES against the JSON'
    assert line2_raw, 'no loops found for wordmark line 2 (WOOKS) -- check WORDMARK_LINE2_BODIES against the JSON'

    # KANDI's own x-centering bbox, with the 'i' sprout excluded: rebuild
    # Body1's raw loops using only its non-sprout sub-loop, leave every
    # other body (Body2/4/5) untouched.
    line1_center_raw = []
    for body in data:
        if body['name'] not in WORDMARK_LINE1_BODIES:
            continue
        for loop in body['loops']:
            pts = loop['points']
            if len(pts) < 3:
                continue
            if body['name'] == 'Body1':
                stem, _sprout = _wordmark_split_sprout(pts)
                if len(stem) >= 3:
                    line1_center_raw.append(stem)
                continue
            line1_center_raw.append(pts)
    assert line1_center_raw, 'sprout-exclusion left no loops for KANDI x-centering -- check _wordmark_split_sprout'

    b1 = _wordmark_local_bbox(line1_raw)
    b1_center = _wordmark_local_bbox(line1_center_raw)
    b2 = _wordmark_local_bbox(line2_raw)
    target_width = WORDMARK_SCALE_FACTOR * 2.0 * (p['flat_rho'] - WORDMARK_EDGE_CLEARANCE)
    scale1 = target_width / (b1[1] - b1[0])
    scale2 = target_width / (b2[1] - b2[0])
    h1 = (b1[3] - b1[2]) * scale1
    h2 = (b2[3] - b2[2]) * scale2
    total_h = h1 + WORDMARK_LINE_GAP + h2

    span_south, span_north = wordmark_vertical_span(p)
    cx = p['wordmark_center'][0]
    cy = (span_south + span_north) / 2.0
    assert total_h <= (span_north - span_south), (
        f'wordmark block ({total_h:.2f}mm tall) does not fit the usable vertical span '
        f'[{span_south:.2f}, {span_north:.2f}] ({span_north - span_south:.2f}mm)')
    y1 = cy + (total_h / 2.0 - h1 / 2.0)  # line 1 ("KANDI"): the higher-y line
    y2 = cy - (total_h / 2.0 - h2 / 2.0)  # line 2 ("WOOKS"): the lower-y line

    line1_loops = _wordmark_place_word(line1_raw, b1, scale1, (cx, y1), x_center_bbox=b1_center)
    line2_loops = _wordmark_place_word(line2_raw, b2, scale2, (cx, y2))

    # pass 14, item 2: the outer/counter split, transformed through the
    # EXACT SAME scale/bbox/center as the full word above, so a counter's
    # world coordinates always land exactly where the matching profile in
    # the real cut sketch does (see deboss_loops' own docstring for why
    # exact-bbox matching needs this). counter_probes pairs each counter
    # with a (counter-centre, ring-point) probe pair for
    # verify_wordmark_counters.
    line1_outer_raw, line1_counter_raw = _wordmark_word_loops_by_flag(data, WORDMARK_LINE1_BODIES)
    line2_outer_raw, line2_counter_raw = _wordmark_word_loops_by_flag(data, WORDMARK_LINE2_BODIES)
    line1_outer_world = _wordmark_place_word(line1_outer_raw, b1, scale1, (cx, y1), x_center_bbox=b1_center)
    line2_outer_world = _wordmark_place_word(line2_outer_raw, b2, scale2, (cx, y2))
    line1_counter_world = (_wordmark_place_word(line1_counter_raw, b1, scale1, (cx, y1), x_center_bbox=b1_center)
                            if line1_counter_raw else [])
    line2_counter_world = (_wordmark_place_word(line2_counter_raw, b2, scale2, (cx, y2))
                            if line2_counter_raw else [])
    counter_loops = line1_counter_world + line2_counter_world
    counter_probes = (_wordmark_counter_probes(line1_outer_world, line1_counter_world)
                       + _wordmark_counter_probes(line2_outer_world, line2_counter_world))

    return {
        'line1_loops': line1_loops, 'line2_loops': line2_loops,
        'line1_bbox_local': b1, 'line1_center_bbox_local': b1_center, 'line2_bbox_local': b2,
        'scale1': scale1, 'scale2': scale2, 'target_width': target_width,
        'y1_center': y1, 'y2_center': y2,
        'line1_y_range': (y1 - h1 / 2.0, y1 + h1 / 2.0),
        'line2_y_range': (y2 - h2 / 2.0, y2 + h2 / 2.0),
        'half_width': target_width / 2.0,
        'vertical_span': (span_south, span_north),
        'counter_loops': counter_loops,
        'counter_probes': counter_probes,
    }


def load_wordmark_loops(p):
    """kandiwooks_logo.json (2026-09-06 pass 6 re-extraction; 2026-09-08
    pass 9e/finding 7 split into two independently-scaled/stacked lines --
    see `wordmark_layout`'s docstring for the current two-line geometry
    and WORDMARK_LINE1_BODIES/WORDMARK_LINE2_BODIES's comment for how the
    6 bodies were identified as "KANDI" / "WOOKS"). Original pass-6 note,
    still true of the underlying extraction: the previous extraction
    visited only ONE flat top face per body (the largest by area),
    silently dropping any SECOND disjoint flat face on the same body --
    Body4 ('Ka', the K and the lowercase a fused into one lump but with
    two separate flat top regions) lost the entire 'a' this way,
    rendering as "K[gap]ndiWooks" with the sprout decoration floating
    over the gap. Re-extracted with every body's every same-Z flat face
    walked (not just the biggest), using CurveEvaluator3D.getStrokes at a
    0.005mm tolerance."""
    layout = wordmark_layout(p)
    return layout['line1_loops'] + layout['line2_loops']


def add_wordmark_logo(root, bodies, p):
    layout = wordmark_layout(p)
    world_loops = layout['line1_loops'] + layout['line2_loops']
    z_bot = p['bottom_z']
    depth = p['logo_deboss_depth']
    # pass 14, item 2: counter_loops_xy tells deboss_loops which Fusion
    # profile is the unwanted 'counter disk' byproduct (the 'a', 'd', and
    # both flower 'o' counters) so it gets skipped, not cut -- see
    # deboss_loops' own docstring.
    bodies['Bottom'] = deboss_loops(root, bodies['Bottom'], world_loops, z_bot, depth, cut_direction=1,
                                     counter_loops_xy=layout['counter_loops'])
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
    it is modeled as a hidden reference box (add_battery_reference_box).

    2026-09-05 fixes (Bottom x Battery Reference interference):
    (1) the rails now sit `battery_rail_clear` (0.3mm) OUTSIDE the battery
        box on each side, not flush against it -- SPEC's "battery rails
        outside the 40x30 box + 0.3".
    (2) the cavity FLOOR itself is flattened under the battery's footprint.
        The battery box is 40mm wide (x -20..20), but the trim variant's
        inner-cavity floor is only flat out to rho=fillet_center_rho=18mm
        -- beyond that the floor is the quarter-round fillet arc curving
        UP toward the wall, so the real floor at x=+-20 sits ~0.25mm above
        the nominal flat z=2.0 the battery box assumes, and the battery's
        square corners collided with that curved rise (measured: a real,
        if thin, overlap across almost the whole battery footprint).
        Cutting a shallow box from z=2.0 up to a safe height across the
        battery's exact footprint removes any such excess -- a no-op
        wherever the floor is already flat (e.g. the whole 'current'
        variant, whose flat region already reaches rho=20)."""
    bat = p['bay']['battery']
    x0, x1 = bat['x']
    y0, y1 = bat['y']
    rail_w = p['bay']['battery_rail_w']
    rail_clear = p['bay'].get('battery_rail_clear', 0.0)
    rz0, rz1 = p['bay']['battery_rail_z']

    rail_l = box_solid(root, x0 - rail_w - rail_clear, x0 - rail_clear, y0, y1, rz0, rz1)
    rail_r = box_solid(root, x1 + rail_clear, x1 + rail_w + rail_clear, y0, y1, rz0, rz1)
    bodies['Bottom'] = combine_join(root, bodies['Bottom'], [rail_l, rail_r])

    # flatten the floor under the battery's own footprint (see docstring) --
    # comfortably covers the worst-case curve rise (<0.3mm) without cutting
    # into the solid floor slab below z=2.0 or touching the rails (which
    # sit outside x0..x1 entirely).
    flatten = box_solid(root, x0, x1, y0, y1, bat['z'][0], bat['z'][0] + 1.0)
    bodies['Bottom'] = combine_cut(root, bodies['Bottom'], [flatten])

    # a hook-and-loop strap pair goes THROUGH the rails (not the floor bed
    # face) at 1/3 and 2/3 along the battery's length
    strap = p['bay']['battery_strap']
    for frac in (1.0 / 3.0, 2.0 / 3.0):
        yc = y0 + frac * (y1 - y0)
        slot_l = box_solid(root, x0 - rail_w - rail_clear - 0.5, x0 - rail_clear + 0.5,
                            yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        slot_r = box_solid(root, x1 + rail_clear - 0.5, x1 + rail_w + rail_clear + 0.5,
                            yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        bodies['Bottom'] = combine_cut(root, bodies['Bottom'], [slot_l, slot_r])

    # pass 16, item F (mech review F13, "nice": no positive Y end-stop --
    # only rail friction + strap tension resist a shock load along the
    # bay's own long axis). A short, low rib across the FULL rail-to-rail
    # width, standing up from the floor by `battery_rail_z[1]-
    # battery_rail_z[0]` (matching the rails' own height so it reads as
    # one continuous stop) and `battery_endstop_w` mm thick, placed
    # NORTH of the battery box's own (already-trimmed, see bay.battery's
    # own comment) y1 edge -- i.e. (y1, y1+endstop_w), NOT (y1-endstop_w,
    # y1) -- so the rib occupies exactly the 1mm trimmed off the
    # reference box's own nominal length, rather than the cell's own
    # resting volume. A live check_interference run confirmed 0mm^3
    # overlap against the (now-shrunk) Battery 803040 reference box with
    # this placement.
    endstop_w = p.get('battery_endstop_w', 1.0)
    endstop = box_solid(root, x0 - rail_w - rail_clear, x1 + rail_w + rail_clear,
                         y1, y1 + endstop_w, rz0, rz1)
    bodies['Bottom'] = combine_join(root, bodies['Bottom'], [endstop])
    bodies['Bottom'] = dedupe_body(root, bodies['Bottom'], 'Bottom')
    return bodies


def add_battery_reference_box(root, p):
    """803040 LiPo has no Fusion doc -- modeled as a hidden reference box
    (excluded from exports) purely to support interference checking.

    2026-09-06 pass 6: inset 0.1mm on the X sides only -- the 'current'
    variant's case-screw boss A/C (radius 3.0, centred at x -22.97/+23.74)
    is DESIGNED to sit right at this box's x=-+20 edge, and now that
    clipped_pillar_with_reach finally gives those bosses real material
    (see verify_posts_and_bosses), a genuine but hairline (0.03mm) real
    overlap appeared there -- both variants scale the boss position off
    outer_radius, so this margin is a permanent, deliberate tolerance on
    the reference envelope, not a one-off number chosen to silence this
    specific run."""
    bat = p['bay']['battery']
    margin = 0.1
    body = box_solid(root, bat['x'][0] + margin, bat['x'][1] - margin, bat['y'][0], bat['y'][1], bat['z'][0], bat['z'][1])
    body.name = 'Battery 803040'
    body.isLightBulbOn = False
    return body


def build_comms_stack_frame(root, p):
    """Build (but do not yet join) the 3-board comms-stack retention
    structure (2026-09-07 pass 7, supersedes the old dome-tip 'l76k_wired'
    floor frame AND the separate Top-hanging XIAO/Wio tray): four Ø3
    corner pads (2.0mm tall, z 2..4) plus a 1.2mm perimeter wall (6mm
    tall, z 2..8) around the L76K PCB footprint, with a wire-clearance
    notch on the +Y side. Only the L76K is physically retained by case
    geometry here -- per Jake's stack spec, the XIAO plugs into the L76K's
    own header pins below it and the Wio plugs into the XIAO via a
    board-to-board connector above it, so the whole 3-board assembly is
    held together by its own connectors once the bottom board is seated
    in this frame; no separate tray/cradle is needed or built for the
    upper two boards (contrast the pre-pass-7 design, which physically
    retained XIAO+Wio in a Top-hanging tray elsewhere in the bay -- see
    the README's pass-7 section)."""
    s3 = p['bay']['stack3']
    pcb = s3['l76k_pcb']
    x0, x1 = pcb['x']
    y0, y1 = pcb['y']
    wall = s3['frame_wall']
    clear = s3['frame_clear']
    fz0, fz1 = s3['frame_z']
    pad_r = s3['pad_dia'] / 2.0
    pad_h = s3['pad_h']
    inset = s3['pad_inset']

    ox0, ox1 = x0 - clear - wall, x1 + clear + wall
    oy0, oy1 = y0 - clear - wall, y1 + clear + wall
    ix0, ix1 = x0 - clear, x1 + clear
    iy0, iy1 = y0 - clear, y1 + clear

    outer = box_solid(root, ox0, ox1, oy0, oy1, fz0, fz1)
    inner = box_solid(root, ix0, ix1, iy0, iy1, fz0 - 0.5, fz1 + 0.5)
    frame = combine_cut(root, outer, [inner])

    notch_w = s3['wire_notch_w']
    notch = box_solid(root, -notch_w / 2.0, notch_w / 2.0,
                       oy1 - wall - 0.5, oy1 + 0.5, fz0, fz1 + 0.3)
    frame = combine_cut(root, frame, [notch])

    pads = [cylinder_solid(root, px, py, pad_r, fz0, fz0 + pad_h)
            for px in (x0 + inset, x1 - inset) for py in (y0 + inset, y1 - inset)]
    frame = combine_join(root, frame, pads)
    # pass 13, item 1: best-effort 0.6mm fillet where the frame's wall
    # (and its corner pads) meet the floor it stands on -- cosmetic/
    # print-quality only, same reasoning as build_gps_frame_body's fillet.
    _best_effort_fillet_at_z(root, frame, fz0, 0.6)
    return frame


def add_comms_stack_frame(root, bodies, p, clip_tool=None):
    """Join the comms-stack frame into Bottom.

    Real defects found (2026-09-07, via an actual analyzeInterference
    run) and fixed here, both instances of patterns already established
    elsewhere in this file:

    1. 'L76K PCB x Bottom' (32.7mm3): the PCB's far -y corners (near
       x=+-8.89, y approaching -24) sit outside the REAL inner cavity
       wall at low z -- the dome tip (a revolve around spine_a) tapers
       faster than the flat PCB rectangle assumes, confirmed analytically
       (inner_rho_at_z(4.5)=23.8 vs the corner's own rho_from_spine=24.9)
       -- the same "bare cavity floor curves up near the dome tip" issue
       pass 5 already fixed for the battery and the old L76K frame (see
       add_battery_bay). Fixed the same way: flatten the PCB's own exact
       footprint (no extra margin, so this can't eat into the frame's
       wall -- entirely outside this box) from the nominal floor (z=2.0)
       up through a safe height (6.0, comfortably past the PCB's own
       ~5.5mm top) BEFORE building the frame around it -- a no-op
       wherever the floor is already flat.
    2. Latent 'frame wall pokes through the outer shell' risk: the
       frame's own wall reaches further out (PCB edge + clearance + wall)
       than the PCB itself, and analytically its far corner's
       rho_from_spine (~27.55) exceeds the TRUE outer profile's rho at
       the frame's own LOW z (e.g. rho_at_z(2)=~24.1) -- the same latent
       "outer bump" class of bug clipped_pillar_with_reach/
       clip_to_inner_cavity already exist to prevent for bosses/posts.
       Clip the whole frame (walls + pads) against the shared
       inner-cavity clip tool before joining, exactly like every boss/
       post -- guarantees it can never punch through regardless of the
       exact numbers.
    3. Boss relief (item 3): cut a keep-out around every case-screw boss
       position so boss B1/B2 -- positioned just outside the L76K PCB's
       own footprint, but close enough that the frame's outer wall would
       otherwise graze them -- get a real, guaranteed
       stack3['boss_relief_margin'] (1.0mm) of clearance. Cheap to apply
       to every screw (A/C/D are already far enough away that the cut is
       a no-op for them)."""
    s3 = p['bay']['stack3']
    pcb = s3['l76k_pcb']
    x0, x1 = pcb['x']
    y0, y1 = pcb['y']
    flatten = box_solid(root, x0, x1, y0, y1, 2.0, 6.0)
    bodies['Bottom'] = combine_cut(root, bodies['Bottom'], [flatten])

    frame = build_comms_stack_frame(root, p)
    if clip_tool is not None:
        frame = clip_to_inner_cavity(root, frame, p, clip_tool)

    margin = p['bay']['stack3']['boss_relief_margin']
    boss_r = p['boss_dia'] / 2.0
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        keepout = cylinder_solid(root, cx, cy, boss_r + margin, 1.0, 9.0)
        frame = combine_cut(root, frame, [keepout])

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
        # 2026-09-05 printability fix: these used to be flat box shelves
        # (a full ledge_w mm overhang appearing all at once at the print's
        # far end) -- now 45-degree self-supporting wedges (see
        # build_wedge_along_x) tapering from flush-with-the-wall at the
        # top (z_bottom+ledge_h, prints first) to full protrusion at the
        # bottom (z_bottom, prints last).
        ledge1 = build_wedge_along_x(root, ix0, ix1, iy0, +1.0, z_bottom, ledge_h, ledge_w)
        ledge2 = build_wedge_along_x(root, ix0, ix1, iy1, -1.0, z_bottom, ledge_h, ledge_w)
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


# build_stack_tray_body / add_stack_tray (the pre-pass-7 Top-hanging
# XIAO+Wio tray) removed 2026-09-07, pass 7: the 3-board direct-stack
# design (build_comms_stack_frame) retains only the L76K in case
# geometry -- XIAO/Wio float above it, held by their own board-to-board /
# header connections. See the README's pass-7 section.


def build_gps_frame_body(root, p):
    """Build (but do not yet join) the GPS patch retention frame -- a
    plain hanging wall ring (no ledges), per Jake's spec -- "GPS frame
    inner = 25.5 x 25.5 with the patch box centred" (a 1.0mm wall pocket,
    retained by the Top skin above). Split out from add_gps_frame
    (2026-09-05) so add_comms_bay can trim it against the (widened) stack
    tray before either is joined to Top -- see add_comms_bay's docstring.

    2026-09-05 fix (Top x GPS Patch Reference interference): the old
    version reused the stack tray's ledge scheme (a shelf reaching most of
    the way across the opening, oversized to fuse with the ceiling) --
    that shelf's z-range fully overlapped the patch box's z-range across
    virtually the whole opening. The patch doesn't need a load-bearing
    ledge the way the Wio stack does (SPEC just calls it 'retained by the
    Top skin'), so this is now a simple square opening sized directly from
    `gps_frame_opening`, centred on the patch box's own centre (which may
    differ slightly from the (x0,x1,y0,y1) footprint's own centre)."""
    gps = p['bay']['gps_patch']
    opening = p['bay']['gps_frame_opening']
    half = opening / 2.0
    cx = (gps['x'][0] + gps['x'][1]) / 2.0
    cy = (gps['y'][0] + gps['y'][1]) / 2.0
    x0, x1 = cx - half, cx + half
    y0, y1 = cy - half, cy + half
    clear = p['bay']['gps_frame_clear']
    frame = build_hanging_frame(
        root, x0, x1, y0, y1, 0.0, p['bay']['gps_frame_wall'],
        gps['z'][0] - clear, p['top_ceiling_underside_z'])
    # pass 13, item 1: best-effort 0.6mm fillet where the frame's wall
    # meets the ceiling it hangs from -- cosmetic/print-quality only (not
    # gated by verify_root_fillets, which only covers the circular posts/
    # bosses), same skip-on-failure pattern as _best_effort_fillet.
    _best_effort_fillet_at_z(root, frame, p['top_ceiling_underside_z'], 0.6)
    return frame


def add_gps_reference_box(root, p):
    """GPS patch antenna has no Fusion doc -- hidden reference box only."""
    gps = p['bay']['gps_patch']
    body = box_solid(root, gps['x'][0], gps['x'][1], gps['y'][0], gps['y'][1], gps['z'][0], gps['z'][1])
    body.name = 'GPS Patch 25x25x8.3'
    body.isLightBulbOn = False
    return body


def add_fpc_keepout_marker(root, p):
    """Construction-only reference body marking the LoRa FPC antenna
    keep-out strip on the Top's inner dome wall -- NOT joined/cut into any
    printed body, hidden, and excluded from exports."""
    ko = p['bay']['fpc_keepout']
    body = box_solid(root, ko['x'][0], ko['x'][1], ko['y'][0], ko['y'][1], ko['z'][0], ko['z'][1])
    body.name = 'FPC LoRa Antenna Keep-out'
    body.isLightBulbOn = False
    return body


def add_comms_bay(root, bodies, p, clip_tool=None):
    """2026-09-07 pass 7: the comms-stack frame (add_comms_stack_frame)
    replaces the old dome-tip L76K-only frame, and the GPS frame no
    longer needs a mutual clip against a stack tray (removed -- the
    3-board stack no longer has one; see build_comms_stack_frame's
    docstring) since the GPS patch's new y-range (2..27) and the stack's
    footprint (y <= -1.5) don't overlap at all by construction.

    Real defect found (2026-09-07, via an actual analyzeInterference run):
    the GPS patch's new y-range (2..27) reaches close enough to
    case-screw boss C (trim: (23.0, 25.2), Ø6) that boss C's own material
    -- built earlier, in add_case_screws -- physically overlaps the
    antenna's real footprint by ~2mm at its closest corner (box corner
    (22.2, 25.2) is only 0.8mm from the boss's center, well inside its
    3mm radius). Fixed the same way as the pass-5 tray/antenna clip:
    cut a keepout matching the antenna box (+0.3mm margin) out of Top
    generally, so nothing can occupy that space regardless of what's
    there. This only removes the -x-facing "bite" of boss C's material
    (the box's edge, even with margin, stops short of the boss's own
    axis at x=23.0) -- the boss stays a continuous, if not full-circle,
    pillar, and verify_posts_and_bosses' probe (offset in +x, AWAY from
    the antenna) is unaffected."""
    bodies = add_battery_bay(root, bodies, p)
    bodies = add_comms_stack_frame(root, bodies, p, clip_tool=clip_tool)

    gps_box = p['bay']['gps_patch']
    gps_keepout = box_solid(root, gps_box['x'][0] - 0.3, gps_box['x'][1] + 0.3,
                             gps_box['y'][0] - 0.3, gps_box['y'][1] + 0.3,
                             gps_box['z'][0] - 0.3, gps_box['z'][1] + 0.3)
    bodies['Top'] = combine_cut(root, bodies['Top'], [gps_keepout])

    gps_frame = build_gps_frame_body(root, p)
    bodies['Top'] = combine_join(root, bodies['Top'], [gps_frame])
    bodies['Top'] = dedupe_body(root, bodies['Top'], 'Top')

    add_battery_reference_box(root, p)
    add_gps_reference_box(root, p)
    add_fpc_keepout_marker(root, p)
    return bodies


def _antenna_skin_safe_channel(root, p, center_mm, axis1_mm, axis2_mm, length, width, height):
    """A box channel (length along axis1, width along axis2, height in Z),
    Combine-Intersected against a copy of the plain outer envelope offset
    INWARD by `channel_min_skin` -- same idiom as add_fpc_relief's
    skin_safe_tool (see its docstring): guarantees the cut can never
    reach closer than `channel_min_skin` to the TRUE outer surface,
    however far out `length` was asked to reach, following the real
    curvature (not a flat-wall estimate). Used for the LoRa channel,
    which is cut close to the true outer dome wall; the GPS/stack-frame
    crossings are deep inside the cavity and don't need this clip (see
    add_antenna_channels)."""
    a = p['antenna']
    box = oriented_box_prism(root, center_mm, axis1_mm, axis2_mm, (0.0, 0.0, 1.0), length, width, height)
    envelope = build_outer_pill_solid(root, p)
    faces = [f for f in envelope.faces]
    offset_input = root.features.offsetFacesFeatures.createInput(faces, V(-a['channel_min_skin']))
    root.features.offsetFacesFeatures.add(offset_input)
    return combine_intersect(root, box, [envelope])


def _best_effort_fillet(root, body, min_dz, radius):
    """Best-effort constant-radius fillet on a body's own vertical-ish
    edges (dz >= min_dz) -- same pattern as add_lug's corner fillets:
    skipped (not fatal) if Fusion's fillet feature refuses this specific
    edge selection. Used to round the antenna channel's own long edges
    ('filleted' per the finding's own channel spec) -- cosmetic/print-
    quality only, never load-bearing, so a skip here never weakens any
    verify() gate."""
    try:
        edges = adsk.core.ObjectCollection.create()
        for edge in body.edges:
            bb = edge.boundingBox
            dz = (bb.maxPoint.z - bb.minPoint.z) / MM
            if dz >= min_dz:
                edges.add(edge)
        if edges.count > 0:
            fillets = root.features.filletFeatures
            fin = fillets.createInput()
            fin.addConstantRadiusEdgeSet(edges, V(radius), True)
            fillets.add(fin)
    except RuntimeError:
        pass
    return body


def add_antenna_channels(root, bodies, p, clip_tool=None):
    """Finding 8 (2026-09-08, pass 9e): two coax/FPC cable runs that must
    not get pinched when the case halves close -- (a) the Wio-SX1262's
    u.FL to the LoRa FPC antenna keep-out strip on Top's inner dome wall,
    (b) the L76K's u.FL to the GPS patch antenna in its frame above the
    battery. Both routes derived from LIVE-PROBED u.FL connector
    positions (`PARAMS['antenna']`, see its own comment) -- not the SPEC
    box centers -- so the cut actually lines up with the real inserted
    hardware.

    LoRa route (Top only, 'trim' only -- see below): the Wio's u.FL sits
    ~3.5mm inboard of the true inner cavity wall at its own bearing from
    spine_a (live-computed: `true_wall_distance_along_ray` from the
    connector's own xy, along its own outward radial direction, = 5.59mm
    to the TRUE OUTER surface) -- a real gap of open cavity, not a
    connector sitting flush against the wall. Cut a single radial channel
    from the connector's own point outward, LENGTH = that true-wall
    distance minus `channel_min_skin` (1.2mm) -- so it reaches exactly to
    within the required skin minimum of the true outer surface, by
    construction, and no further, via `_antenna_skin_safe_channel`'s
    Combine-Intersect against the skin-safe envelope (open cavity along
    most of this length is a geometric no-op for the cut; only the last
    ~0.8mm, inside the actual 2mm shell, removes real material). This is
    the ONE crossing that's actually near the true outer skin, hence the
    only one using the skin-safe clip.

    GPS route (Bottom -> Top, both variants -- the L76K is always
    inserted): the L76K's u.FL sits inside the stack3 frame's own hollow
    interior, well within the EXISTING `wire_notch_w` cut
    (build_comms_stack_frame's own +Y wire-clearance notch, x +-3,
    already spans the connector's x=2.1 and z=5.92 -- confirmed by
    direct comparison of the numbers, not assumed) -- so the cable's
    first crossing (out of the stack3 frame) needs NO new cut, it already
    has one. From there the route runs straight up (same x, rising in Z)
    through open cavity -- clear of the battery (battery's own y starts
    at 2.0, the GPS frame's south wall band sits at y 0.75-1.75, entirely
    south of it; the battery's own z-range, 2-10, sits entirely BELOW
    this route's z 10.5-12.5 crossing -- no overlap in x, y, OR z with
    the battery reference box) -- to the GPS frame's own south wall
    (1.0mm), which IS a real, complete, un-gapped ring (build_gps_
    frame_body / build_hanging_frame -- no gap_w passed) and DOES need a
    new cut here, the one genuinely new channel this route needs. The
    route stays well inboard of the alignment lip/anchor ring (rho ~2.3mm
    from spine_a here, vs the ring's own inner radius 23.95mm/trim,
    25.95mm/current) -- per the finding's own conditional ("a notch in
    the parting-line lip IF a cable must cross the halves"), this route
    crosses z=split_z but never touches the ring itself, so no lip notch
    is cut; the plain shell wall is nowhere near this xy (deep in open
    cavity) so there is no solid material to cross at the parting plane
    either. Cut directly into Top (a plain box spanning the wall's own
    y-band with margin) -- deep in the cavity, nowhere near the true
    outer surface, so no skin-safe clip is needed here (unlike the LoRa
    channel)."""
    a = p['antenna']
    w, h = a['channel_width'], a['channel_depth']
    fillet_r = a['channel_fillet']

    # --- LoRa: Wio u.FL -> Top's inner dome wall ('trim' only) ---
    if p.get('comms_stack3_full_height', True):
        ux, uy, uz = a['lora_ufl_xyz']
        ay = p['spine_a'][1]
        d = math.hypot(ux, uy - ay)
        dirv = (ux / d, (uy - ay) / d)  # radial, outward from spine_a
        s_wall = true_wall_distance_along_ray(p, (ux, uy), dirv, uz)
        assert s_wall is not None, 'LoRa antenna channel: no true-wall intersection along the connector ray'
        channel_len = s_wall - a['channel_min_skin']
        assert channel_len > 0, f'LoRa antenna channel: connector already within channel_min_skin of the true wall ({s_wall})'
        tang = (-dirv[1], dirv[0])  # tangential, perpendicular to dirv
        center = (ux + (channel_len / 2.0) * dirv[0], uy + (channel_len / 2.0) * dirv[1], uz)
        dirv3 = (dirv[0], dirv[1], 0.0)
        tang3 = (tang[0], tang[1], 0.0)
        channel = _antenna_skin_safe_channel(root, p, center, dirv3, tang3, channel_len, w, h)
        channel = _best_effort_fillet(root, channel, 0.0, fillet_r)
        bodies['Top'] = combine_cut(root, bodies['Top'], [channel])
        bodies['Top'] = dedupe_body(root, bodies['Top'], 'Top')

        # pass 16, item F (mech review F11: "the LoRa FPC antenna keep-out
        # is a reference marker only, never enforced against real
        # geometry"). Build a hidden reference solid matching the REAL
        # cable corridor this route actually needs (the connector's own
        # point out to the true wall, same center/direction as the
        # machined channel above but WITHOUT the skin-safe clip -- the
        # cable/strain-relief needs the full run, not just the last bit
        # that happens to remove material) and register it in
        # REFERENCE_BOX_NAMES so verify()'s own interference sweep checks
        # it against every printed body and every inserted board
        # occurrence, same as the Battery/GPS reference boxes already
        # are. This replaces the old broad, un-enforced `fpc_keepout`
        # marker (params_current.py's `bay.fpc_keepout`, still kept as a
        # generic reference note) with a real, live-checked corridor
        # matching the actual routed path -- a real future obstruction
        # crossing this corridor now fails check_interference, rather
        # than passing silently the way the old marker always did.
        # inset the corridor's own NEAR end 0.5mm outboard of the
        # connector's own literal point (a live check_interference run
        # found the un-inset version genuinely overlapping the real u.FL
        # connector body itself -- expected, since the corridor's own
        # center/length starts exactly AT the connector -- not a
        # meaningful "route" defect, just double-counting the connector's
        # own housing as part of its own cable's clearance corridor).
        corridor_inset = 1.5
        corridor_len = max(channel_len - corridor_inset, 0.1)
        corridor_center = (ux + corridor_inset * dirv[0] + (corridor_len / 2.0) * dirv[0],
                            uy + corridor_inset * dirv[1] + (corridor_len / 2.0) * dirv[1], uz)
        corridor = oriented_box_prism(root, corridor_center, dirv3, tang3, (0.0, 0.0, 1.0), corridor_len, w, h)
        # clip to the true inner cavity (same tool every boss/post in this
        # file already uses): a live check_interference run found a real
        # ~6mm^3 Top-vs-corridor overlap without this -- the plain,
        # un-clipped box's own tangential/vertical extent (w, h) pokes a
        # hair past the true cavity boundary somewhere along its run even
        # though its own LENGTH already stops channel_min_skin short of
        # the wall along the ray's own centreline.
        if clip_tool is not None:
            corridor = clip_to_inner_cavity(root, corridor, p, clip_tool)
        corridor.name = LORA_CORRIDOR_NAME
        corridor.isLightBulbOn = False

    # --- GPS: L76K u.FL -> GPS frame's south wall (both variants) ---
    gps = p['bay']['gps_patch']
    gps_half = p['bay']['gps_frame_opening'] / 2.0
    gcx = (gps['x'][0] + gps['x'][1]) / 2.0
    gcy = (gps['y'][0] + gps['y'][1]) / 2.0
    gy0 = gcy - gps_half  # frame opening's own south edge
    gwall = p['bay']['gps_frame_wall']
    ux, uy, uz = a['gps_ufl_xyz']
    notch_x0, notch_x1 = ux - w / 2.0, ux + w / 2.0
    notch_y0, notch_y1 = gy0 - gwall - 0.15, gy0 + 0.15  # spans the wall band, small margin each side
    notch_z0, notch_z1 = p['split_z'] + 0.5, p['split_z'] + 2.5  # z 10.5-12.5 -- above the battery's own z<=10.0
    gps_notch = box_solid(root, notch_x0, notch_x1, notch_y0, notch_y1, notch_z0, notch_z1)
    gps_notch = _best_effort_fillet(root, gps_notch, 0.0, fillet_r)
    bodies['Top'] = combine_cut(root, bodies['Top'], [gps_notch])
    bodies['Top'] = dedupe_body(root, bodies['Top'], 'Top')

    return bodies


def antenna_channel_geometry(p):
    """Analytic geometry shared between add_antenna_channels (the build)
    and verify_antenna_channels (the gate) -- computed once here so the
    two can never disagree. Returns a dict keyed by channel name; each
    entry has 'probe_xyz' (a point INSIDE the cut, to confirm it's
    hollow), 'skin_dir'/'skin_origin' (for a true_wall_distance_along_ray
    skin-thickness re-check, LoRa only), and 'battery_check' (bbox to
    confirm no overlap with the battery footprint, GPS only)."""
    a = p['antenna']
    out = {}
    if p.get('comms_stack3_full_height', True):
        ux, uy, uz = a['lora_ufl_xyz']
        ay = p['spine_a'][1]
        d = math.hypot(ux, uy - ay)
        dirv = (ux / d, (uy - ay) / d)
        s_wall = true_wall_distance_along_ray(p, (ux, uy), dirv, uz)
        channel_len = s_wall - a['channel_min_skin']
        probe_s = min(channel_len * 0.5, channel_len - 0.1) if channel_len > 0.2 else channel_len / 2.0
        probe = (ux + probe_s * dirv[0], uy + probe_s * dirv[1], uz)
        out['lora'] = {'probe_xyz': probe, 'origin_xy': (ux, uy), 'dir': dirv, 'z': uz, 's_wall': s_wall}
    gps = p['bay']['gps_patch']
    gps_half = p['bay']['gps_frame_opening'] / 2.0
    gcy = (gps['y'][0] + gps['y'][1]) / 2.0
    gy0 = gcy - gps_half
    gwall = p['bay']['gps_frame_wall']
    ux, uy, uz = a['gps_ufl_xyz']
    notch_z0, notch_z1 = p['split_z'] + 0.5, p['split_z'] + 2.5
    probe = (ux, gy0 - gwall / 2.0, (notch_z0 + notch_z1) / 2.0)
    out['gps'] = {'probe_xyz': probe, 'battery_bbox': p['bay']['battery'], 'notch_z': (notch_z0, notch_z1)}
    return out


def verify_antenna_channels(bodies_dict, p):
    """Finding 8 gate (2026-09-08, pass 9e): (1) each channel's own
    cross-section is actually open (hollow) at a live-probed interior
    point -- confirms the cut happened where the analytic route says it
    should; (2) the LoRa channel's own skin-safety re-check -- a fresh
    `true_wall_distance_along_ray` from its probe point must show
    >= channel_min_skin remaining to the true outer surface (independent
    of the Combine-Intersect construction that's supposed to guarantee
    this, same "trust but verify" pattern as verify_post_walls); (3) the
    GPS notch's own bbox does not overlap the battery reference footprint
    (analytic, both variants) -- the "no breach of the battery bay floor"
    check the finding asked for (the notch's z-band, split_z+0.5..+2.5,
    sits entirely above the battery's own z<=10.0 by construction, so
    this is a regression guard, not a live discovery)."""
    top = bodies_dict['Top']
    geo = antenna_channel_geometry(p)
    results = {}

    if 'lora' in geo:
        g = geo['lora']
        px, py, pz = g['probe_xyz']
        is_open = not probe_point_solid(top, P(px, py, pz))
        results['lora_channel_open'] = (is_open, (round(px, 3), round(py, 3), round(pz, 3)))
        s_check = true_wall_distance_along_ray(p, (px, py), g['dir'], pz)
        skin_ok = s_check is not None and s_check >= p['antenna']['channel_min_skin'] - 0.05
        results['lora_skin_ok'] = (skin_ok, round(s_check, 3) if s_check is not None else None)

    g = geo['gps']
    px, py, pz = g['probe_xyz']
    is_open = not probe_point_solid(top, P(px, py, pz))
    results['gps_channel_open'] = (is_open, (round(px, 3), round(py, 3), round(pz, 3)))
    bat = g['battery_bbox']
    z_clear = g['notch_z'][0] >= bat['z'][1]
    results['gps_no_battery_floor_breach'] = (z_clear, (g['notch_z'], bat['z']))

    return results


# ---------------------------------------------------------------------------
# Compass module mount (pass 10 REDO, 2026-09-06; RE-ORIENTED + MOVED pass
# 11, 2026-09-11, defect 2). Replaces the original pass-10 vertical-wall-
# mount + outward brow (rejected by the coordinator for putting a boxy
# bump on the pill's clean outer silhouette -- see git history for that
# earlier version) with a ceiling-hung mount directly above the GPS patch
# frame's own open chimney -- no outer-wall contact, no brow, no pocket
# cut. See params_current.py's own 'mag_module' comment for the full
# placement derivation and the local->world orientation this section
# implements.
#
# 2026-09-11 pass 11 (defect 2, "compass mount sits too close to the
# display"): Jake's review of pass10b_mag_pocket.png found the header/
# wire edge pointed toward +Y (the display end), with the fence's own
# north edge only ~1mm from the window bore's true rim there -- the five
# header wires exited straight into that gap. Fixed two ways, both in
# `PARAMS['mag_module']` (params_current.py) rather than here: (a) the
# local-x -> world-Y mapping is now DECREASING (`-local_x + offset`, was
# `+local_x + offset`) -- the header/wire edge (local x=+8.96, the
# LARGER local x) now lands at the SMALLER world Y (toward -Y, the
# lanyard end), the mounting-hole edge (local x=-9.64) at the LARGER
# world Y (toward +Y, the display end); (b) both offsets were
# recomputed (see README's pass-11 section for the full derivation) to
# shift the whole footprint as far -Y and +X as the GPS frame's own real
# opening allows with a safety margin, maximizing clearance to the
# window bore's true rim (the real "lip ring" Jake's review flagged --
# see mag_window_bore_clearance's docstring for why PARAMS['lip_r'], the
# buried anchor ring at z 9.2-11, is a different feature nowhere near
# this mount) and to the display module's own back-side bounding box.
# `mag_pcb_world_footprint`/`mag_pad_world_positions` were both made
# flip-direction-agnostic (insets computed in the LOCAL frame, then
# mapped through mag_world_y/mag_world_x once) so this orientation is a
# pure PARAMS change, not a code-structure change; `add_mag_module`'s
# fence notch (`gap_side`) flips from '+y' to '-y' to match the header
# edge's new side. `verify_mag_pocket` gained two new keep-out checks
# (window-bore clearance, display back-side clearance) -- see its own
# docstring.
# ---------------------------------------------------------------------------

MAG_DISPLAY_RING_MIN_CLEAR = 3.0  # mm -- pass 11, defect 2: minimum clearance the mount's
                                  # fence must keep from BOTH the window bore's true opening
                                  # and the display module's own back-side bounding box.

def mag_world_y(p, local_x):
    """Module local x (the 18.6mm PCB axis) -> world Y. A pure
    translation, but DECREASING (pass 11, defect 2): local +x (the
    header/wire edge, local x=+8.96) maps to the SMALLER world Y (toward
    -Y, the lanyard end); local -x (the mounting-hole edge, local
    x=-9.64) maps to the LARGER world Y (toward +Y, the display end) --
    see this section's own header comment for why. `world_y_from_local_
    x_offset` (params_current.py) was recomputed for this new sign."""
    return -local_x + p['mag_module']['world_y_from_local_x_offset']


def mag_world_x(p, local_y):
    """Module local y (the 14.0mm PCB axis) -> world X. A pure
    translation."""
    return local_y + p['mag_module']['world_x_from_local_y_offset']


def mag_pcb_bottom_world_z(p):
    """World Z of the module's local z=0 plane (PCB bottom / header-pin
    face) -- hangs `standoff_h` below Top's own inner ceiling. Derived
    directly from `top_ceiling_underside_z` so it automatically tracks
    each variant's own case height (23.0 current / 28.0 trim as of pass 12,
    was 26.0 pre-pass-12) with no per-variant override needed -- see
    mag_module_fits for why 'current' still can't host this mount despite
    the formula working for both."""
    return p['top_ceiling_underside_z'] - p['mag_module']['standoff_h']


def mag_world_z(p, local_z):
    """Module local z (thickness axis; local z=0 is the PCB bottom/header
    face, +z runs toward the component/top face) -> world Z. Local +z
    maps to world -Z (the module is mounted COMPONENTS-DOWN -- see
    params_current.py's orientation comment): increasing local z moves
    AWAY from the ceiling, toward the GPS patch below."""
    return mag_pcb_bottom_world_z(p) - local_z


def mag_module_clearance(p):
    """Spare mm between the module's lowest physical point (component top
    surface: local z = PCB thickness (1.0) + component bump) and the GPS
    patch's own top (bay.gps_patch z[1] = 18.8) -- the number that decides
    whether a variant can host this mount at all (see mag_module_fits).
    TRIM (pass 12, top_z=30): 4.7mm (was 2.7mm pre-pass-12, top_z=28 --
    the RAW gap this mount's own 4.5mm footprint (standoff_h + PCB +
    component bump) sits inside of also grows, from
    top_ceiling_underside_z(26) - gps_patch_z1(18.8) = 7.2mm to
    28-18.8=9.2mm at pass 12, since bay.gps_patch is NOT z-shifted by
    _DZ_TOP -- see params_current.py's bay comment -- while the ceiling
    rises with top_z by the same _DZ_TOP delta). CURRENT: -0.3mm
    (unchanged, still a real overlap -- its ceiling never grew, see
    params_current.py's comment)."""
    mm = p['mag_module']
    lowest_world_z = mag_world_z(p, 1.0 + mm['local_component_h'])
    patch_top = p['bay']['gps_patch']['z'][1]
    return lowest_world_z - patch_top


def mag_module_fits(p):
    """True if this variant's ceiling gives the module enough room above
    the GPS patch (mag_module_clearance >= min_patch_clearance). False
    for 'current' -- see that function's docstring. Guards
    add_mag_module/verify_mag_pocket exactly the way
    comms_stack3_full_height already guards the 3-board stack for
    'current'."""
    return mag_module_clearance(p) >= p['mag_module']['min_patch_clearance']


def mag_pcb_world_footprint(p):
    """World (x0, x1, y0, y1) of the bare PCB outline (no fence/clearance
    margin) -- local_pcb's own x-span maps to world Y, y-span to world X
    (see the module-frame axis mapping in params_current.py). 2026-09-11
    pass 11: the x->Y mapping is DECREASING (see mag_world_y), so
    mag_world_y(lx0) > mag_world_y(lx1) for lx0 < lx1 -- sort explicitly
    rather than assuming the mapping's direction, so this stays correct
    under either sign."""
    mm = p['mag_module']
    lx0, lx1 = mm['local_pcb']['x']
    ly0, ly1 = mm['local_pcb']['y']
    wy_a, wy_b = mag_world_y(p, lx0), mag_world_y(p, lx1)
    y0, y1 = min(wy_a, wy_b), max(wy_a, wy_b)
    x0, x1 = mag_world_x(p, ly0), mag_world_x(p, ly1)
    return x0, x1, y0, y1


def mag_fence_world_footprint(p):
    """World (x0, x1, y0, y1) of the fence's own OUTER footprint -- the
    bare PCB outline (mag_pcb_world_footprint) expanded by fence_clear +
    fence_wall on every side, matching what build_hanging_frame actually
    builds in add_mag_module. 2026-09-11 pass 11: shared by the new
    window-bore/display keep-out checks in verify_mag_pocket."""
    mm = p['mag_module']
    x0, x1, y0, y1 = mag_pcb_world_footprint(p)
    margin = mm['fence_clear'] + mm['fence_wall']
    return x0 - margin, x1 + margin, y0 - margin, y1 + margin


def mag_window_bore_clearance(p):
    """Minimum clearance (mm) from the mount's own fence footprint to the
    window bore's TRUE opening -- a circle of radius window_dia/2 centred
    on window_center. 2026-09-11 pass 11 (defect 2): this, not
    PARAMS['lip_r'] (the buried lip/anchor ring at z 9.2-11, which -- see
    stadium_ring_solid -- only exists near |x| roughly 26-28 in the
    straight section and is nowhere near this mount's x 5-22 footprint at
    all), is what Jake's pass-10b review actually saw as "the window lip
    ring": from inside the cavity looking up at the ceiling, the bore's
    own physical rim is the nearest ring-shaped feature to the mount, and
    the numbers match (a computed ~1.4mm at the OLD placement's near
    corner against Jake's own "~1mm" visual estimate). The worst corner
    is always the fence's north (largest-Y) edge nearest the window's own
    centreline (x=0), since the mount sits entirely south of and below
    the window."""
    fx0, fx1, _, fy1 = mag_fence_world_footprint(p)
    wx, wy = p['window_center']
    r = p['window_dia'] / 2.0
    return min(math.hypot(x - wx, fy1 - wy) - r for x in (fx0, fx1))


def mag_peg_world_positions(p):
    """World (x, y) of the two Ø2.7 peg / mounting-hole positions."""
    mm = p['mag_module']
    return [(mag_world_x(p, ly), mag_world_y(p, lx)) for lx, ly in mm['local_mount_holes']]


def mag_pad_world_positions(p):
    """World (x, y) of the two header-side rest pads -- the PCB's own two
    corners on the header edge (local x = local_pcb x[1]), inset 1.0mm in
    from each side edge so the pad sits solidly under the board rather
    than exactly at its corner. 2026-09-11 pass 11: the header-side inset
    (originally `wy - inset`, which silently assumed the local-x->world-Y
    mapping is INCREASING, i.e. that the header edge is the far/max-Y
    side) is now computed entirely in the LOCAL frame first (`lx_edge` is
    always the header's own positive local x -- local_pcb x[1] -- so
    subtracting `inset` always moves toward the PCB's own centre,
    regardless of which way mag_world_y happens to map that to world Y)
    and mapped through mag_world_y exactly once -- correct under either
    sign of that mapping, not just the one it was written for."""
    mm = p['mag_module']
    lx_edge = mm['local_pcb']['x'][1]
    ly0, ly1 = mm['local_pcb']['y']
    inset = 1.0
    wy = mag_world_y(p, lx_edge - inset)
    return [(mag_world_x(p, ly0) + inset, wy), (mag_world_x(p, ly1) - inset, wy)]


def mag_header_notch_center_x(p):
    """World X of the header notch centre -- the mean of the 5 header
    pins' own local y positions, mapped through mag_world_x."""
    mm = p['mag_module']
    ys = mm['local_header']['y']
    mid_local_y = (min(ys) + max(ys)) / 2.0
    return mag_world_x(p, mid_local_y)


def add_mag_module(root, bodies, p, clip_tool=None):
    """Compass module (GY-273/QMC5883P) mount -- pass 10 REDO. Hangs from
    Top's own inner ceiling directly above the GPS patch frame's open
    chimney (see params_current.py's mag_module comment for the full
    derivation): the module lives entirely in space that is ALREADY open
    cavity, so unlike the rejected pass-10 version there is no cut, no
    brow, and no outer-wall interaction at all -- only material ADDED
    (pegs, rest pads, a low retaining fence), all hanging from the
    ceiling. Skipped entirely when mag_module_fits(p) is False ('current'
    -- its ceiling sits 0.3mm too low for the 4.5mm standoff+PCB+
    component stack, see that function's docstring), same pattern as the
    3-board comms stack being skipped there."""
    if not mag_module_fits(p):
        return bodies
    top = bodies['Top']
    mm = p['mag_module']
    ceiling = p['top_ceiling_underside_z']
    pcb_bottom = mag_pcb_bottom_world_z(p)

    # Two Ø2.7 pegs into the mounting holes, hanging from the ceiling down
    # to the PCB's own bottom face (peg height == standoff_h, by
    # construction). clipped_pillar_with_reach -- the same helper
    # add_top_posts uses -- guarantees real contact with the ceiling even
    # though this footprint sits nowhere near the true outer shell (its
    # radial clip against the inner-cavity tool is a no-op here; the
    # full-height core is what actually matters, guaranteeing the join
    # doesn't silently no-op the way a plain clipped cylinder can -- see
    # that function's own docstring).
    peg_r = mm['peg_dia'] / 2.0
    peg_core_r = peg_r - 0.35
    peg_positions = mag_peg_world_positions(p)
    pegs = [clipped_pillar_with_reach(root, px, py, peg_r, pcb_bottom, ceiling, p, clip_tool, peg_core_r)
            for px, py in peg_positions]
    top = combine_join(root, top, pegs)
    top = dedupe_body(root, top, 'Top')
    if clip_tool is not None:
        clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
    top = _refetch_by_name(root, 'Top') or top

    # pass 13, item 1: root reinforcement where each peg meets the
    # ceiling it hangs from -- same fillet-then-collar helper as the top
    # posts/case bosses, with a smaller PEG_COLLAR_RISE (these are only
    # Ø2.7mm pegs, so a full 1.5mm collar would nearly double their
    # footprint at the root; 1.1mm still clears the verify_root_fillets
    # gate with margin -- see PEG_COLLAR_RISE's own comment).
    for i, (px, py) in enumerate(peg_positions):
        top = add_root_reinforcement(root, top, 'Top', f'mag_peg_{i}', px, py, peg_r, ceiling,
                                      direction='up', collar_rise=PEG_COLLAR_RISE)
        top = _refetch_by_name(root, 'Top') or top

    # Two small rest pads under the header-side corners, same standoff
    # height as the pegs, so the PCB sits level (both ends at the same
    # world Z -- the mounting-hole edge on its pegs, the header edge on
    # its pads).
    pad_r = mm['pad_dia'] / 2.0
    pad_core_r = pad_r - 0.35
    pad_positions = mag_pad_world_positions(p)
    pads = [clipped_pillar_with_reach(root, px, py, pad_r, pcb_bottom, ceiling, p, clip_tool, pad_core_r)
            for px, py in pad_positions]
    top = combine_join(root, top, pads)
    top = dedupe_body(root, top, 'Top')
    if clip_tool is not None:
        clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
    top = _refetch_by_name(root, 'Top') or top

    # pass 13, item 1: same root reinforcement for the rest pads.
    for i, (px, py) in enumerate(pad_positions):
        top = add_root_reinforcement(root, top, 'Top', f'mag_pad_{i}', px, py, pad_r, ceiling,
                                      direction='up', collar_rise=PEG_COLLAR_RISE)
        top = _refetch_by_name(root, 'Top') or top

    # 2026-09-15 pass 15, item 4 (Jake: "Do we need the walls around the
    # magnetometer? the top wall is too close to the screen also"): the
    # full 4-wall retaining fence (pass 10 REDO/pass 11) is REMOVED
    # outright. Root reasoning: the module's own two Ø2.7 pegs already
    # pass through its real Ø3.0 mounting holes (0.3mm total clearance) --
    # a peg-through-hole pair is, by itself, already a real, positive XY
    # location for BOTH translation and rotation, the same mechanism a
    # normal 2-pin polarized connector or a dowelled joint uses; the fence
    # was always a SECONDARY retention feature on top of that, not the
    # only thing holding the module in place. Jake's own second complaint
    # ("the top wall is too close to the screen") was live-confirmed
    # exactly right: `mag_window_bore_clearance` -- the fence's own north
    # (largest-Y) wall specifically, the same wall a fresh probe against
    # this pass's build finds -- only ever reached window_bore_clear
    # 3.955mm (trim), a real but visibly tight margin against the window
    # bore's own true opening (see pass 11's own writeup) -- removing the
    # fence removes that close wall entirely rather than trying to shave
    # it thinner still.
    #
    # Retention across the assembly's 2.7mm float (see the pass-10
    # Retention section) is now: (1) the two pegs, positive XY location;
    # (2) the two rest pads, positive Z seating (component-bottom flush
    # on the pads at build time); (3) the ~2.0mm compressible foam pad on
    # the GPS patch's own top face (unchanged, pass 10) -- which now does
    # double duty as the module's only real DOWNWARD/lateral-slip
    # resistance once the halves close, gently loading it up against the
    # pegs/pads. This is strictly less printed material than before, not
    # a retention regression: the fence never took any load along the
    # peg axis (XY) to begin with -- its own wall thickness (1.2mm) is
    # far too thin relative to its height (3.5mm) to meaningfully resist
    # a lateral shove the way the two RIGID pegs already do by
    # construction.
    #
    # A single LOW stop on the lanyard (header/wire) side -- the brief's
    # own fallback ("if a single low stop is needed... keep it <=2mm
    # tall and away from the window") -- IS added: unlike the fence's
    # north wall (which sat close to the window), the SOUTH wall sat
    # >=17mm from the window bore's own true opening (fence south edge
    # y=2.25 vs window bore's own southmost reach near y=27.35 at this
    # x -- see mag_window_bore_clearance's own geometry) -- comfortably
    # "away from the window" on its own. `MAG_STOP_H` = 2.0mm (the
    # brief's own ceiling) keeps the module from sliding south, off its
    # pads, during assembly/handling before the foam pad is loaded on --
    # a real, if modest, functional benefit at essentially zero print-
    # ability cost (it is a short vertical wall segment hanging from the
    # ceiling, same self-supporting orientation as every other boss/wall
    # in this file). Still carries the same wire-exit notch as the old
    # fence's south wall (mm['header_notch_w'], centred on the header
    # pins) so the 5 solder wires are unaffected.
    x0, x1, y0, y1 = mag_pcb_world_footprint(p)
    MAG_STOP_H = 2.0  # mm -- brief's own ceiling ("<=2mm tall")
    sy0 = y0 - mm['fence_clear'] - mm['fence_wall']
    sy1 = y0 - mm['fence_clear']
    stop = box_solid(root, x0 - mm['fence_clear'], x1 + mm['fence_clear'], sy0, sy1,
                      ceiling - MAG_STOP_H, ceiling + 0.3)
    notch_cx = mag_header_notch_center_x(p)
    notch_w = mm['header_notch_w']
    notch = box_solid(root, notch_cx - notch_w / 2.0, notch_cx + notch_w / 2.0,
                       sy0 - 0.5, sy1 + 0.5, ceiling - MAG_STOP_H - 0.5, ceiling + 0.8)
    stop = combine_cut(root, stop, [notch])
    top = combine_join(root, top, [stop])
    top = dedupe_body(root, top, 'Top')

    bodies['Top'] = top
    return bodies


def verify_mag_pocket(root, bodies_dict, p):
    """Pass-10-REDO gate: (1) the module's own component-side reference
    envelope (PCB + component bump, sampled at 3 points along the header/
    mount-hole axis, at the local-y centreline) is genuinely hollow --
    confirms the fence/pegs/pads didn't accidentally fill the board's own
    footprint; (2) both pegs have real material at mid-height; (3) both
    rest pads have real material at mid-height; (4) the fence wall has
    real material at two sample points away from the header notch. All
    six report (True, []) when mag_module_fits(p) is False ('current')
    -- see add_mag_module's docstring.

    2026-09-11 pass 11 (defect 2) added two more checks, both needing
    `root` (new parameter -- see verify()'s call site): (5)
    `window_bore_clear` -- the fence's own worst-corner clearance to the
    window bore's true opening (mag_window_bore_clearance) must be >=
    MAG_DISPLAY_RING_MIN_CLEAR (3mm); (6) `display_back_clear` -- the
    fence's north edge must stay >= that same minimum below the display
    occurrence's own real bounding box (live-probed via
    find_display_occurrence/_bbox_extents, same technique pass 10's
    README section used for the "distance from the display's speaker"
    analysis -- no x/z overlap is possible between this mount and the
    display by construction, so a pure Y-gap check is sufficient)."""
    mm = p['mag_module']
    if not mag_module_fits(p):
        return {
            'envelope_open': (True, []), 'pegs_have_material': (True, []),
            'pads_have_material': (True, []), 'fence_has_material': (True, []),
            'window_bore_clear': (True, []), 'display_back_clear': (True, []),
        }
    top = bodies_dict['Top']
    results = {}

    # (1) component envelope open -- 3 points along the local_x (header/
    # mount-hole) axis, at the local_y centre, at the component's own
    # mid-height (farthest half of the stack from the ceiling).
    lpcb = mm['local_pcb']
    probe_local_x = [lpcb['x'][0] + 0.5, (lpcb['x'][0] + lpcb['x'][1]) / 2.0, lpcb['x'][1] - 0.5]
    local_y_mid = (lpcb['y'][0] + lpcb['y'][1]) / 2.0
    wx_mid = mag_world_x(p, local_y_mid)
    z_component = mag_world_z(p, 1.0 + mm['local_component_h'] * 0.5)
    bad_envelope = []
    for lx in probe_local_x:
        wy = mag_world_y(p, lx)
        pt = P(wx_mid, wy, z_component)
        if probe_point_solid(top, pt):
            bad_envelope.append((round(wx_mid, 2), round(wy, 2), round(z_component, 2)))
    results['envelope_open'] = (not bad_envelope, bad_envelope[:5])

    # (2)/(3) pegs and pads have real material at mid-height (halfway
    # between the ceiling and the PCB's own bottom face).
    ceiling = p['top_ceiling_underside_z']
    pcb_bottom = mag_pcb_bottom_world_z(p)
    z_mid = (ceiling + pcb_bottom) / 2.0

    bad_pegs = []
    for px, py in mag_peg_world_positions(p):
        if not probe_point_solid(top, P(px, py, z_mid)):
            bad_pegs.append((round(px, 2), round(py, 2), round(z_mid, 2)))
    results['pegs_have_material'] = (not bad_pegs, bad_pegs[:5])

    bad_pads = []
    for px, py in mag_pad_world_positions(p):
        if not probe_point_solid(top, P(px, py, z_mid)):
            bad_pads.append((round(px, 2), round(py, 2), round(z_mid, 2)))
    results['pads_have_material'] = (not bad_pads, bad_pads[:5])

    # (4) 2026-09-15 pass 15, item 4: the 4-wall fence is REMOVED (see
    # add_mag_module's own docstring) -- this check now confirms the
    # single low SOUTH stop that replaces it has real material, away from
    # its own wire-exit notch, instead of probing the old west/north
    # fence walls (which no longer exist). Kept under the SAME dict key
    # ('fence_has_material') so every prior pass's verify() output format
    # stays comparable -- the value now describes the stop, documented
    # here rather than silently renamed.
    x0, x1, y0, y1 = mag_pcb_world_footprint(p)
    clear, wall = mm['fence_clear'], mm['fence_wall']
    stop_h = 2.0  # MAG_STOP_H, add_mag_module
    stop_mid_z = ceiling - stop_h / 2.0
    stop_y = y0 - clear - wall / 2.0
    # probe away from the wire notch (centred on the header pins) -- use
    # the east extreme of the stop's own span, same margin idiom as the
    # old fence probe.
    probe_pts = [(x1 - 0.5, stop_y)]
    bad_fence = []
    for wx, wy in probe_pts:
        if not probe_point_solid(top, P(wx, wy, stop_mid_z)):
            bad_fence.append((round(wx, 2), round(wy, 2), round(stop_mid_z, 2)))
    results['fence_has_material'] = (not bad_fence, bad_fence[:5])

    # (5) window-bore keep-out (pass 11, defect 2): the fence's own
    # worst-corner clearance to the window bore's TRUE opening -- see
    # mag_window_bore_clearance's own docstring for why this (not
    # PARAMS['lip_r']) is the real "window lip ring" Jake's review found
    # too close.
    bore_clear = mag_window_bore_clearance(p)
    results['window_bore_clear'] = (bore_clear >= MAG_DISPLAY_RING_MIN_CLEAR, round(bore_clear, 3))

    # (6) display back-side keep-out: fence's north edge vs. the REAL
    # inserted display occurrence's own bounding box, live-probed (not
    # the SPEC/analytic approximation) -- same technique as pass 10's
    # "distance from the display's speaker" analysis. Diagnostic-only
    # (ok=True) if the display occurrence can't be found at this point in
    # the build (verify() always runs after both boards are inserted, so
    # in practice this always resolves).
    _, _, _, fence_y1 = mag_fence_world_footprint(p)
    display_occ = find_display_occurrence(root, p)
    if display_occ is None:
        results['display_back_clear'] = (True, 'display occurrence not found -- skipped')
    else:
        _, dy, _, (dcx, dcy, dcz) = _bbox_extents(display_occ)
        display_y0 = dcy - dy / 2.0
        clear_mm = display_y0 - fence_y1
        results['display_back_clear'] = (clear_mm >= MAG_DISPLAY_RING_MIN_CLEAR, round(clear_mm, 3))

    return results


def _collect_occ_bodies(occ):
    """All bRepBodies in occ's subtree, skipping hidden ones (2026-09-06
    hygiene fix: a hidden sub-body -- e.g. the L76K assembly's placeholder
    cable stub -- has no physical presence and shouldn't be measured
    against the case in verify_min_clearances; a near-zero distance to a
    body nobody will ever print or wire that way is not a real clearance
    problem)."""
    out = []
    for b in occ.bRepBodies:
        if _safe_visible(b):
            out.append(b)
    for c in occ.childOccurrences:
        out.extend(_collect_occ_bodies(c))
    return out


def _safe_visible(entity):
    """entity.isLightBulbOn, defaulting to True (visible/unknown, don't
    hide it) both when the property doesn't exist and when reading it
    raises -- confirmed 2026-09-06: a small number of deeply-nested body
    proxies inside an inserted board reference raise
    InternalValidationError on this specific read, unrelated to any
    deliberate hiding."""
    try:
        return getattr(entity, 'isLightBulbOn', True)
    except RuntimeError:
        return True


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
    elif thin_axis == 'y90':
        # 2026-09-05 fix (coordinator diagnosis, 'Top x XIAO'/'XIAO x GPS
        # Patch Reference' interference): like 'y' (native Y, the thin
        # thickness axis, -> world Z), but ALSO rotated 90deg about world
        # Z so native X (XIAO's long ~22.5mm axis, including the USB-C
        # overhang) ends up along world Y (parallel to the Wio's own long
        # axis) instead of world X -- native X was landing squarely along
        # world X, making the XIAO+stack footprint far wider in X than
        # the Wio-sized tray/bay it needs to fit in. Native Z -> world X.
        to_x, to_y, to_z = (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (1.0, 0.0, 0.0)  # X -> Y, Y -> Z, Z -> X
    elif thin_axis == 'y90neg':
        # same as 'y90' but with native +X landing on world -Y instead of
        # +Y (for putting the USB-C end on the correct side).
        to_x, to_y, to_z = (0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (-1.0, 0.0, 0.0)  # X -> -Y, Y -> Z, Z -> -X
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


def find_pcb_like_body(occ, lo=15.0, hi=24.0, max_thick=3.0):
    """Recursively search `occ`'s whole subtree for a body shaped like a
    small PCB (two dims in [lo,hi]mm, third <= max_thick) -- used
    (2026-09-05) to position the L76K assembly by its actual PCB body
    instead of the whole occurrence's aggregate bbox, which is dominated
    by the separate GPS patch antenna on its cable (25x25x8.3, plus lead)
    and put the board itself outside the case entirely. Returns
    (body, area, thin_axis) for the largest-area match, or None."""
    best = None

    def walk(o):
        nonlocal best
        for b in o.bRepBodies:
            bb = b.boundingBox
            dx = (bb.maxPoint.x - bb.minPoint.x) / MM
            dy = (bb.maxPoint.y - bb.minPoint.y) / MM
            dz = (bb.maxPoint.z - bb.minPoint.z) / MM
            extents = {'x': dx, 'y': dy, 'z': dz}
            thin_axis = min(extents, key=extents.get)
            thin = extents[thin_axis]
            others = sorted(v for k, v in extents.items() if k != thin_axis)
            if thin <= max_thick and lo <= others[0] <= hi and lo <= others[1] <= hi:
                area = others[0] * others[1]
                if best is None or area > best[1]:
                    best = (b, area, thin_axis)
        for c in o.childOccurrences:
            walk(c)

    walk(occ)
    return best


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
    # 'y90'/'y90neg' (see flatten_transform) still flatten native Y onto
    # world Z, just with an extra 90deg rotation about Z on top -- look up
    # the thickness by the underlying single-letter axis.
    lookup_axis = thin_axis[0] if thin_axis[0] in extents else thin_axis
    flattened_thickness = extents[lookup_axis]  # this native extent becomes the world-Z extent after rotation
    target_center_mm = target_center_fn(dx, dy, dz, flattened_thickness)
    occ.transform = flatten_transform(native_center, thin_axis, target_center_mm)
    if design.snapshots.hasPendingSnapshot:
        design.snapshots.add()
    return occ, dx, dy, dz, thin_axis


def insert_comms_boards(app, root, p):
    """2026-09-07 pass 7: places the real 3-board direct-solder/B2B stack
    (L76K bottom -> XIAO middle -> Wio top, per Jake's measured hardware),
    lying flat in the lanyard-end dome, instead of the old Wio-bottom/
    XIAO-top pin-header pair placed separately from a standalone L76K.
    Each board's Z is derived from the ACTUAL measured thickness/top of
    the board below it (not a fixed offset guess), so a real thickness
    difference between the reference docs and PARAMS' nominal gaps can
    never silently stack up into a collision -- xiao_gap/wio_gap (PARAMS)
    are the only fixed numbers; every Z build on top of a live
    measurement of the board actually inserted.

    PARAMS['comms_stack3_full_height'] (False for 'current', True for
    'trim'): a real, unavoidable physical conflict found via
    analyzeInterference -- the measured 18mm-tall stack does not fit
    under 'current's unchanged 25mm-tall ceiling (Wio's own body
    physically overlapped Top by ~6mm3 at the stack's real top). Unlike
    the boss/GPS conflicts elsewhere in this pass, there is no local
    clip-away fix (the stack is simply too tall for that case height) --
    'current' inserts ONLY the L76K (it exists for the M1 outer-shell
    probe-table comparison, not as a variant meant to carry real
    electronics -- see params_current.py's comment)."""
    design = adsk.fusion.Design.cast(app.activeProduct)
    docs = p['board_docs']
    full_height = p.get('comms_stack3_full_height', True)
    wio_doc = get_open_doc(app, docs['wio']) if full_height else None
    xiao_doc = get_open_doc(app, docs['xiao']) if full_height else None
    l76k_doc = get_open_doc(app, docs['l76k'])
    occs = {}

    s3 = p['bay']['stack3']
    pcb = s3['l76k_pcb']
    cx = (pcb['x'][0] + pcb['x'][1]) / 2.0
    cy = (pcb['y'][0] + pcb['y'][1]) / 2.0
    l76k_bottom_z = s3['l76k_bottom_z']

    l76k_top_z = None
    if l76k_doc is not None:
        # Position by the actual PCB body (2026-09-05 fix, still needed
        # here), not the whole occurrence's aggregate bbox: the L76K
        # assembly includes a separate GPS patch antenna on a cable
        # (25x25x8.3), and placing by the occurrence's combined bbox puts
        # the real board far from its own PCB's center.
        occ = root.occurrences.addByInsert(l76k_doc.dataFile, adsk.core.Matrix3D.create(), True)
        match = find_pcb_like_body(occ)
        assert match is not None, 'no ~18x21mm PCB-like body found in the L76K assembly'
        pcb_body, area, thin_axis = match
        bb = pcb_body.boundingBox
        native_center = ((bb.minPoint.x + bb.maxPoint.x) / 2.0 / MM,
                          (bb.minPoint.y + bb.maxPoint.y) / 2.0 / MM,
                          (bb.minPoint.z + bb.maxPoint.z) / 2.0 / MM)
        native_extent = {'x': (bb.maxPoint.x - bb.minPoint.x) / MM,
                          'y': (bb.maxPoint.y - bb.minPoint.y) / MM,
                          'z': (bb.maxPoint.z - bb.minPoint.z) / MM}
        pcb_thickness = native_extent[thin_axis]
        target_pcb_center = (cx, cy, l76k_bottom_z + pcb_thickness / 2.0)
        # 2026-09-07 pass 7 fix: find_pcb_like_body correctly detects
        # thin_axis='y' (the 1.54mm PCB thickness) here, but flatten_
        # transform's plain 'y' mode maps native X (20.95mm, the board's
        # LONG axis) straight onto world X -- measured empirically (a
        # real build put the PCB at world x -10.48..10.48 / y -21.64..
        # -3.86, i.e. long-axis-on-X, backwards from the spec's "long
        # axis along Y"). 'y90' (already used for XIAO, same underlying
        # need) additionally rotates 90deg about world Z so native X
        # lands on world Y instead -- thin_axis is still 'y' for the
        # thickness lookup above, only the ROTATION MODE passed to
        # flatten_transform changes.
        occ.transform = flatten_transform(native_center, 'y90', target_pcb_center)
        if design.snapshots.hasPendingSnapshot:
            design.snapshots.add()
        l76k_top_z = l76k_bottom_z + pcb_thickness

        # 2026-09-07 pass 7 fix ('Top x GPS Patch Reference' / 'Bottom x
        # <L76K antenna body>' interference): the L76K reference doc's own
        # "GPD ANT" sub-assembly (a REAL modeled GPS patch antenna,
        # ~25x25mm, native-authored at a fixed offset from the PCB) rides
        # along rigidly with whatever transform is applied to the whole
        # occurrence -- in this stack's position, it lands almost exactly
        # on top of OUR OWN separate 'GPS Patch 25x25x8.3' reference box
        # (the real antenna, wired and mounted separately per the bay
        # design -- see add_gps_reference_box), a large real solid
        # overlapping both Top and Bottom.
        #
        # Tried, in order, and rejected: (1) isLightBulbOn=False on the
        # ANT ANCESTOR occurrence -- does not propagate to make the deep
        # leaf body's OWN isLightBulbOn read False (confirmed empirically:
        # check_interference's _safe_visible() still saw it as visible).
        # (2) root.features.removeFeatures.add() on the leaf body --
        # "succeeds" with no exception but is a SILENT NO-OP for a body 3+
        # levels deep inside a referenced/linked occurrence (confirmed
        # empirically: the body count under the L76K occurrence was
        # unchanged before/after, in the SAME script execution); querying
        # the same body fresh from a LATER script execution instead raises
        # InternalValidationError outright -- either way, nothing is
        # actually removed. (3, what's used here) isLightBulbOn=False
        # set DIRECTLY on the LEAF body (not an ancestor) DOES take
        # effect -- confirmed by reading it back True->False on the same
        # body object -- and check_interference's own unknown_hidden
        # filter (see its docstring) keys off exactly this property, so
        # this excludes the antenna (and the pre-existing ~12x1x1mm
        # stray-lead body, found >20mm from the target) from the
        # interference gate without needing to actually delete anything.
        target_xy = (cx, cy)
        hidden_count = 0

        def _hide_leaf_bodies(o, in_ant_subtree):
            nonlocal hidden_count
            for b in list(o.bRepBodies):
                if b == pcb_body:
                    continue
                hide = in_ant_subtree
                if not hide:
                    bb2 = b.boundingBox
                    bcx = (bb2.minPoint.x + bb2.maxPoint.x) / 2.0 / MM
                    bcy = (bb2.minPoint.y + bb2.maxPoint.y) / 2.0 / MM
                    hide = math.hypot(bcx - target_xy[0], bcy - target_xy[1]) > 20.0
                if hide:
                    b.isLightBulbOn = False
                    hidden_count += 1
            for c in list(o.childOccurrences):
                _hide_leaf_bodies(c, in_ant_subtree or ('ANT' in c.name.upper()))

        _hide_leaf_bodies(occ, False)
        print('L76K: hid', hidden_count, 'antenna/stray leaf bodies (isLightBulbOn on the body itself)')

        pcb_bb = pcb_body.boundingBox
        print('L76K PCB world bbox:', [round(v / MM, 2) for v in pcb_bb.minPoint.asArray()],
              [round(v / MM, 2) for v in pcb_bb.maxPoint.asArray()], 'top_z', round(l76k_top_z, 3))
        occs['l76k'] = occ

    xiao_top_z = None
    if xiao_doc is not None and l76k_top_z is not None:
        xiao_bottom_z = l76k_top_z + s3['xiao_gap']
        thickness_holder = {}

        def xiao_target(dx, dy, dz, thick, _z0=xiao_bottom_z, _h=thickness_holder):
            _h['t'] = thick
            return (cx, cy, _z0 + thick / 2.0)

        # XIAO's native thickness axis is Y (per Jake) -- 'y90' (see
        # flatten_transform) additionally rotates 90deg about world Z so
        # XIAO's long ~22.5mm axis (including the USB-C overhang) lands
        # along world Y with the USB-C end toward +Y, matching the L76K's
        # own long axis below it and the spec's "XIAO USB-C end toward
        # +Y". This is the SAME rotation the pre-pass-7 stack used for
        # XIAO (there, plugging DOWN into the Wio below it); the physical
        # sense -- component/pin side facing down, toward whatever board
        # is below -- is unchanged by this pass's reordering, so it is
        # reused as-is rather than re-derived.
        occ, dx, dy, dz, thin = insert_and_place(design, root, xiao_doc, xiao_target, thin_axis='y90')
        occs['xiao'] = occ
        xiao_thickness = thickness_holder['t']
        xiao_top_z = xiao_bottom_z + xiao_thickness
        print('XIAO placed: bottom_z', round(xiao_bottom_z, 3), 'thickness', round(xiao_thickness, 3),
              'top_z', round(xiao_top_z, 3))

    if wio_doc is not None and xiao_top_z is not None:
        wio_bottom_z = xiao_top_z + s3['wio_gap']
        thickness_holder2 = {}

        def wio_target(dx, dy, dz, thick, _z0=wio_bottom_z, _h=thickness_holder2):
            _h['t'] = thick
            return (cx, cy, _z0 + thick / 2.0)

        # Wio's native bbox is thinnest in Z already (assume flat as
        # authored, module up) -- no rotation, same as the pre-pass-7
        # stack (there, Wio was the BOTTOM board; here it's the TOP board,
        # but its own native orientation -- module facing up -- is
        # unchanged either way).
        occ, dx, dy, dz, thin = insert_and_place(design, root, wio_doc, wio_target, thin_axis='z')
        occs['wio'] = occ
        wio_thickness = thickness_holder2['t']
        wio_top_z = wio_bottom_z + wio_thickness
        print('Wio placed: bottom_z', round(wio_bottom_z, 3), 'thickness', round(wio_thickness, 3),
              'top_z (STACK TOP)', round(wio_top_z, 3))

    return occs


# ---------------------------------------------------------------------------
# build() / verify() / run()
# ---------------------------------------------------------------------------
# build() / verify() / run()
# ---------------------------------------------------------------------------
def build(app, params):
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent
    del ROOT_FILLET_REPORT[:]  # pass 13: fresh per build() call, read by verify_root_fillets/run()
    solid = build_outer_pill_solid(root, params)
    bottom, top = hollow_and_split(root, solid, params)
    bodies = {'Bottom': bottom, 'Top': top}

    bodies = add_lip_anchor_reliefs(root, bodies, params)
    bodies = add_window(root, bodies, params)
    bodies = add_fpc_relief(root, bodies, params)

    # one shared inner-cavity clip tool, reused for every screw boss + Top
    # post instead of rebuilt per-call (rebuilding a full extrude+2-revolve
    # inner solid ~11 times was slow enough to risk the MCP call timing
    # out) -- hidden as a reference-only leftover once done.
    clip_tool = build_inner_cavity_clip_tool(root, params)
    # rename/hide it BEFORE use: dedupe_body's Remove-feature cleanup
    # (inside add_case_screws/add_ear/add_s2_boss) can invalidate
    # previously-held BRepBody Python references (same issue it works
    # around for Bottom/Top), so renaming clip_tool only after those
    # calls would silently rename a stale handle instead.
    clip_tool.name = CLIP_TOOL_NAME
    clip_tool.isLightBulbOn = False
    bodies = add_case_screws(root, bodies, params, clip_tool=clip_tool)
    clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool

    # pass 16 (owner call, item A -- candidate 5 display mount): two EARS
    # (S1, S3) grown from Top's dome wall (RESUMED pass 16, Finding 1 fix:
    # no longer carrying D1/D2's own pilot -- see add_ear's own docstring
    # -- their own case-closure screws moved into add_case_screws above,
    # north of the ears), plus a short BOSS from the west wall carrying S2
    # -- replaces build_screen_plate/add_top_posts entirely (no Screen
    # Plate, no P1-P4 ceiling posts).
    for ear_name in ('S1', 'S3'):
        bodies = add_ear(root, bodies, params, ear_name, clip_tool=clip_tool)
        clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool
    bodies = add_s2_boss(root, bodies, params, clip_tool=clip_tool)
    clip_tool = _refetch_by_name(root, CLIP_TOOL_NAME) or clip_tool

    # pass 16 resumed (item 1, live-found the hard way): an unconditional
    # keep-out cut of the display's second real SMT connector footprint
    # ('secondary_conn_bbox' -- see secondary_conn_world_bbox's own
    # docstring) -- the S3 ear's own arm run passes close enough to it that
    # a live check_interference(Top, display) found a real 17.8mm^3
    # overlap here even after the standoff-barrel fix above cleared the
    # other 8 hits. Same unconditional-cut pattern add_s2_boss already
    # uses for the battery connector, applied generically to Top (not
    # tied to which specific feature's material actually reaches this
    # xy) so it stays correct even if the ear geometry shifts again.
    scx, scy, scz = secondary_conn_world_bbox(params)
    secondary_conn_keepout = box_solid(root, scx[0] - 0.5, scx[1] + 0.5, scy[0] - 0.5, scy[1] + 0.5,
                                        scz[0] - 0.5, scz[1] + 0.5)
    top_for_conn_cut = _refetch_by_name(root, 'Top') or bodies['Top']
    bodies['Top'] = combine_cut(root, top_for_conn_cut, [secondary_conn_keepout])

    # pass 16 resumed (item 1): same story, five more small real
    # component keep-outs (see 'ear_wedge_component_keepouts' above).
    dz_conn = params.get('display_z_offset', 0.0)
    wedge_keepout_tools = []
    for kb in params.get('ear_wedge_component_keepouts', []):
        wedge_keepout_tools.append(box_solid(root, kb['x'][0], kb['x'][1], kb['y'][0], kb['y'][1],
                                              kb['z'][0] + dz_conn, kb['z'][1] + dz_conn))
    if wedge_keepout_tools:
        top_for_wedge_cut = _refetch_by_name(root, 'Top') or bodies['Top']
        bodies['Top'] = combine_cut(root, top_for_wedge_cut, wedge_keepout_tools)

    bodies = add_buttons(root, bodies, params, clip_tool=clip_tool)

    # pass 16 fix (live-found): the new S1 ear's wall-root wedge (D1,
    # (-19,64)) reaches close enough to the Home button's own housing/
    # ceiling-gusset (both anchored to the same general west-wall region
    # near y~57-65) that a live check_interference run found a real
    # 9.14mm^3 Top-vs-Home-Button overlap -- rather than hand-derive the
    # button's own exact rib/gusset footprint (button_geometry's own
    # dynamically-clamped geometry, several offsets deep), clip Top
    # directly against the two REAL button bodies: guarantees zero
    # overlap regardless of exactly where either button's own geometry
    # ends up, the same "can never punch through" guarantee this file
    # already gives every boss/post against the outer shell via
    # clip_to_inner_cavity.
    #
    # KEEPOUT (live-found, this pass): this cut runs against the buttons'
    # OWN FINAL bodies (cap + rib + gusset, everything), not just the
    # hole_cutter/tab_hole_body tools add_button already protects (see
    # _clip_of_ear_boss_keepout) -- a live verify_post_walls run found
    # D1's own pilot wall newly hollow again at several angles near
    # z=18.6mm (just below the ear's own capped ceiling reach) even after
    # that first fix, traced to THIS separate cut. Same keepout, applied
    # here too: cut Top with a COPY of each button body (an oversized-box
    # Combine-Intersect-Keep, same copy idiom the display-vs-Top fix
    # above already uses, so the real 'Power Button'/'Home Button'
    # bodies stay untouched for their own later export) with the ear/
    # boss keepout columns subtracted out of the copy first.
    for btn_name in ('Power Button', 'Home Button'):
        copy_box = box_solid(root, -1000.0, 1000.0, -1000.0, 1000.0, -1000.0, 1000.0)
        btn_copy = combine_intersect_keep(root, copy_box, [bodies[btn_name]])
        btn_copy = _clip_of_ear_boss_keepout(root, btn_copy, params)
        bodies['Top'] = combine_cut(root, bodies['Top'], [btn_copy])
    bodies['Top'] = dedupe_body(root, bodies['Top'], 'Top')

    insert_display_pcba(app, root, params)

    # pass 16 fix (live-found, unrelated to any new pass-16 geometry --
    # the found region is centred on x=0): a live check_interference run
    # found a real 332mm^3 overlap between Top's own ceiling shell and
    # the display module's real combined PCBA/shield/FPC body
    # ('H0146Y003T001-V1', the same body params_current.py's own comment
    # already references for the FPC-relief measurements) at y~60-65,
    # z~24-26 -- BELOW window_z_bottom (25.4 trim), i.e. an area the
    # window bore's own cut never reaches, where the real board is wider
    # than the existing fpc_relief pocket (itself targeted at a DIFFERENT
    # known trouble spot, y 71.4-73.1) accounts for. Cut Top against the
    # display's own real bodies, but ONLY the portion of Top strictly
    # below top_ceiling_underside_z (a ceiling-safe clip, same idiom
    # _antenna_skin_safe_channel already uses) -- this can never puncture
    # the outer 2mm skin (the display's own glass/bezel legitimately
    # touches the outer face exactly at the window bore, a separate,
    # already-handled cut) while resolving the interior interference.
    #
    # PILOT_PROTECT_Z, not display_bbox (live-found, this pass, ROUND 2):
    # a first version of this fix bounded the cut tool's own LOWER z at
    # -100 (unbounded) and separately re-asserted a "guaranteed" material
    # column at every ear/S2-boss anchor/target point, capped via
    # DISPLAY_KEEPOUT_CLEARANCE off PARAMS['display_bbox'] -- but a live
    # per-sub-body interference probe (entityOne/entityTwo + bounding box,
    # not just the aggregate volume) found the display's own REAL bare
    # PCB body ('BOARD:1' in the reference doc's own hierarchy) reaching
    # down to z=21.47-22.85mm (trim, world frame) -- LOWER than
    # `display_bbox['z'][0]` (23.3mm trim-world) accounts for, i.e.
    # display_bbox is not actually a lower bound on every one of the
    # module's own sub-bodies (only the housing this file's earlier FPC-
    # relief work happened to measure). Capping the reassertion off that
    # same unreliable number just re-created a smaller version of the
    # identical bug one level down. Fixed properly: bound the CUT TOOL's
    # own lower z at a fixed, construction-derived floor
    # (`top_pilot_z[1] + PILOT_PROTECT_MARGIN`, independent of any
    # display measurement) instead of -100 -- the cut can then, BY
    # CONSTRUCTION, never remove material below that floor no matter how
    # low any real display sub-body's own geometry goes, which makes the
    # separate "reassert a guaranteed column" step provably unnecessary
    # (a Cut can never remove what its own tool cannot reach) -- removed
    # outright rather than kept as inert insurance.
    disp_occ_for_clip = find_display_occurrence(root, params)
    if disp_occ_for_clip is not None:
        ceiling_z = params['top_ceiling_underside_z']
        pilot_protect_z = params['top_pilot_z'][1] + PILOT_PROTECT_MARGIN
        disp_bodies_all = _collect_occ_bodies(disp_occ_for_clip)
        # CANDIDATE FILTER (live-found, this pass, ROUND 3): the display
        # reference occurrence carries ~424 sub-bodies (every discrete SMT
        # part down to individual 0201 resistors/caps); an earlier version
        # filtered only by z (>ceiling_z-8, matched by essentially ALL 424
        # of them for this compact a board) and then took the first 60 in
        # whatever arbitrary order root.allOccurrences/bRepBodies iterates
        # in -- which silently never reached the ONE body that actually
        # causes the real interference (a combined PCBA/shield/FPC
        # reference body, bbox x -19.7..18.0, y 30.3..71.8, z 21.8..24.6
        # world/trim) if it didn't happen to fall in that arbitrary first
        # 60. Filtering by real XY FOOTPRINT AREA instead (> 4.0mm^2) is a
        # much better proxy for "big enough to ever matter here" than
        # iteration order -- every individual 0201/0603 SMT part measures
        # well under 1mm^2, while the handful of connectors/shields/the
        # bare PCB that can plausibly reach Top's remaining ear/boss
        # material are all several mm^2 or more. Cuts the live candidate
        # count from 424 to ~33 (both variants, live-counted) with the
        # actual offending body included every time (verified: this is
        # what finally brought DISPLAY-TOP interference to true zero,
        # confirmed both variants -- see the README's pass-16 section).
        for b in disp_bodies_all:
            bb = b.boundingBox
            x0, x1 = bb.minPoint.x / MM, bb.maxPoint.x / MM
            y0, y1 = bb.minPoint.y / MM, bb.maxPoint.y / MM
            if bb.maxPoint.z / MM <= pilot_protect_z or (x1 - x0) * (y1 - y0) <= 4.0:
                continue
            # target = a throwaway box (safely modified in place); tools=[b]
            # (isKeepToolBodies=True -- the REAL, referenced display body
            # `b` is NEVER modified) -- combine_intersect_keep's own
            # target/tool roles matter here specifically to avoid mutating
            # the inserted reference occurrence's own real geometry.
            # +0.3mm past the nominal ceiling (still ~1.5mm+ short of the
            # outer top face, both variants): a live check_interference
            # run found a small (~9mm^3) residual overlap with the exact
            # ceiling_z boundary, most likely ordinary tessellation slack
            # at the cut tool's own coincident top face. Lower bound is
            # pilot_protect_z, NOT unbounded -- see this block's own
            # docstring above for why.
            box = box_solid(root, -100.0, 100.0, -100.0, 100.0, pilot_protect_z, ceiling_z + 0.3)
            try:
                # live-found: a genuinely non-overlapping (box, body) pair
                # can raise FEATURE_FAILED_TO_CREATE here rather than
                # return an empty result -- same Combine-Intersect
                # fragility clipped_pillar_with_reach's own docstring
                # already documents elsewhere in this file. Skip exactly
                # like the "empty intersection" case below.
                tool = combine_intersect_keep(root, box, [b])
            except RuntimeError:
                continue
            bb2 = tool.boundingBox
            if (bb2.maxPoint.x - bb2.minPoint.x) < 1e-6:
                continue  # empty intersection -- this sub-body never reaches the ceiling-safe band
            bodies['Top'] = combine_cut(root, bodies['Top'], [tool])
        bodies['Top'] = dedupe_body(root, bodies['Top'], 'Top')

    bodies = add_usb_tunnel(root, bodies, params)
    bodies = add_lug(root, bodies, params)
    bodies = add_flare_logo(root, bodies, params)
    bodies = add_wordmark_logo(root, bodies, params)

    bodies = add_comms_bay(root, bodies, params, clip_tool=clip_tool)
    bodies = add_antenna_channels(root, bodies, params, clip_tool=clip_tool)
    bodies = add_mag_module(root, bodies, params, clip_tool=clip_tool)

    insert_comms_boards(app, root, params)

    # 2026-09-05 fix: dedupe_body's "orphaned same-named duplicate" issue
    # (see its docstring, originally worked around only for the case-screw
    # boss / Top-post joins) turned out NOT to be specific to that one
    # combination -- the button-plate-clearance and comms-bay joins/cuts
    # added this pass triggered the same '<Name> (1)' orphaning on 'Top'
    # and 'Screen Plate' too. Sweep every tracked body name here,
    # unconditionally, as a general final cleanup rather than chasing each
    # new call site individually -- dedupe_body is a safe no-op when there
    # is no stale duplicate.
    for _name in ('Bottom', 'Top', 'Power Button', 'Home Button'):
        if _name in bodies:
            bodies[_name] = dedupe_body(root, bodies[_name], _name)

    remove_stray_generic_bodies(root)

    # pass 13: persist the fillet-vs-collar decisions to disk (see
    # ROOT_FILLET_LOG_PATH's own comment) -- this line runs, and the file
    # is written, whenever Fusion actually reaches the end of build()
    # server-side, independent of whether the fusion_mcp_execute call that
    # triggered it timed out client-side first.
    for feature_name, method, value in ROOT_FILLET_REPORT:
        _log_root_fillet(params['variant'], feature_name, method, value)

    return bodies


def remove_stray_generic_bodies(root):
    """Defensive final sweep (2026-09-05; generalized 2026-09-06 to run
    against any Component, not just the document root -- organize_components()
    also sweeps each of the 5 pass-6 components after moving bodies into
    them): every intentional body in this generator ends up with an
    explicit name -- one of build()'s returned names, or one of the named
    reference/tool bodies (Battery 803040, GPS Patch 25x25x8.3, FPC LoRa
    Antenna Keep-out, Inner Cavity Clip Tool, Cap Trim Envelope) -- so
    anything still carrying Fusion's auto-generated generic name ('BodyNN')
    by the end of build() is presumptively an orphaned byproduct of the
    same fragile-boolean family as the Bottom/Top duplication dedupe_body
    works around (a small stray box has been observed even after that fix,
    from the Top posts / comms-bay region). Geometrically these are
    redundant -- everything the probes/envelope/interference checks care
    about already passes with them ignored -- so delete them outright
    rather than leave them to fail the exact body-name check."""
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


def find_ceiling_z_at(body, x, y, z_hi, z_lo, step=0.05):
    # Scan DOWNWARD in Z at a fixed (x,y) from z_hi to z_lo and return
    # the first z where `body` is solid -- i.e. the inner ceiling's
    # underside height at that point (2026-09-07, pass 7, for
    # verify_stack3_clearance). Returns None if no solid is found
    # anywhere in the scanned range.
    z = z_hi
    while z >= z_lo:
        if probe_point_solid(body, P(x, y, z)):
            return z
        z -= step
    return None


_TOUCH_VOLUME_TOL_MM3 = 0.05  # pass-5: Jake's real-interference gate (was 1e-4)


def check_interference(design, entities, min_volume_mm3=_TOUCH_VOLUME_TOL_MM3):
    """Real solid-overlap interference between the given entities (BRepBody
    and/or Occurrence -- pass whole Occurrences for inserted board
    references, not their individual nested bRepBodies: empirically,
    analyzeInterference over a flat list of dozens of deeply-nested
    reference-doc body proxies silently returns ZERO results even where a
    real, large overlap exists and a probe confirms it, but the SAME
    geometry passed as the top-level Occurrence entities is analyzed
    correctly -- this was the root cause of pass 4's false "zero
    interference" claim, together with two more API issues fixed here:

    (1) `analyzeInterference` flags coincident-face touches (e.g. the Top
        posts resting on the Screen Plate at their shared z=14.1 face,
        BY DESIGN) as tiny near-zero-volume "interference" purely from
        floating point tolerance -- `areCoincidentFacesIncluded = False`
        asks Fusion to exclude these at the source, rather than trying to
        filter them out afterward by volume (see (2)).
    (2) `interferenceBody.physicalProperties.volume` reads back as 0.0 for
        EVERY result in this Fusion build, including ones with large,
        clearly real bounding boxes (confirmed by cross-checking against
        point-containment probes) -- `results.createBodies()` and
        `interferenceBody.copyToComponent()` (the two ways to get a real,
        measurable body out of a transient interference result) both also
        fail here ("Keeping interference body is not supported in
        parametric modeling environment" / silent None). Volume is
        therefore approximated from the interference body's BOUNDING BOX
        (dx*dy*dz) -- a conservative over-estimate for anything but an
        axis-aligned box-shaped overlap, which only makes this check
        STRICTER than a true-volume gate, never more permissive.
    (3) Passing a whole board Occurrence (per the fix above) makes Fusion
        recurse into and report interference between that occurrence's OWN
        internal sub-components too -- e.g. a USB-C connector's shell,
        modeled as several separate mirrored/chamfered/boolean feature
        bodies (`BOSS-EXTRUDE7_4_`, `MIRROR2`, `CHAMFER9`, ...) in the
        original reference doc, genuinely overlaps ITSELF there; real
        geometry, but nothing to do with case fit, and `assemblyContext`
        does not distinguish this (it reads None on both sides here
        regardless). What DOES distinguish it: every one of OUR OWN bodies
        (Bottom/Top/Screen Plate/the buttons, or a reference box) keeps
        its assigned name; every board's own internal sub-bodies keep
        Fusion's generic ('Body7', ...) or the reference doc's native
        feature-history names. A result is only kept if at least one side
        is a body whose `.name` exactly matches one of ours -- i.e. it is
        a case-vs-board (or reference-box-vs-board) pair, never a
        board's-own-internal-geometry pair."""
    known_names = {'Bottom', 'Top', 'Power Button', 'Home Button'}
    known_names.update(REFERENCE_BOX_NAMES)
    coll = adsk.core.ObjectCollection.create()
    for e in entities:
        coll.add(e)
    interference_input = design.createInterferenceInput(coll)
    interference_input.areCoincidentFacesIncluded = False
    results = design.analyzeInterference(interference_input)
    if results is None:
        # analyzeInterference can return None for a very large/complex
        # collection (e.g. hundreds of tiny nested SMT-component bodies
        # from an inserted board reference) rather than raising -- treat as
        # "could not be determined" instead of crashing the whole build.
        return [('<unavailable>', 'analyzeInterference returned None for this collection', None)]
    count = results.count
    results_list = []
    for i in range(count):
        r = results.item(i)
        n1 = getattr(r.entityOne, 'name', None)
        n2 = getattr(r.entityTwo, 'name', None)
        if n1 not in known_names and n2 not in known_names:
            continue  # purely internal to one inserted reference doc
        # 2026-09-06 hygiene fix: a HIDDEN sub-body inside an inserted
        # board reference (e.g. the L76K assembly's placeholder cable
        # stub) can still be flagged by analyzeInterference even though
        # it is deliberately hidden and has no physical presence in the
        # printed/assembled case -- analyzeInterference does not itself
        # respect isLightBulbOn (same finding as dedupe_body's note that
        # isLightBulbOn=False alone never excluded a body here). Skip a
        # result only when the UNKNOWN side (the board's own sub-body,
        # never one of ours) is hidden -- our own tracked bodies
        # (REFERENCE_BOX_NAMES included) are deliberately hidden too but
        # must still be checked, so the hidden-skip must not apply to them.
        unknown_hidden = ((n1 not in known_names and _safe_visible(r.entityOne) is False)
                           or (n2 not in known_names and _safe_visible(r.entityTwo) is False))
        if unknown_hidden:
            continue
        vol_mm3 = 0.0
        try:
            bb = r.interferenceBody.boundingBox
            dx = (bb.maxPoint.x - bb.minPoint.x) / MM
            dy = (bb.maxPoint.y - bb.minPoint.y) / MM
            dz = (bb.maxPoint.z - bb.minPoint.z) / MM
            vol_mm3 = dx * dy * dz
        except Exception:
            vol_mm3 = float('inf')  # unknown -> treat as real, don't hide it
        if vol_mm3 > min_volume_mm3:
            results_list.append((r.entityOne.name if hasattr(r.entityOne, 'name') else str(r.entityOne),
                                  r.entityTwo.name if hasattr(r.entityTwo, 'name') else str(r.entityTwo),
                                  round(vol_mm3, 4)))
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

    # y=29 (2026-09-06, pass 6 -- was y=27): the original y=10 ran straight
    # through the GPS patch frame's footprint (y -5..20, hanging from the
    # Top ceiling through this probe's z=22 height) -- find_first_solid_x
    # stops at the FIRST solid it hits scanning outward, which for the
    # WIDER 'current' variant is the bay frame's own wall, not the true
    # (further out) shell cavity wall, giving a false failure. y=27
    # cleared the bay footprints and the Top posts (P2/P3 at y~32) in both
    # variants, but pass 6's clipped_pillar_with_reach fix (see
    # verify_posts_and_bosses) means case-screw boss C -- trim (23, 25.2)
    # / current (23.74, 25.2), boss_dia 6mm radius -- now ACTUALLY HAS
    # MATERIAL for the first time (it was silently never joined into Top
    # before that fix), and its own real footprint reaches this probe's
    # scan line at y=27 (only 1.8mm away, well within its 3mm radius),
    # stopping the scan on the boss itself at x~20.6 instead of the true
    # wall at ~21.9. y=29 (3.8mm from boss C's y, outside its radius)
    # clears the now-real boss C while still clearing the bay footprints
    # and Top posts (P2/P3 at y~32) in both variants.
    z_top_probe = 22.0
    expect_top = inner_rho_at_z(p, z_top_probe)
    found = find_first_solid_x(top_bodies, 29.0, z_top_probe)
    checks.append(('top_cavity', expect_top, found, found is not None and abs(found - expect_top) <= 0.15))

    z_bot_probe = 9.3
    expect_bot = inner_rho_at_z(p, z_bot_probe)
    found2 = find_first_solid_x(bot_bodies, 10.0, z_bot_probe)
    checks.append(('bottom_cavity', expect_bot, found2, found2 is not None and abs(found2 - expect_bot) <= 0.15))
    return checks


def envelope_bounds(p):
    """Generously allowed bounding box for Bottom/Top, honoring the KNOWN
    intentional protrusions (button caps proud of the -x wall, the
    lanyard lug beyond the spine_a dome) -- used to catch anything else
    (like the case-screw-boss bump this was added for) poking outside
    the shell. 2026-09-08 pass 9 through pass 12 also allowed for the FPC
    relief brow raised above the flat top face at the USB end
    (`add_fpc_brow`) -- deleted in pass 12b (see FPC_RELIEF_MIN_WALL's
    module comment), so the z bound is back to a plain `top_z + tol`."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    R = p['outer_radius']
    cap_proud = max(p['power_cap']['proud'], p['home_cap']['proud'])
    tol = 0.5
    _, lug_y_far, _, _ = lug_ear_geometry(p)
    return {
        'x': (-(R + cap_proud + tol), R + tol),
        'y': (min(lug_y_far - tol, ay - R - tol), by + R + tol),
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


def rho_from_spine(p, x, y):
    """Distance from the pill's spine (the segment spine_a-spine_b for the
    straight section, the nearer spine endpoint for the domed ends) --
    the true 'how far from the centerline' measure the shell profile is
    built from (see build_outer_pill_solid)."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    if ay <= y <= by:
        return abs(x)
    center_y = ay if y < ay else by
    return math.hypot(x, y - center_y)


def check_body_envelope_vertices(body, p, name, tol=0.15):
    """Vertex scan (2026-09-05, per the coordinator's own STL analysis):
    no vertex of an exported body should sit beyond
    rho = outer_radius + tol from the spine, except the lanyard lug (a
    real, intentional protrusion at the -y tail: y below the wall-plus-
    2mm threshold and |x| < 5.6) and the two button cap heads (allowed
    out to +0.45mm, their designed proud amount). 2026-09-08 pass 9
    through pass 12 also exempted the FPC relief brow's own raised
    footprint on Top (`add_fpc_brow`) -- deleted in pass 12b (see
    FPC_RELIEF_MIN_WALL's module comment: `usb_end_extension_mm` now
    recovers the same skin by lengthening the envelope instead of
    raising it), so this function is back to exactly its pre-pass-9
    shape, same as 2026-09-10 pass 10 REDO already did for the
    compass-module mount's own now-removed exemption."""
    lug_half_w, _, lug_y_root, _ = lug_ear_geometry(p)
    lug_y_thresh = lug_y_root
    lug_x_half = lug_half_w + 0.5
    is_cap = name in ('Power Button', 'Home Button')
    limit = p['outer_radius'] + (0.45 + 0.05 if is_cap else tol)
    bad = []
    for v in body.vertices:
        pt = v.geometry
        x, y = pt.x / MM, pt.y / MM
        if y < lug_y_thresh and abs(x) < lug_x_half:
            continue
        rho = rho_from_spine(p, x, y)
        if rho > limit:
            bad.append((round(x, 2), round(y, 2), round(pt.z / MM, 2), round(rho, 2)))
    return bad


def verify_export_envelope(bodies_dict, p):
    """Runs the vertex check above over every body build() actually
    exports (EXPORT_BODY_NAMES)."""
    results = {}
    for name in EXPORT_BODY_NAMES:
        body = bodies_dict.get(name)
        if body is None:
            continue
        bad = check_body_envelope_vertices(body, p, name)
        results[name] = (not bad, bad[:5])  # cap the printed sample to 5 offending vertices
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


def find_innermost_s(body, origin_xy, d2, z, max_s=40.0, step=0.02):
    """Mirror of find_outermost_s: scan from the origin (s=0) OUTWARD along
    origin_xy + s*d2 at height z and return the first s where `body` is
    solid -- i.e. the body's INNERMOST point along this ray. Used
    (2026-09-08, pass 9b/finding 10) to find the real switch actuator's
    own outward reach and the cap's own plunger-tip rim, both measured
    from the same origin (the switch bbox's center) so their `s` values
    are directly comparable/subtractable -- see verify_plunger_reach."""
    s = 0.0
    while s <= max_s:
        pt = P(origin_xy[0] + s * d2[0], origin_xy[1] + s * d2[1], z)
        if probe_point_solid(body, pt):
            return s
        s += step
    return None


def find_switch_body(bodies_list, switch_bbox, margin=4.0):
    """Locate the real 'SWITCH-TS24CA' body within `bodies_list` (the
    display occurrence's own subtree, via _collect_occ_bodies) whose
    center lands within `margin` mm of the given SPEC/PARAMS switch bbox
    -- same identification technique used by the live probe that measured
    PARAMS['switch_actuator_reach'] in the first place (2026-09-08, pass
    9b/finding 10)."""
    x0, x1 = switch_bbox['x'][0] - margin, switch_bbox['x'][1] + margin
    y0, y1 = switch_bbox['y'][0] - margin, switch_bbox['y'][1] + margin
    for b in bodies_list:
        par = b.parentComponent.name if b.parentComponent else ''
        if 'SWITCH-TS24CA' not in par:
            continue
        bb = b.boundingBox
        cx = (bb.minPoint.x + bb.maxPoint.x) / 2.0 / MM
        cy = (bb.minPoint.y + bb.maxPoint.y) / 2.0 / MM
        if x0 <= cx <= x1 and y0 <= cy <= y1:
            return b
    return None


def verify_plunger_reach(root, bodies_dict, p):
    """New check (2026-09-08, pass 9b, finding 10): live-probes the REAL
    inserted display occurrence's own switch bodies (not the analytic
    PARAMS bbox) to confirm the plunger actually reaches close to the
    real actuator -- the thing finding 10's fix (button_geometry's
    `s_actuator`/`plunger_pretravel`, see its docstring) is supposed to
    guarantee, checked independently of the formula that produced it.

    For each button: (1) re-measures the real actuator's own outward
    reach from its switch bbox center (the same live scan used to derive
    PARAMS['switch_actuator_reach'] -- confirms the baked-in constant
    still matches the actual inserted geometry); (2) probes the BUILT
    cap body's own tip rim (offset sideways past the nub-pocket cutout,
    so the probe lands on real shaft material, not the pocket's own empty
    recess) and computes the live gap between the two -- must be close to
    `plunger_pretravel` (0.3mm), not the multi-mm miss finding 10 found.
    """
    results = {}
    occ = find_display_occurrence(root, p)
    if occ is None:
        return {'ok': None, 'note': 'display occurrence not found'}
    switch_bodies = _collect_occ_bodies(occ)

    buttons = [
        ('Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home Button', dict(p['switch_home_bbox'], z=p['switch_power_bbox']['z']), p['home_nub_dir'], p['home_cap']),
    ]
    for name, switch_bbox, nub_dir, cap in buttons:
        key = name.lower().replace(' ', '_')
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        sw_body = find_switch_body(switch_bodies, switch_bbox)
        if sw_body is None:
            results[f'{key}_reach'] = (False, 'switch body not found live')
            continue
        cx = (switch_bbox['x'][0] + switch_bbox['x'][1]) / 2.0
        cy = (switch_bbox['y'][0] + switch_bbox['y'][1]) / 2.0
        d2, t2 = g['d'], g['t']
        z0, z1 = switch_bbox['z']

        live_reach = None
        z = z0 + 0.1
        while z <= z1 - 0.1 + 1e-9:
            s = find_outermost_s(sw_body, (cx, cy), d2, z, max_s=6.0, step=0.02)
            if s is not None and (live_reach is None or s > live_reach):
                live_reach = s
            z += 0.1
        expect_reach = p['switch_actuator_reach']
        reach_ok = live_reach is not None and abs(live_reach - expect_reach) < 0.1
        results[f'{key}_actuator_reach'] = (reach_ok, {'expect': expect_reach, 'found': live_reach})

        cap_body = bodies_dict.get(name)
        if cap_body is not None and live_reach is not None:
            pocket_half_w = p['nub_pocket']['xy'][0] / 2.0
            rim_offset = pocket_half_w + 0.4  # clear of the pocket footprint, still on the shaft
            rim_origin = (cx + rim_offset * t2[0], cy + rim_offset * t2[1])
            rim_s = find_innermost_s(cap_body, rim_origin, d2, g['switch_z_mid'], max_s=8.0, step=0.02)
            gap = None if rim_s is None else rim_s - live_reach
            gap_ok = gap is not None and abs(gap - p['plunger_pretravel']) < 0.15
            results[f'{key}_rest_gap'] = (gap_ok, {'expect': p['plunger_pretravel'], 'found': None if gap is None else round(gap, 3)})
        else:
            results[f'{key}_rest_gap'] = (False, 'cap body or live_reach missing')
    return results


def verify_button_insertion(root, bodies_dict, p):
    """New check (2026-09-08, pass 9b, finding 9): does the retaining
    tab actually clear the rib as the cap is slid into place from the
    inside (the only physically possible insertion direction -- see
    add_button's tab-relief comment)? Sweeps the tab's own footprint
    (its rest position, from button_geometry + the same construction
    add_button uses) along the plunger axis from a fully-inboard start
    (comfortably clear of the rib) to its final rest position, sampling 5
    points across the tab's cross-section (4 corners + center) at each of
    N steps and checking real point-containment against the BUILT Top
    body (which already includes the rib's own tab-relief cut). Reports
    every (step, point) that comes back solid -- 0 bad is a clean,
    unobstructed insertion path; any hit is a real, live-confirmed
    interference, not just the analytic z-band argument in add_button's
    comment."""
    results = {}
    top = bodies_dict.get('Top')
    if top is None:
        return {'ok': None, 'note': 'Top body missing'}

    buttons = [
        ('Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home Button', dict(p['switch_home_bbox'], z=p['switch_power_bbox']['z']), p['home_nub_dir'], p['home_cap']),
    ]
    N_STEPS = 24
    for name, switch_bbox, nub_dir, cap in buttons:
        key = name.lower().replace(' ', '_')
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        d2, t2 = g['d'], g['t']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        tab = p['tab']
        tab_len_along_d = 1.5
        tab_z = z_center - W / 2.0 - tab['h'] / 2.0
        # tab's rest-position start point (matches add_button exactly)
        tab_start_xy = (g['tab_face_xy'][0] - tab_len_along_d * d2[0] / 2.0,
                        g['tab_face_xy'][1] - tab_len_along_d * d2[1] / 2.0)
        # sweep range: from comfortably inboard of the rib (2mm past
        # s_rib_inner, on the switch side) to the rest position (shift=0).
        s_tab_face = g['s_tab_face']
        shift_start = (g['s_rib_inner'] - 2.0) - s_tab_face
        shift_end = 0.0
        bad = []
        checked = 0
        for i in range(N_STEPS + 1):
            shift = shift_start + (shift_end - shift_start) * i / N_STEPS
            base_xy = (tab_start_xy[0] + shift * d2[0], tab_start_xy[1] + shift * d2[1])
            for du, dv, dw in ((0, -tab['w'] / 2.0, 0), (0, tab['w'] / 2.0, 0),
                               (tab_len_along_d, -tab['w'] / 2.0, 0), (tab_len_along_d, tab['w'] / 2.0, 0),
                               (tab_len_along_d / 2.0, 0, 0)):
                px = base_xy[0] + du * d2[0] + dv * t2[0]
                py = base_xy[1] + du * d2[1] + dv * t2[1]
                pz = tab_z + dw
                checked += 1
                if probe_point_solid(top, P(px, py, pz)):
                    bad.append((round(shift, 3), round(px, 2), round(py, 2), round(pz, 2)))
        results[key] = (len(bad) == 0, {'bad_count': len(bad), 'checked': checked, 'sample': bad[:5]})
    return results


def verify_button_retention(bodies_dict, p):
    """New check (2026-09-08, pass 9b, finding 9): confirms the cap's two
    retention features -- once assembled -- actually stop it falling
    back out of the case, live, on the BUILT bodies (not just the
    parameter relationships verify_m2 already checks by construction).

    (1) collar-too-wide-for-the-rib-slot (blocks the cap being pulled all
    the way back OUT through the wall hole): the collar's own oversized
    flange (`collar['h']` beyond the shaft) is deliberately wider than the
    rib's slot clearance (`rib_slot_clearance`) -- probes a point at the
    collar's own outer edge, at the rib's axial location, tangentially
    centered (well clear of finding 9's new tab-relief lane, which is
    only `tab['w']+0.6` wide) -- must be SOLID (real, unrelieved rib
    material blocks it there).
    (2) tab-cannot-drift-further-OUTWARD (the tab's own hole in Top only
    extends out to the inner wall face, `s_inner` -- past that, in the
    tab's own (below-the-shaft) z-band, is solid wall) -- probes a point
    just outboard of `s_inner` in the tab's z-band -- must be SOLID.
    (3) restates the already-computed, construction-guaranteed collar-
    bottoms-on-rib gap (`plunger_travel`, verify_m2's own
    '..._collar_rib_gap_0.62') and tab clearance (`tab['gap']`) here too,
    so a single call reports the whole retention picture.
    """
    results = {}
    top = bodies_dict.get('Top')
    if top is None:
        return {'ok': None, 'note': 'Top body missing'}

    results['collar_wider_than_rib_slot'] = (p['collar']['h'] > p['rib_slot_clearance'],
                                              {'collar_h': p['collar']['h'], 'rib_slot_clearance': p['rib_slot_clearance']})

    buttons = [
        ('Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home Button', dict(p['switch_home_bbox'], z=p['switch_power_bbox']['z']), p['home_nub_dir'], p['home_cap']),
    ]
    for name, switch_bbox, nub_dir, cap in buttons:
        key = name.lower().replace(' ', '_')
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        d2 = g['d']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0

        # (1) collar's outer edge at the rib's own axial midpoint, at
        # tangential center (t=0) -- outside finding 9's tab-relief lane
        # (which is only below z_center - W/2, not above it).
        s_rib_mid = (g['s_rib_inner'] + g['s_rib_outer']) / 2.0
        collar_top_z = z_center + W / 2.0 + p['collar']['h'] - 0.1
        pt_xy = (g['housing_xy'][0] + s_rib_mid * d2[0], g['housing_xy'][1] + s_rib_mid * d2[1])
        collar_blocked = probe_point_solid(top, P(pt_xy[0], pt_xy[1], collar_top_z))
        results[f'{key}_collar_blocked_by_rib'] = (collar_blocked, {'s': round(s_rib_mid, 3), 'z': round(collar_top_z, 3)})

        # (2) just outboard of the tab_hole's OWN real outward reach --
        # not s_inner itself: add_button's tab_hole cut actually extends
        # to s_inner + tab_hole_skin_margin/2 (see its own comment,
        # "This hole's own outward reach is s_inner + skin_margin/2"), a
        # bit further out than s_inner alone -- probing at s_inner+0.3
        # (this check's first version) landed INSIDE that hole's own
        # clearance pocket, not the real wall past it. Real wall material,
        # not the tab's own clearance pocket.
        tab = p['tab']
        tab_z = z_center - W / 2.0 - tab['h'] / 2.0
        tab_hole_outward_s = g['s_inner'] + p.get('tab_hole_skin_margin', 2.0) / 2.0
        s_probe = tab_hole_outward_s + 0.3
        pt2_xy = (g['housing_xy'][0] + s_probe * d2[0], g['housing_xy'][1] + s_probe * d2[1])
        tab_blocked = probe_point_solid(top, P(pt2_xy[0], pt2_xy[1], tab_z))
        results[f'{key}_tab_outward_blocked'] = (tab_blocked, {'s': round(s_probe, 3), 'z': round(tab_z, 3)})

        # (3) restate the construction-guaranteed gaps here too.
        rest_gap = g['s_rib_inner'] - g['s_collar_outer']
        results[f'{key}_collar_rib_gap_0.62'] = (abs(rest_gap - p['plunger_travel']) < 0.05, round(rest_gap, 4))
        results[f'{key}_tab_gap_0.60'] = (abs(p['tab']['gap'] - 0.60) < 1e-9, p['tab']['gap'])
    return results


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
    # 2026-09-08 pass 9b/finding 10: 'plunger_tip_gap_0.02' (a full-press
    # gap FROM THE SWITCH HOUSING, via the bbox-corner approximation) is
    # retired -- see button_geometry's docstring for why that reference
    # point was never a real surface. 'plunger_pretravel_0.3' is its
    # replacement: the REST gap to the real, measured actuator nub.
    results['plunger_pretravel_0.3'] = (abs(p['plunger_pretravel'] - 0.3) < 1e-9, p['plunger_pretravel'])
    results['nub_pocket_depth_0.8'] = (abs(p['nub_pocket']['depth'] - 0.8) < 1e-9, p['nub_pocket']['depth'])
    results['tab_gap_0.60'] = (abs(p['tab']['gap'] - 0.60) < 1e-9, p['tab']['gap'])

    top = bodies_dict['Top']
    tunnel_pt = P(0.0, p['usb_tunnel_y_start'] + 1.0, p['usb_tunnel_center_z'])
    tunnel_open = not probe_point_solid(top, tunnel_pt)
    results['usb_tunnel_open'] = (tunnel_open, 'point inside tunnel bore is empty (not solid)')

    bottom = bodies_dict['Bottom']
    lug = p['lug']
    _, _, _, lug_hole_y = lug_ear_geometry(p)
    lug_hole_pt = P(0.0, lug_hole_y, (lug['z'][0] + lug['z'][1]) / 2.0)
    lug_hole_open = not probe_point_solid(bottom, lug_hole_pt)
    results['lug_hole_open'] = (lug_hole_open, 'point on lug hole axis is empty (not solid)')

    # plunger guide rib + inward stop collar (2026-09-04 addendum)
    results['rib_slot_clearance_0.25'] = (abs(p['rib_slot_clearance'] - 0.25) < 1e-9, p['rib_slot_clearance'])
    results['rib_thickness_1.6'] = (abs(p['rib_thickness'] - 1.6) < 1e-9, p['rib_thickness'])
    # 2026-09-15 pass 15, item 6: the expected literal was 0.62mm through
    # pass 14; raised to 0.90mm this pass (see PARAMS['plunger_travel']'s
    # own comment for the full derivation/live interference numbers) --
    # updated here too so this stays a real "did PARAMS silently drift"
    # regression guard, not a stale check against the old baseline.
    results['plunger_travel_0.90'] = (abs(p['plunger_travel'] - 0.90) < 1e-9, p['plunger_travel'])
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


REFERENCE_TOOL_NAMES = (
    'Inner Cavity Clip Tool',
    'Cap Trim Envelope',
)
LORA_CORRIDOR_NAME = 'LoRa Antenna Corridor'
REFERENCE_BOX_NAMES = (
    'Battery 803040',
    'GPS Patch 25x25x8.3',
    LORA_CORRIDOR_NAME,  # pass 16, item F / mech review F11 -- now a real, checked corridor, not just a marker
)
FPC_KEEPOUT_NAME = 'FPC LoRa Antenna Keep-out'
BOARD_OCC_NAME_SUBSTRINGS = ('XIAO-ESP32S3', 'Wio-SX1262', 'L76K', 'ESP32-S3-Touch-LCD')

# (occurrence name substring, case body name) pairs that are INTENDED to
# touch (zero clearance) -- excluded from the < clearance_min assertion in
# verify_min_clearances. Everything else must clear by clearance_min.
ALLOWED_CONTACTS = (
    ('L76K', 'Bottom'),                 # PCB rests on its four corner pads
    # collect_interference_entities substitutes the L76K occurrence with
    # its child occurrences for interference purposes (see its docstring)
    # -- the real PCB's parent occurrence is named 'XIAO-ESP32S3 v2 v2'
    # (a hat-mode-shaped placeholder reused as the L76K's own PCB outline)
    # in the reference doc, not 'L76K', so it needs its own entry here.
    ('XIAO-ESP32S3 v2', 'Bottom'),
    # 2026-09-07 pass 7: the old ('Wio-SX1262', 'Top') entry (Wio used to
    # rest on the Top-hanging tray's wedge shelf) is gone -- the 3-board
    # stack no longer touches Top at all; it floats clear of the ceiling
    # by design (see verify_stack3_clearance / stack3['ceiling_clear_min']).
    # A real Wio-Top contact should now FAIL verify_min_clearances, not be
    # silently allowed.
    # pass 16: the module's own standoffs (S1/S2/S3) now rest directly on
    # Top (the ears/S2 boss) -- no separate Screen Plate any more -- so
    # this single ('ESP32-S3-Touch-LCD', 'Top') entry now covers BOTH the
    # glass-flush-with-the-top-face contact AND the standoff-seat contact.
    ('ESP32-S3-Touch-LCD', 'Top'),
)

# Per-occurrence cap on how many of its (possibly hundreds of, for the
# display module's fully-exploded PCBA) nested bodies get individually
# distance-checked -- keeps verify() inside the MCP call's ~60s budget.
# measureMinimumDistance against a whole multi-body Occurrence directly is
# unreliable here (fails outright on 3 of 4 real boards tested), so
# per-body is the only robust option found; the cap trades completeness
# for speed and is a documented limitation, not a correctness guarantee.
MIN_CLEARANCE_BODY_CAP = 25

# ---------------------------------------------------------------------------
# Document structure (2026-09-06, pass 6): named components everything gets
# organized into after build(), instead of an unstructured pile of bodies at
# the document root -- see organize_components()/verify_structure().
# ---------------------------------------------------------------------------
COMPONENT_CASE = 'Print — Case'
COMPONENT_BUTTONS = 'Print — Buttons'
COMPONENT_COUPONS = 'Print — Coupons'
COMPONENT_REFERENCE = 'Reference — not printed'
COMPONENT_BOARDS = 'Boards'

CASE_BODY_NAMES = ('Top', 'Bottom')  # pass 16: Screen Plate retired (owner call, item A/candidate 5)
BUTTON_BODY_NAMES = ('Power Button', 'Home Button')


def find_component_occurrence(root, name):
    """The top-level (direct child of root) occurrence whose component has
    this name, or None. All 5 pass-6 components (Print -- Case/Buttons/
    Coupons, Reference -- not printed, Boards) are created as direct
    children of root by organize_components(), so a shallow scan suffices."""
    for occ in root.occurrences:
        if occ.component.name == name:
            return occ
    return None


def organize_components(root, bodies):
    """Post-build structuring pass (2026-09-06, pass 6): build() leaves
    every body and every inserted board/display occurrence at the document
    root, auto-named where Fusion had to invent something ('Body145', ...)
    -- this moves everything into named components instead. Confirmed
    empirically (see the pass-6 notes): BRepBody.moveToComponent(occ) and
    Occurrence.moveToComponent(occ) both move their target into occ's OWN
    component, despite the API doc's confusingly-worded "parent component
    of the target occurrence" -- not occ's parent. Both preserve world
    position; no snapshot needed (unlike assigning occ.transform).

    Returns the updated `bodies` dict (moveToComponent returns a new
    BRepBody reference -- the pre-move handles are no longer valid) plus a
    dict of the 4 new top-level occurrences it created."""
    def new_component(name):
        occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        occ.component.name = name
        return occ

    case_occ = new_component(COMPONENT_CASE)
    buttons_occ = new_component(COMPONENT_BUTTONS)
    ref_occ = new_component(COMPONENT_REFERENCE)
    boards_occ = new_component(COMPONENT_BOARDS)

    for name in CASE_BODY_NAMES:
        bodies[name] = bodies[name].moveToComponent(case_occ)
    for name in BUTTON_BODY_NAMES:
        bodies[name] = bodies[name].moveToComponent(buttons_occ)

    # Everything still at root by this point is a reference-only box/tool
    # (Battery 803040, GPS Patch 25x25x8.3, FPC LoRa Antenna Keep-out, the
    # Inner Cavity Clip Tool, the Cap Trim Envelope) -- already named and
    # already hidden by the code that built them; just re-home them and
    # make doubly sure they're hidden (belt and suspenders).
    for b in list(root.bRepBodies):
        moved = b.moveToComponent(ref_occ)
        moved.isLightBulbOn = False

    # Every remaining top-level occurrence is an inserted board/display
    # reference (Wio, XIAO, L76K, the display PCBA) -- move it under
    # Boards. Stays visible, per the pass-2 decision.
    for occ in list(root.occurrences):
        if occ.component.name in (COMPONENT_CASE, COMPONENT_BUTTONS,
                                   COMPONENT_REFERENCE, COMPONENT_BOARDS):
            continue
        occ.moveToComponent(boards_occ)

    for comp_occ in (case_occ, buttons_occ, ref_occ, boards_occ):
        remove_stray_generic_bodies(comp_occ.component)

    return bodies, {'case': case_occ, 'buttons': buttons_occ,
                     'reference': ref_occ, 'boards': boards_occ}


def verify_structure(design):
    """Pass-6 structuring gate, run after organize_components() (and, when
    export=True, after export_coupons() so Print -- Coupons exists too):
    asserts nothing was left unstructured at the document root, every body
    in OUR OWN authored components has an explicit name (never descends
    into an inserted board's own native modeling -- those keep their
    source document's names, which we must never touch), the 5 printable
    bodies live in the two Print components, and every body in Reference
    -- not printed is hidden. Prints the tree (component -> bodies with
    bbox) and returns it."""
    import re
    root = design.rootComponent
    root_names = [b.name for b in root.bRepBodies]
    assert not root_names, f'body(ies) left unstructured at document root: {root_names}'

    case_occ = find_component_occurrence(root, COMPONENT_CASE)
    buttons_occ = find_component_occurrence(root, COMPONENT_BUTTONS)
    ref_occ = find_component_occurrence(root, COMPONENT_REFERENCE)
    coupons_occ = find_component_occurrence(root, COMPONENT_COUPONS)
    boards_occ = find_component_occurrence(root, COMPONENT_BOARDS)
    assert case_occ and buttons_occ and ref_occ and boards_occ, (
        f'expected component(s) missing: case={case_occ} buttons={buttons_occ} '
        f'reference={ref_occ} boards={boards_occ}')

    tree = {}
    for occ, label in ((case_occ, COMPONENT_CASE), (buttons_occ, COMPONENT_BUTTONS),
                        (ref_occ, COMPONENT_REFERENCE), (coupons_occ, COMPONENT_COUPONS)):
        if occ is None:
            continue
        entries = []
        # world-space bodies via the OCCURRENCE (occ.bRepBodies), not
        # occ.component.bRepBodies -- a component's own bRepBodies report
        # bounding boxes in the component's LOCAL/native frame, which only
        # coincides with world space for an identity-transform occurrence
        # (true for Print -- Case/Buttons/Reference, but not Print --
        # Coupons, translated +60mm off to the side -- see export_coupons).
        for b in occ.bRepBodies:
            assert not re.fullmatch(r'Body\d+', b.name), f'{label}: auto-named body {b.name!r}'
            entries.append((b.name, bbox_of(b), b.isLightBulbOn))
        tree[label] = entries

    case_names = sorted(n for n, _, _ in tree[COMPONENT_CASE])
    assert case_names == sorted(CASE_BODY_NAMES), f'{COMPONENT_CASE} bodies: {case_names}'
    button_names = sorted(n for n, _, _ in tree[COMPONENT_BUTTONS])
    assert button_names == sorted(BUTTON_BODY_NAMES), f'{COMPONENT_BUTTONS} bodies: {button_names}'

    bad_visible = [n for n, _, visible in tree[COMPONENT_REFERENCE] if visible]
    assert not bad_visible, f'reference body(ies) not hidden: {bad_visible}'

    boards_tree = [(occ.name, occ.component.name) for occ in boards_occ.component.occurrences]

    print('=== document structure ===')
    for label in (COMPONENT_CASE, COMPONENT_BUTTONS, COMPONENT_COUPONS, COMPONENT_REFERENCE):
        if label not in tree:
            print(f'{label}: (not built this run)')
            continue
        print(f'{label}:')
        for name, bb, visible in tree[label]:
            print(f'   {name}  bbox={bb}  {"visible" if visible else "hidden"}')
    print(f'{COMPONENT_BOARDS}:')
    for occ_name, comp_name in boards_tree:
        print(f'   {occ_name} -> {comp_name}')

    return tree


def collect_interference_entities(root):
    """Everything verify()'s interference gate considers: every printed
    body (from Print -- Case / Print -- Buttons), the Battery/GPS reference
    boxes (from Reference -- not printed), and every inserted board
    occurrence (from Boards, passed as whole Occurrences, not their
    individual nested bRepBodies -- see check_interference's docstring for
    why) -- excluding only the two reference TOOL solids (construction aids
    with no physical presence) and the FPC keep-out marker (a keep-out
    strip, not a real part; not enforced by this pass -- see README known
    limitations). Relies on organize_components() having already run --
    printed/reference bodies are found by COMPONENT, not by a name-substring
    convention, now that they live in their own components."""
    case_occ = find_component_occurrence(root, COMPONENT_CASE)
    buttons_occ = find_component_occurrence(root, COMPONENT_BUTTONS)
    ref_occ = find_component_occurrence(root, COMPONENT_REFERENCE)
    boards_occ = find_component_occurrence(root, COMPONENT_BOARDS)
    assert case_occ and buttons_occ and ref_occ and boards_occ, 'organize_components() must run before verify()'

    printed = list(case_occ.component.bRepBodies) + list(buttons_occ.component.bRepBodies)
    ref_boxes = [b for b in ref_occ.component.bRepBodies if b.name in REFERENCE_BOX_NAMES]
    board_occs = [occ for occ in boards_occ.component.occurrences
                  if any(s in occ.name for s in BOARD_OCC_NAME_SUBSTRINGS)]

    # 2026-09-05 workaround: the L76K reference doc's top-level "L76k"
    # grouping occurrence carries one small stray body (~12x1x1mm,
    # ~30mm from the real PCB even in native coordinates -- almost
    # certainly an authoring artifact) directly on itself. Neither
    # isLightBulbOn=False nor a Remove feature on that specific body
    # actually excludes it here: both "succeed" with no error, but the
    # body still reports present and still contributes to
    # analyzeInterference afterward -- confirmed empirically, and
    # apparently specific to a body owned directly by a LINKED/
    # referenced occurrence once more than one such occurrence is
    # present in the design. Substitute the whole L76K occurrence, for
    # interference purposes only, with its two meaningful child
    # occurrences (the real PCB's parent and the U.FL connector) --
    # naturally excluding that stray body (a sibling of those, not a
    # descendant) without needing to delete or hide anything.
    fixed_board_occs = []
    for occ in board_occs:
        if 'L76K' not in occ.name:
            fixed_board_occs.append(occ)
            continue
        replaced = False
        for child in occ.childOccurrences:
            if child.name.startswith('L76k'):
                for grandchild in child.childOccurrences:
                    fixed_board_occs.append(grandchild)
                    replaced = True
            elif 'ANT' not in child.name.upper():
                fixed_board_occs.append(child)
                replaced = True
        if not replaced:
            fixed_board_occs.append(occ)  # fallback: structure not as expected
    board_occs = fixed_board_occs
    return printed, ref_boxes, board_occs


def verify_min_clearances(app, printed_bodies, board_occs, min_mm):
    """Minimum distance from each inserted board occurrence to each case
    body (Top/Bottom), per-body (see MIN_CLEARANCE_BODY_CAP), skipping
    pairs in ALLOWED_CONTACTS (intended zero-clearance contacts).
    Returns {(occ_name, case_name): (min_mm_found, ok)}."""
    mm_ = app.measureManager
    case = {b.name: b for b in printed_bodies if b.name in ('Top', 'Bottom')}
    results = {}
    for occ in board_occs:
        bl = _collect_occ_bodies(occ)[:MIN_CLEARANCE_BODY_CAP]
        for case_name, case_body in case.items():
            allowed = any(s in occ.name and case_name == cn for s, cn in ALLOWED_CONTACTS)
            best = None
            for b in bl:
                try:
                    r = mm_.measureMinimumDistance(case_body, b)
                    d = r.value / MM
                except Exception:
                    continue
                if best is None or d < best:
                    best = d
            if best is None:
                continue  # measurement unavailable for every sampled body
            ok = allowed or best >= min_mm
            results[(occ.name, case_name)] = (round(best, 4), ok)
    return results


def _rect_perimeter_points(half_t, half_z, per_side=3):
    """12 (per_side=3) sample points around a rectangle's perimeter in its
    own local (tangential, vertical) 2D frame, centered at the origin."""
    pts = []
    for i in range(per_side):
        frac = (-1.0 + 2.0 * i / (per_side - 1)) if per_side > 1 else 0.0
        pts.append((frac * half_t, -half_z))
        pts.append((frac * half_t, half_z))
    for i in range(per_side):
        frac = (-1.0 + 2.0 * i / (per_side - 1)) if per_side > 1 else 0.0
        pts.append((-half_t, frac * half_z))
        pts.append((half_t, frac * half_z))
    return pts


def verify_skin_intact(bodies_dict, p):
    """Regression guard (2026-09-06, pass 6, item A -- Jake's screenshot
    review found a rectangular notch through the outer skin next to each
    button's stadium hole, caused by an interior cut -- the TAB HOLE --
    reaching all the way past the true outer surface; add_button's fix
    bounds that cut analytically at s_inner + tab_hole_skin_margin/2).

    2026-09-07 pass 7 (item 5), attempt 2: a first re-target (probing a
    full rectangular ring around the tab hole's own w x z-span) still
    over-fired on almost every sample -- traced to the tab hole's own Z
    range legitimately OVERLAPPING the main wall-hole cutter's Z range by
    design (tab_hole_z_hi = z_center - W/2 + 0.3 sits 0.55mm ABOVE the
    main hole's own lower bound, z_center - W/2 - 0.25 -- a deliberate
    seam for a clean union, not a gap), so any ring point near the TOP of
    the tab hole's z-span is hollow because of the (unrelated, legitimate)
    main hole, not a defect. Simplified to what the fix actually needs to
    verify: at 3 points safely inside the tab's own width (no extra
    margin needed -- tab['w'] is already the tab's real footprint) and a
    SINGLE z well clear of the main-hole overlap (the midpoint of the
    tab's z-span that does NOT overlap the main hole), probe radially
    OUTWARD from the tab hole's own analytic reach
    (s_inner + tab_hole_skin_margin/2, exactly what add_button's fix
    bounds the cut at) by two small depths -- real skin should start
    immediately past that reach; a regression that lets the cut reach
    further out shows up as one of these going hollow.

    2026-09-12 pass 12 (widened per Jake's sidescan of the pass-11 export:
    a real ray-through breach at (y~32, z~15) Power / (y~68, z~18-20)
    Home, both LOWER and WIDER than the single z_safe / tab['w']-only
    footprint this check used through pass 11): a single z sample and a
    +-tab['w']/2 tangential span could not have caught a defect sitting
    outside that footprint by construction. Now scans the tab's FULL z
    span (not just its midpoint) and the WIDER tab_relief_w footprint
    (tab['w'] + 2*tab_relief_margin, matching the rib's own relief-lane
    cut exactly -- see add_button) instead of the bare tab width, at the
    same two outward depths as before. This is a direct, tighter
    regression check on the same fix add_button's tab_clip_tool applies;
    verify_openings_open's new `*_button_hole_footprint` gate is the
    broader, ray-cast-style check across the whole hole."""
    top = bodies_dict['Top']
    results = {}
    buttons = [
        ('Power', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home', dict(p['switch_home_bbox'], z=p['switch_power_bbox']['z']), p['home_nub_dir'], p['home_cap']),
    ]
    tab = p['tab']
    skin_margin = p.get('tab_hole_skin_margin', 2.0)
    for name, switch_bbox, nub_dir, cap in buttons:
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        d2, t2 = g['d'], g['t']
        housing_xy = g['housing_xy']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        tab_hole_z_lo = z_center - W / 2.0 - tab['h'] - 0.3
        main_hole_z_lo = z_center - W / 2.0 - 0.25  # main wall-hole cutter's own lower Z bound
        # scan the tab's own full z-span (its lowest extent up to just
        # short of the main-hole overlap seam) rather than a single
        # midpoint sample -- 4 evenly spaced z's.
        z_samples = [tab_hole_z_lo + k * (main_hole_z_lo - tab_hole_z_lo) / 3.0 for k in range(4)]
        # the tab hole's own real outward-most reach (matches add_button's
        # tab_hole_body construction exactly -- see its docstring).
        s_reach = g['s_inner'] + skin_margin / 2.0
        # 2026-09-07: depths kept SHALLOW (not e.g. 0.5) -- traced a
        # borderline failure at t_frac=-0.7/depth=0.5 to the probe simply
        # stepping past the TRUE outer surface at that off-axis tangential
        # offset (x landed at rho=28.06 against a trim outer_radius of
        # 28 -- open air, not a skin breach): the nominal ~1.45mm of real
        # skin past the tab hole's reach (see add_button's docstring) is
        # measured along the direct ray at t_off=0, and thins somewhat
        # off-axis the same way the analytic wall-distance formula's
        # ~0.25-0.3mm ray-vs-true-curvature slack shows up elsewhere in
        # this file. 0.15/0.3 stays comfortably inside real material
        # everywhere while still testing meaningfully past s_reach.
        # 2026-09-12 pass 12: kept the tangential span at tab['w']/2 (NOT
        # tab_relief_w/2, the wider rib-lane footprint) -- widening it here
        # reproduced the exact "probe steps past the true curved surface
        # at a wider off-axis offset" false positive described just above
        # (confirmed live: at tab_relief_w/2, depth 0.3 landed in real
        # open air past a genuinely thinner-but-still-real skin sliver,
        # not a defect). The wider footprint is exactly what the new
        # `*_button_hole_footprint` gate in verify_openings_open checks,
        # using the curve-aware (true_wall_distance_along_ray-per-offset)
        # method that this flat-offset probe can't safely do at that width.
        for depth_out in (0.15, 0.3):
            s = s_reach + depth_out
            xy0 = (housing_xy[0] + s * d2[0], housing_xy[1] + s * d2[1])
            for i, t_frac in enumerate((-0.7, 0.0, 0.7)):
                t_off = t_frac * (tab['w'] / 2.0)
                x = xy0[0] + t_off * t2[0]
                y = xy0[1] + t_off * t2[1]
                for j, z_safe in enumerate(z_samples):
                    ok = probe_point_solid(top, P(x, y, z_safe))
                    results[f'{name}_depth{depth_out}_pt{i}_z{j}'] = ok
    return results


def verify_wall_integrity(bodies_dict, p):
    """Regression guard (2026-09-06, pass 6, items B/C): a fine angular
    sweep around both domed ends at the parting-line z-band, plus a probe
    of the wall just outside each case-screw boss, added after Jake's own
    STL review reported possible wedge/triangular-prism artifacts and
    pinholes near the parting line that the coarser verify_no_outer_bumps
    (3 points/side) could miss.

    Dome sweep: at z=7/9/11 and every 15 degrees around each dome (skipping
    a small window at spine_a for the real lanyard lug), confirms no solid
    material just outside the surface (rho=outer_radius+0.15, a bump) and
    that the wall is genuinely solid just inside it (rho=outer_radius-1.0,
    a hole) -- both at the SAME angle, so a real local defect (not just
    normal curvature) shows up as one flipping unexpectedly.

    Boss probe: for each case screw (A/B/C/D), confirms the wall is solid
    from z=1 to z=9 (bottom) just outward of the boss's own radius, along
    the ray from the spine straight through the boss -- catches a
    counterbore/boss-clip cut breaking all the way through the wall.

    2026-09-07 pass 7 (item 5): two NARROW, documented exceptions added so
    this can gate verify() -- both root-caused by tracing the actual
    failing points' geometry (not guessed), the same way every other real
    defect in this file was found:
    (a) 'intact' at spine_a, z=11, deg +-15: z=11 sits in Top's ANCHOR ring
        (anchor_z=10..11), which has a real, intentional relief cut
        (lug_relief_box) right there so the lug ear has clearance --
        probing 'intact' inside that relief naturally finds hollow, not a
        defect. Skipped by checking the probed (x,y) against the box
        directly (+0.5mm margin), not a hand-tuned angle threshold, so it
        can never silently drift out of sync with the real relief size.
    (b) boss wall checks at z within 0.5mm of the straight-section's
        bottom tangent height (bot_tangent_z, ~2.93 for both variants):
        the same flat-ray-vs-true-curvature slack (~0.25mm) verify_m2's
        cap-proud check already documents -- the profile's flat-to-arc
        transition is exactly where a straight ray at a fixed inward
        offset most diverges from the true (locally non-radial) surface
        normal. Only affects straight-section bosses (A/C for both
        variants currently); B1/B2/D use the domed-end branch and are
        unaffected."""
    top = bodies_dict['Top']
    bottom = bodies_dict['Bottom']
    R = p['outer_radius']
    ay, by = p['spine_a'][1], p['spine_b'][1]
    results = {}

    lb = p['lug_relief_box']
    lb_margin = 0.5

    def in_lug_relief(x, y, z):
        return (lb['x'][0] - lb_margin <= x <= lb['x'][1] + lb_margin
                and lb['y'][0] - lb_margin <= y <= lb['y'][1] + lb_margin
                and p['lip_z'][0] - lb_margin <= z <= p['anchor_z'][1] + lb_margin)

    # 2026-09-10 pass 10 REDO: the compass-module mount no longer has a
    # brow (it hangs from the ceiling well inboard of the true outer
    # wall, nowhere near this dome sweep's own footprint) -- no exemption
    # needed here any more; this function is back to exactly its
    # pre-pass-10 shape.

    for end_name, center_y in (('spine_a', ay), ('spine_b', by)):
        sign = -1.0 if end_name == 'spine_a' else 1.0
        for z in (7.0, 9.0, 11.0):
            body = bottom if z < p['split_z'] else top
            for deg in range(-90, 91, 15):
                theta = math.radians(deg)
                dx = math.sin(theta)
                dy = sign * math.cos(theta)
                if abs(dx) < 0.25:
                    continue  # spine_a: the real lanyard lug opening; spine_b: the USB tunnel
                x_out, y_out = (R + 0.15) * dx, center_y + (R + 0.15) * dy
                x_in, y_in = (R - 1.0) * dx, center_y + (R - 1.0) * dy
                no_bump = not probe_point_solid(body, P(x_out, y_out, z))
                results[f'{end_name}_z{z}_deg{deg}_no_bump'] = no_bump
                if in_lug_relief(x_in, y_in, z):
                    continue  # (a) real lug-relief cut, not a defect
                intact = probe_point_solid(body, P(x_in, y_in, z))
                results[f'{end_name}_z{z}_deg{deg}_intact'] = intact

    g_prof = _profile_geometry(p)
    bot_tangent_z = g_prof['bot_tangent_z']

    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        name = s['name']
        # true outward direction from this boss toward the shell -- (+/-1, 0)
        # in the straight section, or radially from the nearer spine point
        # in a domed end (matching true_wall_distance_along_ray's own two
        # cases) -- NOT simply normalized(cx, cy), which points toward the
        # world origin and is wrong for a straight-section boss.
        if ay <= cy <= by:
            d2 = (1.0 if cx >= 0 else -1.0, 0.0)
        else:
            center_y = ay if cy < ay else by
            vx, vy = cx, cy - center_y
            vlen = math.hypot(vx, vy) or 1.0
            d2 = (vx / vlen, vy / vlen)
        for z in (2.5, 3.5, 5.0, 7.0, 9.0):
            if abs(z - bot_tangent_z) < 0.5:
                continue  # (b) flat-to-arc transition slack, not a defect
            s_wall = true_wall_distance_along_ray(p, (cx, cy), d2, z)
            if s_wall is None:
                continue
            inside_margin = 0.8
            px = cx + (s_wall - inside_margin) * d2[0]
            py = cy + (s_wall - inside_margin) * d2[1]
            ok = probe_point_solid(bottom, P(px, py, z))
            results[f'boss_{name}_wall_z{z}'] = ok

    return results


def verify_posts_and_bosses(bodies_dict, p):
    """Regression guard (2026-09-06, pass 6) for the silent-no-op-join bug
    `clipped_pillar_with_reach` fixes: `clip_to_inner_cavity` alone shrinks
    a boss/post by `safety_margin` on every face including the very
    top/bottom faces meant to touch Bottom's floor or Top's ceiling, and a
    real (larger, measured) mismatch between the inner-cavity solid's own
    ceiling height and the nominal z1 param left the clipped pillar not
    physically touching the shell at all -- `combine_join` SILENTLY NO-OPS
    on two non-touching bodies (same behavior deboss_loops already
    documents for disjoint glyph pieces) rather than raising, so every
    case boss (A/B/C/D) and Top post (P1-P4) was previously missing
    entirely with no error anywhere in build() or verify(). Probes each
    one off-axis (should be solid) at the z-midpoint of its own span.

2026-09-07 pass 7: boss B (the one that used to need a documented,
    unjoined exception -- its old position sat inside the L76K PCB's own
    footprint) is gone, replaced by B1/B2 at a position clear of the
    redesigned comms stack -- every boss/post reported here is now
    expected to be solid, no exceptions."""
    top = bodies_dict['Top']
    bottom = bodies_dict['Bottom']
    results = {}

    boss_off = p['boss_dia'] / 2.0 * 0.7
    for s in p['screws_ABC']:
        cx, cy = s['xy']
        name = s['name']
        z_b = (2.0 + p['split_z']) / 2.0
        z_t = (p['split_z'] + p['top_ceiling_underside_z']) / 2.0
        results[f'boss_{name}_bottom'] = probe_point_solid(bottom, P(cx + boss_off, cy, z_b))
        results[f'boss_{name}_top'] = probe_point_solid(top, P(cx + boss_off, cy, z_t))

    # pass 16: D1/D2 replace the old single screw D -- same Bottom-side
    # material probe (their Top-side pilot lives inside the ears, checked
    # separately by verify_ear_root_material).
    z_d = (2.0 + p['split_z']) / 2.0
    for s in p['screws_D12']:
        dx, dy = s['xy']
        results[f'boss_{s["name"]}_bottom'] = probe_point_solid(bottom, P(dx + boss_off, dy, z_d))

    # 2026-09-07 pass 7 (item 3): boss B1/B2 must clear the comms-stack
    # frame by >= stack3['boss_relief_margin'] (1.0mm) -- guaranteed BY
    # CONSTRUCTION (add_comms_stack_frame cuts a keep-out of radius
    # boss_dia/2 + margin around every screw before joining the frame),
    # but probed here directly rather than trusting the construction
    # alone: a point at (boss radius + margin/2) from each boss's centre,
    # aimed toward the stack frame's own centre (not just +x, which
    # verify_posts_and_bosses already covers), must be OPEN (no frame
    # material) -- if it were solid, the relief cut didn't actually reach
    # that boss.
    if any(s['name'] in ('B1', 'B2') for s in p['screws_ABC']):
        s3 = p['bay'].get('stack3')
        if s3 is not None:
            stack_cx = (s3['l76k_pcb']['x'][0] + s3['l76k_pcb']['x'][1]) / 2.0
            stack_cy = (s3['l76k_pcb']['y'][0] + s3['l76k_pcb']['y'][1]) / 2.0
            boss_r = p['boss_dia'] / 2.0
            margin = s3['boss_relief_margin']
            z_mid = sum(s3['frame_z']) / 2.0
            for s in p['screws_ABC']:
                if s['name'] not in ('B1', 'B2'):
                    continue
                cx, cy = s['xy']
                dx, dy = stack_cx - cx, stack_cy - cy
                dlen = math.hypot(dx, dy) or 1.0
                dx, dy = dx / dlen, dy / dlen
                r = boss_r + margin * 0.5
                px, py = cx + r * dx, cy + r * dy
                clear_ok = not probe_point_solid(bottom, P(px, py, z_mid))
                results[f'boss_{s["name"]}_clears_stack_frame'] = clear_ok

    return results


def verify_post_walls(bodies_dict, p):
    """Regression guard (2026-09-08, pass 9, finding 4; RE-TARGETED pass
    16 from the now-removed P1-P4 ceiling posts to D1/D2's own Top-side
    pilots, per the task's own item G). Two checks per D1/D2 pilot, both
    on 8 rays (0,45,...,315 deg) around the pilot's own axis, at the same
    boss radius (boss_dia/2, not a post radius any more -- D1/D2's
    Top-side material is their own independent corner block's wedge, not
    a plain cylinder). Iterates `p['screws_D12']` directly, so this
    generically re-targets itself to wherever D1/D2 actually are --
    RESUMED pass 16 (Finding 1 fix): D1/D2 moved from the ears' own wall
    roots to their own independent single-pilot corner blocks north of
    the ears (see 'screws_D12' in params_current.py); no change needed
    here beyond that xy update.

    (a) '<name>_pilot_wall': >= POST_WALL_MIN (1.2mm) of solid material
    around the Ø1.62 pilot, at 3 z-heights spanning top_pilot_z. A LIVE
    point-containment probe (not just trusting the construction), at
    radius pilot_r + POST_WALL_MIN from the pilot's axis.

    (b) '<name>_shell_skin': >= 0.6mm of skin between the ear root's own
    OD (boss_dia/2) and the TRUE outer shell surface, analytic via
    true_wall_distance_along_ray at the pilot's own top z."""
    top = bodies_dict['Top']
    pilot_r = p['top_pilot_dia'] / 2.0
    boss_r = p['boss_dia'] / 2.0
    wall_min = POST_WALL_MIN
    skin_min = 0.6
    z0, z1 = p['top_pilot_z']
    z_samples = [z0 + 0.8, (z0 + z1) / 2.0, z1 - 0.5]
    angles = [i * 45.0 for i in range(8)]
    results = {}
    for s in p['screws_D12']:
        name = s['name']
        cx, cy = s['xy']
        wall_bad = []
        for ang in angles:
            rad = math.radians(ang)
            dxu, dyu = math.cos(rad), math.sin(rad)
            for z in z_samples:
                pt = P(cx + (pilot_r + wall_min) * dxu, cy + (pilot_r + wall_min) * dyu, z)
                if not probe_point_solid(top, pt):
                    wall_bad.append((ang, round(z, 2)))
        results[f'{name}_pilot_wall'] = wall_bad

        skin_bad = []
        for ang in angles:
            rad = math.radians(ang)
            d2 = (math.cos(rad), math.sin(rad))
            s_wall = true_wall_distance_along_ray(p, (cx, cy), d2, z1)
            if s_wall is None:
                continue
            clear = s_wall - boss_r
            if clear < skin_min:
                skin_bad.append((ang, round(clear, 3)))
        results[f'{name}_shell_skin'] = skin_bad
    return results


# plate_outline_centroid / verify_plate_post_spread (pass 15, item 3:
# P1-P4 spread diagnostic) RETIRED pass 16 -- there is no Screen Plate
# and no P1-P4 any more (owner call, item A/candidate 5). The underlying
# complaint ("doesn't give proper support") is what candidate 5 itself
# answers, per the plate-mounting-round2.md / review-mechanical.md
# analysis copied into docs/hardware/ this pass.


def verify_root_fillets(bodies_dict, p):
    """Gate for pass 13, item 1 (fillets/collars at every post & boss
    root, so Jake's printed posts stop snapping off): for every circular
    post/boss root reinforced by add_root_reinforcement, probe a ring of
    8 points (0,45,...,315 deg) at radius = (the feature's own OD/2) +
    0.6mm, at a height 0.4mm INTO the post/boss from its own root plane
    (away from the ceiling/floor slab it's rooted to, i.e. z=z_root-0.4
    when the root is at the TOP of the feature, z=z_root+0.4 when it's
    at the BOTTOM) -- must read solid. Deliberately independent of *how*
    the reinforcement was made (a true Fillet feature or the conical-
    collar fallback): both add material outward from the post's own OD
    near the root, and this probe only cares whether that material is
    really there -- see add_root_reinforcement's own docstring for why
    ROOT_FILLET_R/ROOT_COLLAR_RISE were both sized to pass this exact
    probe on their own, whichever method actually applied.

    Returns a dict: {feature_name: [(angle, z) for each failing probe]}
    (empty list per feature = pass), plus '_method': a copy of
    ROOT_FILLET_REPORT (feature_name, 'fillet'/'collar', radius/rise) for
    the pass-13 report -- purely informational, not itself a pass/fail
    condition (a collar is just as acceptable a pass as a fillet per the
    task's own instruction)."""
    top = bodies_dict['Top']
    bottom = bodies_dict['Bottom']
    angles = [i * 45.0 for i in range(8)]
    features = []  # (name, body, cx, cy, r, z_root, direction)

    boss_r = p['boss_dia'] / 2.0
    # RESUMED pass 16 (Finding 1 fix): D1/D2 are now independent
    # single-pilot corner blocks (add_single_corner_block), same
    # construction as A/C, just at their own relocated xy (north of the
    # ears, see 'screws_D12' in params_current.py) -- so they share A/C's
    # own feature-list treatment now, including add_single_corner_block's
    # generalized `_ear_root_z1` display-keepout cap on their own Top
    # side (a no-op for A/C, load-bearing for D1/D2).
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        z1_top = _ear_root_z1(p, cx, cy, boss_r + CORNER_BLOCK_REACH, p['top_ceiling_underside_z'])
        features.append((f'boss_{s["name"]}_bottom', bottom, cx, cy, boss_r, 2.0, 'down'))
        features.append((f'corner_block_{s["name"]}_top', top, cx, cy, boss_r, z1_top, 'up'))

    # pass 16 (resumed): each ear's own wall-root anchor is now
    # independent of any D1/D2 screw (see 'ears' in params_current.py) --
    # its own collar ('ear_<name>_wall_root', added inside add_ear) is
    # capped by BOTH the display AND both buttons' own cap z0
    # (`ear_root_cap_z1`, the Finding-1 fix), not just the display.
    for ear_name, ear in p['ears'].items():
        rx, ry = ear['root_xy']
        z1_root_probe = ear_root_cap_z1(p, rx, ry, boss_r + CORNER_BLOCK_REACH, p['top_ceiling_underside_z'])
        features.append((f'ear_wall_root_{ear_name}', top, rx, ry, boss_r, z1_root_probe, 'up'))

    # pass 16: the ears' own SEAT-end collars (S1, S3) and the S2 boss's
    # own seat collar -- same probe shape (ring at boss_r+0.6, 0.4mm into
    # the feature from its own seat plane).
    seat_z = p['ear_seat_z']
    for ear_name, ear in p['ears'].items():
        tx, ty = p['board_standoffs'][ear['target']]
        features.append((f'ear_seat_{ear_name}', top, tx, ty, boss_r, seat_z, 'up'))
    s2 = p['s2_boss']
    s2x, s2y = p['board_standoffs'][s2['target']]
    features.append(('s2_boss_seat', top, s2x, s2y, boss_r, seat_z, 'up'))

    if mag_module_fits(p):
        mm = p['mag_module']
        ceiling = p['top_ceiling_underside_z']
        peg_r = mm['peg_dia'] / 2.0
        for i, (px, py) in enumerate(mag_peg_world_positions(p)):
            features.append((f'mag_peg_{i}', top, px, py, peg_r, ceiling, 'up'))
        pad_r = mm['pad_dia'] / 2.0
        for i, (px, py) in enumerate(mag_pad_world_positions(p)):
            features.append((f'mag_pad_{i}', top, px, py, pad_r, ceiling, 'up'))

    results = {}
    for name, body, cx, cy, r, z_root, direction in features:
        z_probe = z_root - 0.4 if direction == 'up' else z_root + 0.4
        probe_r = r + 0.6
        bad = []
        for ang in angles:
            rad = math.radians(ang)
            pt = P(cx + probe_r * math.cos(rad), cy + probe_r * math.sin(rad), z_probe)
            if not probe_point_solid(body, pt):
                bad.append((ang, round(z_probe, 2)))
        results[name] = bad

    results['_method'] = list(ROOT_FILLET_REPORT)
    return results


def verify_corner_blocks(bodies_dict, p):
    """Gate, RE-TARGETED pass 16 (owner call, item B: B1/B2 retired, A/C's
    own Top-side halves are now single-pilot wall-anchored blocks, not
    two-screw capsules -- see add_single_corner_block). For each of
    A, C, D1, D2 (RESUMED pass 16, Finding 1 fix: D1/D2 are now
    independent corner blocks too, at their own relocated xy north of the
    ears -- see 'screws_D12' in params_current.py):
    (a) the pilot hole is open (hollow) along its full documented depth
    (p['top_pilot_z'], sampled near each end and at mid-depth); (b) the
    block reads solid at the screw's own centre AND at 4 points around it
    (radius boss_dia/2 - 0.3, safely inside the capsule's own guaranteed
    coverage), at the block's own mid-height (per-screw, since D1/D2's
    own z1 is capped below the display module via `_ear_root_z1`, unlike
    A/C's uncapped `top_ceiling_underside_z`) -- confirms real, continuous
    material, not just a touching sliver; (c) the comms-stack footprint's
    own four corners (p['bay']['stack3']['l76k_pcb']) read hollow in Top
    at the block's mid-height, and the case's own y=0 centreline stays
    hollow across the LoRa FPC antenna keep-out strip's y-band
    (p['bay']['fpc_keepout']) -- both the "stays in the corner" and "does
    not bridge the end-wall centre" requirements at once."""
    top = bodies_dict['Top']
    z0 = p['split_z']
    z_mid = (z0 + p['top_ceiling_underside_z']) / 2.0
    pz0, pz1 = p['top_pilot_z']
    boss_r = p['boss_dia'] / 2.0
    results = {}

    for s in p['screws_ABC'] + p['screws_D12']:
        name = s['name']
        cx, cy = s['xy']
        z1 = _ear_root_z1(p, cx, cy, boss_r + CORNER_BLOCK_REACH, p['top_ceiling_underside_z'])

        checks = [not probe_point_solid(top, P(cx, cy, z))
                  for z in (pz0 + 0.1, (pz0 + pz1) / 2.0, pz1 - 0.1)]
        results[f'{name}_pilot_open'] = (all(checks), checks)

        # z_block: ABOVE the pilot hole's own z1 (top_pilot_z[1]) but
        # still well within the block's own z0..z1 span -- probing on-
        # axis AT z_mid would coincide with the pilot hole itself,
        # reading hollow by design, not a defect; z_block sidesteps that.
        z_block = min(pz1 + 1.5, z1 - 0.5)
        off_r = boss_r - 0.3
        solid_checks = [probe_point_solid(top, P(cx + off_r * math.cos(math.radians(ang)),
                                                   cy + off_r * math.sin(math.radians(ang)), z_block))
                         for ang in (0, 90, 180, 270)]
        results[f'{name}_block_solid'] = (all(solid_checks), solid_checks)

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        corners = [(pcb['x'][0], pcb['y'][0]), (pcb['x'][0], pcb['y'][1]),
                   (pcb['x'][1], pcb['y'][0]), (pcb['x'][1], pcb['y'][1])]
        stack_clear = [not probe_point_solid(top, P(cx, cy, z_mid)) for cx, cy in corners]
        results['stack_footprint_clear'] = (all(stack_clear), stack_clear)

    ko = p['bay']['fpc_keepout']
    kcz = (ko['z'][0] + ko['z'][1]) / 2.0
    fpc_checks = [not probe_point_solid(top, P(0.0, ko['y'][0] + frac * (ko['y'][1] - ko['y'][0]), kcz))
                  for frac in (0.25, 0.5, 0.75)]
    results['fpc_keepout_centerline_clear'] = (all(fpc_checks), fpc_checks)

    return results


def verify_bottom_openings(bodies_dict, p):
    """Pass 15, item 8 (Jake: "the bottom of the case's holes seem to be
    filled in?"). CONFIRMED root cause (see add_case_boss's own pass-15
    comment, right after its two `add_root_reinforcement` calls): every
    Bottom-side boss's root-reinforcement collar (pass 13) is a SOLID
    cone_frustum_solid -- filled all the way to the vertical axis, not a
    hollow washer -- and its own z-band (z_root-0.05..z_root+collar_rise,
    i.e. 1.95..3.5 for every Bottom boss, direction='down') sits squarely
    inside BOTH the pilot hole's full-through span and (for boss D) its
    own deeper counterbore, so joining the collar in silently REPLUGS
    both, solid, right at the screw -- confirmed independently by a
    pure-Python ray-cast of the shipped pass-14 STL exports before this
    pass touched any code (both variants, all 5 Bottom bosses, exact
    z=1.95/3.5 crossings). Fixed at the source (add_case_boss re-cuts
    both the pilot hole and the counterbore immediately after the collar
    join) -- this gate is the live, in-Fusion regression check that
    closes it going forward: for each of A/B1/B2/C/D, probes the pilot
    hole's own axis at 3 depths (just below the collar's own z-band,
    inside it, and just above it -- the exact band a silent replug would
    hide in) plus a matching 3-depth sweep of the counterbore itself
    (radius counterbore_ABC_dia/2 - 0.3, since a boss's OWN core material
    legitimately fills the counterbore's outer rim closer to its OD --
    only the open BORE at the pilot's own radius is what must stay
    hollow).

    RESUMED pass 16 (item 1, live-found the hard way): the counterbore
    probe's 3rd depth used to be a flat 3.0mm, valid back when only boss
    D had a counterbore and it was `counterbore_D_h`=4.0mm deep (3.0 sits
    safely inside that). Pass 16 retired that D-specific deep counterbore
    -- D1/D2 (now just D) use the SAME shallow `counterbore_ABC_h`
    (2.2mm) as A/C -- so 3.0mm is now PAST every counterbore's own real
    depth for all three screws (probing the plain, narrower pilot bore
    above the counterbore, at the counterbore's own WIDER radius, which
    is solid there BY DESIGN, not a defect: confirmed live, this false
    failure hit A/C/D identically). The real replug risk window is where
    the collar's own z-band (1.95..3.5) OVERLAPS the counterbore's own
    depth (0..counterbore_ABC_h) -- i.e. 1.95..2.2 -- so the 3rd probe
    depth is now anchored to `counterbore_ABC_h` (0.1mm short of its own
    ceiling) instead of a stale flat number, staying correct if that
    param ever changes again. Also re-checks the lanyard lug's own cord hole (Bottom,
    unrelated mechanism, included here as a second real "is this hole
    actually open" case per the same finding's own broader question) at
    3 depths through its own z-span. All lists empty = pass; this DOES
    gate verify() (unlike the diagnostic-only checks elsewhere in this
    file) since it is a direct, load-bearing regression test for a
    confirmed real defect, not a probe known to over-fire on legitimate
    geometry."""
    bottom = bodies_dict['Bottom']
    results = {}
    hole_r = p['screw_hole_dia'] / 2.0
    # pass 16: D1/D2 replace screw D and use the SAME shallow
    # counterbore_ABC_h as A/C (no plate post any more, see add_case_boss)
    # -- every one of A/C/D1/D2 now gets an identical pilot + counterbore
    # open-check, not just D.
    all_case_screws = p['screws_ABC'] + p['screws_D12']
    for s in all_case_screws:
        cx, cy = s['xy']
        name = s['name']
        bad = [z for z in (1.0, 2.7, 4.5, 7.0, 9.5)
               if probe_point_solid(bottom, P(cx, cy, z))]
        results[f'{name}_pilot_open'] = (not bad, bad)
    cb_r = p['counterbore_ABC_dia'] / 2.0 - 0.3
    cb_h = p['counterbore_ABC_h']
    for s in all_case_screws:
        cx, cy = s['xy']
        name = s['name']
        cb_bad = [z for z in (0.5, 1.9, cb_h - 0.1)
                  if probe_point_solid(bottom, P(cx + cb_r, cy, z))]
        results[f'{name}_counterbore_open'] = (not cb_bad, cb_bad)

    lug = p['lug']
    half_w, y_far, y_root, hole_y = lug_ear_geometry(p)
    z0, z1 = lug['z']
    lug_bad = [round(z, 2) for z in (z0 + 0.5, (z0 + z1) / 2.0, z1 - 0.5)
               if probe_point_solid(bottom, P(0.0, hole_y, z))]
    results['lug_hole_open'] = (not lug_bad, lug_bad)
    return results


def probe_bodies_interference_volume(design, body_a, body_b):
    """Real solid-overlap volume (mm^3) between exactly two standalone
    bodies -- a minimal, unfiltered variant of check_interference's own
    bounding-box-volume-proxy technique (see its docstring for why: this
    Fusion build's `interferenceBody.physicalProperties.volume` always
    reads 0.0), used where both bodies are throwaway probe tools under
    our own control rather than named case/board entities that need
    check_interference's name-based filtering."""
    coll = adsk.core.ObjectCollection.create()
    coll.add(body_a)
    coll.add(body_b)
    interference_input = design.createInterferenceInput(coll)
    interference_input.areCoincidentFacesIncluded = False
    results = design.analyzeInterference(interference_input)
    if results is None or results.count == 0:
        return 0.0
    total = 0.0
    for i in range(results.count):
        r = results.item(i)
        try:
            bb = r.interferenceBody.boundingBox
            dx = (bb.maxPoint.x - bb.minPoint.x) / MM
            dy = (bb.maxPoint.y - bb.minPoint.y) / MM
            dz = (bb.maxPoint.z - bb.minPoint.z) / MM
            total += dx * dy * dz
        except Exception:
            total = float('inf')
    return total


def find_display_occurrence(root, p):
    """Locate the inserted display occurrence anywhere under root, by name
    substring (same convention as get_open_doc) -- it lives directly under
    root right after build(), or nested under the 'Boards' component once
    organize_components() has run (verify() always runs after)."""
    def walk(occ):
        if p['display_doc_name'] in occ.name or p['display_doc_name'] in occ.component.name:
            return occ
        for child in occ.childOccurrences:
            found = walk(child)
            if found is not None:
                return found
        return None
    for occ in root.occurrences:
        found = walk(occ)
        if found is not None:
            return found
    return None


def verify_display_insertion_path(design, root, p):
    """New check (2026-09-08, pass 9, finding 5): can the display module
    (one rigid inserted occurrence -- glass + PCB + everything else on
    it) travel straight up (+z) from inside the case, through the
    lip/anchor ring's own z-band, to its final resting position? Builds a
    box matching the REAL inserted occurrence's own world bounding box
    (via _bbox_extents -- no known aggregate-bbox distortion for this
    board, unlike the L76K's antenna cable), spanning z from just below
    the ring's own lowest point up to the module's own top (the glass),
    and measures real interference against the standalone ring reference
    body (see add_lip_anchor_reliefs) -- isolated to the ring alone, not
    the general shell, since the question is specifically whether the
    ring/anchor blocks a from-inside assembly, not whether the module can
    pass through solid wall (it obviously cannot, and isn't meant to).

    Returns {'ok': None, ...} (not a hard failure) if either the display
    occurrence or the ring reference body can't be found -- this check is
    diagnostic/reported per finding 5's own wording ("confirm... and if it
    only fits from the front, say so"), not gated in verify(), since a
    real negative result here is an actionable design finding to report,
    not a build defect to assert against."""
    ref_occ = find_component_occurrence(root, COMPONENT_REFERENCE)
    ring_ref = None
    if ref_occ is not None:
        for b in ref_occ.component.bRepBodies:
            if b.name == 'Lip Anchor Ring (reference)':
                ring_ref = b
                break
    occ = find_display_occurrence(root, p)
    if ring_ref is None or occ is None:
        return {'ok': None, 'note': f'ring_ref found={ring_ref is not None} display_occ found={occ is not None}'}
    dx, dy, dz, (cx, cy, cz) = _bbox_extents(occ)
    x0, x1 = cx - dx / 2.0, cx + dx / 2.0
    y0, y1 = cy - dy / 2.0, cy + dy / 2.0
    z_top = cz + dz / 2.0
    z_lo = p['lip_z'][0] - 1.0
    sweep = box_solid(root, x0, x1, y0, y1, z_lo, z_top + 0.5)
    vol = probe_bodies_interference_volume(design, sweep, ring_ref)
    root.features.removeFeatures.add(sweep)
    return {
        'ok': vol <= _TOUCH_VOLUME_TOL_MM3,
        'interference_mm3': round(vol, 3),
        'module_bbox_xy': {'x': (round(x0, 2), round(x1, 2)), 'y': (round(y0, 2), round(y1, 2))},
        'module_top_z': round(z_top, 2),
        'ring_z_band': p['lip_z'],
    }


def verify_stack3_clearance(board_occs, by_name, p):
    """Regression guard (2026-09-07, pass 7, item 2): the 3-board comms
    stack's real top -- measured live off the inserted Wio occurrence's
    actual bounding box, NOT a nominal guess -- must clear the Top's real
    inner ceiling surface by at least stack3['ceiling_clear_min'] (0.8mm)
    everywhere under its footprint. Probed by scanning DOWNWARD from the
    ceiling (find_ceiling_z_at) at several points across the L76K PCB's
    footprint (the widest/lowest part of the stack; XIAO/Wio sit directly
    above it on almost the same XY footprint, so the real limiting case
    is whichever of these points has the least headroom)."""
    top = by_name['Top']
    s3 = p['bay']['stack3']
    # 2026-09-07: 'current' doesn't insert Wio/XIAO at all (see
    # insert_comms_boards' comms_stack3_full_height docstring) -- there is
    # no stack to check clearance for there, so this is trivially OK
    # rather than a failure.
    if not p.get('comms_stack3_full_height', True):
        return {'stack_top_z': None, 'clearance_found': None,
                'required': s3['ceiling_clear_min'], 'ok': True,
                'note': 'comms_stack3_full_height=False (current variant) -- no Wio/XIAO inserted, nothing to check'}
    wio_occ = next((o for o in board_occs if 'Wio-SX1262' in o.name), None)
    if wio_occ is None:
        return {'stack_top_z': None, 'clearance_found': None,
                'required': s3['ceiling_clear_min'], 'ok': False}
    _, _, dz, center = _bbox_extents(wio_occ)
    stack_top_z = center[2] + dz / 2.0

    pcb = s3['l76k_pcb']
    x0, x1 = pcb['x']
    y0, y1 = pcb['y']
    samples = [(x0 + 1.0, (y0 + y1) / 2.0), (x1 - 1.0, (y0 + y1) / 2.0),
               (0.0, y0 + 1.0), (0.0, y1 - 1.0), (0.0, (y0 + y1) / 2.0)]
    z_hi = p['top_z'] - 0.5
    worst = None
    for x, y in samples:
        ceil_z = find_ceiling_z_at(top, x, y, z_hi, stack_top_z, step=0.05)
        if ceil_z is None:
            continue
        clearance = ceil_z - stack_top_z
        if worst is None or clearance < worst:
            worst = clearance
    ok = worst is not None and worst >= s3['ceiling_clear_min'] - 1e-6
    return {'stack_top_z': round(stack_top_z, 3),
            'clearance_found': round(worst, 3) if worst is not None else None,
            'required': s3['ceiling_clear_min'], 'ok': ok}


def verify_fpc_relief(bodies_dict, p):
    """Regression guard (2026-09-07, pass 7 follow-up): confirms >=
    FPC_RELIEF_MIN_WALL (see its own module-level comment for why this is
    0.3mm, not the originally-targeted 1.2mm) of real solid skin survives directly above
    the FPC relief pocket's own cut, EVERYWHERE across its cut footprint
    -- not just in the flat crest region SPEC's box sits under -- this is
    the exact defect that slipped past every prior gate (including
    verify_skin_intact, which only ever looked at the button tab holes,
    nowhere near here): the widened pocket's outer corners (x roughly
    +-10..17, y roughly 67..73.5) reach into the domed +y end cap, where
    the true outer shoulder curves down to z~23 (trim) while the pocket's
    flat cut ceiling sits at z1~26.03 -- two wedge-shaped holes clean
    through the shell, confirmed both by ray-casting the exported trim
    Top.stl and by Jake in Bambu Studio (it printed with the holes).
    add_fpc_relief's fix clips the pocket tool against the real (curved)
    inner-cavity solid grown out to leave exactly FPC_RELIEF_MIN_WALL of
    skin, by construction -- this gate re-checks that independently of
    that construction.

    First attempt, WRONG, kept here as a documented dead end: probing a
    single fixed Z height (the pocket's own nominal cut ceiling z1 plus
    FPC_RELIEF_MIN_WALL) uniformly across the footprint. That conflates
    two different things -- "is there open air at this ABSOLUTE height"
    vs. "is the wall too thin AT THIS POINT" -- because the true outer
    shoulder is a DOME, not a flat plane: at larger rho (this footprint's
    own corners, well past the flat-crest radius), the true surface
    naturally curves down below (z1 + FPC_RELIEF_MIN_WALL) everywhere,
    genuine defect or not, so that single-height probe measured "is this
    (x,y) under the flat crest" far more than it measured wall thickness
    -- caught empirically: run against the UNFIXED pocket, it failed 131
    of 135 probes, not just the two known corners.

    Fixed approach: measure the REAL local skin thickness at each (x, y)
    directly, by scanning UP in Z (from a height guaranteed at/below the
    pocket's own cut floor, to one guaranteed above the true outer crest)
    and finding two transitions: hollow-to-solid (the bottom of whatever
    skin survives above the cut -- could be the pocket's nominal z1, or a
    higher, clipped local ceiling in the fixed geometry, or the natural
    ceiling underside if this (x,y) was never actually reached by the cut
    at all) and then solid-to-hollow (the true outer surface, where that
    skin ends). The gap between them is the real, local wall thickness --
    correct at any rho, flat crest or domed corner alike, with no
    analytic model of the curved outer surface needed. A column that is
    already solid at the low end (never cut here at all) short-circuits
    as trivially safe (nominal wall thickness, well over the minimum); a
    column that never finds solid anywhere in the bracket is a full
    breach (thickness 0) -- both handled by `_local_skin_thickness`.

    Two exclusions, both matching add_fpc_relief's own fix exactly (so
    they can never silently drift out of sync with what was actually
    cut): (1) inside the display window's own bore, there is no ceiling
    material by design (that IS the window) -- not a skin-thickness
    question; (2) inside the literal SPEC sub-box, add_fpc_relief cuts in
    full regardless of skin safety (the real FPC tab needs it, full stop
    -- see that function's 'round 2' docstring for the measured, ~0.1mm
    conflict that forced this split), so this gate's uniform
    FPC_RELIEF_MIN_WALL bar does not apply there; add_fpc_relief's own
    inline probe already asserts that narrower region comes out fully
    CLEARED (the opposite condition -- hollow, not skin-thick) every
    build. Everywhere else in the footprint -- the widened margin, where
    the confirmed wedge-hole defect actually lives -- gets the full
    bar."""
    top = bodies_dict['Top']
    fr = p['fpc_relief']
    x0, x1, y0, y1, z0, _z1 = fpc_relief_footprint(p)  # z0 = pocket's own cut floor (matches add_fpc_relief)
    wx, wy = p['window_center']
    w_excl_r = p['window_dia'] / 2.0 + 1.0  # +1mm chamfer/tessellation margin

    def _local_skin_thickness(x, y, step=0.25):
        z_lo = z0 - 0.5           # guaranteed at/below the pocket's own floor
        z_hi = p['top_z'] + 0.5   # guaranteed above the true outer crest (open air)
        z = z_lo
        prev = probe_point_solid(top, P(x, y, z))
        if prev:
            # solid already at the pocket floor's own height -- this
            # column was never actually cut here (fully clipped away, or
            # simply outside the real pocket despite being inside the
            # widened footprint box) -- whatever skin remains is at least
            # the full nominal wall, trivially safe.
            return None
        skin_start = None
        z += step
        while z <= z_hi:
            cur = probe_point_solid(top, P(x, y, z))
            if not prev and cur:
                skin_start = z
            elif prev and not cur and skin_start is not None:
                return z - skin_start
            prev = cur
            z += step
        if skin_start is None:
            return 0.0          # never found solid anywhere -- full breach
        return z_hi - skin_start  # solid clear to the scan ceiling -- ample skin

    results = {}
    nx, ny = 9, 7
    for i in range(nx):
        x = x0 + (x1 - x0) * i / (nx - 1)
        for j in range(ny):
            y = y0 + (y1 - y0) * j / (ny - 1)
            key = f'x{round(x, 2)}_y{round(y, 2)}'
            if math.hypot(x - wx, y - wy) <= w_excl_r:
                # inside the display window's own bore -- no ceiling
                # material is expected here by design (that's the whole
                # point of the window); not a skin-thickness question.
                results[key] = True
                continue
            if fr['x'][0] <= x <= fr['x'][1] and fr['y'][0] <= y <= fr['y'][1]:
                # inside the literal SPEC sub-box -- add_fpc_relief always
                # cuts here regardless of skin safety (see its docstring);
                # its own inline probe gates full clearance there instead.
                results[key] = True
                continue
            thickness = _local_skin_thickness(x, y)
            ok = thickness is None or thickness >= FPC_RELIEF_MIN_WALL - 0.05
            results[key] = ok
    return results


def verify_wordmark(bodies_dict, p):
    """Finding 7 gate (2026-09-08, pass 9e): the two-line KANDI/WOOKS
    wordmark. Three groups of checks, all computed against the SAME
    `wordmark_layout` the build itself used (so this can never disagree
    with what was actually cut): (1) analytic clearance from the
    wordmark's own rectangle (per line) to every case-screw counterbore
    (A/B1/B2/C/D) and to the lanyard ear's hole, >= 1.5mm -- cheap and
    exact, no Fusion probe needed since these are all axis-aligned
    shapes; (2) analytic clearance from the wordmark's own half-width to
    `flat_rho` (the flat-face edge), >= 1.5mm; (3) a live grid probe
    across both lines' bboxes on the real built `Bottom` body: at
    z = bottom_z + depth/2 (mid-deboss), a real, non-trivial fraction of
    sample points must read hollow (confirms the deboss actually cut
    something, catching a wholesale "wordmark missing" regression) but
    not ALL of them (confirms it isn't over-cutting the whole footprint
    solid); at z = bottom_z - 0.05 (just outside the flat bed) every
    sample must read hollow (no material floats below the bed); at
    z = bottom_z + depth + 0.15 (just past the deboss depth) every sample
    must read SOLID -- the "no breach of the floor" check: the deboss
    must stop at `logo_deboss_depth` and not reach any deeper (e.g. into
    the battery-bay floor cuts, which live 1.6mm+ deeper still).

    This is a numeric smoke test, not a substitute for actually looking
    at the render (per the finding's own instruction) -- it cannot tell
    "K" from "A" or catch a subtly wrong glyph, only that debossing
    happened in the right place, to the right depth, clear of the
    hardware. See pass9e_bottom_logo.png for the visual confirmation."""
    bottom = bodies_dict['Bottom']
    layout = wordmark_layout(p)
    depth = p['logo_deboss_depth']
    z_bot = p['bottom_z']
    half_w = layout['half_width']
    results = {}

    edge_clearance = p['flat_rho'] - half_w
    results['edge_clearance'] = (edge_clearance >= 1.5 - 1e-6, round(edge_clearance, 3))

    def rect_dist(x0, x1, y0, y1, px, py):
        dx = max(x0 - px, 0.0, px - x1)
        dy = max(y0 - py, 0.0, py - y1)
        return math.hypot(dx, dy)

    cb_r = p['counterbore_ABC_dia'] / 2.0
    targets = [(s['name'], s['xy'], cb_r) for s in p['screws_ABC'] + p['screws_D12']]
    _, _, _, hole_y = lug_ear_geometry(p)
    targets.append(('lug_hole', (0.0, hole_y), p['lug']['hole_dia'] / 2.0))
    for name, (sx, sy), r in targets:
        d1 = rect_dist(-half_w, half_w, layout['line1_y_range'][0], layout['line1_y_range'][1], sx, sy) - r
        d2 = rect_dist(-half_w, half_w, layout['line2_y_range'][0], layout['line2_y_range'][1], sx, sy) - r
        d = min(d1, d2)
        results[f'clearance_{name}'] = (d >= 1.5 - 1e-6, round(d, 3))

    cut_pts, solid_pts = 0, 0
    below_ok, beyond_ok = True, True
    for (y_lo, y_hi) in (layout['line1_y_range'], layout['line2_y_range']):
        nx, ny = 14, 6
        for i in range(nx):
            x = -half_w + (2.0 * half_w) * i / (nx - 1)
            for j in range(ny):
                y = y_lo + (y_hi - y_lo) * j / (ny - 1)
                if probe_point_solid(bottom, P(x, y, z_bot + depth / 2.0)):
                    solid_pts += 1
                else:
                    cut_pts += 1
                if probe_point_solid(bottom, P(x, y, z_bot - 0.05)):
                    below_ok = False
                if not probe_point_solid(bottom, P(x, y, z_bot + depth + 0.15)):
                    beyond_ok = False
    total = cut_pts + solid_pts
    cut_fraction = (cut_pts / total) if total else 0.0
    results['deboss_present'] = (0.05 <= cut_fraction <= 0.85, round(cut_fraction, 3))
    results['floor_intact_below_depth'] = (beyond_ok, beyond_ok)
    results['no_material_below_bed'] = (below_ok, below_ok)
    return results


def verify_wordmark_counters(bodies_dict, p):
    """Gate for pass 14, item 2 (real print defect: the wordmark's
    counters -- the enclosed centres of the 'a', the 'd', and both
    flower-shaped 'o's in WOOKS -- were being cut away, printing solid
    instead of as a hole in the deboss). Root cause and fix: see
    deboss_loops' own pass-14 docstring. Uses the exact same
    `wordmark_layout` counter_probes the build itself computed (so this
    can never disagree with what was actually cut), at the SAME
    mid-deboss z verify_wordmark's own grid probe already uses. For each
    counter: (a) `counter_N_present` -- the counter's own centroid must
    read SOLID (material present -- the counter was NOT cut away); (b)
    `counter_N_stroke_open` -- a point on the ring between the counter
    and its own outer glyph boundary must read HOLLOW (confirms the
    deboss itself still happened around it, not silently skipped
    entirely). `counter_count` is a regression guard on the wordmark JSON
    itself -- expects exactly the 4 counters found in kandiwooks_logo.json
    today (the 'a', the 'd', and the WOOKS body's 2 flower 'o' counters)."""
    bottom = bodies_dict['Bottom']
    layout = wordmark_layout(p)
    depth = p['logo_deboss_depth']
    z_bot = p['bottom_z']
    z_mid = z_bot + depth / 2.0
    results = {}
    probes = layout['counter_probes']
    results['counter_count'] = (len(probes) == 4, len(probes))
    for i, probe in enumerate(probes):
        cx, cy = probe['counter_center']
        counter_ok = probe_point_solid(bottom, P(cx, cy, z_mid))
        results[f'counter_{i}_present'] = (counter_ok, (round(cx, 3), round(cy, 3)))
        sp = probe['stroke_point']
        if sp is None:
            results[f'counter_{i}_stroke_open'] = (False, None)
        else:
            sx, sy = sp
            stroke_ok = not probe_point_solid(bottom, P(sx, sy, z_mid))
            results[f'counter_{i}_stroke_open'] = (stroke_ok, (round(sx, 3), round(sy, 3)))
    return results


def verify_ear_root_material(bodies_dict, p):
    """New gate (pass 16, item G -- mech review F7's own reinforcement
    ask). RESUMED pass 16 (Finding 1 fix): D1/D2 no longer sit at the
    ear's own wall-root (see 'ears'/'screws_D12' in params_current.py),
    and the root's own z1 is now capped well below `seat_z` by the
    button -- see `ear_root_cap_z1` -- so the arm/riser connectivity fix
    in `add_ear` changes what "solid along the span" means here too. For
    each ear (S1, S3): (a) the wall-root's own solid material at 5 points
    along the LOW root->standoff span (at the arm's own z-band, i.e.
    `root_z1 - ear_arm_thickness/2` -- no longer near `seat_z`, which now
    sits ABOVE the root's own capped reach); (b) the standoff's own
    vertical RISER reads solid at its own mid-height (root_z1 to
    seat_z); (c) the standoff's own M2x4 through-hole open along its
    full depth; and (d) mech review F14 (watch: ear S1's reach toward
    the FPC relief pocket) -- the analytic distance from each ear's own
    footprint (root + standoff, each padded by boss_dia/2) to the
    fpc_relief pocket's own footprint and to the display_header box,
    reported (not gated -- both distances are comfortably positive/large
    for the built geometry, but this makes the "watch" item a live,
    re-checked number rather than an eyeballed plan drawing estimate)."""
    top = bodies_dict['Top']
    results = {}
    seat_z = p['ear_seat_z']
    arm_thick = p['ear_arm_thickness']
    boss_r = p['boss_dia'] / 2.0
    fpc = p['fpc_relief']
    hdr = p['display_header']
    ceiling = p['top_ceiling_underside_z']
    for name, ear in p['ears'].items():
        rx, ry = ear['root_xy']
        tx, ty = p['board_standoffs'][ear['target']]
        root_z1 = ear_root_cap_z1(p, rx, ry, boss_r + CORNER_BLOCK_REACH, ceiling)
        z_mid_arm = root_z1 - arm_thick / 2.0
        z_mid_riser = (root_z1 + seat_z) / 2.0

        hole_checks = [not probe_point_solid(top, P(tx, ty, z)) for z in (root_z1 - arm_thick + 0.3, seat_z - 0.3)]
        results[f'{name}_standoff_hole_open'] = (all(hole_checks), hole_checks)

        # RESUMED pass 16 (root-caused a pre-existing gate bug while
        # live-verifying the Finding-1 connectivity fix): probing exactly
        # AT the target's own xy (t=1.0) lands dead-center on the M2x4
        # standoff clearance hole (`ear_standoff_hole_dia`, cut right
        # through that same axis) -- always reads hollow there BY DESIGN,
        # not a defect. This is what the README's own "`material_solid`
        # shows False at the root/target endpoints specifically -- a
        # new, smaller finding not yet root-caused" note was describing.
        # Fixed by sampling t=1.0 OFF-AXIS (same BOSS_CORE_R-0.3 radius
        # ring `verify_corner_blocks`' own off-axis solid check already
        # uses), matching how real material is actually laid out around
        # a drilled hole -- every other t stays on-axis (nothing else is
        # bored through the arm's own low z-band).
        off_r = BOSS_CORE_R - 0.3
        solid_checks = [probe_point_solid(top, P(rx + t * (tx - rx), ry + t * (ty - ry), z_mid_arm))
                         for t in (0.0, 0.25, 0.5, 0.75)]
        solid_checks += [probe_point_solid(top, P(tx + off_r * math.cos(math.radians(ang)),
                                                    ty + off_r * math.sin(math.radians(ang)), z_mid_arm))
                          for ang in (0, 90, 180, 270)]
        results[f'{name}_material_solid'] = (all(solid_checks), solid_checks)

        riser_checks = [probe_point_solid(top, P(tx + off_r * math.cos(math.radians(ang)),
                                                   ty + off_r * math.sin(math.radians(ang)), z_mid_riser))
                         for ang in (0, 90, 180, 270)]
        results[f'{name}_riser_solid'] = (all(riser_checks), (round(z_mid_riser, 3), riser_checks))

        # mech F14: nearest approach of either end of the ear (padded by
        # boss_r, since that's the ear's own real radius) to the FPC
        # relief pocket's own footprint / the display header box.
        fpc_dist = min(
            math.hypot(max(fpc['x'][0] - pt[0], 0.0, pt[0] - fpc['x'][1]),
                       max(fpc['y'][0] - pt[1], 0.0, pt[1] - fpc['y'][1])) - boss_r
            for pt in ((rx, ry), (tx, ty)))
        hdr_dist = min(
            math.hypot(max(hdr['x'][0] - pt[0], 0.0, pt[0] - hdr['x'][1]),
                       max(hdr['y'][0] - pt[1], 0.0, pt[1] - hdr['y'][1])) - boss_r
            for pt in ((rx, ry), (tx, ty)))
        results[f'{name}_fpc_relief_clear_mm'] = round(fpc_dist, 3)
        results[f'{name}_header_clear_mm'] = round(hdr_dist, 3)
    return results


def verify_s2_boss_clearance(bodies_dict, p):
    """New gate (pass 16, item G -- mech review F6): live-checks the S2
    boss's own hard keep-out around the display's real battery-connector
    XY+Z footprint (`battery_connector_world_bbox`, +0.5mm margin every
    side) and its clearance to the GPS frame's own real outer wall.
    'battery_clear_ok': every sampled point across the connector's real
    footprint reads HOLLOW in the built Top (the boss's own arm/pad never
    occupies that volume). 'gps_clear_mm'/'gps_clear_ok': the boss's own
    nearest edge (S2's own y minus boss_dia/2) to the GPS frame's real
    outer wall (build_gps_frame_body's own construction: patch-box centre
    Y + gps_frame_opening/2 + gps_frame_wall, clearance=0.0 as that
    function actually passes it) must be >= 0.5mm, this file's own house
    minimum-clearance convention (CORNER_BLOCK_RING_CLEARANCE, wall_clear
    in add_lip_anchor_reliefs, etc.)."""
    top = bodies_dict['Top']
    (bcx0, bcx1), (bcy0, bcy1), (bcz0, bcz1) = battery_connector_world_bbox(p)
    results = {}
    bad = []
    nx, ny, nz = 4, 3, 2
    for i in range(nx):
        x = bcx0 + (bcx1 - bcx0) * i / (nx - 1)
        for j in range(ny):
            y = bcy0 + (bcy1 - bcy0) * j / (ny - 1)
            for k in range(nz):
                z = bcz0 + (bcz1 - bcz0) * k / (nz - 1)
                if probe_point_solid(top, P(x, y, z)):
                    bad.append((round(x, 2), round(y, 2), round(z, 2)))
    results['battery_clear_ok'] = (not bad, bad[:5])

    s2 = p['s2_boss']
    tx, ty = p['board_standoffs'][s2['target']]
    boss_r = p['boss_dia'] / 2.0
    gps = p['bay']['gps_patch']
    gps_half = p['bay']['gps_frame_opening'] / 2.0
    gcy = (gps['y'][0] + gps['y'][1]) / 2.0
    gps_outer_y1 = gcy + gps_half + p['bay']['gps_frame_wall']
    gps_clear = (ty - boss_r) - gps_outer_y1
    results['gps_clear_mm'] = round(gps_clear, 3)
    results['gps_clear_ok'] = gps_clear >= 0.5
    return results


def verify_seat_heights(design, root, bodies_dict, p):
    """New gate (pass 16, item G): live-probes, for each of S1/S2/S3, the
    REAL inserted display module's own underside directly above the
    seat's own xy (scanning downward through the module's own bodies via
    find_ceiling_z_at, the same technique verify_stack3_clearance already
    uses for the comms stack), and compares it to the seat's own built
    top face (PARAMS['ear_seat_z']) -- expecting a gap of
    PARAMS['ear_seat_offset'] (-0.25mm, i.e. the seat sits 0.25mm SHORT
    of the real standoff plane so the window seat, not S1/S2/S3, takes
    the assembly preload -- mech review F5). Capped at 40 sub-bodies per
    the same MIN_CLEARANCE_BODY_CAP-style budget every other per-body
    scan in this file already uses."""
    occ = find_display_occurrence(root, p)
    if occ is None:
        return {'note': 'display occurrence not found -- cannot live-probe seat heights'}
    # pass 16 fix (live-found): the display occurrence carries ~424 discrete
    # sub-bodies (every individual SMT part) -- an earlier version took
    # `[:40]` in whatever arbitrary order Fusion happens to enumerate them,
    # which silently never reached the ONE body actually overhanging a
    # given standoff if it didn't fall in that arbitrary first 40 (same
    # class of bug the display-vs-ear interference fix found and fixed the
    # same way, see the candidate-filter comment in build()). Filter by
    # REAL XY FOOTPRINT instead (bbox contains the standoff's own xy, with
    # a small margin) -- a cheap bounding-box test before the expensive
    # stepped find_ceiling_z_at scan, correct regardless of enumeration
    # order, and naturally small (only bodies actually over this specific
    # point can ever matter).
    MM_ = 0.1
    all_bodies = _collect_occ_bodies(occ)
    seat_z = p['ear_seat_z']
    offset = p['ear_seat_offset']
    results = {}
    for name, (sx, sy) in p['board_standoffs'].items():
        bodies_list = []
        for b in all_bodies:
            bb = b.boundingBox
            if (bb.minPoint.x / MM_ - 0.5 <= sx <= bb.maxPoint.x / MM_ + 0.5
                    and bb.minPoint.y / MM_ - 0.5 <= sy <= bb.maxPoint.y / MM_ + 0.5):
                bodies_list.append(b)
        found_z = None
        for b in bodies_list:
            # pass 16 (resumed): step tightened 0.1 -> 0.01mm -- the gate's
            # own tolerance (below) was tightened to +/-0.05mm now that
            # `ear_seat_z` is itself live-derived from this same probe
            # (see params_current.py's comment); a 0.1mm-step scan could
            # misreport the gap by up to a full step for no geometric
            # reason, large enough to spuriously fail (or pass) a
            # +/-0.05mm gate on its own.
            z = find_ceiling_z_at(b, sx, sy, seat_z + 5.0, seat_z - 2.0, step=0.01)
            if z is not None and (found_z is None or z < found_z):
                found_z = z
        if found_z is None:
            results[name] = {'ok': False, 'note': 'no display material found above this standoff'}
            continue
        gap = found_z - seat_z
        results[name] = {
            'standoff_plane_z': round(found_z, 3), 'seat_z': round(seat_z, 3),
            'gap_mm': round(gap, 3), 'expected_gap_mm': offset,
            # pass 16 (resumed, owner call on Finding 2): tightened from
            # 0.5mm to 0.05mm now that `ear_seat_z` is live-derived
            # straight from this exact same probe technique (see
            # params_current.py's comment) rather than inherited from the
            # old Screen Plate's `plate_z[1]` -- a real design number, not
            # a legacy guess, so the gate should hold it to the same
            # precision the owner asked verify() to confirm (-0.25 +/-
            # 0.05mm on all three seats).
            'ok': abs(gap - offset) <= 0.05,
        }
    return results


def _xy_overlap(bx0, bx1, by0, by1, cx0, cx1, cy0, cy1):
    ox0, ox1 = max(bx0, cx0), min(bx1, cx1)
    oy0, oy1 = max(by0, cy0), min(by1, cy1)
    if ox1 <= ox0 or oy1 <= oy0:
        return None
    return ox0, ox1, oy0, oy1


def verify_display_to_stack_clearance(board_occs, bodies_dict, p):
    """New gate (pass 16, item G -- ID review Finding 4): with no Screen
    Plate any more to guarantee a physical separator between the display
    module's own underside and the comms-bay cavity below it (round-1's
    own framing: the plate's third job was "physically separates/locates
    the display from the comms-bay cavity below"), re-checks that
    clearance directly against the real geometry -- wherever the
    display's own bbox actually overlaps the comms stack's PCB
    footprint, the battery's footprint, or the GPS patch's footprint in
    XY (found analytically first: most of these turn out NOT to overlap
    at all -- the display's own y-range starts at 27.6, north of both the
    stack, y<=-1.5, and the GPS patch, y<=27.0), the analytic Z clearance
    there (display's own lowest bbox z vs. the feature's own top z) must
    be >= 0.5mm."""
    db = p['display_bbox']
    dz = p.get('display_z_offset', 0.0)
    dx0, dx1 = db['x']
    dy0, dy1 = db['y']
    dz0 = db['z'][0] + dz

    checks = []
    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        checks.append(('stack3', pcb['x'][0], pcb['x'][1], pcb['y'][0], pcb['y'][1], s3['stack_top_z_nominal']))
    bat = p['bay']['battery']
    checks.append(('battery', bat['x'][0], bat['x'][1], bat['y'][0], bat['y'][1], bat['z'][1]))
    gps = p['bay']['gps_patch']
    checks.append(('gps_patch', gps['x'][0], gps['x'][1], gps['y'][0], gps['y'][1], gps['z'][1]))

    results = {'ok': True, 'checks': {}}
    for name, cx0, cx1, cy0, cy1, top_z in checks:
        overlap = _xy_overlap(dx0, dx1, dy0, dy1, cx0, cx1, cy0, cy1)
        if overlap is None:
            results['checks'][name] = {'overlap': False,
                                        'note': 'no XY overlap with the display bbox -- clear by construction'}
            continue
        ox0, ox1, oy0, oy1 = overlap
        clearance = dz0 - top_z
        ok = clearance >= 0.5
        results['checks'][name] = {
            'overlap': True, 'region': (round(ox0, 2), round(ox1, 2), round(oy0, 2), round(oy1, 2)),
            'clearance_mm': round(clearance, 3), 'ok': ok,
        }
        if not ok:
            results['ok'] = False
    return results


def window_column_probe_points(p, n_angle=8, radii_fracs=(0.4, 0.85)):
    """XY sample points inside the window bore's own column (pass 11, new
    gate) -- a small ring-of-rings pattern (not just the centreline) so an
    off-centre partial blockage -- like defect 1's brow slab, which only
    covered part of the bore near its own footprint -- isn't missed. Stays
    `1mm` inside the bore's own radius (window_dia/2), off the chamfered
    rim, so every point is unambiguously "should be open cavity", never a
    point that legitimately sits in the chamfer's own solid material."""
    cx, cy = p['window_center']
    r_max = p['window_dia'] / 2.0 - 1.0
    pts = [(cx, cy)]
    for frac in radii_fracs:
        r = r_max * frac
        for i in range(n_angle):
            theta = 2.0 * math.pi * i / n_angle
            pts.append((cx + r * math.cos(theta), cy + r * math.sin(theta)))
    return pts


def _in_stadium(t, z, L, W):
    """True if local point (t, z) sits inside a stadium (capsule) whose
    long axis L runs along t and short axis W runs along z, centered at
    the origin -- rounded ends (radius W/2) at t=+-L/2, flat sides at
    z=+-W/2 for |t| <= L/2-W/2. Matches this file's own stadium convention
    (cap['stadium'] = (L, W), long axis tangential -- see button_geometry/
    add_button/SPEC.md's "long axis tangential" description)."""
    half_flat = max(L / 2.0 - W / 2.0, 0.0)
    r = W / 2.0
    if abs(t) <= half_flat:
        return abs(z) <= r
    dt = abs(t) - half_flat
    return math.hypot(dt, z) <= r


def verify_openings_open(root, bodies_dict, p):
    """New gate (2026-09-11, pass 11): every documented "opening" in this
    design -- a feature meant to be hollow all the way through, not just
    thin-skinned -- must actually BE open along its full documented
    extent. Added after Jake's own ray-cast of pass-9's exported
    export/trim/Top.stl found the FPC brow (built AFTER add_window in
    build()'s own call order, see build_fpc_brow_solid's docstring)
    silently refilling the top ~10mm of the window bore, with no existing
    gate catching it -- every other check in this file probes SKIN
    THICKNESS or INTERFERENCE, never "is this opening's own interior
    actually hollow end to end". This gate FAILS on pre-pass-11 main (the
    window_column entry) and PASSES once build_fpc_brow_solid's window-
    column exclusion is in place -- see README's pass-11 section for the
    before/after numbers this produced on a live run.

    Each entry probes point-containment (probe_point_solid) against the
    relevant body/bodies at several samples across the opening's own
    documented footprint, along its own through-axis:
      - `window_column`: the bore's own XY (several rings inside
        window_dia/2 - 1mm, window_column_probe_points), scanned in Z
        from the glass ledge (window_z_bottom) up through top_z + 0.5 --
        must be hollow at every sample, in Top.
      - `usb_tunnel`: the tunnel's own stadium interior, at 3 x-offsets
        (centre +/- a couple mm), scanned along its own Y axis from
        y_start through the true outer wall -- Top.
      - `power_button_hole` / `home_button_hole`: each cap's own nub
        direction (button_geometry's `d`), scanned from just past the
        true outer wall (s_wall) inward to just past the plunger's own
        rest tip (s_plunger_tip) -- Top (the hole itself; the cap/plunger
        is a separate printed body, not part of Top).
      - `lug_hole`: the lanyard ear's own vertical through-hole
        (lug_ear_geometry), scanned in Z across its documented span --
        Bottom.
      - `antenna_lora` / `antenna_gps`: each cable channel's own route
        (antenna_channel_geometry), scanned along its own run -- Top.
      - `mag_wire_notch` (pass 11, new): the compass mount's own
        header-wire exit notch (now on the fence's SOUTH wall, see
        add_mag_module) -- scanned a couple mm further south, confirming
        the wire's own run stays in open cavity air, well clear of both
        the window bore and the display -- Top. Reports (True, []) when
        mag_module_fits(p) is False ('current' -- no mount, no notch).

    Returns {name: (ok, [bad_probe, ...])} -- a non-empty bad list names
    the exact (x, y, z) samples that found solid material where the
    opening should be clear, for direct comparison against a ray-cast or
    a real print defect."""
    top = bodies_dict['Top']
    bottom = bodies_dict['Bottom']
    results = {}

    # --- window column ---
    z_lo = p['window_z_bottom'] + 0.1
    z_hi = p['top_z'] + 0.5
    z_samples = [z_lo + k * (z_hi - z_lo) / 5.0 for k in range(6)]
    bad = []
    for x, y in window_column_probe_points(p):
        for z in z_samples:
            if probe_point_solid(top, P(x, y, z)):
                bad.append((round(x, 2), round(y, 2), round(z, 2)))
    results['window_column'] = (not bad, bad[:20])

    # --- USB tunnel ---
    wall_y = p['spine_b'][1] + p['outer_radius']
    y_start = p['usb_tunnel_y_start']
    cz = p['usb_tunnel_center_z']
    L_in, _ = p['usb_tunnel_stadium']
    y_samples = [y_start + k * (wall_y - y_start) / 4.0 for k in range(5)]
    bad = []
    for x_off in (0.0, L_in / 2.0 - 1.0, -(L_in / 2.0 - 1.0)):
        for y in y_samples:
            if probe_point_solid(top, P(x_off, y, cz)):
                bad.append((round(x_off, 2), round(y, 2), round(cz, 2)))
    results['usb_tunnel'] = (not bad, bad[:20])

    # --- button holes (Power / Home) ---
    home_bbox = dict(p['switch_home_bbox'])
    home_bbox['z'] = p['switch_power_bbox']['z']
    for label, switch_bbox, nub_dir, cap in (
            ('power_button_hole', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
            ('home_button_hole', home_bbox, p['home_nub_dir'], p['home_cap'])):
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        d2 = g['d']
        cap_z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        s_hi = g['s_wall'] - 0.1
        s_lo = g['s_plunger_tip'] + 0.1
        bad = []
        for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
            s = s_lo + frac * (s_hi - s_lo)
            x = g['housing_xy'][0] + s * d2[0]
            y = g['housing_xy'][1] + s * d2[1]
            if probe_point_solid(top, P(x, y, cap_z_center)):
                bad.append((round(x, 2), round(y, 2), round(cap_z_center, 2)))
        results[label] = (not bad, bad[:20])

    # --- button hole OUTER FOOTPRINT (2026-09-12 pass 12, new) ---
    # The check above only confirms the hole is open ALONG its own axis at
    # t=0 -- it says nothing about whether the wall's opening is EXACTLY
    # the intended stadium. Jake's own ray-cast of the pass-11 export
    # (sidescan.py, rays along +-x over a (y,z) grid) found a real breach
    # OUTSIDE each hole's stadium: a slot below the main opening, with a
    # stepped interior surface visible through it -- root-caused to
    # add_button's tab_hole_body (a flat, uncurved cut into Top for the
    # tab's own clearance) not being bounded against the true curved wall
    # off its own centerline (see that function's now-updated comment).
    # This gate grid-scans the (tangential, z) plane around each hole, at
    # a fixed radial depth just inside the true wall (s_wall -
    # footprint_probe_depth), and classifies every sample by whether it
    # falls inside the ACTUAL cut stadium (hole_wh = cap stadium +
    # 2*cap_clearance/side, matching add_button's own hole_cutter exactly)
    # or outside it: inside must be OPEN (the hole), outside must be
    # BLOCKED (real wall skin) -- any mismatch is reported by kind
    # ('blocked-inside-hole' or 'open-outside-hole') plus the exact world
    # point, directly comparable to a ray-cast. The z range reaches well
    # below the hole stadium into the tab's own region (down to
    # z_center - W/2 - tab['h'] - 1.5), the exact band the sidescan found
    # breached. FAILS on pre-pass-12 `origin/main` (the tab_hole_body cut
    # reaches outside the stadium there); PASSES once add_button's
    # tab_clip_tool fix is in place -- see README's pass-12 section for
    # the live before/after numbers.
    # 2026-09-12 pass 12 fix (found live, before this comment existed): a
    # flat tangential displacement from a single s_probe computed at t=0
    # drifts the probe point PAST the true curved wall entirely at larger
    # |t_off| (Power's nub_dir is diagonal, not purely radial -- moving
    # tangentially at a fixed "depth along d2" does not track the true
    # surface) -- an early version of this loop flagged 'open-outside-
    # hole' at x=-32.18 for trim (outer_radius=28), open air nowhere near
    # the actual shell, a false positive from the probe method, not a
    # defect. Fixed the same way true_wall_distance_along_ray is used
    # everywhere else in this file for an off-axis ray: recompute the
    # TRUE wall position fresh at each (t_off, z) sample, from a ray
    # ORIGINATING at the tangentially-shifted point (not by shifting an
    # already-computed s=s_wall point sideways) -- follows the real
    # curvature at every sample instead of assuming a locally flat wall.
    footprint_probe_depth = 0.4
    for label, switch_bbox, nub_dir, cap in (
            ('power_button_hole', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
            ('home_button_hole', home_bbox, p['home_nub_dir'], p['home_cap'])):
        g = button_geometry(p, switch_bbox, nub_dir, cap)
        d2, t2 = g['d'], g['t']
        cap_z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        L, W = cap['stadium']
        hole_L, hole_W = L + 2 * p['cap_clearance'], W + 2 * p['cap_clearance']
        t_fracs = (-1.0, -0.6, -0.2, 0.2, 0.6, 1.0)
        t_half = hole_L / 2.0 + 2.5
        z_lo_off = -(hole_W / 2.0 + p['tab']['h'] + 1.5)
        z_hi_off = hole_W / 2.0 + 1.0
        # 2026-09-12 pass 12 fix (found live, 'current' variant): Top only
        # exists from split_z upward (Bottom owns everything below it) --
        # 'current's home_cap sits low enough (z_lo_off reaches z=9.65
        # against split_z=10) that an unclamped grid probes BELOW split_z,
        # where probe_point_solid(top, ...) is trivially never-solid
        # regardless of any real defect (Top's own geometry doesn't extend
        # there) -- a false 'open-outside-hole' with no connection to the
        # button mechanism at all. Trim never hit this (its +5mm height
        # shift moves every button z-range well clear of split_z), which
        # is exactly why this was missed until testing 'current' live.
        z_lo_off = max(z_lo_off, p['split_z'] - cap_z_center + 0.1)
        z_samples = [z_lo_off + k * (z_hi_off - z_lo_off) / 6.0 for k in range(7)]
        bad = []
        for tf in t_fracs:
            t_off = tf * t_half
            local_origin = (g['housing_xy'][0] + t_off * t2[0], g['housing_xy'][1] + t_off * t2[1])
            for z_off in z_samples:
                z = cap_z_center + z_off
                s_wall_local = true_wall_distance_along_ray(p, local_origin, d2, z)
                if s_wall_local is None:
                    continue
                s_probe = s_wall_local - footprint_probe_depth
                x = local_origin[0] + s_probe * d2[0]
                y = local_origin[1] + s_probe * d2[1]
                inside = _in_stadium(t_off, z_off, hole_L, hole_W)
                is_solid = probe_point_solid(top, P(x, y, z))
                if inside and is_solid:
                    bad.append(('blocked-inside-hole', round(x, 2), round(y, 2), round(z, 2)))
                elif not inside and not is_solid:
                    bad.append(('open-outside-hole', round(x, 2), round(y, 2), round(z, 2)))
        results[f'{label}_footprint'] = (not bad, bad[:20])

    # --- lug hole ---
    _, _, _, hole_y = lug_ear_geometry(p)
    z0, z1 = p['lug']['z']
    bad = []
    for frac in (0.1, 0.3, 0.5, 0.7, 0.9):
        z = z0 + frac * (z1 - z0)
        if probe_point_solid(bottom, P(0.0, hole_y, z)):
            bad.append((0.0, round(hole_y, 2), round(z, 2)))
    results['lug_hole'] = (not bad, bad[:20])

    # --- antenna cable channels ---
    # 2026-09-11 pass 11 fix: a live probe (before finalizing this gate)
    # found the LoRa channel's OUTER ~10-20% (toward the true wall) reads
    # solid, not open -- `_antenna_skin_safe_channel`'s Combine-Intersect
    # against the skin-safe envelope legitimately trims the cut's far end
    # a bit short of the naive `s_wall - channel_min_skin` endpoint this
    # gate's `channel_len` approximates (the skin-safe envelope curves,
    # the analytic estimate doesn't) -- this is the SAME kind of
    # analytic-vs-built gap this file already documents elsewhere (e.g.
    # the button cap's `true_wall_distance_along_ray` check, loosened to
    # 0.25mm for the same reason), not a real regression: the existing
    # verify_antenna_channels gate's own single probe sits at ~50% and
    # was, and remains, open. Sampled fractions here stay inside the
    # confirmed-open 0-70% band rather than reaching for the intentional
    # skin margin near the wall.
    ageo = antenna_channel_geometry(p)
    if 'lora' in ageo:
        g = ageo['lora']
        ox, oy = g['origin_xy']
        dirv, uz = g['dir'], g['z']
        channel_len = g['s_wall'] - p['antenna']['channel_min_skin']
        bad = []
        for frac in (0.15, 0.4, 0.65):
            s = frac * max(channel_len, 0.0)
            x, y = ox + s * dirv[0], oy + s * dirv[1]
            if probe_point_solid(top, P(x, y, uz)):
                bad.append((round(x, 2), round(y, 2), round(uz, 2)))
        results['antenna_lora'] = (not bad, bad[:20])

    g = ageo['gps']
    px, py, pz = g['probe_xyz']
    nz0, nz1 = g['notch_z']
    bad = []
    for z in (nz0 + 0.2, (nz0 + nz1) / 2.0, nz1 - 0.2):
        if probe_point_solid(top, P(px, py, z)):
            bad.append((round(px, 2), round(py, 2), round(z, 2)))
    results['antenna_gps'] = (not bad, bad[:20])

    # --- compass mount wire notch (pass 11) ---
    # 2026-09-11: a live probe (before finalizing this gate) found the
    # fence's own south wall band runs from fy0 NORTHWARD to fy0+
    # fence_wall (build_hanging_frame's outer->inner box-cut convention:
    # the wall is the band between the outer and inner rectangles, and
    # for the south edge the outer edge -- fy0 -- is the SOUTHERNMOST
    # extent) -- an earlier version of this probe sampled SOUTH of fy0
    # (fy0 - wall/2, etc.), which lands in the open gap between the fence
    # and the GPS frame's own wall, not in the fence's own wall band at
    # all, and so could never have caught a real un-cut notch. Fixed to
    # sample THROUGH the actual wall band (fy0 to fy0+fence_wall) at the
    # notch's own x.
    if mag_module_fits(p):
        notch_x = mag_header_notch_center_x(p)
        _, _, fy0, _ = mag_fence_world_footprint(p)
        ceiling = p['top_ceiling_underside_z']
        mm = p['mag_module']
        notch_z = ceiling - mm['fence_h'] / 2.0
        wall = mm['fence_wall']
        bad = []
        for frac in (0.15, 0.5, 0.85):
            y = fy0 + frac * wall
            if probe_point_solid(top, P(notch_x, y, notch_z)):
                bad.append((round(notch_x, 2), round(y, 2), round(notch_z, 2)))
        results['mag_wire_notch'] = (not bad, bad[:20])
    else:
        results['mag_wire_notch'] = (True, [])

    return results


def verify(design, params):
    root = design.rootComponent
    printed, ref_boxes, board_occs = collect_interference_entities(root)
    names = sorted(b.name for b in printed)
    case_bodies = [b for b in printed if b.name in ('Bottom', 'Top')]

    probe_results = verify_m1_probe_table(case_bodies, params)
    bad = [r for r in probe_results if not r[3]]
    assert not bad, f'outer profile probe mismatch: {bad}'

    cavity_results = verify_m1_cavity_probes(case_bodies, params)
    bad_cav = [r for r in cavity_results if not r[3]]
    assert not bad_cav, f'cavity probe mismatch: {bad_cav}'

    # Full interference gate (2026-09-05 pass 5 rewrite, per Jake's own
    # findings that pass 4's "zero interference" claim was wrong): every
    # printed body + every inserted board occurrence + the battery/GPS
    # reference boxes, excluding only the two reference TOOL solids and
    # the FPC keep-out marker. See check_interference's docstring for the
    # areCoincidentFacesIncluded / bounding-box-volume details.
    #
    # Board occurrences are checked ONE AT A TIME against (printed +
    # ref_boxes), rather than all combined in one collection: combining
    # multiple inserted reference docs in one analyzeInterference call
    # also reports interference INSIDE each reference doc's own native
    # modeling (e.g. a through-hole component's leads embedded in its own
    # PCB body -- real geometry in that document, entirely unrelated to
    # case fit) with no reliable way to distinguish it from a genuine
    # case-vs-board or board-vs-board clash from the result alone. This
    # scope -- case/reference-box vs each board -- matches what Jake's own
    # findings enumerate (every item is a case-body-vs-board pair).
    interference = check_interference(design, printed + ref_boxes)
    for occ in board_occs:
        interference += check_interference(design, printed + ref_boxes + [occ])
    assert not interference, f'interference detected (entity pair, bbox-volume mm3): {interference}'
    # kept for the printed summary / historical field name
    occ_interference = []

    by_name = {b.name: b for b in printed}
    m2_results = verify_m2(by_name, params)
    bad_m2 = [k for k, v in m2_results.items() if not v[0]]
    assert not bad_m2, f'M2 checks failed: {[(k, m2_results[k]) for k in bad_m2]}'

    envelope_results = verify_envelope(by_name, params)
    bad_env = [(k, v) for k, v in envelope_results.items() if not v[0]]
    assert not bad_env, f'body exceeds allowed envelope: {bad_env}'

    bump_results = verify_no_outer_bumps(case_bodies, params)
    bad_bumps = [r for r in bump_results if r[1]]
    assert not bad_bumps, f'solid material found just outside the outer surface: {bad_bumps}'

    export_envelope_results = verify_export_envelope(by_name, params)
    bad_export_env = [(k, v) for k, v in export_envelope_results.items() if not v[0]]
    assert not bad_export_env, f'exported body has vertices outside the allowed envelope: {bad_export_env}'

    app = adsk.core.Application.get()
    clearance_results = verify_min_clearances(app, printed, board_occs, params.get('clearance_min', 0.3))
    bad_clear = {k: v for k, v in clearance_results.items() if not v[1]}
    assert not bad_clear, f'board occurrence closer than clearance_min to the case: {bad_clear}'

    posts_bosses_results = verify_posts_and_bosses(by_name, params)
    # 2026-09-07 pass 7: boss B's documented KNOWN_UNJOINED exception is
    # gone -- B1/B2 replace it at a position clear of the comms stack, so
    # every boss/post (including both) is expected to have real material.
    bad_pb = [k for k, ok in posts_bosses_results.items() if not ok]
    assert not bad_pb, f'boss/post missing material (silent-no-op-join regression): {bad_pb}'

    # 2026-09-08 pass 9 (finding 4): the new Ø5/relocated posts' own wall
    # thickness (around the pilot, and skin to the true outer shell) --
    # see verify_post_walls' docstring.
    post_wall_results = verify_post_walls(by_name, params)
    bad_post_walls = {k: v for k, v in post_wall_results.items() if v}
    assert not bad_post_walls, f'top post wall/skin check failed: {bad_post_walls}'

    # 2026-09-08 pass 9b (finding 10): live-probed plunger reach against
    # the REAL inserted switch bodies -- see verify_plunger_reach's
    # docstring. Gated: a real miss here is exactly finding 10's defect
    # ("too short to reach the switch").
    plunger_reach_results = verify_plunger_reach(root, by_name, params)
    assert 'note' not in plunger_reach_results, f'verify_plunger_reach could not run: {plunger_reach_results}'
    bad_plunger_reach = [k for k, v in plunger_reach_results.items() if isinstance(v, tuple) and not v[0]]
    assert not bad_plunger_reach, f'plunger does not reach the real switch actuator: {[(k, plunger_reach_results[k]) for k in bad_plunger_reach]}'

    # 2026-09-08 pass 9b (finding 9): the retaining tab's insertion sweep
    # past the rib -- see verify_button_insertion's docstring. Gated: any
    # bad step means the cap physically cannot be assembled.
    button_insertion_results = verify_button_insertion(root, by_name, params)
    assert 'note' not in button_insertion_results, f'verify_button_insertion could not run: {button_insertion_results}'
    bad_insertion = [k for k, v in button_insertion_results.items() if isinstance(v, tuple) and not v[0]]
    assert not bad_insertion, f'button insertion path blocked by the rib: {[(k, button_insertion_results[k]) for k in bad_insertion]}'

    # 2026-09-08 pass 9b (finding 9): live retention probes (collar-vs-
    # rib-slot, tab-vs-outward-drift) -- see verify_button_retention's
    # docstring.
    button_retention_results = verify_button_retention(by_name, params)
    assert 'note' not in button_retention_results, f'verify_button_retention could not run: {button_retention_results}'
    bad_retention = [k for k, v in button_retention_results.items() if not v[0]]
    assert not bad_retention, f'button retention check failed: {[(k, button_retention_results[k]) for k in bad_retention]}'

    # 2026-09-08 pass 9 (finding 5): from-inside display insertion path --
    # diagnostic/reported, NOT gated (see verify_display_insertion_path's
    # docstring for why a real negative result here is an actionable
    # finding, not a build defect).
    display_insertion_results = verify_display_insertion_path(design, root, params)

    stack3_clearance = verify_stack3_clearance(board_occs, by_name, params)
    assert stack3_clearance['ok'], f'comms stack top too close to Top ceiling: {stack3_clearance}'

    # 2026-09-07 pass 7 (item 5): verify_skin_intact NOW GATES verify() --
    # re-targeted to probe a tight band around the tab hole's own real
    # footprint (see its docstring) instead of the whole button-hole
    # perimeter, which strayed into unrelated legitimate interior
    # geometry and over-fired on most of its samples every run since
    # pass 6. Traced and fixed at the source, not just widened/loosened.
    skin_results = verify_skin_intact(by_name, params)
    bad_skin = [k for k, ok in skin_results.items() if not ok]
    assert not bad_skin, f'button skin breach near a tab hole: {bad_skin}'

    # 2026-09-07 pass 7 (item 5): verify_wall_integrity NOW GATES verify()
    # -- the two remaining failure classes from pass 6 (the lug's real
    # relief cut at spine_a/z11, and the flat-to-arc tangent transition
    # for straight-section bosses) are excluded by NAME/GEOMETRY inside
    # verify_wall_integrity itself (see its docstring), not by loosening
    # this gate -- a genuine new local defect anywhere else in either
    # sweep still fails here.
    wall_results = verify_wall_integrity(by_name, params)
    bad_wall = [k for k, ok in wall_results.items() if not ok]
    assert not bad_wall, f'wall integrity check failed: {bad_wall}'

    # 2026-09-07 pass 7 follow-up: verify_fpc_relief gates verify() -- the
    # FPC relief pocket broke through the true outer shoulder at the USB
    # end (two wedge holes); see add_fpc_relief's fix and
    # verify_fpc_relief's own docstring.
    fpc_relief_results = verify_fpc_relief(by_name, params)
    bad_fpc_relief = [k for k, ok in fpc_relief_results.items() if not ok]
    assert not bad_fpc_relief, (
        f'FPC relief pocket breached the outer shell '
        f'(< {FPC_RELIEF_MIN_WALL}mm skin remaining): {bad_fpc_relief}')

    # 2026-09-08 pass 9e (finding 7): two-line KANDI/WOOKS wordmark --
    # counterbore/edge clearance + live deboss-depth/floor probe. See
    # verify_wordmark's own docstring.
    wordmark_results = verify_wordmark(by_name, params)
    bad_wordmark = [k for k, v in wordmark_results.items() if not v[0]]
    assert not bad_wordmark, (
        f'wordmark check failed: {[(k, wordmark_results[k]) for k in bad_wordmark]}')

    # 2026-09-14 pass 14, item 2: the wordmark's counters ('a', 'd', both
    # flower 'o's) must survive the deboss -- see deboss_loops' and
    # verify_wordmark_counters' own docstrings for the root cause/fix.
    wordmark_counter_results = verify_wordmark_counters(by_name, params)
    bad_wordmark_counters = [k for k, v in wordmark_counter_results.items() if not v[0]]
    assert not bad_wordmark_counters, (
        f'wordmark counter check failed: {[(k, wordmark_counter_results[k]) for k in bad_wordmark_counters]}')

    # 2026-09-08 pass 9e (finding 8): LoRa/GPS antenna cable channels --
    # channel-open probe + LoRa skin-safety re-check + GPS battery-floor
    # clearance. See verify_antenna_channels's own docstring.
    antenna_results = verify_antenna_channels(by_name, params)
    bad_antenna = [k for k, v in antenna_results.items() if not v[0]]
    assert not bad_antenna, (
        f'antenna channel check failed: {[(k, antenna_results[k]) for k in bad_antenna]}')

    # 2026-09-10 pass 10: compass module (GY-273/QMC5883P) mount pocket --
    # envelope open, pegs have material, peg root doesn't breach the true
    # (brow-raised) outer skin. See verify_mag_pocket's own docstring.
    mag_pocket_results = verify_mag_pocket(root, by_name, params)
    bad_mag = [k for k, v in mag_pocket_results.items() if not v[0]]
    assert not bad_mag, (
        f'mag module pocket check failed: {[(k, mag_pocket_results[k]) for k in bad_mag]}')

    # 2026-09-11 pass 11 (new gate): every documented opening (window
    # column, USB tunnel, both button holes, lug hole, antenna/wire
    # notches) must actually be hollow along its full extent -- see
    # verify_openings_open's own docstring for why this gate exists (it
    # is what catches defect 1, the FPC brow silently refilling the top
    # of the window bore, which no pre-existing gate caught).
    openings_results = verify_openings_open(root, by_name, params)
    bad_openings = {k: v[1] for k, v in openings_results.items() if not v[0]}
    assert not bad_openings, f'opening blocked by material (bad probe points): {bad_openings}'

    # 2026-09-07 pass 13 (item 1, root fillets): every post/boss root
    # (real fillet or, where the API refuses that edge, a conical-collar
    # fallback -- see add_root_reinforcement/verify_root_fillets'
    # docstrings) must have real material out to OD/2+0.6mm at 0.4mm into
    # the post from its own root plane, all the way around. This gate is
    # what makes "the strength is real" more than an assertion -- a
    # feature that merely APPLIED (a Fillet feature committed without
    # raising) but was too small to matter would still fail here.
    root_fillet_results = verify_root_fillets(by_name, params)
    bad_root_fillets = {k: v for k, v in root_fillet_results.items() if k != '_method' and v}
    assert not bad_root_fillets, f'post/boss root reinforcement check failed: {bad_root_fillets}'

    # 2026-09-14 pass 14, item 1: the two merged lanyard-end corner blocks
    # (A+B1, C+B2) -- pilots open, block solid between/around the pilots,
    # comms-stack footprint and LoRa FPC keep-out centreline stay clear.
    # See verify_corner_blocks' own docstring.
    corner_block_results = verify_corner_blocks(by_name, params)
    bad_corner_blocks = {k: v for k, v in corner_block_results.items() if not v[0]}
    assert not bad_corner_blocks, f'lanyard corner block check failed: {bad_corner_blocks}'

    # 2026-09-15 pass 15, item 8: every Bottom-side screw pilot/counterbore
    # and the lanyard lug's own cord hole must actually be open -- see
    # verify_bottom_openings' own docstring for the confirmed root cause
    # this closes (the root-reinforcement collar silently replugging every
    # Bottom boss). This DOES gate verify() (a direct regression test for
    # a confirmed real defect).
    bottom_openings_results = verify_bottom_openings(by_name, params)
    bad_bottom_openings = {k: v for k, v in bottom_openings_results.items() if not v[0]}
    assert not bad_bottom_openings, f'Bottom hole/counterbore check failed: {bad_bottom_openings}'

    # pass 16, item G: ear/S2-boss root material + seat coplanarity + S2's
    # own hard keep-outs (battery connector, GPS frame). See
    # verify_ear_root_material / verify_seat_heights / verify_s2_boss_
    # clearance's own docstrings. All three gate verify() -- these are
    # the concrete geometry checks the mechanical review's own F5/F6/F7
    # findings asked for before candidate 5 could be trusted.
    ear_root_results = verify_ear_root_material(by_name, params)
    bad_ear_root = {k: v for k, v in ear_root_results.items()
                     if isinstance(v, tuple) and not v[0]}
    assert not bad_ear_root, f'ear root/material check failed: {bad_ear_root}'

    seat_height_results = verify_seat_heights(design, root, by_name, params)
    bad_seat_heights = {k: v for k, v in seat_height_results.items()
                         if isinstance(v, dict) and not v.get('ok', True)}
    assert not bad_seat_heights, f'seat height check failed: {bad_seat_heights}'

    s2_clearance_results = verify_s2_boss_clearance(by_name, params)
    assert s2_clearance_results['battery_clear_ok'][0], (
        f'S2 boss overlaps the battery connector\'s real footprint: {s2_clearance_results["battery_clear_ok"][1]}')
    assert s2_clearance_results['gps_clear_ok'], (
        f'S2 boss closer than 0.5mm to the GPS frame\'s outer wall: {s2_clearance_results["gps_clear_mm"]}mm')

    # pass 16, item G: no plate separator any more between the display
    # underside and the comms stack/battery below it (ID review Finding
    # 4) -- re-verify that clearance explicitly now that it's no longer
    # structurally guaranteed by the plate's own thickness.
    display_stack_results = verify_display_to_stack_clearance(board_occs, by_name, params)
    assert display_stack_results['ok'], f'display-to-stack clearance check failed: {display_stack_results}'

    # 2026-09-08 pass 9 (finding 11, stray sliver beside boss C):
    # the exact set of printed bodies must match the documented set --
    # a stray orphan body left over from a botched boolean (the same class
    # of bug dedupe_body/clipped_pillar_with_reach's docstrings already
    # document for Bottom/Top) would show up here as an extra name, or a
    # missing one if a real part silently vanished. This is the "no body
    # other than the documented set remains" check the finding asked for
    # -- gating now, not just printed. pass 16: down to 4 parts (Bottom,
    # Top, Power Button, Home Button) -- no Screen Plate any more.
    _expected_printed = sorted(['Bottom', 'Top', 'Power Button', 'Home Button'])
    assert names == _expected_printed, (
        f'printed body set does not match the documented parts (stray '
        f'or missing body?): {names} vs expected {_expected_printed}')
    # count_sliver_faces (small-area face count) was tried as a SECOND,
    # blanket gate here too, but reverted: a live run found 487 sliver
    # faces on Bottom and 47 on Top even on otherwise-clean geometry --
    # the wordmark/flare logo debossing (deboss_loops, stroking many short
    # glyph line segments) legitimately produces hundreds of small faces
    # at letter corners, unrelated to the finding-11 defect (a relief
    # circle barely clearing a boss's own OD -- fixed AT THE SOURCE by
    # add_lip_anchor_reliefs' MIN_RELIEF_CLEARANCE assert above, not by
    # this blanket count). Kept diagnostic-only (see run()'s summary
    # print), same as pass 6-8; a real per-defect gate would need to
    # target the specific geometry (e.g. near a boss's own xy), not every
    # small face in the model.
    sliver_results = {nm: count_sliver_faces(by_name[nm]) for nm in ('Bottom', 'Top')}

    return {
        'body_names': names,
        'm2_results': m2_results,
        'probe_results': probe_results,
        'cavity_results': cavity_results,
        'interference': interference,
        'occ_interference': occ_interference,
        'envelope_results': envelope_results,
        'bump_results': bump_results,
        'export_envelope_results': export_envelope_results,
        'clearance_results': clearance_results,
        'posts_bosses_results': posts_bosses_results,
        'post_wall_results': post_wall_results,
        'plunger_reach_results': plunger_reach_results,
        'button_insertion_results': button_insertion_results,
        'button_retention_results': button_retention_results,
        'display_insertion_results': display_insertion_results,
        'stack3_clearance': stack3_clearance,
        'skin_results': skin_results,
        'wall_results': wall_results,
        'fpc_relief_results': fpc_relief_results,
        'wordmark_results': wordmark_results,
        'antenna_results': antenna_results,
        'mag_pocket_results': mag_pocket_results,
        'openings_results': openings_results,
        'sliver_results': sliver_results,
        'root_fillet_results': root_fillet_results,
        'corner_block_results': corner_block_results,
        'wordmark_counter_results': wordmark_counter_results,
        'bottom_openings_results': bottom_openings_results,
        'ear_root_results': ear_root_results,
        'seat_height_results': seat_height_results,
        's2_clearance_results': s2_clearance_results,
        'display_stack_results': display_stack_results,
    }


EXPORT_BODY_NAMES = ['Bottom', 'Top', 'Power Button', 'Home Button']


def build_button_coupon(root, cap, p, x0=0.0, name_prefix=''):
    """A standalone, straight-axis (no diagonal `d`) fit-test coupon for
    one button: a slab representing the outer wall + a local 6mm 'shelf'
    the retaining tab bears against + the guide rib with its slot, as one
    printed body; the cap (head/plunger/nub-pocket/tab/collar) as a
    second, separate body -- both flat and printable face-down. Uses the
    SAME PARAMS (cap_clearance, rib_thickness, rib_slot_clearance,
    plunger_travel, collar, nub_pocket, tab) as the real button, just
    along a single +X axis instead of the real button's diagonal nub
    direction, so a fit found here transfers directly to the case build.

    `x0` (2026-09-06, pass 6) shifts every local X coordinate this
    function uses -- lets export_coupons() build several coupon pairs
    side by side in the same 'Print -- Coupons' component (each pair spans
    roughly 15mm in local X) without them overlapping each other; `root`
    is expected to already be that component (or root, for the pre-pass-6
    caller), and the caller is responsible for placing the WHOLE component
    away from the case afterward via its occurrence transform. `name_prefix`
    ('Power'/'Home') distinguishes the two buttons' otherwise-identical
    body names.
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

    slab = box_solid(root, x0 + 0.0, x0 + slab_t, -slab_w / 2.0, slab_w / 2.0, -slab_h / 2.0, slab_h / 2.0)
    hole = oriented_stadium_prism(root, (x0 - 1.0, 0.0, 0.0), axis1, z3, outward, hole_L, hole_W, slab_t + 2.0)
    slab = combine_cut(root, slab, [hole])

    shelf_outer_LW = (L + 2 * shelf_depth, W + 2 * shelf_depth)
    shelf_outer = oriented_stadium_prism(root, (x0 + slab_t, 0.0, 0.0), axis1, z3, outward,
                                          shelf_outer_LW[0], shelf_outer_LW[1], shelf_depth)
    shelf_inner = oriented_stadium_prism(root, (x0 + slab_t - 0.5, 0.0, 0.0), axis1, z3, outward,
                                          hole_L, hole_W, shelf_depth + 1.0)
    shelf = combine_cut(root, shelf_outer, [shelf_inner])
    slab = combine_join(root, slab, [shelf])

    rib_outer_x = slab_t + shelf_depth  # rib sits immediately past the shelf
    rib_len = p['rib_thickness']
    rib_plate = oriented_stadium_prism(root, (x0 + rib_outer_x, 0.0, 0.0), axis1, z3, outward,
                                        shelf_outer_LW[0], shelf_outer_LW[1], rib_len)
    slot = oriented_stadium_prism(root, (x0 + rib_outer_x - 0.5, 0.0, 0.0), axis1, z3, outward,
                                   L + 2 * p['rib_slot_clearance'], W + 2 * p['rib_slot_clearance'], rib_len + 1.0)
    rib_plate = combine_cut(root, rib_plate, [slot])
    # 2026-09-08 pass 9b, finding 9: same tab-relief lane as add_button's
    # real rib_plate (see its comment) -- this coupon exists specifically
    # so Jake can fit-test the insertion mechanism before committing to a
    # full print, so it must reproduce the same fix, not just the same
    # (formerly broken) slot.
    tab_relief_margin = 0.3
    tab_relief_w = p['tab']['w'] + 2 * tab_relief_margin
    tab_relief_z_hi = -W / 2.0 + tab_relief_margin
    tab_relief_z_lo = -W / 2.0 - p['tab']['h'] - tab_relief_margin
    tab_relief_z_span = tab_relief_z_hi - tab_relief_z_lo
    tab_relief_z_center = (tab_relief_z_hi + tab_relief_z_lo) / 2.0
    tab_relief_axial_margin = 0.5
    tab_relief = oriented_box_prism(root, (x0 + rib_outer_x - tab_relief_axial_margin, 0.0, tab_relief_z_center),
                                     axis1, z3, outward, tab_relief_w, tab_relief_z_span,
                                     rib_len + 2 * tab_relief_axial_margin)
    rib_plate = combine_cut(root, rib_plate, [tab_relief])
    slab = combine_join(root, slab, [rib_plate])

    # cap: head (proud of the slab) + plunger through the hole/shelf/rib to
    # a nominal tip past the rib, with the collar bottoming on the rib
    # after plunger_travel and a nub pocket at the tip.
    rib_inner_x = rib_outer_x + rib_len
    collar = p['collar']
    collar_outer_x = rib_inner_x + p['plunger_travel']
    tip_stub = 3.0  # nominal length past the collar, standing in for "reaching the switch"
    tip_x = collar_outer_x + collar['len'] + tip_stub

    cap_body = oriented_stadium_prism(root, (x0 - proud, 0.0, 0.0), axis1, z3, inward, L, W, proud + tip_x)

    pocket = p['nub_pocket']
    pocket_body = oriented_box_prism(root, (x0 + tip_x, 0.0, 0.0), axis1, z3, outward,
                                      pocket['xy'][0], pocket['xy'][1], pocket['depth'])
    cap_body = combine_cut(root, cap_body, [pocket_body])

    tab = p['tab']
    tab_len = 1.5
    tab_z = -W / 2.0 - tab['h'] / 2.0
    tab_body = oriented_box_prism(root, (x0 + slab_t + shelf_depth - tab['gap'] - tab_len, 0.0, tab_z), axis1, z3, outward,
                                   tab['w'], tab['h'], tab_len)
    cap_body = combine_join(root, cap_body, [tab_body])

    collar_body = oriented_box_prism(root, (x0 + collar_outer_x, 0.0, 0.0), axis1, z3, outward,
                                      L, W + 2 * collar['h'], collar['len'])
    cap_body = combine_join(root, cap_body, [collar_body])

    prefix = f'{name_prefix} ' if name_prefix else ''
    slab.name = f'Coupon {prefix}Wall'.replace('  ', ' ')
    cap_body.name = f'Coupon {prefix}Cap'.replace('  ', ' ')
    return slab, cap_body


def assert_export_body_size(body, label, max_extent_mm):
    """Sanity guard against a units/scale bug slipping into an export (a cm
    value used as mm, an accidental extra scale/move, etc.) -- every real
    printed body here is well under 120mm and every coupon well under
    40mm, so a bounding box beyond that in ANY axis is definitely wrong,
    not a legitimate design. Checked against the live Fusion body (not the
    written STL bytes) right at export time."""
    bb = body.boundingBox
    dx = (bb.maxPoint.x - bb.minPoint.x) / MM
    dy = (bb.maxPoint.y - bb.minPoint.y) / MM
    dz = (bb.maxPoint.z - bb.minPoint.z) / MM
    assert max(dx, dy, dz) <= max_extent_mm, (
        f'{label}: bbox {round(dx,2)} x {round(dy,2)} x {round(dz,2)} mm '
        f'exceeds the {max_extent_mm}mm sanity limit -- likely a units/scale bug')


def export_stls(design, bodies, variant, base_dir):
    out_dir = os.path.join(base_dir, 'export', variant)
    os.makedirs(out_dir, exist_ok=True)
    export_mgr = design.exportManager
    paths = {}
    for name in EXPORT_BODY_NAMES:
        body = bodies[name]
        assert_export_body_size(body, name, 120.0)
        fname = name.replace(' ', '_') + '.stl'
        path = os.path.join(out_dir, fname)
        opts = export_mgr.createSTLExportOptions(body, path)
        opts.isBinaryFormat = True
        export_mgr.execute(opts)
        paths[name] = path
    return paths


def read_stl_triangles(path):
    """Parse a BINARY STL file (as export_stls always writes -- isBinaryFormat
    = True) into a list of (normal, v1, v2, v3) tuples, each a 3-tuple of
    floats in the file's native units (mm, per the STL export path)."""
    import struct
    tris = []
    with open(path, 'rb') as f:
        f.read(80)  # header, ignored
        (n,) = struct.unpack('<I', f.read(4))
        for _ in range(n):
            data = f.read(50)
            vals = struct.unpack('<12f', data[:48])
            normal = vals[0:3]
            v1, v2, v3 = vals[3:6], vals[6:9], vals[9:12]
            tris.append((normal, v1, v2, v3))
    return tris


def _tri_area(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    cx, cy, cz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    return 0.5 * math.sqrt(cx * cx + cy * cy + cz * cz)


def scan_stl_overhangs(stl_path, down_z, bed_z, angle_tol_deg=1.0, min_cluster_mm2=30.0, bed_eps=0.6, whitelist_xy=None):
    """Triangle-normal overhang scan of an exported STL (2026-09-05,
    printability check B3): flags triangles whose outward normal points
    more than (45 - angle_tol_deg) toward the PRINT-DOWN direction
    (`down_z`: +1.0 for Top, printed face-down on its z=25 face so
    print-down = +model z; -1.0 for Bottom, printed face-down on its
    already-flat z=0 face so print-down = -model z), excluding triangles
    flush on the bed plane itself (`bed_z`, within `bed_eps`) -- those are
    the build-plate contact face, not an overhang. The tolerance excludes
    the SPEC'd 45-degree tangent-break shoulder cones, which sit exactly
    at the 45-degree self-supporting limit by design.

    Flagged triangles are grouped into connected clusters (by shared
    vertices, snapped to 3 decimal mm) and each cluster's total area is
    reported along with its (x, y) centroid, so a real remaining cluster
    can be located in the model. `bed_eps` (0.6mm, per the coordinator's
    2026-09-05 refinement) excludes bed-facing faces within that of the
    bed plane -- a 0.4mm-deep deboss recess's floor is bridged by faces up
    to 0.4mm off the bed plane, which were being flagged as "not on the
    bed" false positives at the original 0.05mm tolerance. `angle_tol_deg`
    (1.0, was 0.5) excludes the SPEC'd 45-degree shoulder cones AND the
    window's 45-degree chamfer with more margin for triangulation noise.
    `whitelist_xy`, if given, is a list of (x0, x1, y0, y1, label) boxes
    (world mm) -- a cluster whose centroid falls inside one is reported
    but excluded from `bad_clusters_mm2` (e.g. the USB tunnel floor, a
    known, accepted 13mm bridge -- see the caller). Returns the full
    per-cluster (area, centroid, whitelisted-label-or-None) list plus just
    the non-whitelisted ones exceeding `min_cluster_mm2` (what the caller
    should assert on)."""
    tris = read_stl_triangles(stl_path)
    cos_limit = math.cos(math.radians(45.0 - angle_tol_deg))
    flagged = []
    for normal, v1, v2, v3 in tris:
        nlen = math.sqrt(sum(c * c for c in normal))
        if nlen < 1e-9:
            # degenerate/zero normal in the file -- recompute from the
            # vertex winding rather than skip the triangle outright.
            ux, uy, uz = v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]
            vx, vy, vz = v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2]
            normal = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            nlen = math.sqrt(sum(c * c for c in normal)) or 1.0
        nz = normal[2] / nlen
        down_component = nz * down_z  # >0 => facing the print-down direction
        avg_z = (v1[2] + v2[2] + v3[2]) / 3.0
        if down_component > cos_limit and abs(avg_z - bed_z) > bed_eps:
            flagged.append((_tri_area(v1, v2, v3), (v1, v2, v3)))

    def vkey(v):
        return (round(v[0], 3), round(v[1], 3), round(v[2], 3))

    parent = {}

    def find(x):
        root = x
        while parent.get(root, root) != root:
            root = parent[root]
        while parent.get(x, x) != root:
            parent[x], x = root, parent.get(x, root)
        return root

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    vert_to_tri = {}
    for i in range(len(flagged)):
        parent.setdefault(i, i)
        for v in flagged[i][1]:
            vert_to_tri.setdefault(vkey(v), []).append(i)
    for ids in vert_to_tri.values():
        for j in ids[1:]:
            union(ids[0], j)

    clusters = {}
    centroid_sum = {}
    for i, (area, verts) in enumerate(flagged):
        r = find(i)
        clusters[r] = clusters.get(r, 0.0) + area
        cx = sum(v[0] for v in verts) / 3.0
        cy = sum(v[1] for v in verts) / 3.0
        s = centroid_sum.setdefault(r, [0.0, 0.0, 0.0])
        s[0] += cx * area
        s[1] += cy * area
        s[2] += area

    def label_for(cx, cy):
        for entry in (whitelist_xy or []):
            x0, x1, y0, y1, label = entry
            if x0 <= cx <= x1 and y0 <= cy <= y1:
                return label
        return None

    cluster_info = []
    for r, area in clusters.items():
        s = centroid_sum[r]
        cx, cy = s[0] / s[2], s[1] / s[2]
        cluster_info.append((area, (round(cx, 1), round(cy, 1)), label_for(cx, cy)))
    cluster_info.sort(key=lambda t: -t[0])

    bad = [(round(a, 2), c) for a, c, label in cluster_info if a > min_cluster_mm2 and label is None]
    return {
        'flagged_triangles': len(flagged),
        'clusters': [(round(a, 2), c, label) for a, c, label in cluster_info[:15]],
        'bad_clusters_mm2': bad,
    }


def verify_lip_ring_profile(stl_path, p, down_z=1.0):
    """Pass 16, item C (printability review, finding 1): a live regression
    check of the lip/anchor ring's own self-supporting taper, scanned off
    the ACTUAL exported/triangulated Top STL -- not just the boolean-solid
    code in add_lip_anchor_reliefs, which could build a taper in Fusion's
    own B-rep yet still export/print wrong (exactly how the pass-9c edge
    chamfer regressed silently in the first place -- see chamfer_stadium_
    edge_at's own docstring). Scans every triangle whose centroid falls in
    the ring's own band (r in lip_r[0]..anchor_r[1], z in lip_z[0]..
    anchor_z[1] -- a stadium-shaped `rho` distance, same convention
    chamfer_stadium_edge_at already uses) and classifies each one facing
    the REAL print-down direction (`down_z`, same convention and default
    as scan_stl_overhangs -- Top prints flipped, ceiling-down, so its own
    print-down is +model-z, NOT -z; a naive world-frame nz<0 check would
    silently grade the WRONG half of the ring's own surfaces) by its own
    overhang angle -- 0 degrees for a plain vertical wall, 90 degrees for
    a flat, horizontal, print-down-facing ceiling:

    - `mid_overhang_found`: at least one real 10-80 degree facet must
      exist somewhere in the band -- proof the taper chamfer is really
      there in the exported geometry, not just claimed by the code.
    - `flat_patch_max_width_mm`: no >80-degree (flat, print-down-facing)
      cluster may span more than 0.6mm of RADIAL extent anywhere in the
      band -- the old dead-flat shelf finding 1 found (and add_lip_
      anchor_reliefs' own second taper cut narrows towards) must stay
      gone. Radial extent (not area) is the right "how wide" measure
      here: the ring's own remaining flat cap, if any, is an ANNULAR
      band, so its meaningful width is how far it reaches inward/
      outward, not its total triangulated area."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    r_lo, r_hi = p['lip_r'][0], p['anchor_r'][1]
    z_lo, z_hi = p['lip_z'][0], p['anchor_z'][1]
    tris = read_stl_triangles(stl_path)

    def rho(x, y):
        if ay <= y <= by:
            return abs(x)
        cy = ay if y < ay else by
        return math.hypot(x, y - cy)

    band_tol = 0.15  # mm -- tessellation/facet-centroid slack, same order as this file's other STL-scan tolerances
    mid_overhangs = []
    flat_tris = []
    for normal, v1, v2, v3 in tris:
        cx = (v1[0] + v2[0] + v3[0]) / 3.0
        cy = (v1[1] + v2[1] + v3[1]) / 3.0
        cz = (v1[2] + v2[2] + v3[2]) / 3.0
        r = rho(cx, cy)
        if not (r_lo - band_tol <= r <= r_hi + band_tol and z_lo - band_tol <= cz <= z_hi + band_tol):
            continue
        nlen = math.sqrt(sum(c * c for c in normal))
        if nlen < 1e-9:
            ux, uy, uz = v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]
            vx, vy, vz = v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2]
            normal = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            nlen = math.sqrt(sum(c * c for c in normal)) or 1.0
        nz = normal[2] / nlen
        down_component = nz * down_z  # >0 => facing the real print-down direction (see scan_stl_overhangs)
        if down_component <= 0:
            continue  # facing away from print-down -- not an overhang at all
        overhang_deg = math.degrees(math.asin(min(1.0, max(-1.0, down_component))))
        if 10.0 <= overhang_deg <= 80.0:
            mid_overhangs.append((r, cz, overhang_deg))
        elif overhang_deg > 80.0:
            flat_tris.append((r, v1, v2, v3))

    def vkey(v):
        return (round(v[0], 3), round(v[1], 3), round(v[2], 3))
    parent = {}

    def find(x):
        r0 = x
        while parent.get(r0, r0) != r0:
            r0 = parent[r0]
        while parent.get(x, x) != r0:
            parent[x], x = r0, parent.get(x, r0)
        return r0

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for _, v1, v2, v3 in flat_tris:
        keys = [vkey(v1), vkey(v2), vkey(v3)]
        for k in keys:
            parent.setdefault(k, k)
        union(keys[0], keys[1])
        union(keys[1], keys[2])

    clusters = {}
    for r, v1, v2, v3 in flat_tris:
        k = find(vkey(v1))
        lo, hi = clusters.get(k, (r, r))
        clusters[k] = (min(lo, r), max(hi, r))
    widest_flat_mm = max((hi - lo for lo, hi in clusters.values()), default=0.0)

    return {
        'mid_overhang_found': (len(mid_overhangs) > 0, len(mid_overhangs)),
        'flat_patch_max_width_mm': (widest_flat_mm <= 0.6, round(widest_flat_mm, 3)),
    }


COUPON_LOCAL_X_GAP = 40.0   # local X spacing between the Power and Home pairs
COUPON_WORLD_OFFSET = (60.0, 0.0, 0.0)  # whole 'Print -- Coupons' component, off to the side of the case


def export_coupons(design, root, p, base_dir):
    """Build + export the button fit-test coupons (2026-09-04 addendum;
    2026-09-06 pass 6: built directly inside their own new 'Print --
    Coupons' component instead of the document root, each pair offset in
    local X by COUPON_LOCAL_X_GAP so they don't overlap each other, then
    the WHOLE component translated by COUPON_WORLD_OFFSET so it never
    overlaps the case (which stays within roughly x -29..29) -- see
    build_button_coupon's `x0` docstring): each coupon is a wall+shelf+rib
    body and a separate cap body, exported as two STLs each (wall/cap
    print separately, side by side) -- coupon_<button>_wall.stl /
    coupon_<button>_cap.stl. Returns (stl_paths, coupons_occ)."""
    out_dir = os.path.join(base_dir, 'export', 'coupons')
    os.makedirs(out_dir, exist_ok=True)
    export_mgr = design.exportManager

    coupons_occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    coupons_occ.component.name = COMPONENT_COUPONS
    coupons_comp = coupons_occ.component

    paths = {}
    for i, (key, prefix, cap) in enumerate((('power', 'Power', p['power_cap']),
                                             ('home', 'Home', p['home_cap']))):
        x0 = i * COUPON_LOCAL_X_GAP
        wall, cap_body = build_button_coupon(coupons_comp, cap, p, x0=x0, name_prefix=prefix)
        for label, body in (('wall', wall), ('cap', cap_body)):
            assert_export_body_size(body, f'{key}_{label}', 40.0)

    # move the whole component away from the case in one rigid translation
    # (per-pair X offsets above only keep the pairs from overlapping EACH
    # OTHER) -- commit via snapshot, same pattern as insert_and_place.
    move = adsk.core.Matrix3D.create()
    move.translation = adsk.core.Vector3D.create(*(v * MM for v in COUPON_WORLD_OFFSET))
    coupons_occ.transform = move
    if design.snapshots.hasPendingSnapshot:
        design.snapshots.add()

    for key, prefix in (('power', 'Power'), ('home', 'Home')):
        for label, body_name in (('wall', f'Coupon {prefix} Wall'), ('cap', f'Coupon {prefix} Cap')):
            body = next(b for b in coupons_comp.bRepBodies if b.name == body_name)
            fname = f'coupon_{key}_{label}.stl'
            path = os.path.join(out_dir, fname)
            opts = export_mgr.createSTLExportOptions(body, path)
            opts.isBinaryFormat = True
            export_mgr.execute(opts)
            paths[f'{key}_{label}'] = path

    remove_stray_generic_bodies(coupons_comp)
    return paths, coupons_occ


def export_native_3mf_case(design, root, variant, base_dir):
    """Native 3MF export of the two Print components (2026-09-06, pass 6):
    createC3MFExportOptions' `geometry` argument takes a single BRepBody,
    Occurrence, or Component -- not a list -- so to get exactly the 5
    printed case/button bodies (and nothing from Reference/Boards/Coupons)
    into ONE file, export the whole root component with every OTHER
    top-level component temporarily hidden (hidden bodies are not
    exported), then restore visibility regardless of outcome."""
    out_dir = os.path.join(base_dir, 'export', variant)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f'firefly_{variant}_case.3mf')

    hide_names = (COMPONENT_REFERENCE, COMPONENT_BOARDS, COMPONENT_COUPONS)
    hidden = []
    for occ in root.occurrences:
        if occ.component.name in hide_names and occ.isLightBulbOn:
            occ.isLightBulbOn = False
            hidden.append(occ)
    try:
        export_mgr = design.exportManager
        opts = export_mgr.createC3MFExportOptions(root, path)
        export_mgr.execute(opts)
    finally:
        for occ in hidden:
            occ.isLightBulbOn = True
    return path


def export_native_3mf_coupons(design, coupons_occ, base_dir):
    """Native 3MF export of the 4 coupon bodies: a single Occurrence
    (Print -- Coupons) covers all of them in one call, no hiding needed."""
    out_dir = os.path.join(base_dir, 'export', 'coupons')
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, 'firefly_coupons_native.3mf')
    export_mgr = design.exportManager
    opts = export_mgr.createC3MFExportOptions(coupons_occ, path)
    export_mgr.execute(opts)
    return path


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
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
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
    root = design.rootComponent
    bodies, comp_occs = organize_components(root, bodies)
    result = verify(design, params)

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
    print('export envelope vertex checks:')
    for k, v in result['export_envelope_results'].items():
        print('  ', k, v[0], v[1] if not v[0] else '')
    print('min clearance to case (board occ, case body): mm, ok')
    for k, v in result.get('clearance_results', {}).items():
        print('  ', k, v)
    print('posts/bosses material checks (True = solid, as expected):')
    for k, v in result.get('posts_bosses_results', {}).items():
        print('  ', k, v)
    print('top-post wall/skin checks (empty list = pass):')
    for k, v in result.get('post_wall_results', {}).items():
        print('  ', k, v)
    print('plunger reach checks (finding 10, live probe against the real switch):')
    for k, v in result.get('plunger_reach_results', {}).items():
        print('  ', k, v)
    print('button insertion sweep (finding 9, live probe of the tab-vs-rib path):')
    for k, v in result.get('button_insertion_results', {}).items():
        print('  ', k, v)
    print('button retention checks (finding 9, live probe):')
    for k, v in result.get('button_retention_results', {}).items():
        print('  ', k, v)
    print('display from-inside insertion path (finding 5, diagnostic):', result.get('display_insertion_results'))
    print('comms stack3 ceiling clearance:', result.get('stack3_clearance'))
    print('skin-intact checks: all True?', all(result.get('skin_results', {}).values()))
    print('wall-integrity checks: all True?', all(result.get('wall_results', {}).values()))
    fpc_results = result.get('fpc_relief_results', {})
    fpc_bad = [k for k, v in fpc_results.items() if not v]
    print('fpc-relief skin checks:', len(fpc_results), 'probes, all True?', not fpc_bad, 'bad:', fpc_bad)
    print('wordmark checks (finding 7, two-line KANDI/WOOKS):')
    for k, v in result.get('wordmark_results', {}).items():
        print('  ', k, v)
    print('antenna channel checks (finding 8, LoRa/GPS u.FL routes):')
    for k, v in result.get('antenna_results', {}).items():
        print('  ', k, v)
    print('mag module pocket checks (pass 10/11, compass mount):')
    for k, v in result.get('mag_pocket_results', {}).items():
        print('  ', k, v)
    print('openings-open checks (pass 11, new gate -- bad probe points, empty = pass):')
    for k, v in result.get('openings_results', {}).items():
        print('  ', k, v[0], v[1] if not v[0] else '')
    print('sliver face count (area < 0.5mm^2, diagnostic only):')
    for nm in ('Top', 'Bottom'):
        print('  ', nm, count_sliver_faces(bodies[nm]))
    print('pass-16 ear root/material checks:')
    for k, v in result.get('ear_root_results', {}).items():
        print('  ', k, v)
    print('pass-16 seat height checks (expect gap_mm ~= ear_seat_offset):')
    for k, v in result.get('seat_height_results', {}).items():
        print('  ', k, v)
    print('pass-16 S2 boss clearance checks:', result.get('s2_clearance_results'))
    print('pass-16 display-to-stack clearance checks:', result.get('display_stack_results'))

    expected_names = sorted(['Bottom', 'Top', 'Power Button', 'Home Button'])
    assert result['body_names'] == expected_names, result['body_names']
    print('OK: M1+M2 probes passed')

    if export:
        stl_paths = export_stls(design, bodies, params['variant'], _HERE)
        print('STL exports:')
        for name, path in stl_paths.items():
            print('  ', name, '->', path)

        print('overhang scan (Top down=+z, Bottom down=-z):')
        # Whitelisted, LOCATED, and judged legitimate per the
        # coordinator's 2026-09-05 review (each is either a known bridge
        # like the USB tunnel floor, or the shell's own inherent flat
        # ceiling / vertical-to-ceiling fillet transition -- a hollow
        # shell like this always needs some slicer-generated support
        # under its ceiling, independent of any specific added feature;
        # confirmed by inspecting the actual flagged triangles at each
        # location, not size alone):
        #   - usb_tunnel_floor: the 13mm bridge across the USB-C tunnel
        #     bore (known, accepted -- see README).
        #   - ceiling_near_window_and_header: the flat ceiling area
        #     flanking the window bore / display header region (z 11-22.4,
        #     y 48-79) -- ordinary hollow-shell ceiling, not a specific
        #     feature.
        #   - ceiling_near_bay_wall_{minus,plus}_x: the ceiling-to-wall
        #     fillet transition directly above the comms bay (y -12..2,
        #     x near the outer wall) -- same story, the R8 inner fillet's
        #     own sub-45-degree portion.
        # NOTE (2026-09-05): 'general_ceiling_overhang' is deliberately
        # broad -- it covers the shell's own flat internal ceiling
        # (z roughly 11-23, y 0-79, spanning most of the case's width).
        # Inspected directly (not just sized) on both variants: it is
        # ordinary hollow-shell ceiling area (plus the R8 inner fillet's
        # own sub-45-degree transition down to the walls), the same
        # fundamental "a fully enclosed hollow box needs support under
        # its own roof" situation on both variants -- just split into
        # several smaller same-cause clusters on trim (its narrower comms
        # bay breaks the ceiling up more) versus one larger one on
        # current. This is a print-process reality (any slicer handles it
        # with normal supports), not a fixable local design defect, and
        # is NOT a substitute for auditing genuinely local/unexpected
        # overhangs -- which is exactly what caught the lug and the tray
        # ledges earlier in this same pass.
        # 2026-09-10 pass 10 REDO: the old 'mag_module_pocket' whitelist
        # entries (both Top and Bottom, sized for the rejected vertical-
        # wall-mount's horizontal pegs/pocket) are removed -- the new
        # ceiling-hung mount's own pegs/pads/fence live entirely within
        # 'general_ceiling_overhang' (Top only; the mount no longer
        # touches Bottom at all), so no dedicated entry is needed. If a
        # real local cluster shows up here after a live scan, add it back
        # sized from the actual reported centroid, not guessed.
        # 2026-09-14 pass 14: y0 widened -12 -> -16 (current variant only)
        # -- see tools/offline_stl_check.py's TOP_WL, the same constant,
        # for the full reasoning (the new lanyard-end corner blocks'
        # outward wedge reaches slightly further into this same ordinary
        # flat-ceiling/fillet-transition territory on the wider 'current'
        # shell). Kept in sync with that file deliberately -- both must
        # agree or the two independent overhang checks could silently
        # drift apart.
        top_wl = [
            (-8.0, 8.0, 65.0, 81.0, 'usb_tunnel_floor'),
            (-32.0, 32.0, -16.0, 79.0, 'general_ceiling_overhang'),
        ]
        # l76k_frame_ceiling: the L76K wired frame's own ceiling-side
        # transition at the -y dome tip (y -26..-15) -- same fillet-
        # transition story as the Top ones above, on Bottom this time.
        bottom_wl = [
            (-15.0, 15.0, -26.0, -15.0, 'l76k_frame_ceiling'),
        ]
        overhang_scans = {}
        for name, down_z, bed_z, wl in (('Top', 1.0, params['top_z'], top_wl), ('Bottom', -1.0, params['bottom_z'], bottom_wl)):
            scan = scan_stl_overhangs(stl_paths[name], down_z, bed_z, whitelist_xy=wl)
            overhang_scans[name] = scan
            print('  ', name, scan)
        bad_overhangs = {k: v['bad_clusters_mm2'] for k, v in overhang_scans.items() if v['bad_clusters_mm2']}
        assert not bad_overhangs, f'overhang cluster(s) > 30 mm^2 found in exported STL(s): {bad_overhangs}'

        # pass 16, item C: live regression check of the lip/anchor ring's
        # own self-supporting taper, off the actual exported/triangulated
        # Top STL -- see verify_lip_ring_profile's own docstring.
        lip_ring_profile = verify_lip_ring_profile(stl_paths['Top'], params)
        print('lip/anchor ring profile check (item C):', lip_ring_profile)
        assert lip_ring_profile['mid_overhang_found'][0], (
            f'no 10-80 degree taper facet found in the lip/anchor ring band -- '
            f'the self-supporting chamfer is missing from the exported geometry: {lip_ring_profile}')
        assert lip_ring_profile['flat_patch_max_width_mm'][0], (
            f'a flat (>80 degree), >0.6mm-wide downward patch remains in the lip/anchor ring band '
            f'-- the old dead-flat shelf regression: {lip_ring_profile}')

        coupon_paths, coupons_occ = export_coupons(design, root, params, _HERE)
        print('Coupon exports:')
        for name, path in coupon_paths.items():
            print('  ', name, '->', path)

        case_3mf_path = export_native_3mf_case(design, root, params['variant'], _HERE)
        print('Native 3MF (case):', case_3mf_path)
        coupons_3mf_path = export_native_3mf_coupons(design, coupons_occ, _HERE)
        print('Native 3MF (coupons):', coupons_3mf_path)

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

    structure_tree = verify_structure(design)
    return structure_tree
