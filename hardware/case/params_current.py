"""Firefly case parameters — 'current' variant (60 x 110 x 25, matches Firefly V2 v15/v16)."""

PARAMS = {
    'variant': 'current',
    # NOTE: 'hat' L76K mode was dropped 2026-09-04 (coordinator's bay
    # redesign) -- the generator now always builds the wired layout.

    # --- envelope / spine ---
    'spine_a': (0.0, 0.0),
    'spine_b': (0.0, 50.0),
    'outer_radius': 30.0,
    'wall': 2.0,
    'split_z': 10.0,
    'top_z': 25.0,
    'bottom_z': 0.0,

    # --- outer edge profile (shared top/bottom, mirrored) ---
    'flat_rho': 24.14,          # flat bed extends to this radius
    'shoulder_angle_deg': 45.0,
    'fillet_r': 10.0,           # outer fillet radius
    'fillet_center_rho': 20.0,  # fillet center offset from spine
    'top_fillet_center_z': 15.0,
    'bottom_fillet_center_z': 10.0,
    # derived tangent point: rho=27.07, dz=7.07 from fillet center (see SPEC)

    # --- inner cavity ---
    'inner_fillet_r': 8.0,      # nominal (result of 2mm shell offset of R10)

    # --- window ---
    'window_dia': 45.30,
    'window_center': (0.0, 50.0),
    'window_z_bottom': 22.4,
    'window_chamfer': 0.5,
    'window_chamfer_deg': 45.0,

    # --- display module (Waveshare ESP32-S3-Touch-LCD-1.46) ---
    'display_doc_name': 'ESP32-S3-Touch-LCD-1_46',
    'display_glass_dia': 44.79,
    'display_bbox': {'x': (-22.39, 22.39), 'y': (27.6, 73.13), 'z': (20.3, 25.0)},
    'display_pcb': {'xy': (39.2, 41.4), 'z': (17.59, 18.81)},
    'display_underside_z_min': 12.04,
    'display_header': {'x': (11.5, 17.0), 'y': (43.7, 56.1)},
    'top_ceiling_underside_z': 23.0,
    'display_z_offset': 0.0,  # pass 7: trim overrides this to +3 (case grows 25->28mm)
    # 2026-09-07 pass 7 (item 2), real physical conflict found via an
    # actual analyzeInterference run: the real 3-board B2B stack is
    # 18mm tall (measured, L76K underside to SX1262-module top); trim's
    # case grows 25->28mm specifically to fit it (top_ceiling_underside_z
    # 23->26), but Jake's instruction keeps 'current' at height 25
    # (top_ceiling_underside_z stays 23) 'for the probe comparison' --
    # meaning 'current' genuinely CANNOT fit the stack under its
    # unchanged ceiling (confirmed: Wio's own body physically overlaps
    # Top by ~6mm3 at the stack's real top, z 22.13-22.94, vs a nominal
    # 23.0 ceiling that's locally even lower near the dome-tip curve).
    # This is not a local clip-away-a-corner fix like the boss/GPS
    # conflicts elsewhere in this pass -- there is no room, period.
    # 'current' exists solely for the M1 outer-shell probe-table
    # comparison against the original 60x110x25 reference (see
    # README/SPEC.md), not as a variant meant to carry real electronics,
    # so insert_comms_boards inserts ONLY the L76K for 'current' (skips
    # XIAO/Wio) when this is False -- trim overrides it True.
    'comms_stack3_full_height': False,
    'fpc_relief': {'x': (-6.2, 7.02), 'y': (71.44, 73.12), 'z': (21.83, 22.93)},

    # --- case screws (Bottom -> Top), M2 socket head ---
    'screw_head_dia': 3.8,
    'screw_head_h': 2.0,
    # 2026-09-07 pass 7: boss B (0,-24/-23) moved off the comms stack --
    # it sat squarely inside the L76K PCB's own footprint even before the
    # 3-board stack redesign (see the pre-pass-7 README known-limitations
    # entry) and now the stack occupies that whole dome-tip footprint.
    # Replaced with a symmetric pair B1/B2 straddling the stack's centre
    # line at (+-12.5, -15) -- rho=19.5 from spine_a, comfortably inside
    # the trim flat bed (flat_rho=22.14) so their Ø4.5 counterbores land
    # fully on flat material, not the shoulder curve. These are ABSOLUTE
    # mm positions, deliberately the SAME for both variants (like the bay
    # layout) -- current's wider shell just has more margin around them.
    #
    # 2026-09-08 pass 9 (finding 2, first-print holes/bumps at A/C): A and
    # C used to sit near y=25 (rho~23-24 from spine_a) at x scaled off
    # outer_radius -- CONFIRMED as the root cause of holes in Bottom's
    # side wall and bumps on Top's shoulder at both variants: at that y,
    # the case's flat bed only extends to `flat_rho` (22.14 trim / 24.14
    # current), so a boss whose OD (boss_dia=6, +1mm margin) reaches
    # beyond flat_rho sits partly ON the 45-degree shoulder rather than
    # inside the flat face -- the boss body, its Ø4.5 counterbore (cut
    # from z=0, i.e. exactly at the shoulder's narrowest point), and the
    # Top-side M2 pilot near the ceiling all breach the true outer surface
    # there. The fix ("every boss must fit fully inside flat_rho, with
    # margin") can't just slide A/C inboard at y=25, though: that y is
    # deep inside the battery bay's footprint (`bay.battery`: x -20..20,
    # y 2..32, z 2..10 -- exactly the bottom boss's own z-span) AND the
    # GPS frame's (x -2.8..22.2, y 2..27, z 10.5..18.8 -- overlapping the
    # Top-side boss's z-span too) AND the display module's own rectangular
    # PCB bbox (x +-22.39, y 27.6..73.13) covers everything further up --
    # there is no (x, y) with |x| small enough to clear flat_rho that
    # isn't already claimed by one of those three, confirmed by direct
    # computation (battery alone leaves <1mm of usable width outside its
    # rails at flat_rho, for either variant). So, per Jake's own fallback
    # ("shift toward the lanyard end where the cavity is free"), A/C move
    # to the dome-tip region alongside B1/B2 instead -- ABSOLUTE mm
    # positions, same for both variants, same reasoning as B1/B2/D. 2D
    # distance from spine_a is 17.44mm (well inside flat_rho - boss_dia/2
    # - 1.0mm margin = 18.14mm for trim, the tighter variant, so current
    # inherits even more margin) and >5mm clear of the L76K stack's own
    # frame footprint (x <= ~10.4) and of B1/B2 themselves (>=7.6mm
    # center-to-center) -- add_comms_stack_frame's existing per-boss
    # keep-out cut (iterates `screws_ABC + [screw_D]` generically) applies
    # to A/C automatically, same as it already does for B1/B2/D.
    'screws_ABC': [
        {'name': 'A', 'xy': (-15.5, -8.0)},
        {'name': 'B1', 'xy': (-12.5, -15.0)},
        {'name': 'B2', 'xy': (12.5, -15.0)},
        {'name': 'C', 'xy': (15.5, -8.0)},
    ],
    'boss_dia': 6.0,
    'screw_hole_dia': 2.4,
    'counterbore_ABC_dia': 4.5,
    'counterbore_ABC_h': 2.2,
    'top_pilot_dia': 1.62,
    'top_pilot_z': (10.0, 19.1),   # -> M2x12

    # 2026-09-06 pass 6: moved from (0, 65) to (0, 60), a couple mm
    # further from the Screen Plate's own header cutout (y 42.7..57.1)
    # for margin, while staying within its outline (y 28.8..69.6) --
    # incidental to the actual fix for boss D never joining into Bottom
    # (BOSS_CORE_R, above), which turned out to be the real cause (this
    # position's local floor genuinely starts a bit further from center
    # than boss B/A/C's did, but well within the wider core's reach).
    'screw_D': {'name': 'D', 'xy': (0.0, 60.0)},
    'counterbore_D_h': 4.0,
    'plate_post_D_z': (10.0, 13.1),  # post on Screen Plate, hole Ø1.62 -> M2x10
    'usb_shell_z': 14.35,             # screw tip must stay <= 14.1

    # --- alignment lip / anchor (on Top) ---
    'lip_r': (26.95, 27.75),
    'lip_z': (9.2, 10.0),
    'anchor_r': (26.95, 28.40),
    'anchor_z': (10.0, 11.0),
    # 2026-09-06 pass 6: widened generously past 6.6mm -- measured (see
    # firefly_case.py's add_lip_anchor_reliefs) to leave a thin wedge-
    # shaped sliver of ring material at screw B, whose relief circle just
    # barely failed to clear the anchor ring's own outer radius. This only
    # needs to clear each boss's own footprint (boss_dia + a small
    # margin), not reach the ring's outer edge -- 10.0mm gives a
    # comfortable margin over boss_dia (6.0) at every screw position in
    # both variants.
    'boss_relief_dia': 10.0,
    # widened to 14.0mm (matching the pass-6 ear rebuild's width) plus
    # margin; y-range covers the lip/anchor ring band near spine_a in both
    # variants (see params_trim.py's scaling).
    'lug_relief_box': {'x': (-8.5, 8.5), 'y': (-29.5, -24.5)},

    # --- screen plate ---
    'plate_z': (13.1, 14.1),
    'plate_outline': {'x': (-26.63, 22.89), 'y': (28.8, 69.6)},
    'plate_header_cutout': {'x': (11.5 - 1.0, 17.0 + 1.0), 'y': (43.7 - 1.0, 56.1 + 1.0)},
    'plate_hole_dia': 2.4,
    'plate_pad_dia': 6.0,
    # 2026-09-06 pass 6: P2 moved from (-17.45, 31.8) to (-13.0, 31.8) --
    # discovered empirically (a real, if latent, 'Top x Power Button'
    # interference: pass 6's clipped_pillar_with_reach fix finally gives
    # this post real material for the first time -- see
    # verify_posts_and_bosses -- and its old position was inside the
    # Power Button's own rib/collar footprint the whole time, just never
    # visible because the post never actually joined into Top before).
    # 3.55mm further inboard (away from the -x wall) clears it with
    # margin; verified by removing the post entirely and confirming the
    # residual interference (a separate, smaller ~0.68mm3 tab-area issue,
    # fixed separately -- see add_button's skin_margin) is unchanged, so
    # this move addresses only the post-specific portion.
    'top_posts': {
        'P1': (-23.63, 58.84),
        'P2': (-13.0, 31.8),
        'P3': (17.0, 32.0),
        'P4': (19.89, 65.47),
    },
    'board_standoffs': {
        'S1': (-12.0, 65.0),
        'S2': (0.04, 32.22),
        'S3': (11.6, 65.46),
    },
    'top_post_dia': 4.0,
    'top_post_z': (14.1, 23.0),
    'top_post_pilot_dia': 1.62,
    'top_post_pilot_z': (14.1, 20.6),  # -> M2x6

    # --- buttons ---
    'switch_power_bbox': {'x': (-17.46, -11.83), 'y': (36.82, 42.67), 'z': (15.79, 18.01)},
    'switch_home_bbox': {'x': (-17.87, -12.24), 'y': (57.62, 63.46), 'z': (999, 999)},  # z not given; use power's span
    'nub_protrusion': 1.2,
    'power_nub_dir': (-0.8, -0.6),
    'home_nub_dir': (-0.84, 0.54),
    'power_cap': {'stadium': (10.0, 5.8), 'z': (13.8, 19.6), 'proud': 0.45, 'y_center': 29.7},
    'home_cap': {'stadium': (8.94, 6.6), 'z': (13.4, 20.0), 'proud': 0.45},
    'plunger_tip_gap': 0.02,   # gap at FULL PRESS (collar bottomed on the rib), not at rest
    'nub_pocket': {'xy': (1.3, 1.6), 'depth': 0.8},
    'tab': {'w': 2.9, 'h': 2.0, 'gap': 0.60},
    # how far short of the true outer skin the tab-hole cut stops --
    # shared by add_button (the cut) and verify_skin_intact (the probe
    # target), 2026-09-07 pass 7, so they can never drift out of sync.
    'tab_hole_skin_margin': 2.0,
    'cap_clearance': 0.25,  # per-side clearance between the cap head and its wall hole -- tune here for a fit-test coupon re-print
    # plunger guide rib + inward stop collar (added 2026-09-04 per Jake's
    # print-test feedback: caps bound, and a hard press loaded the switch's
    # solder joints with nothing else to stop inward travel).
    'rib_thickness': 1.6,        # along the plunger travel axis
    'rib_slot_clearance': 0.25,  # per side, around the plunger cross-section
    'rib_inboard_offset': 6.0,   # rib's outboard face, mm inboard of the outer wall (5-7mm range)
    'plunger_travel': 0.62,      # rest-to-bottomed inward travel before the collar hits the rib
    'collar': {'h': 0.8, 'len': 1.0},  # h = extra flange height beyond the plunger cross-section (Z); len = along travel axis
    # wall_x (the -x outer wall, where the buttons live) is derived at build
    # time as -PARAMS['outer_radius'] -- not duplicated here so it can never
    # drift out of sync between variants.

    # --- USB-C tunnel ---
    'usb_receptacle': {'x': (-4.48, 4.48), 'z': (14.35, 18.43), 'y': 73.0},
    'usb_tunnel_stadium': (13.0, 7.0),
    'usb_tunnel_center_z': 16.4,
    'usb_tunnel_y_start': 73.5,
    'usb_liner_thickness': 1.6,
    'usb_liner_outer_stadium': (16.2, 10.2),

    # --- lanyard lug/ear (Bottom) ---
    # z = (0.0, 10.0) (pass-5 printability fix, 2026-09-05): the lug's
    # underside used to sit at z=3.0 -- a horizontal overhang floating
    # 3mm above the bed with nothing under it when Bottom prints face-down
    # on z=0. z=0 puts the underside flush on the bed for the whole ear.
    # 2026-09-06 pass 6: rebuilt as an integrated ear (see add_lug /
    # lug_ear_geometry) after the previous box+cylinder tab was found to
    # intrude into the hollow cavity (its inner end crossed the inner
    # wall -- reads as a floating cylinder from inside, next to the
    # L76K). 'width'/'protrusion'/'hole_from_tip' replace the old
    # x/y_root/y_tip/tip_r/hole_xy -- the ear's actual Y position is now
    # derived from the shell's TRUE curved surface (true_wall_distance_
    # along_ray), not a hand-picked constant, so it can never re-drift
    # into the cavity or float outside the true wall regardless of variant.
    'lug': {
        'width': 14.0, 'protrusion': 6.0, 'z': (0.0, 10.0),
        'hole_dia': 4.0, 'hole_from_tip': 3.5,
        'fillet_r': 3.0, 'hole_chamfer': 0.6,
    },

    # --- logos (debossed 0.4mm) ---
    'logo_deboss_depth': 0.4,
    'flare_center': (0.0, 2.0),
    'flare': {
        'center_dia': 3.0, 'long_ray': 7.0, 'short_ray': 4.5,
        'bar_start_r': 2.2, 'bar_w': (1.4, 0.5),
    },
    'wordmark_center': (0.0, 25.0),
    'wordmark_width': 30.0,

    # --- comms bay (current variant) ---
    # Comms bay v2 (2026-09-04 redesign, replaces the earlier flat-cross-
    # section layout): the lower cavity is a half-disc at the spine_a end
    # (radius = outer_radius - wall = 26mm for trim / 28mm for current)
    # plus the straight band y 0..27. These are ABSOLUTE mm positions --
    # deliberately the SAME for both variants (the hardware doesn't change
    # size, and trim's R=26 is the tighter constraint; current's R=28 just
    # has 2mm more slack everywhere). See params_trim.py's docstring note
    # and README's Known Limitations for why this isn't re-scaled per
    # variant.
    'bay': {
        'cavity_r': 26.0,  # nominal half-disc radius this layout was fit to (trim); current has 2mm more
        # battery (2026-09-07 pass 7): shifted +6mm in Y (was -4..26) to
        # y 2..32, off the comms stack's new dome-tip footprint (the stack
        # replaces the old bay area the battery used to abut). Same 30mm
        # span, x/z unchanged -- the display board's underside parts don't
        # start until z>=12 above y=28, so the battery may extend under
        # them with no conflict (nothing else occupies z 2..10 there).
        'battery': {'xyz': (8.0, 40.0, 30.0), 'x': (-20.0, 20.0), 'y': (2.0, 32.0), 'z': (2.0, 10.0)},
        'battery_rail_w': 1.2, 'battery_rail_z': (2.0, 6.0), 'battery_rail_clear': 0.3,
        'battery_strap': {'w': 6.0, 'h': 1.5},  # slot through the rails, not the floor

        # --- 2026-09-07 pass 7: the 3-board comms stack (L76K + XIAO +
        # Wio), lying flat in the lanyard-end (-y) dome, ON THE BOTTOM.
        # Supersedes the old 'l76k_wired' floor frame AND the old
        # Top-hanging 'stack'/tray (XIAO+Wio used to sit separately, in
        # the straight band on their own wedge-supported tray) -- the real
        # hardware kit is a direct vertical stack (board-to-board + a
        # soldered connection to the L76K below), not two independent
        # placements. Per Jake's measurement: stack height (L76K underside
        # to SX1262-module top) is 18mm, requiring the case height bump
        # (see PARAMS['top_z'] / z_top in params_trim.py) to fit under the
        # ceiling. All ABSOLUTE mm, same for both variants (like the old
        # bay layout) -- current's wider shell just has more margin.
        'stack3': {
            # L76K PCB: long axis along Y, XIAO's USB-C end toward +Y.
            'l76k_pcb': {'x': (-8.9, 8.9), 'y': (-24.0, -1.5)},
            'l76k_bottom_z': 4.0,          # PCB bottom, resting on the pads
            'pad_dia': 3.0, 'pad_h': 2.0,  # 4x corner pads, z 2.0..4.0
            'pad_inset': 1.8,              # pad center inset from each PCB corner
            'frame_wall': 1.2, 'frame_clear': 0.3, 'frame_z': (2.0, 8.0),
            'wire_notch_w': 6.0,           # +Y side (toward the XIAO/Wio wires)
            'xiao_gap': 3.8,               # XIAO pcb_bottom = L76K pcb_top + this
            'wio_gap': 1.5,                # Wio pcb_bottom = XIAO pcb_top + this
            'stack_top_z_nominal': 22.0,   # 4.0 + 18mm measured stack height
            'ceiling_clear_min': 0.8,      # min gap, stack top -> Top inner surface
            'boss_relief_margin': 1.0,     # keep-out margin cut into the frame around each case-screw boss
        },

        'gps_patch': {'xyz': (25.0, 25.0, 8.3), 'x': (-2.8, 22.2), 'y': (2.0, 27.0), 'z': (10.5, 18.8)},
        'gps_frame_wall': 1.0, 'gps_frame_clear': 0.3,
        # pass-5 fix: the GPS frame is a plain hanging ring with a clear
        # 25.5x25.5 opening CENTRED on the patch box (per Jake's spec --
        # "GPS frame inner = 25.5 x 25.5 with the patch box centred"),
        # replacing the old ledge-based frame whose shelf, oversized to
        # reach the Top ceiling, fully overlapped the patch box's z-range
        # (Top x GPS Patch Reference interference). 2026-09-07 pass 7: GPS
        # patch shifted +7mm in Y (was -5..20) to y 2..27, above the new
        # battery position (x/z unchanged) -- both now clear of the
        # relocated comms stack (y < -1.0) by construction.
        'gps_frame_opening': 25.5,
        'fpc_keepout': {'x': (-20.0, 20.0), 'y': (-26.0, -15.0), 'z': (12.0, 22.0)},  # Top inner dome wall, reference only
    },

    'board_docs': {
        'xiao': 'XIAO-ESP32S3 v3',
        'wio': 'Wio-SX1262_for_XIAO_3D_file V2',
        'l76k': 'L76K GNSS Module for  XIAO v1',
    },

    'clearance_min': 0.3,
}
