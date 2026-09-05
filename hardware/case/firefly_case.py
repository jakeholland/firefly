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


def add_case_boss(root, bodies, cx, cy, p, is_D=False):
    boss_r = p['boss_dia'] / 2.0
    bottom_boss = cylinder_solid(root, cx, cy, boss_r, 2.0, p['split_z'])
    bottom = combine_join(root, bodies['Bottom'], [bottom_boss])
    hole = cylinder_solid(root, cx, cy, p['screw_hole_dia'] / 2.0, -0.5, p['split_z'] + 0.5)
    bottom = combine_cut(root, bottom, [hole])
    cb_h = p['counterbore_D_h'] if is_D else p['counterbore_ABC_h']
    cb = cylinder_solid(root, cx, cy, p['counterbore_ABC_dia'] / 2.0, -0.5, cb_h)
    bottom = combine_cut(root, bottom, [cb])
    bodies['Bottom'] = bottom

    if not is_D:
        top_boss = cylinder_solid(root, cx, cy, boss_r, p['split_z'], p['top_ceiling_underside_z'])
        top = combine_join(root, bodies['Top'], [top_boss])
        pilot = cylinder_solid(root, cx, cy, p['top_pilot_dia'] / 2.0, p['top_pilot_z'][0], p['top_pilot_z'][1])
        top = combine_cut(root, top, [pilot])
        bodies['Top'] = top
    return bodies


def add_case_screws(root, bodies, p):
    for s in p['screws_ABC']:
        cx, cy = s['xy']
        bodies = add_case_boss(root, bodies, cx, cy, p, is_D=False)
    cx, cy = p['screw_D']['xy']
    bodies = add_case_boss(root, bodies, cx, cy, p, is_D=True)
    return bodies


def add_top_posts(root, bodies, p):
    top = bodies['Top']
    for name, (cx, cy) in p['top_posts'].items():
        post = cylinder_solid(root, cx, cy, p['top_post_dia'] / 2.0, p['top_post_z'][0], p['top_post_z'][1])
        top = combine_join(root, top, [post])
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
    bodies = add_case_screws(root, bodies, params)
    bodies = add_top_posts(root, bodies, params)

    plate = build_screen_plate(root, params)
    bodies['Screen Plate'] = plate

    insert_display_pcba(app, root, params)

    return bodies


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

    z_top_probe = 22.0
    expect_top = inner_rho_at_z(p, z_top_probe)
    found = find_first_solid_x(top_bodies, 10.0, z_top_probe)
    checks.append(('top_cavity', expect_top, found, found is not None and abs(found - expect_top) <= 0.15))

    z_bot_probe = 9.3
    expect_bot = inner_rho_at_z(p, z_bot_probe)
    found2 = find_first_solid_x(bot_bodies, 10.0, z_bot_probe)
    checks.append(('bottom_cavity', expect_bot, found2, found2 is not None and abs(found2 - expect_bot) <= 0.15))
    return checks


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


def verify(design, params):
    root = design.rootComponent
    bodies = [b for b in root.bRepBodies]
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

    return {
        'body_names': names,
        'probe_results': probe_results,
        'cavity_results': cavity_results,
        'interference': interference,
        'occ_interference': occ_interference,
    }


def _find_or_create_doc(app, doc_name):
    for d in app.documents:
        if d.name == doc_name:
            d.activate()
            return d
    doc = app.documents.add(adsk.fusion.DocumentTypes.FusionDesignDocumentType)
    doc.name = doc_name
    return doc


def run(_context: str, variant=None):
    """variant: optional override ('current' | 'trim'); defaults to the
    module-level VARIANT (currently 'trim', Jake's default)."""
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

    assert result['body_names'] == sorted(['Bottom', 'Screen Plate', 'Top']), result['body_names']
    print('OK: M1 probes passed')
