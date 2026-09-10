"""Ears (S1/S3 display mount) + S2 boss -- build123d port of
firefly_case.py's pass-16 `add_ear` (:2381) and `add_s2_boss` (:2536),
plus their own height-cap helpers `_ear_wedge_wall_touch_z1` (:2125) and
`ear_root_cap_z1` (:2154). `_ear_root_z1`, `_wall_outward_axes`,
`_nearer_spine_y`, `_corner_block_ring_limit_r`, and
`add_root_reinforcement` are NOT re-ported here -- they are the exact
same wall-anchored-member helpers `features/corner_blocks.py` already
built for the A/C/D screws (mech review F7: "generalize add_lanyard_
corner_block's pattern"), reused directly (`cb.*`/`geo.*` below).

Per docs/hardware/headless-port-plan.md and the port brief: the ear/S2
seat height and the standoff target XY are NOT the historical typed
`board_standoffs` numbers -- they come from `components.py`'s
`measure_standoffs` (a live measurement of the real display STEP), which
this module's callers must compute once and pass in as `standoffs`
(measure_standoffs' own return dict, keyed 'S1'/'S2'/'S3').

The one piece of source geometry deliberately NOT ported: `add_ear`'s
own axis2-sign flip (comparing `_dot(axis2, (mx, my-ay, 0))` and negating
axis2 if negative) has no effect on the built solid -- `oriented_stadium_
prism`'s stadium cross-section is symmetric under axis2 -> -axis2 by
construction (see geometry.py), so the flip is inert in the source too;
omitted here rather than translated as dead code.
"""
import math

import build123d as bd

from .. import components as comp
from .. import geometry as geo
from . import corner_blocks as cb

STANDOFF_BARREL_DIA = 4.2      # firefly_case.py:1867 -- measured 3.53mm OD + print-tolerance clearance
STANDOFF_BARREL_DEPTH = 4.70   # firefly_case.py:1868
STANDOFF_BARREL_MARGIN = 0.30  # firefly_case.py:1869

BUTTON_RIB_GUSSET_MARGIN = 2.0  # firefly_case.py:2320


def _ear_wedge_wall_touch_z1(p):
    """firefly_case.py:2125 _ear_wedge_wall_touch_z1."""
    margin = 0.4
    return min(p['power_cap']['z'][0], p['home_cap']['z'][0]) - BUTTON_RIB_GUSSET_MARGIN - margin


def ear_root_cap_z1(p, cx, cy, r, z1_nominal):
    """firefly_case.py:2154 ear_root_cap_z1."""
    return min(cb._ear_root_z1(p, cx, cy, r, z1_nominal), _ear_wedge_wall_touch_z1(p))


def _target_xy_seat_z(p, standoffs, name):
    m = standoffs[name]
    tx, ty = m['world_xy']
    seat_z = m['standoff_plane_z'] - p['ear_seat_offset']
    return tx, ty, seat_z


def add_ear(bodies, p, name, standoffs):
    """Port of add_ear (:2381) for ear `name` ('S1' or 'S3')."""
    ear = p['ears'][name]
    rx, ry = ear['root_xy']
    tx, ty, seat_z = _target_xy_seat_z(p, standoffs, ear['target'])
    boss_r = p['boss_dia'] / 2.0
    z0, z1_nominal = p['split_z'], p['top_ceiling_underside_z']

    root_z1 = ear_root_cap_z1(p, rx, ry, boss_r + cb.CORNER_BLOCK_REACH, z1_nominal)

    # (1) wall-anchored root (capped, no pilot -- D1/D2 moved to their own
    # independent corner blocks, see features/corner_blocks.py).
    axis1w, axis2w = geo.wall_outward_axes(p, rx, ry)
    root_capsule = geo.cylinder_solid(rx, ry, boss_r, z0, root_z1)
    wedge_len = cb.CORNER_BLOCK_REACH + cb.CORNER_BLOCK_WEDGE_OVERLAP
    wedge_offset = boss_r + cb.CORNER_BLOCK_REACH / 2.0 - cb.CORNER_BLOCK_WEDGE_OVERLAP / 2.0
    wedge_center = (rx + axis2w[0] * wedge_offset, ry + axis2w[1] * wedge_offset, z0)
    wedge = geo.oriented_box_prism(wedge_center, axis1w, axis2w, (0.0, 0.0, 1.0),
                                    2.0 * boss_r, wedge_len, root_z1 - z0)
    root_wide = root_capsule + wedge
    root_wide = root_wide & geo.build_inner_cavity_clip_tool(p)

    ring_limit_r = cb._corner_block_ring_limit_r(p, rx, ry)
    ring_limit = geo.cylinder_solid(0.0, geo._nearer_spine_y(p, ry), ring_limit_r, z0 - 1.0, root_z1 + 1.0)
    root_wide = root_wide & ring_limit

    s3 = p['bay'].get('stack3')
    if s3 is not None:
        pcb = s3['l76k_pcb']
        m = cb.CORNER_BLOCK_STACK_MARGIN
        stack_keepout = geo.box_solid(pcb['x'][0] - m, pcb['x'][1] + m, pcb['y'][0] - m, pcb['y'][1] + m,
                                       z0 - 0.5, root_z1 + 0.5)
        root_wide = root_wide - stack_keepout

    root_core = geo.cylinder_solid(rx, ry, cb.BOSS_CORE_R, z0, root_z1)
    root_block = root_wide + root_core

    # (2) seat arm (at the root's own low, button-safe z-band -- Finding-1
    # connectivity fix) + target riser (climbs from the arm up to the
    # real seat_z).
    arm_z1 = root_z1
    arm_z0 = arm_z1 - p['ear_arm_thickness']
    dx, dy = tx - rx, ty - ry
    seg_len = math.hypot(dx, dy)
    axis1 = (dx / seg_len, dy / seg_len, 0.0)
    axis2 = (-axis1[1], axis1[0], 0.0)
    mx, my = (rx + tx) / 2.0, (ry + ty) / 2.0
    arm = geo.oriented_stadium_prism((mx, my, arm_z0), axis1, axis2, (0.0, 0.0, 1.0),
                                      seg_len + 2.0 * boss_r, 2.0 * boss_r, arm_z1 - arm_z0)
    arm = arm & geo.build_inner_cavity_clip_tool(p)

    barrel_z0 = seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN
    riser_wide_z0 = max(arm_z0, barrel_z0)
    riser = geo.cylinder_solid(tx, ty, cb.BOSS_CORE_R, arm_z0, riser_wide_z0)
    riser_wide = geo.cylinder_solid(tx, ty, boss_r, riser_wide_z0, seat_z)
    arm = arm + riser + riser_wide

    block = root_block + arm
    top = bodies['Top'] + block

    def _cut_hole(t):
        hole = geo.cylinder_solid(tx, ty, p['ear_standoff_hole_dia'] / 2.0, arm_z0 - 0.5, seat_z + 0.5)
        barrel_clear = geo.cylinder_solid(tx, ty, STANDOFF_BARREL_DIA / 2.0, barrel_z0, seat_z + 0.5)
        return t - hole - barrel_clear

    top = _cut_hole(top)
    top = cb.add_root_reinforcement(top, rx, ry, boss_r, root_z1, direction='up', z_floor=z0)
    top = cb.add_root_reinforcement(top, tx, ty, boss_r, seat_z, direction='up', z_floor=z0)
    # pass-15 lesson (re-cut after every collar joins, since a
    # solid-to-the-axis collar can silently replug a hole in its own band).
    top = _cut_hole(top)

    bodies['Top'] = top
    return bodies


def add_s2_boss(bodies, p, standoffs):
    """Port of add_s2_boss (:2536): a short boss from the west wall
    carrying S2, height-clamped below the display's real battery
    connector (`components.battery_connector_world_bbox`) whenever the
    arm's own y actually runs under it, plus an unconditional keep-out
    cut of that same connector footprint (+1mm margin) regardless."""
    s2 = p['s2_boss']
    tx, ty, seat_z = _target_xy_seat_z(p, standoffs, s2['target'])
    boss_r = p['boss_dia'] / 2.0
    wall_x = -(p['outer_radius'] - p['wall'])
    wy = ty

    (bcx0, bcx1), (bcy0, bcy1), (bcz0, bcz1) = comp.battery_connector_world_bbox(p)
    clear = p['s2_battery_clear']
    arm_z1 = seat_z
    if bcy0 - boss_r <= wy <= bcy1 + boss_r:
        arm_z1 = min(seat_z, bcz0 - clear)
    arm_z0 = arm_z1 - p.get('ear_arm_thickness', 3.0)

    wx_embed = wall_x - cb.CORNER_BLOCK_REACH
    dx, dy = tx - wx_embed, ty - wy
    seg_len = math.hypot(dx, dy)
    axis1 = (dx / seg_len, dy / seg_len, 0.0)
    axis2 = (-axis1[1], axis1[0], 0.0)
    mx, my = (wx_embed + tx) / 2.0, (wy + ty) / 2.0
    arm = geo.oriented_stadium_prism((mx, my, arm_z0), axis1, axis2, (0.0, 0.0, 1.0),
                                      seg_len + 2.0 * boss_r, 2.0 * boss_r, arm_z1 - arm_z0)

    # JOIN-SAFE clip (negative safety_margin -- grows the cavity tool
    # OUTWARD instead of shrinking it): firefly_case.py's own comment on
    # this exact line documents why the shared +0.1mm-shrink clip leaves
    # a horizontal wall-reaching member's west end 0.1mm short of the
    # true wall (a live no-op join there) -- -0.15mm guarantees a real
    # 0.15mm overlap with Top's true wall material instead.
    join_clip_tool = geo.build_inner_cavity_clip_tool(p, safety_margin=-0.15)
    arm = arm & join_clip_tool

    conn_keepout = geo.box_solid(bcx0 - 1.0, bcx1 + 1.0, bcy0 - 1.0, bcy1 + 1.0, bcz0 - clear, bcz1 + 1.0)
    arm = arm - conn_keepout

    target_core = geo.cylinder_solid(tx, ty, cb.BOSS_CORE_R, arm_z0, arm_z1)
    arm = arm + target_core

    barrel_z0 = seat_z - STANDOFF_BARREL_DEPTH - STANDOFF_BARREL_MARGIN
    pad_z0 = min(arm_z1, barrel_z0)
    if seat_z - pad_z0 > 1e-6:
        pad = geo.cylinder_solid(tx, ty, boss_r, pad_z0, seat_z)
        arm = arm + pad

    arm = _best_effort_underside_edge_chamfer(arm, arm_z0)

    top = bodies['Top'] + arm

    def _cut_hole(t):
        hole = geo.cylinder_solid(tx, ty, p['ear_standoff_hole_dia'] / 2.0, arm_z0 - 0.5, seat_z + 0.5)
        barrel_clear = geo.cylinder_solid(tx, ty, STANDOFF_BARREL_DIA / 2.0, barrel_z0, seat_z + 0.5)
        return t - hole - barrel_clear

    top = _cut_hole(top)
    top = cb.add_root_reinforcement(top, tx, ty, boss_r, seat_z, direction='up', z_floor=p['split_z'])
    top = _cut_hole(top)

    bodies['Top'] = top
    return bodies


def _best_effort_underside_edge_chamfer(arm, arm_z0, chamf=0.6):
    """Not in firefly_case.py -- a phase-2 printability experiment per
    the port brief ("try a chamfer, document if support is still
    needed"): breaks the arm's bottom-face PERIMETER edge (where the
    vertical side wall meets the flat underside) with a 45-degree-ish
    chamfer, best-effort (skip on any solver failure), same idiom as
    lug.py's own best-effort fillets.

    Documented finding: this measurably shrinks the flagged overhang
    area right at the arm's own edges, but the arm's underside is a
    FLAT, horizontal (Z-normal) face over its full span -- extruded
    along Z as it is, the interior of that face is 0 degrees off
    horizontal everywhere, and no perimeter-only chamfer changes that;
    only a full lengthwise taper (turning the arm into a wedge, not a
    constant-thickness beam) would remove the need for slicer support
    under the middle of the span. See gates.py's own overhang report and
    docs/hardware/headless-port-parity.md for the measured before/after
    cluster area -- the pass-16 printability review already reached the
    same conclusion and accepted support here (whitelisted as
    'general_ceiling_overhang' in tools/offline_stl_check.py)."""
    try:
        edges = []
        for e in arm.edges():
            bb = e.bounding_box()
            if bb.max.Z - bb.min.Z < 1e-3 and abs(bb.min.Z - arm_z0) < 0.05:
                edges.append(e)
        if edges:
            return bd.chamfer(edges, length=chamf)
    except Exception:
        pass
    return arm
