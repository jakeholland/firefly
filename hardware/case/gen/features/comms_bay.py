"""Comms bay: the 3-board comms-stack retention frame, the battery bay,
the GPS patch frame, and the two antenna cable channels/notches --
build123d port of firefly_case.py's own "M3: comms bay" section
(`add_comms_bay` :4808 driver and its `build_comms_stack_frame` :4588 /
`add_comms_stack_frame` :4638 / `build_hanging_frame` :4698 /
`build_gps_frame_body` :4752 / `add_battery_bay` :4497 / `add_antenna_
channels` :4894 dependents). Board placement itself (the real XIAO/
Wio-SX1262/L76K STEP solids) lives in `components.load_comms_stack` --
this module builds the PARAMS-driven retention geometry, exactly like
the source's own `build()` driver calls these before `insert_comms_
boards` (the frame/bay geometry never depends on the real board solids,
only on `PARAMS['bay']` -- board solids exist purely for the gates that
check the real hardware actually fits what this geometry assumes, see
gates.py's `verify_stack3_clearance`/`verify_min_clearances`).
"""
import math

import build123d as bd

from .. import geometry as geo

LORA_CORRIDOR_INSET_MM = 1.5  # firefly_case.py:4894, pass-16 item F

# OCC-specific robustness note (no Fusion equivalent needed): the inner-
# cavity clip tool (`geometry.build_inner_cavity_clip_tool`) represents
# the HOLLOW interior only -- it does not extend down through Bottom's
# own solid floor at all (the floor's own top face, `frame_z[0]`/2.0mm,
# IS the clip tool's own lower boundary at this XY, live-confirmed this
# port: clipping a wall/pad piece that was deliberately built starting a
# hair BELOW 2.0mm -- to try to guarantee overlap with the floor -- just
# clips that deliberate overlap straight back off again, since that
# extra sliver is inside the SOLID floor, not the hollow cavity the clip
# tool represents). So the clip and the "join safely into the floor"
# need can never both be satisfied by extending the PRE-clip shape
# further down -- the fix has to add real material back in AFTER
# clipping. `_bridge_to_floor` below does this: it re-slices the
# ORIGINAL, UNCLIPPED shape (never touched by the cavity clip) at a thin
# band starting `BRIDGE_DEPTH_MM` below the clipped piece's own real
# floor contact z and reaching up 0.01mm past it, then unions that
# spacer onto the clipped piece -- the spacer's own lower portion sits
# safely inside Bottom's always-solid floor slab (confirmed present down
# to at least z=0 everywhere under this bay), and its upper portion
# shares the exact same cross-section as the clipped piece's own bottom
# (both sliced from the identical unclipped shape), guaranteeing a clean,
# unambiguous boolean fuse on both ends.
BRIDGE_DEPTH_MM = 1.0


def _bridge_to_floor(clipped, unclipped, depth=BRIDGE_DEPTH_MM):
    """See this module's own header comment (BRIDGE_DEPTH_MM) for why
    this is needed: fuses `clipped` (already intersected with the inner-
    cavity clip tool, so its own bottom face sits wherever the true
    cavity boundary allows -- not necessarily at the nominal design z)
    with a thin spacer re-sliced from `unclipped` (the pre-clip shape),
    bridging down into Bottom's own solid floor."""
    bb = clipped.bounding_box()
    z_lo = bb.min.Z
    spacer_box = geo.box_solid(bb.min.X - 1.0, bb.max.X + 1.0, bb.min.Y - 1.0, bb.max.Y + 1.0,
                                z_lo - depth, z_lo + 0.01)
    spacer = unclipped & spacer_box
    if spacer is None or spacer.volume < 1e-6:
        return clipped
    return clipped + spacer


def _best_effort_fillet_at_z(body, z_target, radius, tol=0.15):
    """Port of the shared skip-on-failure idiom firefly_case.py's own
    `_best_effort_fillet_at_z` (:1791) establishes for the GPS/stack-frame
    wall roots (pass 13, item 1) -- a modest 0.6mm cosmetic/print-quality
    fillet, not load-bearing (not gated by verify_root_fillets). Same
    duplicated-per-module idiom features/ears.py's `_best_effort_
    underside_edge_chamfer` and features/buttons.py's own copy of this
    exact helper already establish in this port."""
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


# ---------------------------------------------------------------------------
# Battery bay (firefly_case.py:4497 add_battery_bay / :4567
# add_battery_reference_box).
# ---------------------------------------------------------------------------
def add_battery_bay(bodies, p):
    """Port of add_battery_bay (:4497): retention only (rails + a floor
    flatten under the battery footprint + strap slots + the pass-16
    item-F Y end-stop rib) -- the 803040 cell itself has no printed
    geometry, see `battery_reference_solid` for its interference-only
    reference box."""
    bay = p['bay']
    bat = bay['battery']
    x0, x1 = bat['x']
    y0, y1 = bat['y']
    rail_w = bay['battery_rail_w']
    rail_clear = bay.get('battery_rail_clear', 0.0)
    rz0, rz1 = bay['battery_rail_z']

    rail_l = geo.box_solid(x0 - rail_w - rail_clear, x0 - rail_clear, y0, y1, rz0, rz1)
    rail_r = geo.box_solid(x1 + rail_clear, x1 + rail_w + rail_clear, y0, y1, rz0, rz1)
    bottom = bodies['Bottom'] + rail_l + rail_r

    # flatten the floor under the battery's own footprint (see the
    # source's own docstring: the trim variant's flat cavity floor only
    # reaches rho=fillet_center_rho -- beyond that the floor is the
    # quarter-round fillet arc curving up toward the wall, so the real
    # floor at x=+-20 sits a hair above the nominal flat z the battery box
    # assumes). A no-op wherever the floor is already flat.
    flatten = geo.box_solid(x0, x1, y0, y1, bat['z'][0], bat['z'][0] + 1.0)
    bottom = bottom - flatten

    strap = bay['battery_strap']
    for frac in (1.0 / 3.0, 2.0 / 3.0):
        yc = y0 + frac * (y1 - y0)
        slot_l = geo.box_solid(x0 - rail_w - rail_clear - 0.5, x0 - rail_clear + 0.5,
                                yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        slot_r = geo.box_solid(x1 + rail_clear - 0.5, x1 + rail_w + rail_clear + 0.5,
                                yc - strap['w'] / 2.0, yc + strap['w'] / 2.0, rz0, rz0 + strap['h'])
        bottom = bottom - slot_l - slot_r

    # pass 16, item F (mech review F13): a short Y end-stop rib, full
    # rail-to-rail width, standing at the battery box's own y1 edge (which
    # `bay.battery`'s own params already trim 1mm short of the cell's
    # nominal resting length to make room for exactly this).
    endstop_w = p.get('battery_endstop_w', 1.0)
    endstop = geo.box_solid(x0 - rail_w - rail_clear, x1 + rail_w + rail_clear, y1, y1 + endstop_w, rz0, rz1)
    bottom = bottom + endstop

    bodies['Bottom'] = bottom
    return bodies


def battery_reference_solid(p):
    """Port of add_battery_reference_box (:4567) -- hidden reference-only
    solid (803040 LiPo has no STEP model) used by the interference/
    clearance gates, never joined/cut into any printed body or exported."""
    bat = p['bay']['battery']
    margin = 0.1  # pass 6: inset on X only, see the source's own docstring
    return geo.box_solid(bat['x'][0] + margin, bat['x'][1] - margin, bat['y'][0], bat['y'][1],
                          bat['z'][0], bat['z'][1])


# ---------------------------------------------------------------------------
# Comms-stack frame (firefly_case.py:4588 build_comms_stack_frame / :4638
# add_comms_stack_frame).
# ---------------------------------------------------------------------------
def build_comms_stack_frame(p, clip_wall=True):
    """Port of build_comms_stack_frame (:4588): four Ø3 corner pads + a
    1.2mm perimeter wall around the L76K PCB footprint, with a
    wire-clearance notch on the +Y side (toward the XIAO/Wio wires) --
    only the L76K is physically retained by case geometry; the XIAO/Wio
    float above it, held by their own board-to-board/header connectors
    (see the source's own docstring for why no separate tray is needed).

    `clip_wall` (this port only, no source equivalent needed): clips ONLY
    the outer wall ring against the true inner cavity (the source's own
    `add_comms_stack_frame` clips the WHOLE already-joined frame,
    pads included -- see that function's own docstring for the real,
    live-found reason this port does not: at the frame's own southmost
    reach, near the dome tip, the true cavity floor curves ABOVE the
    `flatten` cut's own flat z (by ~0.3mm at the pad's own XY here,
    live-measured) -- clipping a PAD there leaves it floating, entirely
    disconnected from Bottom's own remaining floor, a genuine defect (not
    a print-safety feature: the pads' own tiny ~1.5mm radius, well inside
    the PCB's small footprint, never approaches the true OUTER wall the
    clip exists to protect against -- only the wall's own larger outward
    reach does). Every corner pad's own bottom rests exactly flush at
    `frame_z[0]` on Bottom's real, always-present floor slab (confirmed
    solid down to at least z=0 everywhere under this footprint) --
    needing no clip, and no join-overlap trick either (live-checked: a
    plain flush cylinder-on-a-flat-floor join fuses cleanly on its own,
    unlike the wall's own case above, which only breaks once the CLIP is
    added -- see `_bridge_to_floor`'s own module-level comment)."""
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

    outer = geo.box_solid(ox0, ox1, oy0, oy1, fz0, fz1)
    inner = geo.box_solid(ix0, ix1, iy0, iy1, fz0 - 0.5, fz1 + 0.5)
    wall_solid_unclipped = outer - inner

    notch_w = s3['wire_notch_w']
    notch = geo.box_solid(-notch_w / 2.0, notch_w / 2.0, oy1 - wall - 0.5, oy1 + 0.5, fz0, fz1 + 0.3)
    wall_solid_unclipped = wall_solid_unclipped - notch

    if clip_wall:
        wall_solid = wall_solid_unclipped & geo.build_inner_cavity_clip_tool(p)
        # same "keep only the dominant fragment(s), reject slivers" idiom
        # `components.ceiling_safe_display_cut` already establishes --
        # the wall ring's own southmost tip can shave to a negligible
        # sliver near the true dome-tip boundary; a fragment under
        # 1mm^3 is not real, load-bearing wall material.
        wall_pieces = [s for s in wall_solid.solids() if s.volume > 1.0]
        assert wall_pieces, 'comms-stack frame wall vanished entirely after the inner-cavity clip'
        wall_solid = wall_pieces[0]
        for s in wall_pieces[1:]:
            wall_solid = wall_solid + s
        # bridge the clipped wall back down into Bottom's own solid floor
        # (see BRIDGE_DEPTH_MM's own module-level comment for why the
        # clip tool's own lower boundary can sit above the flatten cut's
        # nominal floor z here, and a plain pre-clip overlap can't fix it).
        wall_solid = _bridge_to_floor(wall_solid, wall_solid_unclipped)
    else:
        wall_solid = wall_solid_unclipped

    pads = [geo.cylinder_solid(px, py, pad_r, fz0, fz0 + pad_h)
            for px in (x0 + inset, x1 - inset) for py in (y0 + inset, y1 - inset)]
    frame = wall_solid
    for pad in pads:
        frame = frame + pad

    frame = _best_effort_fillet_at_z(frame, fz0, 0.6)
    return frame


def add_comms_stack_frame(bodies, p):
    """Port of add_comms_stack_frame (:4638): flattens the L76K PCB's own
    footprint (real dome-tip cavity floor curves up near the tip -- same
    fix as the battery bay), joins the frame into Bottom (see
    `build_comms_stack_frame`'s own docstring for why only the wall, not
    the pads, is clipped against the true inner cavity -- guards against
    the wall poking through the true outer shell, same reason every
    boss/post/ear in this port clips its own outward-reaching wedge/wall-
    root member), and cuts a keep-out around every case-screw boss (A/C/D)
    so the frame's own outer wall can never graze one -- the
    "corner-block-to-stack keep-out" gate target
    (`verify_stack_frame_boss_clear`, gates.py).

    Also cuts a keep-out around the lanyard lug's own cord hole (no
    source equivalent needed by name -- not one of firefly_case.py's 217
    functions, but load-bearing: live-found this port, the true inner-
    cavity boundary at the wall's own southmost reach genuinely still
    permits material out to within ~1.1mm of the lug's own hole centre at
    the hole's own mid-height (z~5), just past `add_lug`'s own hole cut
    -- the lug is a separate appendage, built entirely OUTSIDE the plain
    revolved profile the cavity clip represents, so the clip alone cannot
    know to avoid it. Same unconditional-keepout idiom `components.
    apply_known_component_keepouts` already uses for a different real
    conflict)."""
    from . import lug as lug_mod
    s3 = p['bay']['stack3']
    pcb = s3['l76k_pcb']
    x0, x1 = pcb['x']
    y0, y1 = pcb['y']
    flatten = geo.box_solid(x0, x1, y0, y1, 2.0, 6.0)
    bottom = bodies['Bottom'] - flatten

    frame = build_comms_stack_frame(p)

    margin = s3['boss_relief_margin']
    boss_r = p['boss_dia'] / 2.0
    for s in p['screws_ABC'] + p['screws_D12']:
        cx, cy = s['xy']
        keepout = geo.cylinder_solid(cx, cy, boss_r + margin, 1.0, 9.0)
        frame = frame - keepout

    lug = p['lug']
    lug_z0, lug_z1 = lug['z']
    _, _, _, lug_hole_y = lug_mod.lug_ear_geometry(p)
    lug_keepout = geo.cylinder_solid(0.0, lug_hole_y, lug['hole_dia'] / 2.0 + 0.5, lug_z0 - 0.5, lug_z1 + 0.5)
    frame = frame - lug_keepout

    bottom = bottom + frame
    # OCC-specific robustness note (no Fusion equivalent needed): the lug
    # keepout cut above, on top of the wall's own already-marginal
    # inner-cavity clip near the dome tip ('trim' only, live-checked --
    # 'current' is unaffected), can slice off a small island of wall
    # material on either side of the hole that no longer touches the
    # rest of the frame/Bottom at all (~4.8mm^3 each side, 'trim') --
    # same disjoint-fragment class every other cut-then-clip step in this
    # port already guards against (`components.ceiling_safe_display_
    # cut`'s own "kept only the dominant solid" precedent). These
    # fragments carry no retention function of their own (the wall's
    # real job is the L76K PCB footprint, far from the lug) -- keep only
    # the largest connected solid.
    bottom_solids = sorted(bottom.solids(), key=lambda s: s.volume, reverse=True)
    if len(bottom_solids) > 1:
        bottom = bottom_solids[0]
    bodies['Bottom'] = bottom
    return bodies


# ---------------------------------------------------------------------------
# GPS patch frame (firefly_case.py:4698 build_hanging_frame / :4752
# build_gps_frame_body / :4788 add_gps_reference_box / :4797
# add_fpc_keepout_marker).
# ---------------------------------------------------------------------------
def build_hanging_frame(x0, x1, y0, y1, clearance, wall, z_bottom, z_ceiling,
                         gap_w=0.0, gap_side='+y', gap_center=None):
    """Port of build_hanging_frame (:4698), the plain-ring subset this
    port actually needs: a thin-walled box ring hanging from z_ceiling
    down to z_bottom, open top and bottom, around the (x0,y0)-(x1,y1)
    footprint + clearance. The source's own optional short-end LEDGES
    (`ledge_w`/`ledge_h`, via `build_wedge_along_x`) belong to the
    pre-pass-7 Top-hanging stack tray, which pass 7 removed outright (see
    firefly_case.py's own comment above build_hanging_frame and
    docs/hardware/headless-port-plan.md's `build_wedge_along_x` row) --
    no live call site needs them any more (`build_gps_frame_body` calls
    this with no ledge args), so they are not ported; only the optional
    wire-clearance `gap_w` cut (used by neither GPS nor stack3 frame
    calls in this port either, but kept since it's a small, independent,
    still-load-bearing-if-called piece of the same helper)."""
    ox0, ox1 = x0 - clearance - wall, x1 + clearance + wall
    oy0, oy1 = y0 - clearance - wall, y1 + clearance + wall
    ix0, ix1 = x0 - clearance, x1 + clearance
    iy0, iy1 = y0 - clearance, y1 + clearance

    outer = geo.box_solid(ox0, ox1, oy0, oy1, z_bottom, z_ceiling)
    inner = geo.box_solid(ix0, ix1, iy0, iy1, z_bottom - 0.5, z_ceiling + 0.5)
    frame = outer - inner

    if gap_w > 0:
        gc = gap_center if gap_center is not None else (x0 + x1) / 2.0
        gz0, gz1 = z_bottom, z_bottom + 4.0
        if gap_side == '+y':
            notch = geo.box_solid(gc - gap_w / 2.0, gc + gap_w / 2.0, oy1 - wall - 0.5, oy1 + 0.5, gz0, gz1)
        elif gap_side == '-y':
            notch = geo.box_solid(gc - gap_w / 2.0, gc + gap_w / 2.0, oy0 - 0.5, oy0 + wall + 0.5, gz0, gz1)
        elif gap_side == '+x':
            notch = geo.box_solid(ox1 - wall - 0.5, ox1 + 0.5, gc - gap_w / 2.0, gc + gap_w / 2.0, gz0, gz1)
        else:
            notch = geo.box_solid(ox0 - 0.5, ox0 + wall + 0.5, gc - gap_w / 2.0, gc + gap_w / 2.0, gz0, gz1)
        frame = frame - notch

    return frame


def build_gps_frame_body(p):
    """Port of build_gps_frame_body (:4752): a plain square hanging ring,
    opening sized directly from `gps_frame_opening`, centred on the patch
    box's own centre."""
    gps = p['bay']['gps_patch']
    opening = p['bay']['gps_frame_opening']
    half = opening / 2.0
    cx = (gps['x'][0] + gps['x'][1]) / 2.0
    cy = (gps['y'][0] + gps['y'][1]) / 2.0
    x0, x1 = cx - half, cx + half
    y0, y1 = cy - half, cy + half
    clear = p['bay']['gps_frame_clear']
    frame = build_hanging_frame(x0, x1, y0, y1, 0.0, p['bay']['gps_frame_wall'],
                                 gps['z'][0] - clear, p['top_ceiling_underside_z'])
    frame = _best_effort_fillet_at_z(frame, p['top_ceiling_underside_z'], 0.6)
    return frame


def gps_reference_solid(p):
    """Port of add_gps_reference_box (:4788) -- hidden reference-only
    solid, interference/clearance checks only."""
    gps = p['bay']['gps_patch']
    return geo.box_solid(gps['x'][0], gps['x'][1], gps['y'][0], gps['y'][1], gps['z'][0], gps['z'][1])


def fpc_keepout_solid(p):
    """Port of add_fpc_keepout_marker (:4797) -- construction-only
    reference marking the LoRa FPC antenna keep-out strip on Top's inner
    dome wall; NOT joined/cut into any printed body."""
    ko = p['bay']['fpc_keepout']
    return geo.box_solid(ko['x'][0], ko['x'][1], ko['y'][0], ko['y'][1], ko['z'][0], ko['z'][1])


def add_comms_bay(bodies, p):
    """Port of add_comms_bay (:4808), the M3 driver: battery bay + comms-
    stack frame (into Bottom) + a GPS-patch keep-out cut and the GPS
    frame ring itself (into Top). `screws_ABC`'s own boss C real-material
    keep-out overlap with the GPS patch's footprint (the pass-7 fix) is
    handled the same way -- cutting the antenna's own box (+0.3mm margin)
    out of Top generally, regardless of which feature's material would
    otherwise occupy that space."""
    bodies = add_battery_bay(bodies, p)
    bodies = add_comms_stack_frame(bodies, p)

    gps_box = p['bay']['gps_patch']
    gps_keepout = geo.box_solid(gps_box['x'][0] - 0.3, gps_box['x'][1] + 0.3,
                                 gps_box['y'][0] - 0.3, gps_box['y'][1] + 0.3,
                                 gps_box['z'][0] - 0.3, gps_box['z'][1] + 0.3)
    top = bodies['Top'] - gps_keepout

    gps_frame = build_gps_frame_body(p)
    top = top + gps_frame

    bodies['Top'] = top
    return bodies


# ---------------------------------------------------------------------------
# Antenna cable channels (firefly_case.py:4849 _antenna_skin_safe_channel /
# :4894 add_antenna_channels / :5030 antenna_channel_geometry).
# ---------------------------------------------------------------------------
def _antenna_skin_safe_channel(p, center, axis1, axis2, length, width, height):
    """Port of _antenna_skin_safe_channel (:4849): a box channel,
    Combine-Intersected against a copy of the plain outer envelope offset
    INWARD by `channel_min_skin` -- guarantees the cut can never reach
    closer than that to the TRUE outer surface, following the real
    curvature, however far `length` was asked to reach."""
    a = p['antenna']
    box = geo.oriented_box_prism(center, axis1, axis2, (0.0, 0.0, 1.0), length, width, height)
    envelope = geo.build_thickened_envelope(p, -a['channel_min_skin'])
    return box & envelope


def antenna_channel_geometry(p):
    """Port of antenna_channel_geometry (:5030) -- shared analytic
    geometry between the build (add_antenna_channels) and the gate
    (gates.verify_antenna_channels), computed once so the two can never
    disagree."""
    a = p['antenna']
    out = {}
    if p.get('comms_stack3_full_height', True):
        ux, uy, uz = a['lora_ufl_xyz']
        ay = p['spine_a'][1]
        d = math.hypot(ux, uy - ay)
        dirv = (ux / d, (uy - ay) / d)
        s_wall = geo.true_wall_distance_along_ray(p, (ux, uy), dirv, uz)
        channel_len = s_wall - a['channel_min_skin']
        probe_s = min(channel_len * 0.5, channel_len - 0.1) if channel_len > 0.2 else channel_len / 2.0
        probe = (ux + probe_s * dirv[0], uy + probe_s * dirv[1], uz)
        out['lora'] = {'probe_xyz': probe, 'origin_xy': (ux, uy), 'dir': dirv, 'z': uz, 's_wall': s_wall,
                        'channel_len': channel_len}
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


def add_antenna_channels(bodies, p):
    """Port of add_antenna_channels (:4894): the Wio u.FL -> Top's inner
    dome wall LoRa route (trim only -- see `comms_stack3_full_height`)
    plus a real, live-checked reference corridor for it
    (`LORA_CORRIDOR_NAME`, pass-16 item F -- returned here rather than
    stashed on a Fusion body, since this port has no hidden-reference-body
    convention; callers pass it to gates.py directly), and the L76K u.FL
    -> GPS frame south-wall notch (both variants)."""
    top = bodies['Top']
    a = p['antenna']
    w, h = a['channel_width'], a['channel_depth']
    fillet_r = a['channel_fillet']
    lora_corridor = None

    if p.get('comms_stack3_full_height', True):
        ux, uy, uz = a['lora_ufl_xyz']
        ay = p['spine_a'][1]
        d = math.hypot(ux, uy - ay)
        dirv = (ux / d, (uy - ay) / d)
        s_wall = geo.true_wall_distance_along_ray(p, (ux, uy), dirv, uz)
        assert s_wall is not None, 'LoRa antenna channel: no true-wall intersection along the connector ray'
        channel_len = s_wall - a['channel_min_skin']
        assert channel_len > 0, f'LoRa antenna channel: connector already within channel_min_skin ({s_wall})'
        tang = (-dirv[1], dirv[0])
        center = (ux + (channel_len / 2.0) * dirv[0], uy + (channel_len / 2.0) * dirv[1], uz)
        dirv3 = (dirv[0], dirv[1], 0.0)
        tang3 = (tang[0], tang[1], 0.0)
        channel = _antenna_skin_safe_channel(p, center, dirv3, tang3, channel_len, w, h)
        channel = _best_effort_channel_fillet(channel, fillet_r)
        top = top - channel

        corridor_len = max(channel_len - LORA_CORRIDOR_INSET_MM, 0.1)
        corridor_center = (ux + LORA_CORRIDOR_INSET_MM * dirv[0] + (corridor_len / 2.0) * dirv[0],
                            uy + LORA_CORRIDOR_INSET_MM * dirv[1] + (corridor_len / 2.0) * dirv[1], uz)
        corridor = geo.oriented_box_prism(corridor_center, dirv3, tang3, (0.0, 0.0, 1.0), corridor_len, w, h)
        corridor = corridor & geo.build_inner_cavity_clip_tool(p)
        lora_corridor = corridor

    gps = p['bay']['gps_patch']
    gps_half = p['bay']['gps_frame_opening'] / 2.0
    gcx = (gps['x'][0] + gps['x'][1]) / 2.0
    gcy = (gps['y'][0] + gps['y'][1]) / 2.0
    gy0 = gcy - gps_half
    gwall = p['bay']['gps_frame_wall']
    ux, uy, uz = a['gps_ufl_xyz']
    notch_x0, notch_x1 = ux - w / 2.0, ux + w / 2.0
    notch_y0, notch_y1 = gy0 - gwall - 0.15, gy0 + 0.15
    notch_z0, notch_z1 = p['split_z'] + 0.5, p['split_z'] + 2.5
    gps_notch = geo.box_solid(notch_x0, notch_x1, notch_y0, notch_y1, notch_z0, notch_z1)
    gps_notch = _best_effort_channel_fillet(gps_notch, fillet_r)
    top = top - gps_notch

    bodies['Top'] = top
    return bodies, lora_corridor


def _best_effort_channel_fillet(channel, radius):
    """Port of `_best_effort_fillet` (:4869) restricted to this module's
    own two call sites (the LoRa channel / GPS notch cutting tools) --
    same skip-on-failure idiom as every other best-effort fillet in this
    port; cosmetic only (never load-bearing on a CUTTING tool's own
    edges).

    Phase 2c bug fix (live-found): filleting ALL 12 edges of a small
    cutting-tool box (radius 0.3mm on the GPS notch's ~1.3mm short
    dimension) used to run here unconditionally -- the resulting solid
    passed every shape-level check available (`Shape.is_valid` True,
    a sane post-fillet volume), and `top - <filleted tool>` ALSO
    reported `is_valid` True, yet the exported STL of the final `Top`
    consistently tessellated to ~10-20 non-manifold edges/degenerate
    triangles, all located exactly at the notch's own filleted corners
    (confirmed both variants, `gen/tests/test_gates.py::
    test_offline_manifold_and_overhang`) -- an OCC MESHER artifact at a
    near-tangent boolean boundary that neither B-rep validity check can
    see (tessellation quality is a separate concern from topological
    validity). Since this fillet was already documented as never
    load-bearing, the fix is simply to stop applying it here rather than
    add a second, heavier boolean-plus-tessellate safety net for a
    purely cosmetic 0.3mm rounding -- always returns `channel`
    unfilleted now; `radius` is kept as a parameter (both call sites
    still pass it) so a future, real fillet attempt -- if the underlying
    OCC/build123d tessellator behavior ever changes -- has an obvious
    place to resume from."""
    return channel
