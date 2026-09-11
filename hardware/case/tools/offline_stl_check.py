#!/usr/bin/env python3
"""Offline STL checks for the Firefly case exports (pass 7 verify sweep):
manifold-edge check + envelope-vertex check + overhang scan -- ported
from hardware/case/firefly_case.py's scan_stl_overhangs/rho_from_spine/
check_body_envelope_vertices, but pure Python (no adsk) so it runs
without Fusion, directly against the exported STL bytes on disk.
"""
import math
import struct
import sys
import json


def read_stl_triangles(path):
    tris = []
    with open(path, 'rb') as f:
        header = f.read(80)
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


def check_body_count(tris, tol=1e-4):
    """Pass 16, item G: every exported part must be EXACTLY ONE connected
    body -- a stray disjoint island (a silent no-op boolean join, the
    same class of bug this file's own dedupe_body/clipped_pillar_with_
    reach docstrings warn about elsewhere) would still pass the manifold
    check above (each disjoint piece is its own perfectly watertight
    mesh) and could still slice/print as several separate floating
    parts. Union-find over shared (snapped) vertices, same idiom
    check_manifold's own vkey already uses -- the number of resulting
    connected components is the real body count."""
    def vkey(v):
        return (round(v[0] / tol) * tol, round(v[1] / tol) * tol, round(v[2] / tol) * tol)

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

    for _, v1, v2, v3 in tris:
        keys = [vkey(v1), vkey(v2), vkey(v3)]
        for k in keys:
            parent.setdefault(k, k)
        union(keys[0], keys[1])
        union(keys[1], keys[2])

    roots = {find(k) for k in parent}
    return {'body_count': len(roots), 'one_body': len(roots) == 1}


def check_manifold(tris, tol=1e-4):
    """Every edge of a watertight (manifold) mesh is shared by exactly 2
    triangles (one traversal in each direction). Vertices are snapped to
    `tol` mm to merge coincident-but-not-bit-identical STL vertices."""
    def vkey(v):
        return (round(v[0] / tol) * tol, round(v[1] / tol) * tol, round(v[2] / tol) * tol)

    edge_count = {}
    for _, v1, v2, v3 in tris:
        verts = [vkey(v1), vkey(v2), vkey(v3)]
        for i in range(3):
            a, b = verts[i], verts[(i + 1) % 3]
            key = (a, b) if a < b else (b, a)
            edge_count[key] = edge_count.get(key, 0) + 1
    bad_edges = [(k, c) for k, c in edge_count.items() if c != 2]
    degenerate = sum(1 for _, v1, v2, v3 in tris if _tri_area(v1, v2, v3) < 1e-9)
    return {
        'triangle_count': len(tris),
        'edge_count': len(edge_count),
        'non_manifold_edges': len(bad_edges),
        'sample_bad_edges': bad_edges[:5],
        'degenerate_triangles': degenerate,
        'manifold': len(bad_edges) == 0,
    }


def rho_from_spine(spine_a_y, spine_b_y, x, y):
    ay, by = spine_a_y, spine_b_y
    if ay <= y <= by:
        return abs(x)
    center_y = ay if y < ay else by
    return math.hypot(x, y - center_y)


def check_envelope(tris, params, name):
    ay = params['spine_a'][1]
    by = params['spine_b'][1]
    outer_r = params['outer_radius']
    lug = params['lug']
    half_w = lug['width'] / 2.0
    z0, z1 = lug['z']
    s_wall = min(rho_at_z_approx(params, z0), rho_at_z_approx(params, z1))
    y_outer = ay - s_wall
    y_far = y_outer - lug['protrusion']
    y_root = ay - (s_wall - 3.0)
    lug_y_thresh = y_root
    lug_x_half = half_w + 0.5
    is_cap = name in ('Power_Button', 'Home_Button')
    limit = outer_r + (0.45 + 0.05 if is_cap else 0.15)

    # 2026-09-08 pass 9 through pass 12: the FPC relief brow (add_fpc_brow
    # in firefly_case.py) intentionally raised Top's outer surface over
    # the relief pocket's own footprint, and this offline script carried
    # a ported exception matching firefly_case.py's live
    # check_body_envelope_vertices so the two could never disagree.
    # 2026-09-13 pass 12b: the brow is deleted (usb_end_extension_mm
    # recovers the same skin by lengthening the envelope instead of
    # raising it -- see firefly_case.py's FPC_RELIEF_MIN_WALL module
    # comment), so this script is back to exactly its pre-pass-9 shape,
    # same as pass 10 REDO already did for the compass-mount exemption.

    bad = []
    seen = set()
    for _, v1, v2, v3 in tris:
        for v in (v1, v2, v3):
            key = (round(v[0], 3), round(v[1], 3), round(v[2], 3))
            if key in seen:
                continue
            seen.add(key)
            x, y, z = v
            if y < lug_y_thresh and abs(x) < lug_x_half:
                continue
            rho = rho_from_spine(ay, by, x, y)
            if rho > limit:
                bad.append((round(x, 2), round(y, 2), round(z, 2), round(rho, 2)))
    return {'ok': not bad, 'limit': limit, 'sample_bad': bad[:5], 'bad_count': len(bad)}


def rho_at_z_approx(p, z):
    """Exact port of firefly_case.py's rho_at_z(p, z) (same
    _profile_geometry derivation), so this offline script's envelope
    check can never disagree with the in-Fusion one."""
    flat_rho = p['flat_rho']
    fillet_r = p['fillet_r']
    fc_rho = p['fillet_center_rho']
    outer_r = p['outer_radius']
    top_c_z = p['top_fillet_center_z']
    bot_c_z = p['bottom_fillet_center_z']
    top_z = p['top_z']
    bot_z = p['bottom_z']

    assert abs(fc_rho - (outer_r - fillet_r)) < 1e-6, (fc_rho, outer_r, fillet_r)
    dz_tangent = fillet_r * math.sin(math.radians(45))
    top_tangent_z = top_c_z + dz_tangent
    bot_tangent_z = bot_c_z - dz_tangent

    if z >= top_tangent_z:
        return flat_rho + (top_z - z)
    if z >= top_c_z:
        return fc_rho + math.sqrt(max(fillet_r * fillet_r - (z - top_c_z) ** 2, 0.0))
    if z >= bot_c_z:
        return outer_r
    if z >= bot_tangent_z:
        return fc_rho + math.sqrt(max(fillet_r * fillet_r - (z - bot_c_z) ** 2, 0.0))
    return flat_rho + (z - bot_z)


def scan_stl_overhangs(tris, down_z, bed_z, angle_tol_deg=1.0, min_cluster_mm2=30.0, bed_eps=0.6, whitelist_xy=None):
    cos_limit = math.cos(math.radians(45.0 - angle_tol_deg))
    flagged = []
    for normal, v1, v2, v3 in tris:
        nlen = math.sqrt(sum(c * c for c in normal))
        if nlen < 1e-9:
            ux, uy, uz = v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]
            vx, vy, vz = v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2]
            normal = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            nlen = math.sqrt(sum(c * c for c in normal)) or 1.0
        nz = normal[2] / nlen
        down_component = nz * down_z
        avg_z = (v1[2] + v2[2] + v3[2]) / 3.0
        if down_component > cos_limit and abs(avg_z - bed_z) > bed_eps:
            area = _tri_area(v1, v2, v3)
            # Phase 2c bug fix (live-found): a genuinely zero-area
            # (degenerate) triangle -- its STORED STL normal can still
            # read as a valid steep-down-facing vector even though its
            # own 3 vertices are coincident/collinear (the tessellator's
            # own artifact at a tight new cut boundary, e.g. the phase 2c
            # comms-stack-frame wall's best-effort fillet/bridge splice)
            # -- used to be appended here with area 0.0. A cluster made
            # up ONLY of such triangles then summed to a total area of
            # exactly 0.0, and the centroid division below (`s[0] /
            # s[2]`) raised ZeroDivisionError. A zero-area triangle is not
            # real overhang surface either way (nothing for a slicer to
            # support), so it is dropped here rather than counted.
            if area > 1e-9:
                flagged.append((area, (v1, v2, v3)))

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


TOP_WL = [
    (-8.0, 8.0, 65.0, 81.0, 'usb_tunnel_floor'),
    # 2026-09-14 pass 14: y0 widened -12 -> -16 (current variant only
    # exposes this) -- the new lanyard-end corner blocks' own outward
    # wedge (add_lanyard_corner_block) reaches further out toward the
    # true wall than the old individual bosses did (current's wider
    # lip_r[0] gives it more room before CORNER_BLOCK_RING_CLEARANCE
    # clamps it), so it clips against a slightly larger patch of the same
    # curved inner-cavity ceiling/fillet transition this whitelist entry
    # already covers everywhere else. Live-inspected the actual flagged
    # triangles (not just their size) before widening: two z-bands, a
    # flat z=11 shelf (down_component angle 0 degrees -- ordinary flat
    # interior ceiling, same as every other flat ceiling patch here) and
    # a z~21-23 band at ~40 degrees (the inner-cavity fillet's own
    # sub-45-degree transition, identical in kind to the existing
    # ceiling_near_bay_wall entries below) -- both the same "hollow shell
    # needs ordinary slicer support here" reality this whitelist already
    # accepts, not a new local/structural defect. Southmost point found:
    # y=-15.87; margin to -16.
    (-32.0, 32.0, -16.0, 79.0, 'general_ceiling_overhang'),
]
BOTTOM_WL = [
    (-15.0, 15.0, -26.0, -15.0, 'l76k_frame_ceiling'),
]
# 2026-09-10 pass 10 REDO: the old 'mag_module_pocket' entries above (both
# TOP_WL and BOTTOM_WL) were sized for the rejected vertical-wall mount's
# horizontal pegs -- the new ceiling-hung mount (pegs/pads/fence, all
# hanging from Top's own ceiling, nowhere near Bottom) falls entirely
# within 'general_ceiling_overhang' on Top and touches Bottom not at all.
# If a live scan of the new geometry reports a real local cluster here,
# add a fresh entry sized from the actual reported centroid.


def main():
    import importlib.util
    import os
    variant_dir = sys.argv[1]
    variant = sys.argv[2]  # 'trim' or 'current'
    params_path = sys.argv[3]
    sys.path.insert(0, os.path.dirname(params_path))  # params_trim.py does `from params_current import PARAMS`

    spec = importlib.util.spec_from_file_location('params_mod', params_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    params = mod.PARAMS

    bodies = {
        'Bottom': (variant_dir + '/Bottom.stl', -1.0, params['bottom_z'], BOTTOM_WL),
        'Top': (variant_dir + '/Top.stl', 1.0, params['top_z'], TOP_WL),
    }
    # pass 16: no Screen Plate any more (candidate-5 display mount) -- the
    # documented, exported part set is exactly Bottom/Top/Power_Button/
    # Home_Button (EXPORT_BODY_NAMES in firefly_case.py). 'Screen_Plate'
    # removed from this list (it was silently skipped via the
    # FileNotFoundError continue below anyway, but a stale name here is
    # misleading -- see the pass-16 README section).
    extra = ['Power_Button', 'Home_Button']

    report = {}
    for name, (path, down_z, bed_z, wl) in bodies.items():
        tris = read_stl_triangles(path)
        manifold = check_manifold(tris)
        body_count = check_body_count(tris)
        envelope = check_envelope(tris, params, name)
        overhang = scan_stl_overhangs(tris, down_z, bed_z, whitelist_xy=wl)
        report[name] = {'manifold': manifold, 'body_count': body_count, 'envelope': envelope, 'overhang': overhang}

    for name in extra:
        path = f'{variant_dir}/{name}.stl'
        try:
            tris = read_stl_triangles(path)
        except FileNotFoundError:
            continue
        manifold = check_manifold(tris)
        body_count = check_body_count(tris)
        envelope = check_envelope(tris, params, name)
        report[name] = {'manifold': manifold, 'body_count': body_count, 'envelope': envelope}

    print(f'=== Offline STL checks: {variant} ===')
    print(json.dumps(report, indent=2, default=str))

    fail = False
    for name, r in report.items():
        if not r['manifold']['manifold']:
            print(f'FAIL manifold: {name}')
            fail = True
        # pass 16, item G: each exported part must be exactly one body --
        # a stray disjoint island would still pass the manifold check
        # above (each piece is its own watertight mesh) but is a real,
        # separately-floating part, not a single printable solid. See
        # check_body_count's own docstring.
        if not r['body_count']['one_body']:
            print(f"FAIL body_count: {name} ({r['body_count']['body_count']} bodies, expected 1)")
            fail = True
        if not r['envelope']['ok']:
            print(f'FAIL envelope: {name}')
            fail = True
        if 'overhang' in r and r['overhang']['bad_clusters_mm2']:
            print(f'FAIL overhang: {name}')
            fail = True
    print('OVERALL:', 'FAIL' if fail else 'PASS')
    sys.exit(1 if fail else 0)


if __name__ == '__main__':
    main()
