"""Offline gates -- reuses hardware/case/tools/offline_stl_check.py AS-IS
(pure Python, no Fusion dependency, so it needs no porting) for the
manifold/body/overhang scan on an exported STL, plus two NEW headless
live-geometry gates that replace what verify_post_walls and a Fusion
interference check would do inside Fusion:

  * `pilot_wall_probe` -- the same ray/point-containment technique
    verify_post_walls (firefly_case.py:6437) uses, run directly against
    the in-memory OCC solid (via build123d's `Solid.is_inside`, which
    wraps OCP's BRepClass3d_SolidClassifier) instead of against a body
    living inside a running Fusion document.

  * `interference_volume` -- boolean-intersect two solids and read the
    resulting volume, the headless equivalent of Fusion's
    checkInterferenceInput/InterferenceResults API used throughout
    firefly_case.py's verify_min_clearances / check_interference calls.
"""
import importlib.util
import math
import os

import build123d as bd

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
_OFFLINE_CHECK_PATH = os.path.join(_CASE_DIR, 'tools', 'offline_stl_check.py')

_spec = importlib.util.spec_from_file_location('offline_stl_check', _OFFLINE_CHECK_PATH)
offline_stl_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(offline_stl_check)


def manifold_and_overhang_check(stl_path, down_z, bed_z, whitelist_xy=None):
    """Direct reuse of offline_stl_check.py's read_stl_triangles/
    check_manifold/scan_stl_overhangs -- zero lines changed."""
    tris = offline_stl_check.read_stl_triangles(stl_path)
    manifold = offline_stl_check.check_manifold(tris)
    overhang = offline_stl_check.scan_stl_overhangs(tris, down_z, bed_z, whitelist_xy=whitelist_xy or [])
    return {'triangle_count': len(tris), 'manifold': manifold, 'overhang': overhang}


def pilot_wall_probe(solid, cx, cy, pilot_dia, wall_min, z_samples, angles_deg=None):
    """Port of firefly_case.py:6437 verify_post_walls' check (a) --
    probe at radius (pilot_r + wall_min) from the pilot's own axis, at
    each of 8 angles x 3 z-heights, must read SOLID everywhere (a hollow
    hit means real wall there is under wall_min). Uses build123d's
    Solid.is_inside (OCP's BRepClass3d_SolidClassifier) directly on the
    in-memory OCC solid -- no export/mesh round-trip needed, unlike the
    STL-based gates above."""
    if angles_deg is None:
        angles_deg = [i * 45.0 for i in range(8)]
    pilot_r = pilot_dia / 2.0
    probe_r = pilot_r + wall_min
    solid_shape = solid.solid() if hasattr(solid, 'solid') else solid
    bad = []
    for ang in angles_deg:
        rad = math.radians(ang)
        dx, dy = math.cos(rad), math.sin(rad)
        for z in z_samples:
            pt = (cx + probe_r * dx, cy + probe_r * dy, z)
            if not solid_shape.is_inside(pt):
                bad.append((ang, round(z, 2)))
    return bad


def interference_volume(solid_a, solid_b):
    """Headless equivalent of Fusion's live checkInterferenceInput call
    (used throughout firefly_case.py's verify_min_clearances /
    check_interference): boolean-intersect two solids and read the
    resulting volume directly -- non-zero means real 3D overlap."""
    overlap = solid_a & solid_b
    vol = overlap.volume if overlap is not None else 0.0
    return vol


def mesh_point_containment(trimesh_mesh, points):
    """Point-in-mesh probe against an imported STL (trimesh), standing in
    for 'import the display module's own STEP/mesh and probe/boolean
    against it' -- see the spike doc's item 4 (no Fusion means no STEP
    export of the display module was possible this session, so the
    existing exported trim Top.stl is used as a stand-in 'big mesh' for
    timing this operation)."""
    return trimesh_mesh.contains(points)
