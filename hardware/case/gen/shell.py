"""Top/Bottom shell, window bore, and lip/anchor ring -- build123d port of
firefly_case.py's hollow_and_split (:851), add_window (:1122), and
add_lip_anchor_reliefs (:893, the pass-16 continuous-taper version).
"""
import math

import build123d as bd

from . import geometry as geo

MIN_RELIEF_CLEARANCE = 1.0   # firefly_case.py:413
LIP_RING_FLAT_TARGET_MM = 0.5  # firefly_case.py:414


def build_shells(p):
    """Port of hollow_and_split: outer - inner, split at z=split_z. Unlike
    Fusion's SplitBodyFeature (which needs a live timeline operation),
    OCC gets both halves directly by intersecting the hollow shell with a
    clip box on each side of the parting plane."""
    outer = geo.build_outer_pill_solid(p)
    inner = geo.build_inner_pill_solid(p)
    hollow = outer - inner

    ay, by = p['spine_a'][1], p['spine_b'][1]
    r = p['outer_radius'] + 5.0
    split_z = p['split_z']

    bottom_clip = geo.box_solid(-r, r, ay - r, by + r, p['bottom_z'] - 1.0, split_z)
    top_clip = geo.box_solid(-r, r, ay - r, by + r, split_z, p['top_z'] + 1.0)

    bottom = hollow & bottom_clip
    top = hollow & top_clip
    return {'Bottom': bottom, 'Top': top}


def add_window(bodies, p):
    """Port of add_window: bore + pass-15 print-orientation cone chamfer +
    pass-16 glass-seat chamfer, each followed by the same build-time
    regression probe firefly_case.py itself runs (a hollow ring sampled
    at every 10 degrees, just inside the cut cone's own slope)."""
    top = bodies['Top']
    cx, cy = p['window_center']
    r = p['window_dia'] / 2.0

    bore = geo.cylinder_solid(cx, cy, r, p['window_z_bottom'], p['top_z'] + 1.0)
    top = top - bore

    chamf = p['window_chamfer']
    cone_r_lo, cone_z_lo = r - 0.05, p['top_z'] - chamf - 0.1
    cone_r_hi, cone_z_hi = r + chamf + 0.2, p['top_z'] + 0.5
    chamfer_tool = geo.cone_frustum_solid(cx, cy, cone_r_lo, cone_r_hi, cone_z_lo, cone_z_hi)
    top = top - chamfer_tool
    _assert_cone_cut_clean(top, cx, cy, cone_r_lo, cone_r_hi, cone_z_lo, cone_z_hi,
                            'add_window: print-orientation chamfer')

    glass_chamf = p.get('glass_seat_chamfer', 0.0)
    if glass_chamf > 0:
        gz0, gz1 = p['window_z_bottom'] - 0.1, p['window_z_bottom'] + glass_chamf
        glass_tool = geo.cone_frustum_solid(cx, cy, r - 0.05, r + glass_chamf, gz0, gz1)
        top = top - glass_tool
        _assert_cone_cut_clean(top, cx, cy, r - 0.05, r + glass_chamf, gz0, gz1,
                                'add_window: glass-seat chamfer')

    bodies['Top'] = top
    return bodies


def _assert_cone_cut_clean(solid, cx, cy, r_lo, r_hi, z_lo, z_hi, label):
    probe_z = (z_lo + z_hi) / 2.0
    frac = (probe_z - z_lo) / (z_hi - z_lo)
    r_at_z = r_lo + frac * (r_hi - r_lo)
    probe_r = r_at_z - 0.15
    bad = []
    for deg in range(0, 360, 10):
        rad = math.radians(deg)
        pt = (cx + probe_r * math.cos(rad), cy + probe_r * math.sin(rad), probe_z)
        if geo.probe_point_solid(solid, pt):
            bad.append(deg)
    assert not bad, f'{label}: cut did not remove material at angles {bad} (deg)'


def add_lip_anchor_reliefs(bodies, p):
    """Port of add_lip_anchor_reliefs (pass-16, item C): a continuous
    self-supporting taper from lip_r[1] (at lip_z[0]) out to anchor_r[1]
    (at anchor_z[1]), built as a constant-radius land plus 4 taper wedges
    (2 domed-end revolves + 2 straight-side extrudes), then narrowed by a
    second, steeper taper cut so the remaining flat cap at anchor_z[1] is
    only LIP_RING_FLAT_TARGET_MM wide -- plus the per-boss relief cuts and
    the lug's own single-lanyard-holder relief."""
    ay, by = p['spine_a'][1], p['spine_b'][1]

    land_z0, land_z1 = p['lip_z'][0], p['anchor_z'][1]
    taper_z0, taper_z1 = p['anchor_z'][0], p['anchor_z'][1]
    land = geo.stadium_ring_solid(ay, by, p['lip_r'][0], p['lip_r'][1], land_z0, land_z1)
    taper_r_lo, taper_r_hi = p['lip_r'][1], p['anchor_r'][1]
    end_a = geo.revolve_taper_wedge(0.0, ay, taper_r_lo, taper_r_hi, taper_z0, taper_z1, -180.0)
    end_b = geo.revolve_taper_wedge(0.0, by, taper_r_lo, taper_r_hi, taper_z0, taper_z1, 180.0)
    side_pos = geo.extrude_taper_wedge_along_y(+1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    side_neg = geo.extrude_taper_wedge_along_y(-1, taper_r_lo, taper_r_hi, taper_z0, taper_z1, ay, by)
    ring = land + end_a + end_b + side_pos + side_neg

    inner_r_lo, inner_r_hi = p['lip_r'][0], p['anchor_r'][1] - LIP_RING_FLAT_TARGET_MM
    cut_a = geo.cone_frustum_solid(0.0, ay, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    cut_b = geo.cone_frustum_solid(0.0, by, inner_r_lo, inner_r_hi, taper_z0, taper_z1)
    cut_pos = geo.extrude_taper_cut_along_y(+1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    cut_neg = geo.extrude_taper_cut_along_y(-1, inner_r_lo, inner_r_hi, taper_z0, taper_z1, ay, by)
    ring = ring - cut_a - cut_b - cut_pos - cut_neg

    top = bodies['Top'] + ring

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        stack_keepout = geo.box_solid(
            pcb['x'][0] - 1.0, pcb['x'][1] + 1.0, pcb['y'][0] - 1.0, pcb['y'][1] + 1.0,
            p['lip_z'][0] - 0.5, p['anchor_z'][1] + 0.5)
        top = top - stack_keepout

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
        s_wall = geo.true_wall_distance_along_ray(p, (cx, cy), d2, relief_z_mid)
        relief_r = nominal_r if s_wall is None else min(nominal_r, s_wall - wall_clear)
        boss_r = p['boss_dia'] / 2.0
        assert relief_r - boss_r >= MIN_RELIEF_CLEARANCE, (
            f'add_lip_anchor_reliefs: boss {s["name"]} at ({cx},{cy}) sits too close to '
            f'the true outer wall for a clean relief -- relief_r={relief_r:.3f} leaves only '
            f'{relief_r - boss_r:.3f}mm over the boss OD (need >= {MIN_RELIEF_CLEARANCE}mm)')
        relief = geo.cylinder_solid(cx, cy, relief_r, relief_z0, relief_z1)
        top = top - relief

    lb = p['lug_relief_box']
    lug_wall = geo.true_wall_distance_along_ray(p, (0.0, ay), (0.0, -1.0), relief_z_mid)
    lb_y0 = lb['y'][0]
    if lug_wall is not None:
        lb_y0 = max(lb_y0, ay - (lug_wall - wall_clear))
    relief_box = geo.box_solid(lb['x'][0], lb['x'][1], lb_y0, lb['y'][1], relief_z0, relief_z1)
    top = top - relief_box

    bodies['Top'] = top
    return bodies
