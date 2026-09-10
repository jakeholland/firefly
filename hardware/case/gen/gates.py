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

from . import components as comp
from . import geometry as geo
from .features import buttons as btn
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


def all_gates(bodies, p, stl_paths, whitelist_xy=None, standoffs=None):
    """Run every phase-1 gate (plus, when `standoffs` is given --
    components.measure_standoffs' own return dict -- every phase-2
    ears/S2-boss gate) and return one combined report dict."""
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


def check_display_interference_near_ears(bodies, p, standoffs, margin=6.0):
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
    idealized cylinder the barrel-clearance cut assumes."""
    disp = comp.load_display(p)
    top = bodies['Top']

    xs, ys = [], []
    for ear in p['ears'].values():
        xs.append(ear['root_xy'][0])
        ys.append(ear['root_xy'][1])
    for m in standoffs.values():
        xs.append(m['world_xy'][0])
        ys.append(m['world_xy'][1])
    x0, x1 = min(xs) - margin, max(xs) + margin
    y0, y1 = min(ys) - margin, max(ys) + margin

    hits = []
    for s in disp.solids():
        bb = s.bounding_box()
        cx, cy = (bb.min.X + bb.max.X) / 2.0, (bb.min.Y + bb.max.Y) / 2.0
        if not (x0 <= cx <= x1 and y0 <= cy <= y1):
            continue
        vol = interference_volume(top, s)
        if vol > 1e-4:
            hits.append({
                'bbox_xy': (round(bb.min.X, 2), round(bb.max.X, 2), round(bb.min.Y, 2), round(bb.max.Y, 2)),
                'bbox_z': (round(bb.min.Z, 2), round(bb.max.Z, 2)),
                'volume_mm3': round(vol, 4),
            })
    # NOISE_FLOOR_MM3: boolean-cleanup/tessellation noise at a shared
    # boundary (same class check_interference_pairs' own 1e-4mm^3
    # coincident-touch tolerance already accepts elsewhere in this file,
    # just a hair looser here for a genuinely negligible residual --
    # live-checked, this port's own smallest real fix was ~1.16mm^3, five
    # orders of magnitude above this floor).
    NOISE_FLOOR_MM3 = 0.001
    real_hits = [h for h in hits if h['volume_mm3'] > NOISE_FLOOR_MM3]
    return {'ok': not real_hits, 'region_xy': (round(x0, 1), round(x1, 1), round(y0, 1), round(y1, 1)),
            'hits': hits, 'real_hits': real_hits}


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
