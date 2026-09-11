"""Verification gates -- headless build123d port of firefly_case.py's own
verify_* functions, restricted to what phase 1 actually builds (A/C/D
case screws, the lip/anchor ring, USB tunnel, FPC relief, the lanyard
lug). Ears/S2-boss/buttons/wordmark/comms-stack gates are deferred to
whichever phase ports those features (see
docs/hardware/headless-port-plan.md).

`tools/offline_stl_check.py` is reused AS-IS (pure Python, no Fusion
dependency -- it already needs no porting) for the manifold/body-count/
overhang scan on an exported STL. Every other gate below replaces a
Fusion-live probe (`design.analyzeInterference`, a body's own
`pointContainment`) with the OCC equivalent: `Solid.is_inside`
(`geometry.probe_point_solid`, wrapping OCP's BRepClass3d_
SolidClassifier) for point probes, and a boolean intersect + `.volume`
read for interference.
"""
import importlib.util
import math
import os

import build123d as bd

from . import components as comp
from . import geometry as geo
from .features import buttons as btn
from .features import comms_bay
from .features import compass as compass_mod
from .features import corner_blocks as cb

_CASE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
_OFFLINE_CHECK_PATH = os.path.join(_CASE_DIR, 'tools', 'offline_stl_check.py')

_spec = importlib.util.spec_from_file_location('offline_stl_check', _OFFLINE_CHECK_PATH)
offline_stl_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(offline_stl_check)


# ---------------------------------------------------------------------------
# Offline (exported-STL) gates -- direct reuse of the existing tool.
# ---------------------------------------------------------------------------
def manifold_and_overhang_check(stl_path, down_z, bed_z, whitelist_xy=None):
    tris = offline_stl_check.read_stl_triangles(stl_path)
    manifold = offline_stl_check.check_manifold(tris)
    body_count = offline_stl_check.check_body_count(tris) if hasattr(offline_stl_check, 'check_body_count') else None
    overhang = offline_stl_check.scan_stl_overhangs(tris, down_z, bed_z, whitelist_xy=whitelist_xy or [])
    return {'triangle_count': len(tris), 'manifold': manifold, 'body_count': body_count, 'overhang': overhang}


def verify_lip_ring_profile(stl_path, p, down_z=1.0):
    """Port of firefly_case.py:9224 verify_lip_ring_profile -- scans the
    exported Top STL's own triangles in the ring's band for a real
    10-80 degree taper facet and no >0.6mm-wide flat (>80 degree) patch."""
    ay, by = p['spine_a'][1], p['spine_b'][1]
    r_lo, r_hi = p['lip_r'][0], p['anchor_r'][1]
    z_lo, z_hi = p['lip_z'][0], p['anchor_z'][1]
    tris = offline_stl_check.read_stl_triangles(stl_path)

    def rho(x, y):
        if ay <= y <= by:
            return abs(x)
        cy = ay if y < ay else by
        return math.hypot(x, y - cy)

    band_tol = 0.15
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
        down_component = nz * down_z
        if down_component <= 0:
            continue
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


# ---------------------------------------------------------------------------
# Live in-memory (OCC solid) gates.
# ---------------------------------------------------------------------------
POST_WALL_MIN = 1.2


def verify_post_walls(bodies, p):
    """Port of firefly_case.py:7431 verify_post_walls, targeting D
    (screws_D12) same as the pass-16 source."""
    top = bodies['Top']
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
                pt = (cx + (pilot_r + wall_min) * dxu, cy + (pilot_r + wall_min) * dyu, z)
                if not geo.probe_point_solid(top, pt):
                    wall_bad.append((ang, round(z, 2)))
        results[f'{name}_pilot_wall'] = wall_bad

        skin_bad = []
        for ang in angles:
            rad = math.radians(ang)
            d2 = (math.cos(rad), math.sin(rad))
            s_wall = geo.true_wall_distance_along_ray(p, (cx, cy), d2, z1)
            if s_wall is None:
                continue
            clear = s_wall - boss_r
            if clear < skin_min:
                skin_bad.append((ang, round(clear, 3)))
        results[f'{name}_shell_skin'] = skin_bad
    return results


def verify_root_fillets(bodies, p):
    """Port of firefly_case.py:7497 verify_root_fillets, restricted to
    the case-screw bosses actually built in phase 1 (A/C/D) -- ear/S2/mag
    features are deferred, so their own entries are simply absent rather
    than reported red."""
    top, bottom = bodies['Top'], bodies['Bottom']
    angles = [i * 45.0 for i in range(8)]
    boss_r = p['boss_dia'] / 2.0
    features = []
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        z1_top = cb._ear_root_z1(p, cx, cy, boss_r + cb.CORNER_BLOCK_REACH, p['top_ceiling_underside_z'])
        features.append((f'boss_{s["name"]}_bottom', bottom, cx, cy, boss_r, 2.0, 'down'))
        features.append((f'corner_block_{s["name"]}_top', top, cx, cy, boss_r, z1_top, 'up'))

    results = {}
    for name, body, cx, cy, r, z_root, direction in features:
        z_probe = z_root - 0.4 if direction == 'up' else z_root + 0.4
        probe_r = r + 0.6
        bad = []
        for ang in angles:
            rad = math.radians(ang)
            pt = (cx + probe_r * math.cos(rad), cy + probe_r * math.sin(rad), z_probe)
            if not geo.probe_point_solid(body, pt):
                bad.append((ang, round(z_probe, 2)))
        results[name] = bad
    return results


def verify_corner_blocks(bodies, p):
    """Port of firefly_case.py:7585 verify_corner_blocks for A/C/D."""
    top = bodies['Top']
    z0 = p['split_z']
    pz0, pz1 = p['top_pilot_z']
    boss_r = p['boss_dia'] / 2.0
    results = {}

    for s in p['screws_ABC'] + p['screws_D12']:
        name = s['name']
        cx, cy = s['xy']
        z1 = cb._ear_root_z1(p, cx, cy, boss_r + cb.CORNER_BLOCK_REACH, p['top_ceiling_underside_z'])

        checks = [not geo.probe_point_solid(top, (cx, cy, z))
                  for z in (pz0 + 0.1, (pz0 + pz1) / 2.0, pz1 - 0.1)]
        results[f'{name}_pilot_open'] = (all(checks), checks)

        z_block = min(pz1 + 1.5, z1 - 0.5)
        off_r = boss_r - 0.3
        solid_checks = [geo.probe_point_solid(top, (cx + off_r * math.cos(math.radians(ang)),
                                                      cy + off_r * math.sin(math.radians(ang)), z_block))
                         for ang in (0, 90, 180, 270)]
        results[f'{name}_block_solid'] = (all(solid_checks), solid_checks)

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        z_mid = (z0 + p['top_ceiling_underside_z']) / 2.0
        corners = [(pcb['x'][0], pcb['y'][0]), (pcb['x'][0], pcb['y'][1]),
                   (pcb['x'][1], pcb['y'][0]), (pcb['x'][1], pcb['y'][1])]
        stack_clear = [not geo.probe_point_solid(top, (cx, cy, z_mid)) for cx, cy in corners]
        results['stack_footprint_clear'] = (all(stack_clear), stack_clear)

    ko = p['bay']['fpc_keepout']
    kcz = (ko['z'][0] + ko['z'][1]) / 2.0
    fpc_checks = [not geo.probe_point_solid(top, (0.0, ko['y'][0] + frac * (ko['y'][1] - ko['y'][0]), kcz))
                  for frac in (0.25, 0.5, 0.75)]
    results['fpc_keepout_centerline_clear'] = (all(fpc_checks), fpc_checks)
    return results


def verify_bottom_openings(bodies, p):
    """Port of firefly_case.py:7649 verify_bottom_openings for A/C/D +
    the lanyard lug's own cord hole."""
    from .features import lug as lug_mod
    bottom = bodies['Bottom']
    results = {}
    all_case_screws = p['screws_ABC'] + p['screws_D12']
    for s in all_case_screws:
        cx, cy = s['xy']
        name = s['name']
        bad = [z for z in (1.0, 2.7, 4.5, 7.0, 9.5)
               if geo.probe_point_solid(bottom, (cx, cy, z))]
        results[f'{name}_pilot_open'] = (not bad, bad)
    cb_r = p['counterbore_ABC_dia'] / 2.0 - 0.3
    cb_h = p['counterbore_ABC_h']
    for s in all_case_screws:
        cx, cy = s['xy']
        name = s['name']
        cb_bad = [z for z in (0.5, 1.9, cb_h - 0.1)
                  if geo.probe_point_solid(bottom, (cx + cb_r, cy, z))]
        results[f'{name}_counterbore_open'] = (not cb_bad, cb_bad)

    lug = p['lug']
    half_w, y_far, y_root, hole_y = lug_mod.lug_ear_geometry(p)
    z0, z1 = lug['z']
    lug_bad = [round(z, 2) for z in (z0 + 0.5, (z0 + z1) / 2.0, z1 - 0.5)
               if geo.probe_point_solid(bottom, (0.0, hole_y, z))]
    results['lug_hole_open'] = (not lug_bad, lug_bad)
    return results


def interference_volume(solid_a, solid_b):
    """Headless equivalent of Fusion's live checkInterferenceInput call."""
    overlap = solid_a & solid_b
    return overlap.volume if overlap is not None else 0.0


def check_interference_pairs(bodies):
    """All-pairs interference check between the printed bodies
    (Top/Bottom/Power_Button/Home_Button once those exist) -- ignores
    razor-thin coincident-face touches under 1e-4 mm^3, matching
    firefly_case.py:6199 check_interference's own _TOUCH_VOLUME_TOL_MM3."""
    names = list(bodies.keys())
    results = {}
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = names[i], names[j]
            vol = interference_volume(bodies[a], bodies[b])
            if vol > 1e-4:
                results[f'{a}_vs_{b}'] = vol
    return results


def all_gates(bodies, p, stl_paths, whitelist_xy=None, standoffs=None, comms_stack=None, lora_corridor=None):
    """Run every phase-1 gate (plus, when `standoffs` is given --
    components.measure_standoffs' own return dict -- every phase-2
    ears/S2-boss gate, and when `comms_stack` is given --
    components.load_comms_stack's own return dict -- every phase-2c
    comms-stack/GPS/battery/compass gate) and return one combined report
    dict."""
    report = {}
    report['interference'] = check_interference_pairs({'Top': bodies['Top'], 'Bottom': bodies['Bottom']})
    report['post_walls'] = verify_post_walls(bodies, p)
    report['root_fillets'] = verify_root_fillets(bodies, p)
    report['corner_blocks'] = verify_corner_blocks(bodies, p)
    report['bottom_openings'] = verify_bottom_openings(bodies, p)
    for name, stl_path in stl_paths.items():
        if name not in ('Top', 'Bottom'):
            continue
        down_z = 1.0
        bed_z = p['top_z'] if name == 'Top' else p['bottom_z']
        report[f'offline_{name}'] = manifold_and_overhang_check(stl_path, down_z, bed_z, whitelist_xy)
    if 'Top' in stl_paths:
        report['lip_ring_profile'] = verify_lip_ring_profile(stl_paths['Top'], p)
    if standoffs is not None:
        report['seat_heights'] = verify_seat_heights(p, standoffs)
        report['ear_root_material'] = verify_ear_root_material(bodies, p, standoffs)
        report['s2_boss_clearance'] = verify_s2_boss_clearance(bodies, p, standoffs)
        report['display_to_stack_clearance'] = verify_display_to_stack_clearance(p)
        report['display_interference_near_ears'] = check_display_interference_near_ears(bodies, p, standoffs)
    if 'Power Button' in bodies and 'Home Button' in bodies:
        # widen the all-pairs interference check to include both cap
        # parts, not just Top/Bottom.
        report['interference'] = check_interference_pairs(bodies)
        report['button_insertion'] = verify_button_insertion(bodies, p)
        report['button_retention'] = verify_button_retention(bodies, p)
        report['plunger_reach'] = verify_plunger_reach(bodies, p)
        report['skin_intact'] = verify_skin_intact(bodies, p)
    if comms_stack is not None:
        report['stack_frame_boss_clear'] = verify_stack_frame_boss_clear(bodies, p)
        report['stack3_clearance'] = verify_stack3_clearance(bodies, p, comms_stack)
        report['antenna_channels'] = verify_antenna_channels(bodies, p, lora_corridor)
        report['mag_pocket'] = verify_mag_pocket(bodies, p)
        report['min_clearances'] = verify_min_clearances(bodies, p, comms_stack)
        report['board_interference'] = check_board_interference(bodies, p, comms_stack)
    return report


# ---------------------------------------------------------------------------
# Phase-2 gates -- ears (S1/S3) + S2 boss (features/ears.py).
# ---------------------------------------------------------------------------
def verify_seat_heights(p, standoffs):
    """Port of firefly_case.py:8229 verify_seat_heights -- headlessly, this
    reduces to a construction identity (features/ears.py builds each
    seat's top face at exactly `standoffs[name]['standoff_plane_z'] -
    ear_seat_offset`), so this gate instead independently re-derives each
    barrel's OWN measured local top-z (not the shared
    STANDOFF_BARREL_TOP_LOCAL_Z constant `measure_standoffs` uses) from
    `components.find_standoff_barrels` and re-checks the seat gap against
    it -- catching a genuine per-barrel STEP anomaly the shared constant
    could otherwise hide. `gap` = measured_plane_z - built_seat_z: the
    built seat sits `ear_seat_offset` (0.25mm) BELOW the real measured
    plane (mech review F5, so the window seat -- not S1/S2/S3 -- takes
    the assembly preload), so `gap` is expected at +0.25 +/- 0.05mm."""
    top_z = p['top_z']
    offset = p['ear_seat_offset']
    barrels = comp.find_standoff_barrels()
    results = {}
    for name, m in standoffs.items():
        lx, ly = m['local_xy']
        _, _, _, lz_top = min(barrels, key=lambda b: math.hypot(b[0] - lx, b[1] - ly))
        measured_plane_z = top_z + lz_top
        seat_z = m['standoff_plane_z'] - offset
        gap = measured_plane_z - seat_z
        results[name] = {
            'measured_plane_z': round(measured_plane_z, 3),
            'built_seat_z': round(seat_z, 3),
            'gap_mm': round(gap, 3),
            'ok': 0.20 <= gap <= 0.30,
        }
    return results


def verify_ear_root_material(bodies_dict, p, standoffs):
    """Port of firefly_case.py:8107 verify_ear_root_material for S1/S3:
    (a) the standoff's own M2x4 through-hole reads open along its depth;
    (b) the wall-root->standoff arm reads solid at its own low z-band
    (off-axis at the standoff end, since t=1.0 on-axis lands dead-centre
    on the through-hole by design); (c) the standoff riser reads solid at
    its own mid-height; (d) the analytic clearance from each ear's own
    footprint to the FPC relief pocket / display_header box (reported,
    not gated, matching the source)."""
    from .features import ears as ears_mod
    top = bodies_dict['Top']
    results = {}
    arm_thick = p['ear_arm_thickness']
    boss_r = p['boss_dia'] / 2.0
    off_r = cb.BOSS_CORE_R - 0.3
    fpc = p['fpc_relief']
    hdr = p['display_header']
    ceiling = p['top_ceiling_underside_z']
    for name, ear in p['ears'].items():
        rx, ry = ear['root_xy']
        tx, ty, seat_z = ears_mod._target_xy_seat_z(p, standoffs, ear['target'])
        root_z1 = ears_mod.ear_root_cap_z1(p, rx, ry, boss_r + cb.CORNER_BLOCK_REACH, ceiling)
        z_mid_arm = root_z1 - arm_thick / 2.0
        z_mid_riser = (root_z1 + seat_z) / 2.0

        hole_checks = [not geo.probe_point_solid(top, (tx, ty, z))
                       for z in (root_z1 - arm_thick + 0.3, seat_z - 0.3)]
        results[f'{name}_standoff_hole_open'] = (all(hole_checks), hole_checks)

        solid_checks = [geo.probe_point_solid(top, (rx + t * (tx - rx), ry + t * (ty - ry), z_mid_arm))
                         for t in (0.0, 0.25, 0.5, 0.75)]
        solid_checks += [geo.probe_point_solid(top, (tx + off_r * math.cos(math.radians(ang)),
                                                       ty + off_r * math.sin(math.radians(ang)), z_mid_arm))
                          for ang in (0, 90, 180, 270)]
        results[f'{name}_material_solid'] = (all(solid_checks), solid_checks)

        riser_checks = [geo.probe_point_solid(top, (tx + off_r * math.cos(math.radians(ang)),
                                                      ty + off_r * math.sin(math.radians(ang)), z_mid_riser))
                         for ang in (0, 90, 180, 270)]
        results[f'{name}_riser_solid'] = (all(riser_checks), (round(z_mid_riser, 3), riser_checks))

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


def verify_s2_boss_clearance(bodies_dict, p, standoffs):
    """Port of firefly_case.py:8187 verify_s2_boss_clearance: the boss's
    own hard keep-out around the display's real battery-connector XY+Z
    footprint reads fully hollow in the built Top, and the boss's own
    nearest edge clears the GPS frame's real outer wall by >= 0.5mm."""
    top = bodies_dict['Top']
    (bcx0, bcx1), (bcy0, bcy1), (bcz0, bcz1) = comp.battery_connector_world_bbox(p)
    results = {}
    bad = []
    nx, ny, nz = 4, 3, 2
    for i in range(nx):
        x = bcx0 + (bcx1 - bcx0) * i / (nx - 1)
        for j in range(ny):
            y = bcy0 + (bcy1 - bcy0) * j / (ny - 1)
            for k in range(nz):
                z = bcz0 + (bcz1 - bcz0) * k / (nz - 1)
                if geo.probe_point_solid(top, (x, y, z)):
                    bad.append((round(x, 2), round(y, 2), round(z, 2)))
    results['battery_clear_ok'] = (not bad, bad[:5])

    s2 = p['s2_boss']
    _, ty = standoffs[s2['target']]['world_xy']
    boss_r = p['boss_dia'] / 2.0
    gps = p['bay']['gps_patch']
    gps_half = p['bay']['gps_frame_opening'] / 2.0
    gcy = (gps['y'][0] + gps['y'][1]) / 2.0
    gps_outer_y1 = gcy + gps_half + p['bay']['gps_frame_wall']
    gps_clear = (ty - boss_r) - gps_outer_y1
    results['gps_clear_mm'] = round(gps_clear, 3)
    results['gps_clear_ok'] = gps_clear >= 0.5
    return results


def verify_display_to_stack_clearance(p):
    """Port of firefly_case.py:8308 verify_display_to_stack_clearance --
    purely analytic (bay params only): wherever the display's own real
    bbox overlaps the comms stack / battery / GPS-patch footprint in XY,
    the Z clearance there must be >= 0.5mm. `bay['stack3']` is None until
    phase 2 item 3 (comms stack) lands -- that check is then simply
    absent, not reported red, same convention `verify_corner_blocks`
    already uses for the same not-yet-built feature."""
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
    for name, cx0, cx1, cy0, cy1, top_z_feat in checks:
        overlap = cb._xy_overlap(dx0, dx1, dy0, dy1, cx0, cx1, cy0, cy1)
        if overlap is None:
            results['checks'][name] = {'overlap': False,
                                        'note': 'no XY overlap with the display bbox -- clear by construction'}
            continue
        ox0, ox1, oy0, oy1 = overlap
        clearance = dz0 - top_z_feat
        ok = clearance >= 0.5
        results['checks'][name] = {
            'overlap': True, 'region': (round(ox0, 2), round(ox1, 2), round(oy0, 2), round(oy1, 2)),
            'clearance_mm': round(clearance, 3), 'ok': ok,
        }
        if not ok:
            results['ok'] = False
    return results


def _display_near_ears_region(p, standoffs, margin):
    xs, ys = [], []
    for ear in p['ears'].values():
        xs.append(ear['root_xy'][0])
        ys.append(ear['root_xy'][1])
    for m in standoffs.values():
        xs.append(m['world_xy'][0])
        ys.append(m['world_xy'][1])
    return min(xs) - margin, max(xs) + margin, min(ys) - margin, max(ys) + margin


def _near_ears_candidates(p, standoffs, margin):
    x0, x1, y0, y1 = _display_near_ears_region(p, standoffs, margin)
    disp = comp.load_display(p)
    for s in disp.solids():
        bb = s.bounding_box()
        cx, cy = (bb.min.X + bb.max.X) / 2.0, (bb.min.Y + bb.max.Y) / 2.0
        if x0 <= cx <= x1 and y0 <= cy <= y1:
            yield s


def check_display_interference_near_ears(bodies, p, standoffs, margin=6.0, exact=True, use_cache=True):
    """Not a firefly_case.py function by name -- the source's own
    `check_interference` walks Fusion's LIVE occurrence tree (every
    inserted board, including the display) against Top/Bottom; this port
    has no occurrence tree, only `components.load_display`'s ~420-solid
    compound (see that module's own docstring), and a full boolean
    intersect of Top/Bottom against all 420 of those (most of which sit
    nowhere near any case material by design, floating in open cavity
    air) is not worth its own runtime. Scoped instead to the one place
    phase-2 actually put NEW material near the real board: a padded XY
    box around every ear root/target and the S2 target -- any display
    solid whose own bbox centre falls inside it is boolean-intersected
    against Top for real. Zero hits confirms the standoff through-holes/
    barrel counterbores genuinely clear the real barrels, not just the
    idealized cylinder the barrel-clearance cut assumes.

    Item 3 (this phase's own cycle-time brief) tried the SAME fused/
    cached-tool speedup `components._fused_ceiling_cut_tool` uses here
    too, in two forms, and abandoned both -- kept always-exact (the
    `exact`/`use_cache` params exist only so callers/the CLI's own
    `--exact-display` flag can pass them uniformly without a branch):
    (1) fusing the REAL per-candidate solids (this check's own candidate
    set is denser and each candidate individually more complex than the
    ceiling-cut's simple `band & s` boxes) made the one-time fuse itself
    run for MINUTES, not seconds -- not a usable one-time cost even
    cached. (2) A box-per-candidate ("convex-hull-per-body", using the
    simplest possible hull -- an axis-aligned bbox) prefilter, safe by
    construction (`real solid ⊆ its own bbox`, so a clean box-union
    result is a valid fast 'ok'), fused fast but triggered a real hit on
    the packed near-ears geometry almost every time (live-measured: a
    ~0.0007mm^3 REAL residual is real enough to blow well past any
    sensible box-overlap tolerance too, since nearby SMT parts' own
    bounding boxes routinely overlap the ear/riser material in a way the
    real, smaller solids underneath do not) -- the fast check essentially
    always fell through to the exact loop anyway, at which point it had
    only added the box-fuse's own cost on top, net SLOWER than skipping
    it. Left ported as originally shipped (phase 2a); not the cost this
    phase's own cycle-time win targets (see `ceiling_safe_display_cut`
    for the one that worked)."""
    x0, x1, y0, y1 = _display_near_ears_region(p, standoffs, margin)
    top = bodies['Top']
    # boolean-cleanup/tessellation noise at a shared boundary (same class
    # check_interference_pairs' own 1e-4mm^3 coincident-touch tolerance
    # already accepts elsewhere in this file, just a hair looser here for
    # a genuinely negligible residual -- live-checked, this port's own
    # smallest real fix was ~1.16mm^3, five orders of magnitude above
    # this floor).
    NOISE_FLOOR_MM3 = 0.001

    hits = []
    for s in _near_ears_candidates(p, standoffs, margin):
        bb = s.bounding_box()
        vol = interference_volume(top, s)
        if vol > 1e-4:
            hits.append({
                'bbox_xy': (round(bb.min.X, 2), round(bb.max.X, 2), round(bb.min.Y, 2), round(bb.max.Y, 2)),
                'bbox_z': (round(bb.min.Z, 2), round(bb.max.Z, 2)),
                'volume_mm3': round(vol, 4),
            })
    real_hits = [h for h in hits if h['volume_mm3'] > NOISE_FLOOR_MM3]
    return {'ok': not real_hits, 'region_xy': (round(x0, 1), round(x1, 1), round(y0, 1), round(y1, 1)),
            'hits': hits, 'real_hits': real_hits, 'mode': 'exact'}


# ---------------------------------------------------------------------------
# Phase 2b gates -- buttons (features/buttons.py).
# ---------------------------------------------------------------------------
# Same 0.001mm^3 boolean-cleanup/tessellation-noise floor
# check_display_interference_near_ears already establishes for a
# genuinely negligible residual at a shared coincident face (this port's
# own collar-vs-existing-material fix, features/buttons.py's add_button,
# leaves exactly one such sliver, Home only -- see that fix's own
# comment for why a cleaner bd.offset-based fix was tried and reverted).
BUTTON_INTERFERENCE_NOISE_FLOOR_MM3 = 0.001


def verify_button_insertion(bodies_dict, p):
    """Port of verify_button_insertion (:6624, pass 9b finding 9): sweeps
    the retaining tab's own footprint from a fully-inboard start to its
    rest position, probing 5 points (4 corners + center) at 24+1 steps
    against the BUILT Top -- 0 bad is a clean, unobstructed insertion
    path. Must read 0/125 bad for both buttons (Jake's own live-print
    regression target, pass-16 FIX item 4)."""
    results = {}
    top = bodies_dict.get('Top')
    if top is None:
        return {'ok': None, 'note': 'Top body missing'}
    N_STEPS = 24
    for name, switch_bbox, nub_dir, cap in btn._button_defs(p):
        key = name.lower().replace(' ', '_')
        g = btn.button_geometry(p, switch_bbox, nub_dir, cap)
        d2, t2 = g['d'], g['t']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        tab = p['tab']
        tab_len_along_d = 1.5
        tab_z = z_center - W / 2.0 - tab['h'] / 2.0
        tab_start_xy = (g['tab_face_xy'][0] - tab_len_along_d * d2[0] / 2.0,
                        g['tab_face_xy'][1] - tab_len_along_d * d2[1] / 2.0)
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
                if geo.probe_point_solid(top, (px, py, pz)):
                    bad.append((round(shift, 3), round(px, 2), round(py, 2), round(pz, 2)))
        results[key] = (len(bad) == 0, {'bad_count': len(bad), 'checked': checked, 'sample': bad[:5]})
    return results


def verify_button_retention(bodies_dict, p):
    """Port of verify_button_retention (:6684, pass 9b finding 9): (1)
    the collar's own oversized flange reads blocked by real, unrelieved
    rib material (can't be pulled back out through the wall hole); (2)
    the tab can't drift further outward past the wall's own real
    material past the tab hole's own reach; (3) restates the
    construction-guaranteed collar-bottoms-on-rib travel gap
    (`plunger_travel`, 0.90mm) and tab gap (0.60mm)."""
    results = {}
    top = bodies_dict.get('Top')
    if top is None:
        return {'ok': None, 'note': 'Top body missing'}
    results['collar_wider_than_rib_slot'] = (p['collar']['h'] > p['rib_slot_clearance'],
                                              {'collar_h': p['collar']['h'], 'rib_slot_clearance': p['rib_slot_clearance']})
    for name, switch_bbox, nub_dir, cap in btn._button_defs(p):
        key = name.lower().replace(' ', '_')
        g = btn.button_geometry(p, switch_bbox, nub_dir, cap)
        d2 = g['d']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0

        s_rib_mid = (g['s_rib_inner'] + g['s_rib_outer']) / 2.0
        collar_top_z = z_center + W / 2.0 + p['collar']['h'] - 0.1
        pt_xy = (g['housing_xy'][0] + s_rib_mid * d2[0], g['housing_xy'][1] + s_rib_mid * d2[1])
        collar_blocked = geo.probe_point_solid(top, (pt_xy[0], pt_xy[1], collar_top_z))
        results[f'{key}_collar_blocked_by_rib'] = (collar_blocked, {'s': round(s_rib_mid, 3), 'z': round(collar_top_z, 3)})

        tab = p['tab']
        tab_z = z_center - W / 2.0 - tab['h'] / 2.0
        tab_hole_outward_s = g['s_inner'] + p.get('tab_hole_skin_margin', 2.0) / 2.0
        s_probe = tab_hole_outward_s + 0.3
        pt2_xy = (g['housing_xy'][0] + s_probe * d2[0], g['housing_xy'][1] + s_probe * d2[1])
        tab_blocked = geo.probe_point_solid(top, (pt2_xy[0], pt2_xy[1], tab_z))
        results[f'{key}_tab_outward_blocked'] = (tab_blocked, {'s': round(s_probe, 3), 'z': round(tab_z, 3)})

        rest_gap = g['s_rib_inner'] - g['s_collar_outer']
        results[f'{key}_collar_rib_gap_0.90'] = (abs(rest_gap - p['plunger_travel']) < 0.05, round(rest_gap, 4))
        results[f'{key}_tab_gap_0.60'] = (abs(p['tab']['gap'] - 0.60) < 1e-9, p['tab']['gap'])
    return results


def verify_plunger_reach(bodies_dict, p):
    """Port of verify_plunger_reach (:6560, pass 9b finding 10):
    live-probes the REAL display STEP's own switch bodies (via
    `components.load_display`/`buttons.find_switch_body`, this port's
    no-occurrence-tree equivalent of the source's live Fusion probe) to
    confirm the plunger's REST position sits `plunger_pretravel` (0.3mm)
    from the real actuator nub, not the multi-mm miss finding 10 found
    against an uninhabited bbox corner."""
    results = {}
    disp = comp.load_display(p)
    for name, switch_bbox, nub_dir, cap in btn._button_defs(p):
        key = name.lower().replace(' ', '_')
        g = btn.button_geometry(p, switch_bbox, nub_dir, cap)
        sw_body = btn.find_switch_body(disp, switch_bbox)
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
            s = btn.find_outermost_s(sw_body, (cx, cy), d2, z, max_s=6.0, step=0.05)
            if s is not None and (live_reach is None or s > live_reach):
                live_reach = s
            z += 0.2
        expect_reach = p['switch_actuator_reach']
        reach_ok = live_reach is not None and abs(live_reach - expect_reach) < 0.1
        results[f'{key}_actuator_reach'] = (reach_ok, {'expect': expect_reach, 'found': live_reach})

        cap_body = bodies_dict.get(name)
        if cap_body is not None and live_reach is not None:
            pocket_half_w = p['nub_pocket']['xy'][0] / 2.0
            rim_offset = pocket_half_w + 0.4
            rim_origin = (cx + rim_offset * t2[0], cy + rim_offset * t2[1])
            rim_s = btn.find_innermost_s(cap_body, rim_origin, d2, g['switch_z_mid'], max_s=8.0, step=0.05)
            gap = None if rim_s is None else rim_s - live_reach
            gap_ok = gap is not None and abs(gap - p['plunger_pretravel']) < 0.15
            results[f'{key}_rest_gap'] = (gap_ok, {'expect': p['plunger_pretravel'],
                                                     'found': None if gap is None else round(gap, 3)})
        else:
            results[f'{key}_rest_gap'] = (False, 'cap body or live_reach missing')
    return results


def verify_skin_intact(bodies_dict, p):
    """Port of verify_skin_intact (:7150, pass 6 item A / pass 12
    widened): regression guard against an interior cut (the tab hole/
    tab-relief lane) reaching past the true outer skin. Scans the tab's
    full z-span at the wider tab-relief footprint, probing radially
    outward from the tab hole's own analytic reach
    (s_inner + tab_hole_skin_margin/2) by two small depths -- real skin
    should start immediately past that reach."""
    top = bodies_dict['Top']
    results = {}
    tab = p['tab']
    skin_margin = p.get('tab_hole_skin_margin', 2.0)
    tab_relief_margin = 0.3
    for name, switch_bbox, nub_dir, cap in btn._button_defs(p):
        key = name.lower().replace(' ', '_').replace('_button', '')
        g = btn.button_geometry(p, switch_bbox, nub_dir, cap)
        d2, t2 = g['d'], g['t']
        housing_xy = g['housing_xy']
        W = cap['stadium'][1]
        z_center = (cap['z'][0] + cap['z'][1]) / 2.0
        tab_hole_z_lo = z_center - W / 2.0 - tab['h'] - 0.3
        main_hole_z_lo = z_center - W / 2.0 - 0.25
        z_samples = [tab_hole_z_lo + k * (main_hole_z_lo - tab_hole_z_lo) / 3.0 for k in range(4)]
        s_reach = g['s_inner'] + skin_margin / 2.0
        bad = []
        checked = 0
        for depth_out in (0.15, 0.3):
            s_probe = s_reach + depth_out
            for z in z_samples:
                for t_frac in (-0.5, 0.0, 0.5):
                    t_off = t_frac * (tab['w'] / 2.0)
                    px = housing_xy[0] + s_probe * d2[0] + t_off * t2[0]
                    py = housing_xy[1] + s_probe * d2[1] + t_off * t2[1]
                    checked += 1
                    if not geo.probe_point_solid(top, (px, py, z)):
                        bad.append((round(depth_out, 2), round(z, 2), round(t_frac, 2)))
        results[key] = (len(bad) == 0, {'bad_count': len(bad), 'checked': checked, 'sample': bad[:5]})
    return results


# ---------------------------------------------------------------------------
# Phase 2c gates -- comms stack / GPS frame / battery bay
# (features/comms_bay.py) + compass module (features/compass.py).
# ---------------------------------------------------------------------------
def find_ceiling_z_at(body, x, y, z_hi, z_lo, step=0.05):
    """Port of firefly_case.py:6199 find_ceiling_z_at -- scans DOWNWARD in
    Z at a fixed (x,y) from z_hi to z_lo and returns the first z where
    `body` is solid (the inner ceiling's own underside height there), or
    None if no solid is found anywhere in the scanned range."""
    z = z_hi
    while z >= z_lo:
        if geo.probe_point_solid(body, (x, y, z)):
            return z
        z -= step
    return None


def verify_stack3_clearance(bodies, p, comms_stack):
    """Port of firefly_case.py:7826 verify_stack3_clearance -- the
    3-board comms stack's real top (`components.load_comms_stack`'s own
    `stack_top_z`, from the REAL placed Wio solid, not a nominal guess)
    must clear Top's real inner ceiling by >= `stack3['ceiling_clear_
    min']` (0.8mm) everywhere under the L76K PCB's own footprint (the
    widest/lowest part of the stack). Trivially OK for `current`
    (`comms_stack3_full_height=False` -- no Wio/XIAO loaded, see
    components.load_comms_stack's own docstring), same convention the
    source uses."""
    s3 = p['bay']['stack3']
    if not p.get('comms_stack3_full_height', True):
        return {'stack_top_z': None, 'clearance_found': None, 'required': s3['ceiling_clear_min'], 'ok': True,
                'note': 'comms_stack3_full_height=False (current variant) -- no Wio/XIAO loaded, nothing to check'}
    stack_top_z = comms_stack.get('stack_top_z')
    if stack_top_z is None or comms_stack.get('wio') is None:
        return {'stack_top_z': None, 'clearance_found': None, 'required': s3['ceiling_clear_min'], 'ok': False}
    top = bodies['Top']
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


def verify_stack_frame_boss_clear(bodies, p):
    """Not a firefly_case.py function by name -- the "corner-block-to-
    stack keep-out" gate this phase's own brief asks for. Every case-
    screw boss (A/C/D)'s own full-height core (`corner_blocks.BOSS_CORE_R`
    -- the boss's real load path) must stay intact through the comms-
    stack frame's own z-band (`stack3['frame_z']`), independently
    re-confirming `add_comms_stack_frame`'s own `boss_relief_margin` cut
    never ate into a boss's core -- same "construction guarantee,
    re-checked independently" idiom `verify_post_walls`/`verify_corner_
    blocks` already establish for the case screws' wall thickness."""
    bottom = bodies['Bottom']
    s3 = p['bay']['stack3']
    fz0, fz1 = s3['frame_z']
    z_mid = (fz0 + fz1) / 2.0
    core_r = cb.BOSS_CORE_R - 0.3
    angles = (0.0, 90.0, 180.0, 270.0)
    results = {}
    for s in p['screws_ABC'] + p['screws_D12']:
        name = s['name']
        cx, cy = s['xy']
        checks = [geo.probe_point_solid(bottom, (cx + core_r * math.cos(math.radians(ang)),
                                                   cy + core_r * math.sin(math.radians(ang)), z_mid))
                  for ang in angles]
        results[f'{name}_core_intact'] = (all(checks), checks)
    return results


# (board key, case body name) pairs INTENDED to touch (zero clearance) --
# port of firefly_case.py:6890 ALLOWED_CONTACTS, restricted to this
# port's own three comms-stack boards (the display's own allowed
# contacts are handled separately by check_display_interference_near_ears/
# verify_ear_root_material, which already check the REAL display STEP
# against the ears/S2-boss's own construction, not a generic distance
# sweep).
BOARD_CASE_ALLOWED_CONTACTS = {
    ('l76k', 'Bottom'),  # PCB rests directly on its four corner pads (build_comms_stack_frame)
}


def verify_min_clearances(bodies, p, comms_stack, min_mm=None):
    """Port of firefly_case.py:7123 verify_min_clearances, for the three
    comms-stack boards this phase places (`components.load_comms_stack`).
    The source caps how many of a board's own nested bodies get
    individually distance-checked (`MIN_CLEARANCE_BODY_CAP`, a Fusion
    measureManager robustness workaround) -- this port's boards are
    already a handful of solids each (L76K pre-filtered by `components.
    _filter_l76k_placed`; XIAO/Wio are clean single-board reference
    docs), so every solid is checked directly via `Shape.distance_to`
    (OCP's `BRepExtrema_DistShapeShape`), no cap needed."""
    if min_mm is None:
        min_mm = p.get('clearance_min', 0.3)
    results = {}
    for board_name in ('l76k', 'xiao', 'wio'):
        board = comms_stack.get(board_name)
        if board is None:
            continue
        for case_name in ('Top', 'Bottom'):
            case_body = bodies.get(case_name)
            if case_body is None:
                continue
            allowed = (board_name, case_name) in BOARD_CASE_ALLOWED_CONTACTS
            try:
                d = board.distance_to(case_body)
            except Exception:
                continue
            ok = allowed or d >= min_mm - 1e-6
            results[f'{board_name}_vs_{case_name}'] = {
                'distance_mm': round(d, 4), 'required': None if allowed else min_mm,
                'allowed_contact': allowed, 'ok': ok,
            }
    return results


def check_board_interference(bodies, p, comms_stack):
    """Port of firefly_case.py:6199 check_interference, restricted to the
    three comms-stack boards vs. Top/Bottom (the OCC equivalent of the
    source's own live `analyzeInterference`, same `interference_volume`
    boolean-intersect idiom `check_interference_pairs` already uses for
    the printed bodies) -- skips `BOARD_CASE_ALLOWED_CONTACTS` pairs, same
    as `verify_min_clearances`."""
    results = {}
    for board_name in ('l76k', 'xiao', 'wio'):
        board = comms_stack.get(board_name)
        if board is None:
            continue
        for case_name in ('Top', 'Bottom'):
            case_body = bodies.get(case_name)
            if case_body is None:
                continue
            if (board_name, case_name) in BOARD_CASE_ALLOWED_CONTACTS:
                continue
            vol = interference_volume(case_body, board)
            if vol > 1e-4:
                results[f'{board_name}_vs_{case_name}'] = round(vol, 4)
    return results


def verify_antenna_channels(bodies, p, lora_corridor=None):
    """Port of firefly_case.py:5062 verify_antenna_channels: (1) each
    channel's own cross-section is actually open at a live-probed
    interior point; (2) the LoRa channel's own skin-safety re-check
    (independent of the Combine-Intersect construction that's supposed to
    guarantee it); (3) the GPS notch's own bbox doesn't breach the battery
    floor (regression guard, true by construction); (4) (this port only,
    no source equivalent needed since the source enforces this by
    checking the corridor reference body against every live occurrence
    generically) the LoRa cable corridor reference solid
    (`comms_bay.add_antenna_channels`' own return value) reads genuinely
    open against the built Top -- the "LoRa FPC antenna keep-out enforced
    in interference" item 1 asks for."""
    top = bodies['Top']
    geom = comms_bay.antenna_channel_geometry(p)
    results = {}

    if 'lora' in geom:
        g = geom['lora']
        px, py, pz = g['probe_xyz']
        is_open = not geo.probe_point_solid(top, (px, py, pz))
        results['lora_channel_open'] = (is_open, (round(px, 3), round(py, 3), round(pz, 3)))
        s_check = geo.true_wall_distance_along_ray(p, (px, py), g['dir'], pz)
        skin_ok = s_check is not None and s_check >= p['antenna']['channel_min_skin'] - 0.05
        results['lora_skin_ok'] = (skin_ok, round(s_check, 3) if s_check is not None else None)
        if lora_corridor is not None:
            vol = interference_volume(top, lora_corridor)
            results['lora_corridor_clear'] = (vol <= 1e-4, round(vol, 4))

    g = geom['gps']
    px, py, pz = g['probe_xyz']
    is_open = not geo.probe_point_solid(top, (px, py, pz))
    results['gps_channel_open'] = (is_open, (round(px, 3), round(py, 3), round(pz, 3)))
    bat = g['battery_bbox']
    z_clear = g['notch_z'][0] >= bat['z'][1]
    results['gps_no_battery_floor_breach'] = (z_clear, (g['notch_z'], bat['z']))
    return results


def verify_mag_pocket(bodies, p):
    """Port of firefly_case.py:5434 verify_mag_pocket: (1) the module's
    own component-side reference envelope reads genuinely hollow; (2)
    both pegs and (3) both rest pads have real material at mid-height;
    (4) the pass-15 single south stop (superseding the old 4-wall fence,
    see features/compass.py's own docstring -- kept under the historical
    `stop_has_material` key rather than the source's `fence_has_material`,
    since this port's own naming already documents the pass-15 change
    inline) has real material away from its own wire-exit notch; (5) the
    fence/stop footprint's own worst-corner clearance to the window
    bore's TRUE opening; (6) the same footprint's clearance to the REAL
    display module's own bounding box (this port measures the ACTUAL
    imported STEP compound here, not the typed `display_bbox` -- more
    faithful to the 'measured, not typed' convention `components.
    measure_standoffs` already establishes elsewhere in this port than
    the source's own live-Fusion-occurrence probe, which this port has
    no equivalent for anyway). All six report (True, ...) when
    `mag_module_fits(p)` is False ('current')."""
    mm = p['mag_module']
    if not compass_mod.mag_module_fits(p):
        return {
            'envelope_open': (True, []), 'pegs_have_material': (True, []),
            'pads_have_material': (True, []), 'stop_has_material': (True, []),
            'window_bore_clear': (True, []), 'display_back_clear': (True, 'mag_module_fits is False -- skipped'),
        }
    top = bodies['Top']
    results = {}

    lpcb = mm['local_pcb']
    probe_local_x = [lpcb['x'][0] + 0.5, (lpcb['x'][0] + lpcb['x'][1]) / 2.0, lpcb['x'][1] - 0.5]
    local_y_mid = (lpcb['y'][0] + lpcb['y'][1]) / 2.0
    wx_mid = compass_mod.mag_world_x(p, local_y_mid)
    z_component = compass_mod.mag_world_z(p, 1.0 + mm['local_component_h'] * 0.5)
    bad_envelope = []
    for lx in probe_local_x:
        wy = compass_mod.mag_world_y(p, lx)
        if geo.probe_point_solid(top, (wx_mid, wy, z_component)):
            bad_envelope.append((round(wx_mid, 2), round(wy, 2), round(z_component, 2)))
    results['envelope_open'] = (not bad_envelope, bad_envelope[:5])

    ceiling = p['top_ceiling_underside_z']
    pcb_bottom = compass_mod.mag_pcb_bottom_world_z(p)
    z_mid = (ceiling + pcb_bottom) / 2.0
    bad_pegs = [(round(px, 2), round(py, 2), round(z_mid, 2)) for px, py in compass_mod.mag_peg_world_positions(p)
                if not geo.probe_point_solid(top, (px, py, z_mid))]
    results['pegs_have_material'] = (not bad_pegs, bad_pegs[:5])
    bad_pads = [(round(px, 2), round(py, 2), round(z_mid, 2)) for px, py in compass_mod.mag_pad_world_positions(p)
                if not geo.probe_point_solid(top, (px, py, z_mid))]
    results['pads_have_material'] = (not bad_pads, bad_pads[:5])

    x0, x1, y0, y1 = compass_mod.mag_pcb_world_footprint(p)
    clear, wall = mm['fence_clear'], mm['fence_wall']
    stop_h = compass_mod.MAG_STOP_H
    stop_mid_z = ceiling - stop_h / 2.0
    stop_y = y0 - clear - wall / 2.0
    probe_pts = [(x1 - 0.5, stop_y)]
    bad_stop = [(round(wx, 2), round(wy, 2), round(stop_mid_z, 2)) for wx, wy in probe_pts
                if not geo.probe_point_solid(top, (wx, wy, stop_mid_z))]
    results['stop_has_material'] = (not bad_stop, bad_stop[:5])

    bore_clear = compass_mod.mag_window_bore_clearance(p)
    results['window_bore_clear'] = (bore_clear >= compass_mod.MAG_DISPLAY_RING_MIN_CLEAR, round(bore_clear, 3))

    _, _, _, fence_y1 = compass_mod.mag_fence_world_footprint(p)
    disp = comp.load_display(p)
    dbb = disp.bounding_box()
    display_back_clear = dbb.min.Y - fence_y1
    results['display_back_clear'] = (display_back_clear >= compass_mod.MAG_DISPLAY_RING_MIN_CLEAR,
                                      round(display_back_clear, 3))
    return results
