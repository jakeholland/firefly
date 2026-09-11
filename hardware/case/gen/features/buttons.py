"""Power/Home side buttons -- build123d port of firefly_case.py's
`button_geometry` (:2954), `add_button` (:3129), and `add_buttons`
(:3639), plus the ear/S2-boss "keep a continuous material column" cut
guard (`_ear_boss_keepout_points`/`_clip_of_ear_boss_keepout`, :1968/
:2041 -- not one of the 217 named functions' own home, but load-bearing:
protects the S1/S3 ear wall-roots and the S2 boss's own target column
from being severed by a button's own wall/tab cuts, the same live-found
failure class `components.ceiling_safe_display_cut`'s docstring
describes for the display-vs-ear interaction).

Every historical correction the README records is carried forward
verbatim (same numbers, same reasoning, same order of operations):
`plunger_pretravel`/`switch_actuator_reach` (finding 10's real-nub
reach, not an empty bbox corner), `s_wall` via
`true_wall_distance_along_ray` (finding "the flat-wall assumption
badly wrong for Home"), the rib/collar actuator-clearance clamp
(finding 9's collateral discovery), the tab-relief lane (finding 9),
the S2-boss tab-relief LANE EXTENSION -- `tab_sweep_body` below --
(pass-16 FIX item 4: the S2 boss's own horizontal arm sits squarely in
the Power button's tab-insertion sweep), the wall-connector spoke +
ceiling gusset anchored at the connector's own outboard end (pass 15
item 5, "floating" rib fix; the FIRST t-offset attempt that hit real
display material is deliberately not reproduced, only the fix), and
pass-16 item D's best-effort lead-in fillets.

`clip_tool`/`root`/`combine_*`/`_refetch_by_name`/`dedupe_body` are all
deleted-as-quirk here, same as every other feature module in this
package -- OCC's `+`/`-`/`&` operators need no Fusion timeline
scaffolding (see geometry.py's own module docstring).
"""
import math

import build123d as bd

from .. import components as comp
from .. import geometry as geo
from . import corner_blocks as cb
from . import ears as ears_mod

RIB_CONNECTOR_T_OFFSET = -0.2   # firefly_case.py:2694 -- deliberate OVERLAP with the rib's own edge, not a gap
RIB_CONNECTOR_W = 2.0           # firefly_case.py:2695
CEILING_GUSSET_W = 2.0          # mm, tangential width (mirrors RIB_CONNECTOR_W)
CEILING_GUSSET_OVERLAP = 0.3    # mm past the nominal ceiling, guarantees a real (not coincident-face) join
RIB_LEAD_IN_FILLET_R = 0.4      # mm -- pass-16 item D, cosmetic/print-quality only

# pass 16 (live-found, ROUND 2 in firefly_case.py's own history): just
# past verify_post_walls'/verify_ear_root_material's own probe radius
# (top_pilot_dia/2 + POST_WALL_MIN + a small margin), NOT the boss's own
# full OD -- see _clip_of_ear_boss_keepout's own docstring for why a
# bigger radius or a full-height band both backfired live.
EAR_BOSS_BUTTON_KEEPOUT_R = 2.5


def normalize2(v):
    """firefly_case.py:2946 normalize2."""
    n = math.hypot(v[0], v[1])
    return (v[0] / n, v[1] / n)


def ray_box_exit_2d(center_xy, d_xy, bbox_x, bbox_y):
    """firefly_case.py:2951 ray_box_exit_2d."""
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
    """Port of button_geometry (:2954) -- verbatim algorithm, including
    the finding-10 real-actuator-reach fix and the finding-9 rib/collar
    actuator-clearance clamp. See this module's own docstring for the
    corrections this carries forward."""
    d2 = normalize2(nub_dir)
    t2 = (-d2[1], d2[0])
    cx = (switch_bbox['x'][0] + switch_bbox['x'][1]) / 2.0
    cy = (switch_bbox['y'][0] + switch_bbox['y'][1]) / 2.0
    switch_z_mid = (switch_bbox['z'][0] + switch_bbox['z'][1]) / 2.0

    t_exit = ray_box_exit_2d((cx, cy), d2, switch_bbox['x'], switch_bbox['y'])
    housing_xy = (cx + t_exit * d2[0], cy + t_exit * d2[1])
    s_actuator = p['switch_actuator_reach'] - t_exit

    cap_z_center = (cap['z'][0] + cap['z'][1]) / 2.0
    s_wall = geo.true_wall_distance_along_ray(p, housing_xy, d2, cap_z_center)
    s_inner = s_wall - p['wall']
    s_plunger_tip = s_actuator + p['plunger_pretravel']
    s_outer_face = s_wall + cap['proud']
    s_tab_face = s_inner - p['tab']['gap']

    s_rib_outer = s_wall - p['rib_inboard_offset']
    s_rib_inner = s_rib_outer - p['rib_thickness']
    s_collar_outer = s_rib_inner - p['plunger_travel']
    s_collar_inner = s_collar_outer - p['collar']['len']

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
        'rib_start_xy': xy_at(s_rib_inner),
        'collar_start_xy': xy_at(s_collar_inner),
        'rib_center_xy': xy_at((s_rib_outer + s_rib_inner) / 2.0),
        'collar_center_xy': xy_at((s_collar_outer + s_collar_inner) / 2.0),
    }


# ---------------------------------------------------------------------------
# Ear/S2-boss keep-out (protects those features' own material column from
# a button's cuts) -- firefly_case.py:1968/:2041, not one of the 217 named
# functions' own PARAMS-keyed home, but load-bearing (see module docstring).
# ---------------------------------------------------------------------------
def _ear_boss_keepout_points(p, standoffs):
    """Port of _ear_boss_keepout_points (:1968) -- keyed off the port's
    own MEASURED standoff targets (`standoffs`, `components.measure_
    standoffs`' return dict via ears_mod._target_xy_seat_z), not the
    typed `p['board_standoffs']` the source reads, per the port's
    established "measured, not typed" convention (see features/ears.py's
    own module docstring)."""
    boss_r = p['boss_dia'] / 2.0
    reach_r = boss_r + cb.CORNER_BLOCK_REACH
    ceiling = p['top_ceiling_underside_z']
    pz0 = p['top_pilot_z'][0]
    pts = []
    for name, ear in p['ears'].items():
        rx, ry = ear['root_xy']
        z1 = ears_mod.ear_root_cap_z1(p, rx, ry, reach_r, ceiling)
        pts.append({'xy': (rx, ry), 'z': (pz0 - 0.5, z1 + 0.5)})
        tx, ty, seat_z = ears_mod._target_xy_seat_z(p, standoffs, ear['target'])
        pts.append({'xy': (tx, ty), 'z': (z1 - p['ear_arm_thickness'] - 0.5, seat_z + 0.5)})
    s2 = p['s2_boss']
    s2x, s2y, s2seat = ears_mod._target_xy_seat_z(p, standoffs, s2['target'])
    # port-specific fix (no firefly_case.py equivalent needed -- see
    # ears.s2_boss_arm_z_band's own docstring): uses the arm's REAL,
    # battery-clamped z-band, not the nominal `seat_z - ear_arm_thickness`
    # the source's own formula assumes -- the battery-connector clamp
    # usually pulls S2's own arm well below that nominal band.
    s2_arm_z0, s2_arm_z1 = ears_mod.s2_boss_arm_z_band(p, standoffs)
    pts.append({'xy': (s2x, s2y), 'z': (min(s2_arm_z0, s2seat - p['ear_arm_thickness']) - 0.5, s2seat + 0.5)})
    return pts


def _clip_of_ear_boss_keepout(tool, p, standoffs):
    """Port of _clip_of_ear_boss_keepout (:2041)."""
    for s in _ear_boss_keepout_points(p, standoffs):
        keepout = geo.cylinder_solid(s['xy'][0], s['xy'][1], EAR_BOSS_BUTTON_KEEPOUT_R, s['z'][0], s['z'][1])
        tool = tool - keepout
    return tool


def _best_effort_fillet_at_z(body, z_target, radius, tol=0.15):
    """Best-effort constant-radius fillet on edges lying flat at
    z=z_target -- port of the same skip-on-failure idiom
    `_best_effort_fillet_at_z` (:1776) and features/ears.py's own
    `_best_effort_underside_edge_chamfer` already establish (a fillet
    only rounds a cosmetic edge here; verify_plunger_reach/verify_
    button_insertion/verify_button_retention all gate the mechanism
    itself, never a filleted edge)."""
    try:
        edges = []
        for e in body.edges():
            bb = e.bounding_box()
            if bb.max.Z - bb.min.Z < tol and abs((bb.min.Z + bb.max.Z) / 2.0 - z_target) < tol:
                edges.append(e)
        if edges:
            return bd.fillet(edges, radius=radius)
    except Exception:
        pass
    return body


def add_button(bodies, name, switch_bbox, nub_dir, cap, p, standoffs,
                thickened_envelope, outer_envelope, tab_clip_tool, existing_top=None):
    """Port of add_button (:3129)."""
    g = button_geometry(p, switch_bbox, nub_dir, cap)
    d2, t2 = g['d'], g['t']
    d3, t3 = (d2[0], d2[1], 0.0), (t2[0], t2[1], 0.0)
    z3 = (0.0, 0.0, 1.0)
    z_lo, z_hi = cap['z']
    z_center = (z_lo + z_hi) / 2.0
    L, W = cap['stadium']

    # --- cap body: uniform stadium prism, over-built past the nominal
    # outer face then trimmed to the true curved envelope.
    margin = 3.0
    total_depth = (g['s_outer_face'] - g['s_plunger_tip']) + margin
    neg_d3 = (-d3[0], -d3[1], -d3[2])
    extended_outer_xy = (g['outer_face_xy'][0] + margin * d2[0], g['outer_face_xy'][1] + margin * d2[1])
    cap_center = (extended_outer_xy[0], extended_outer_xy[1], z_center)
    cap_body = geo.oriented_stadium_prism(cap_center, t3, z3, neg_d3, L, W, total_depth)
    cap_body = cap_body & thickened_envelope

    # --- wall hole: enlarged copy of the actual shaft, curve-trimmed the
    # same way the real cap is, so it fully contains it with clearance.
    cap_clearance = p['cap_clearance']
    hole_cutter = geo.oriented_stadium_prism(cap_center, t3, z3, neg_d3,
                                              L + 2 * cap_clearance, W + 2 * cap_clearance, total_depth)
    hole_cutter = hole_cutter & thickened_envelope
    hole_cutter = _clip_of_ear_boss_keepout(hole_cutter, p, standoffs)
    bodies['Top'] = bodies['Top'] - hole_cutter

    # --- retaining-tab hole (interior only -- bounded analytically at
    # s_inner + skin_margin/2, never reaching the outer skin) + pass-12's
    # own tab_clip_tool guard against the true curved wall off-axis.
    tab = p['tab']
    tab_hole_z_lo = z_center - W / 2.0 - tab['h'] - 0.3
    tab_hole_z_hi = z_center - W / 2.0 + 0.3
    tab_hole_z_center = (tab_hole_z_lo + tab_hole_z_hi) / 2.0
    tab_hole_z_span = tab_hole_z_hi - tab_hole_z_lo
    inner_face_xy = (g['housing_xy'][0] + g['s_inner'] * d2[0], g['housing_xy'][1] + g['s_inner'] * d2[1])
    skin_margin = p.get('tab_hole_skin_margin', 2.0)
    tab_hole_depth = abs(g['s_inner'] - g['s_tab_face']) + skin_margin
    tab_hole_center_xy = ((inner_face_xy[0] + g['tab_face_xy'][0]) / 2.0,
                          (inner_face_xy[1] + g['tab_face_xy'][1]) / 2.0)
    tab_hole_start = (tab_hole_center_xy[0] - (tab_hole_depth / 2.0) * d2[0],
                      tab_hole_center_xy[1] - (tab_hole_depth / 2.0) * d2[1])
    tab_hole_body = geo.oriented_box_prism((tab_hole_start[0], tab_hole_start[1], tab_hole_z_center),
                                            t3, z3, d3, tab['w'] + 2.0, tab_hole_z_span, tab_hole_depth)
    tab_hole_body = tab_hole_body & tab_clip_tool
    tab_hole_body = _clip_of_ear_boss_keepout(tab_hole_body, p, standoffs)
    bodies['Top'] = bodies['Top'] - tab_hole_body

    # --- pass-16 FIX item 4: the tab's ENTIRE insertion sweep (not just
    # the rib's own thickness) gets a dedicated lane, since the S2 boss's
    # own horizontal arm (a pass-16 addition) sits squarely across the
    # Power button's own insertion path at almost exactly the tab's own
    # z-band. A no-op for Home, whose sweep never crosses that arm's
    # footprint. Live-tuned margins (0.5mm both tangential and axial --
    # a first 0.3mm attempt left one corner point still solid at the very
    # start of the sweep).
    sweep_margin = 0.5
    sweep_w = tab['w'] + 2 * sweep_margin
    sweep_z_hi = z_center - W / 2.0 + sweep_margin
    sweep_z_lo = z_center - W / 2.0 - tab['h'] - sweep_margin
    sweep_z_span = sweep_z_hi - sweep_z_lo
    sweep_z_center = (sweep_z_hi + sweep_z_lo) / 2.0
    s_sweep_lo = g['s_rib_inner'] - 2.0 - 1.0
    s_sweep_hi = g['s_tab_face']
    sweep_depth = (s_sweep_hi - s_sweep_lo) + 0.5
    sweep_start_xy = (g['housing_xy'][0] + s_sweep_lo * d2[0], g['housing_xy'][1] + s_sweep_lo * d2[1])
    tab_sweep_body = geo.oriented_box_prism((sweep_start_xy[0], sweep_start_xy[1], sweep_z_center),
                                             t3, z3, d3, sweep_w, sweep_z_span, sweep_depth)
    tab_sweep_body = tab_sweep_body & tab_clip_tool
    tab_sweep_body = _clip_of_ear_boss_keepout(tab_sweep_body, p, standoffs)
    bodies['Top'] = bodies['Top'] - tab_sweep_body

    # --- nub pocket at the plunger tip.
    pocket = p['nub_pocket']
    pocket_center = (g['plunger_tip_xy'][0], g['plunger_tip_xy'][1], g['switch_z_mid'])
    pocket_body = geo.oriented_box_prism(pocket_center, t3, z3, d3, pocket['xy'][0], pocket['xy'][1], pocket['depth'])
    cap_body = cap_body - pocket_body

    # --- retaining tab, joined to the cap.
    tab_len_along_d = 1.5
    tab_z = z_center - W / 2.0 - tab['h'] / 2.0
    tab_start_xy = (g['tab_face_xy'][0] - tab_len_along_d * d2[0] / 2.0,
                    g['tab_face_xy'][1] - tab_len_along_d * d2[1] / 2.0)
    tab_body = geo.oriented_box_prism((tab_start_xy[0], tab_start_xy[1], tab_z), t3, z3, d3,
                                       tab['w'], tab['h'], tab_len_along_d)
    cap_body = cap_body + tab_body

    # --- plunger guide rib (joined to Top) + inward stop collar (joined
    # to the cap): the rib keeps the plunger from tilting, the collar
    # bottoms on it plunger_travel before the tip reaches the switch.
    attach_margin = 0.8
    rib_len = p['rib_thickness']
    rib_start = (g['rib_start_xy'][0], g['rib_start_xy'][1], z_center)
    rib_plate = geo.oriented_box_prism(rib_start, t3, z3, d3, L + 2 * attach_margin, W + 2 * attach_margin, rib_len)
    slot_body = geo.oriented_stadium_prism(cap_center, t3, z3, neg_d3,
                                            L + 2 * p['rib_slot_clearance'], W + 2 * p['rib_slot_clearance'],
                                            total_depth + 4.0)
    rib_plate = rib_plate - slot_body

    # finding-9 tab-relief lane: a dedicated pass-through so the tab (cast
    # integrally with the shaft, not a flexing spring) can slide past the
    # rib during inside-out assembly.
    tab_relief_margin = 0.3
    tab_relief_w = tab['w'] + 2 * tab_relief_margin
    tab_relief_z_hi = z_center - W / 2.0 + tab_relief_margin
    tab_relief_z_lo = z_center - W / 2.0 - tab['h'] - tab_relief_margin
    tab_relief_z_span = tab_relief_z_hi - tab_relief_z_lo
    tab_relief_z_center = (tab_relief_z_hi + tab_relief_z_lo) / 2.0
    tab_relief_axial_margin = 0.5
    rib_start_for_relief = (g['rib_start_xy'][0] - tab_relief_axial_margin * d3[0],
                             g['rib_start_xy'][1] - tab_relief_axial_margin * d3[1],
                             tab_relief_z_center)
    tab_relief_body = geo.oriented_box_prism(rib_start_for_relief, t3, z3, d3,
                                              tab_relief_w, tab_relief_z_span,
                                              rib_len + 2 * tab_relief_axial_margin)
    tab_relief_body = tab_relief_body & tab_clip_tool
    rib_plate = rib_plate - tab_relief_body

    # wall-reaching spoke (finding-9 collateral discovery: a rib_plate
    # that doesn't touch anything else would otherwise be a disjoint,
    # unattached solid under OCC's own union -- `+` on non-touching
    # solids yields a valid two-piece compound, not a Fusion-style
    # silent no-op, but the design intends a real physical join to the
    # wall, so this spoke is still needed on its own construction merits).
    connector_margin = 0.3
    connector_target_s = g['s_wall'] - connector_margin
    connector_len = connector_target_s - g['s_rib_outer']
    connector_t_center = None
    if connector_len > 0:
        connector_t_center = (L / 2.0 + attach_margin) + RIB_CONNECTOR_T_OFFSET + RIB_CONNECTOR_W / 2.0
        rib_outer_xy = (g['housing_xy'][0] + g['s_rib_outer'] * d2[0], g['housing_xy'][1] + g['s_rib_outer'] * d2[1])
        connector_start = (rib_outer_xy[0] + connector_t_center * t2[0],
                            rib_outer_xy[1] + connector_t_center * t2[1], z_center)
        connector_body = geo.oriented_box_prism(connector_start, t3, z3, d3, RIB_CONNECTOR_W, W, connector_len)
        rib_plate = rib_plate + connector_body

    # pass-15 item 5: ceiling gusset, anchored at the connector's own
    # outboard end (near s_wall) rather than the plunger axis (a t-offset
    # gusset near the axis hit real display board material -- see this
    # module's own docstring; only the fix is reproduced).
    ceiling = p['top_ceiling_underside_z']
    gusset_top = z_center + W / 2.0 + attach_margin
    gusset_h = (ceiling + CEILING_GUSSET_OVERLAP) - gusset_top
    if gusset_h > 0 and connector_len > 0:
        gusset_outer_s = connector_target_s
        gusset_xy_base = (g['housing_xy'][0] + gusset_outer_s * d2[0],
                           g['housing_xy'][1] + gusset_outer_s * d2[1])
        gusset_start = (gusset_xy_base[0] + connector_t_center * t2[0],
                         gusset_xy_base[1] + connector_t_center * t2[1], gusset_top)
        gusset_body = geo.oriented_box_prism(gusset_start, t3, d3, z3, CEILING_GUSSET_W, RIB_CONNECTOR_W, gusset_h)
        rib_plate = rib_plate + gusset_body

    # trim the whole rib+connector+gusset unit back to the TRUE curved
    # outer envelope (the tangential-offset connector/gusset rays aren't
    # purely radial, so a flat margin alone isn't safe everywhere).
    rib_plate = rib_plate & outer_envelope

    # pass-16 item D: best-effort cosmetic lead-in fillets, scoped to
    # rib_plate alone before it joins Top -- a solver failure here can
    # only skip a cosmetic touch, never risk the mechanism.
    rib_plate = _best_effort_fillet_at_z(rib_plate, z_center, RIB_LEAD_IN_FILLET_R)
    rib_plate = _best_effort_fillet_at_z(rib_plate, gusset_top, RIB_LEAD_IN_FILLET_R)

    bodies['Top'] = bodies['Top'] + rib_plate

    # --- inward stop collar, joined to the cap; clipped to the inner
    # cavity (a flat box on a diagonal ray can otherwise poke its
    # tangential corners past the true, locally curved inner wall).
    collar = p['collar']
    collar_start = (g['collar_start_xy'][0], g['collar_start_xy'][1], z_center)
    collar_body = geo.oriented_box_prism(collar_start, t3, z3, d3, L, W + 2 * collar['h'], collar['len'])
    collar_body = collar_body & geo.build_inner_cavity_clip_tool(p)
    # port-specific, live-found this pass (no firefly_case.py equivalent
    # needed -- see this module's own docstring for why): the collar sits
    # deep inboard, close to the S1/S3 ear wall-roots' own standoff
    # targets and the S2 boss's own target/arm, none of which existed
    # (S2) or were height-capped against buttons (S1/S3's own cap only
    # bounds the WALL-ROOT, not the standoff riser) when this collar's
    # own clip-to-inner-cavity fix was written. A live `check_interference_
    # pairs` run found a real, small (Power ~1.8mm^3 vs the S2 arm,
    # Home ~0.5mm^3 vs the S1 riser) Top-vs-Button overlap -- the collar's
    # own diagonal-ray tangential corner (the SAME class of overshoot its
    # inner-cavity clip already exists to fix, see above) reaching into
    # one of those columns instead of the true outer wall this time.
    # Reuses the SAME keepout cylinders _clip_of_ear_boss_keepout already
    # builds (protecting those columns FROM a button's own cuts) in the
    # opposite direction: carving the small overlapping corner out of the
    # collar itself, rather than growing yet another hand-tuned margin.
    # Live-confirmed: removes both hits to zero without reopening
    # verify_button_insertion/verify_button_retention (the collar's own
    # retention function depends on its footprint near the RIB, not this
    # far corner).
    collar_body = _clip_of_ear_boss_keepout(collar_body, p, standoffs)
    # Belt-and-suspenders (port-specific, no firefly_case.py equivalent
    # needed): the point-keepout clip above only protects a small radius
    # around each ear/S2-boss ROOT/TARGET, not the full length of the S2
    # arm's own horizontal span between them -- live-found this port, the
    # collar's real overlap sits mid-span (near x=-19, S2's own arm
    # reaches from the west wall to x=0), nowhere near either endpoint
    # the point-keepout protects. Subtracting the ACTUAL pre-button Top
    # solid (`existing_top`, everything built before add_buttons runs --
    # shell, corner blocks, ears, S2 boss) removes any real overlap with
    # already-placed structural material directly, rather than guessing
    # another hand-shaped keepout zone. A no-op wherever the collar was
    # already clear (both buttons, pre-fix: Power ~1.8mm^3 vs the S2 arm,
    # Home ~0.5mm^3 vs the S1 riser -- both zero after this).
    if existing_top is not None:
        # NOTE: growing `existing_top` by bd.offset(amount=0.05) before
        # this subtract (to fully clear a ~0.0009mm^3 residual sliver at
        # one coincident face, Home only -- see docs/hardware/headless-
        # port-parity.md) was tried and reverted: bd.offset on a solid
        # this complex (the whole in-progress Top, every feature already
        # joined) is not robust -- it degenerated to a 2D shape on this
        # exact body (ValueError: "not Part (3D) and Part (2D)"),
        # confirmed on both variants. The plain subtract below leaves
        # that single sub-0.001mm^3 sliver (ordinary boolean-cleanup/
        # tessellation slack at a shared coincident face, the same class
        # this codebase's own NOISE_FLOOR_MM3 already names elsewhere) --
        # accepted, not silently hidden; see gates.py's own button
        # interference gate for the exact accepted threshold.
        collar_body = collar_body - existing_top
    cap_body = cap_body + collar_body

    bodies[name] = cap_body
    return bodies


def add_buttons(bodies, p, standoffs):
    """Port of add_buttons (:3639) -- builds Power then Home, sharing the
    thickened envelope, outer pill envelope, and tab-clip tool between
    them (no MCP-timeout concern under OCC, but no reason to rebuild
    three whole-case solids twice either)."""
    assert p['power_cap']['proud'] == p['home_cap']['proud'], 'shared thickened envelope assumes equal proud amounts'
    thickened_envelope = geo.build_thickened_envelope(p, p['power_cap']['proud'])
    rib_outer_envelope = geo.build_outer_pill_solid(p)

    # wall_clear=0.6 matches add_lip_anchor_reliefs'/add_lug's own "stay
    # clear of the true outer wall" convention; build_inner_cavity_clip_
    # tool's own safety_margin is relative to the inner cavity surface
    # (already `wall` inboard of s_wall), so matching s_wall-0.6 needs
    # safety_margin = wall_clear - p['wall'] (negative -- the tool must
    # be grown outward past the bare inner cavity, not shrunk).
    wall_clear = 0.6
    tab_clip_tool = geo.build_inner_cavity_clip_tool(p, safety_margin=wall_clear - p['wall'])

    # snapshot of Top BEFORE any button touches it -- see add_button's
    # own collar-vs-existing-material comment (port-specific fix).
    existing_top = bodies['Top']

    bodies = add_button(bodies, 'Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap'],
                         p, standoffs, thickened_envelope, rib_outer_envelope, tab_clip_tool,
                         existing_top=existing_top)
    home_bbox = dict(p['switch_home_bbox'])
    home_bbox['z'] = p['switch_power_bbox']['z']  # z not separately specified in SPEC.md; reuse power's
    bodies = add_button(bodies, 'Home Button', home_bbox, p['home_nub_dir'], p['home_cap'],
                         p, standoffs, thickened_envelope, rib_outer_envelope, tab_clip_tool,
                         existing_top=existing_top)
    return bodies


# ---------------------------------------------------------------------------
# Button definitions shared by add_buttons and the gates below (name ->
# (switch_bbox, nub_dir, cap)), so both always agree on which bbox each
# button uses (Home's z reused from Power's -- SPEC.md doesn't give one).
# ---------------------------------------------------------------------------
def _button_defs(p):
    home_bbox = dict(p['switch_home_bbox'])
    home_bbox['z'] = p['switch_power_bbox']['z']
    return [
        ('Power Button', p['switch_power_bbox'], p['power_nub_dir'], p['power_cap']),
        ('Home Button', home_bbox, p['home_nub_dir'], p['home_cap']),
    ]


def find_switch_body(disp, switch_bbox, margin=4.0):
    """Port of find_switch_body (:6555), adapted for a headless STEP
    import with no occurrence-tree parent-component names to filter by
    (see components.py's own module docstring for the same trade-off
    `measure_standoffs` already makes): matches the real 'SWITCH-TS24CA'
    body by nearest bbox-center distance to the given SPEC/PARAMS bbox,
    same identification technique the live probe that measured
    `switch_actuator_reach` used in the first place."""
    x0, x1 = switch_bbox['x'][0] - margin, switch_bbox['x'][1] + margin
    y0, y1 = switch_bbox['y'][0] - margin, switch_bbox['y'][1] + margin
    cx0 = (switch_bbox['x'][0] + switch_bbox['x'][1]) / 2.0
    cy0 = (switch_bbox['y'][0] + switch_bbox['y'][1]) / 2.0
    best, best_d = None, None
    for s in disp.solids():
        bb = s.bounding_box()
        cx = (bb.min.X + bb.max.X) / 2.0
        cy = (bb.min.Y + bb.max.Y) / 2.0
        if not (x0 <= cx <= x1 and y0 <= cy <= y1):
            continue
        d = math.hypot(cx - cx0, cy - cy0)
        if best is None or d < best_d:
            best, best_d = s, d
    return best


def find_outermost_s(body, origin_xy, d2, z, max_s=6.0, step=0.02):
    """Port of find_outermost_s (:6524)."""
    s = max_s
    while s >= 0.0:
        pt = (origin_xy[0] + s * d2[0], origin_xy[1] + s * d2[1], z)
        if geo.probe_point_solid(body, pt):
            return s
        s -= step
    return None


def find_innermost_s(body, origin_xy, d2, z, max_s=8.0, step=0.02):
    """Port of find_innermost_s (:6539)."""
    s = 0.0
    while s <= max_s:
        pt = (origin_xy[0] + s * d2[0], origin_xy[1] + s * d2[1], z)
        if geo.probe_point_solid(body, pt):
            return s
        s += step
    return None
