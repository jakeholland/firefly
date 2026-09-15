"""Lanyard lug/ear -- build123d port of firefly_case.py:1747
lug_ear_geometry / :3792 add_lug.

The two best-effort cosmetic fillets (outward corners, root) are ported
using build123d's own `bd.fillet`, wrapped in the same skip-on-failure
try/except pattern -- unlike Fusion, OCC's fillet solver is not expected
to need this, but the geometry-based edge selection (matching by bbox,
not by a stable edge/face reference) is kept identical to the source so
a refusal here can never roll back load-bearing geometry, matching the
original's own stated intent.
"""
import build123d as bd

from .. import geometry as geo


def lug_ear_geometry(p):
    lug = p['lug']
    half_w = lug['width'] / 2.0
    ay = p['spine_a'][1]
    z0, z1 = lug['z']
    s_wall = min(geo.rho_at_z(p, z0), geo.rho_at_z(p, z1))
    y_outer = ay - s_wall
    y_far = y_outer - lug['protrusion']
    y_root = ay - (s_wall - 3.0)
    hole_y = y_far + lug['hole_from_tip']
    return half_w, y_far, y_root, hole_y


def add_lug(bodies, p):
    lug = p['lug']
    z0, z1 = lug['z']
    half_w, y_far, y_root, hole_y = lug_ear_geometry(p)

    ear = geo.box_solid(-half_w, half_w, y_far, y_root, z0, z1)

    void = geo.build_inner_pill_solid(p)
    ear = ear - void

    thickened = geo.build_thickened_envelope(p, lug['protrusion'])
    ear = ear & thickened

    fillet_r = lug.get('fillet_r')
    if fillet_r:
        ear = _best_effort_outer_corner_fillet(ear, half_w, y_far, z0, z1, fillet_r)

    bottom = bodies['Bottom'] + ear

    root_fillet_r = lug.get('root_fillet_r')
    if root_fillet_r:
        bottom = _best_effort_root_fillet(bottom, half_w, y_far, y_root, z0, z1, root_fillet_r)

    hole_r = lug['hole_dia'] / 2.0
    hole = geo.cylinder_solid(0.0, hole_y, hole_r, z0 - 0.5, z1 + 0.5)
    bottom = bottom - hole

    chamf = lug.get('hole_chamfer')
    if chamf:
        bottom = _best_effort_hole_chamfer(bottom, hole_y, hole_r, z0, z1, chamf)

    bodies['Bottom'] = bottom
    return bodies


def _best_effort_outer_corner_fillet(ear, half_w, y_far, z0, z1, radius):
    """Port of add_lug's R3 vertical-corner fillet, selected the same way
    (a vertical edge, full z-height, sitting at y_far and either
    x=+-half_w)."""
    try:
        edges = []
        for e in ear.edges():
            bb = e.bounding_box()
            dx = bb.max.X - bb.min.X
            dy = bb.max.Y - bb.min.Y
            dz = bb.max.Z - bb.min.Z
            if dx < 0.05 and dy < 0.05 and dz > (z1 - z0) - 0.1:
                ex, ey = bb.min.X, bb.min.Y
                if abs(ey - y_far) < 0.05 and (abs(ex - half_w) < 0.05 or abs(ex + half_w) < 0.05):
                    edges.append(e)
        if edges:
            return bd.fillet(edges, radius=radius)
    except Exception:
        pass
    return ear


def _best_effort_root_fillet(bottom, half_w, y_far, y_root, z0, z1, radius):
    """Port of add_lug's pass-15 root fillet (top/bottom edges along the
    ear's own length, at the shell attachment)."""
    try:
        edges = []
        for e in bottom.edges():
            bb = e.bounding_box()
            ex0, ex1 = bb.min.X, bb.max.X
            ey0, ey1 = bb.min.Y, bb.max.Y
            ez0, ez1 = bb.min.Z, bb.max.Z
            flat_z = abs(ez1 - ez0) < 0.05 and (abs(ez0 - z0) < 0.05 or abs(ez0 - z1) < 0.05)
            within_ear = (-half_w - 0.1 <= ex0 and ex1 <= half_w + 0.1
                          and y_far - 0.1 <= ey0 and ey1 <= y_root + 0.1
                          and (ey1 - ey0) > 1.0)
            if flat_z and within_ear:
                edges.append(e)
        if edges:
            return bd.fillet(edges, radius=radius)
    except Exception:
        pass
    return bottom


def _best_effort_hole_chamfer(bottom, hole_y, hole_r, z0, z1, chamf):
    try:
        edges = []
        for e in bottom.edges():
            bb = e.bounding_box()
            if bb.max.Z - bb.min.Z < 1e-3 and abs(bb.min.Z - z0) < 0.05 or abs(bb.min.Z - z1) < 0.05:
                cx = (bb.min.X + bb.max.X) / 2.0
                cy = (bb.min.Y + bb.max.Y) / 2.0
                r = max(bb.max.X - bb.min.X, bb.max.Y - bb.min.Y) / 2.0
                if abs(cx) < 0.1 and abs(cy - hole_y) < 0.1 and abs(r - hole_r) < 0.1:
                    edges.append(e)
        if edges:
            return bd.chamfer(edges, length=chamf)
    except Exception:
        pass
    return bottom
