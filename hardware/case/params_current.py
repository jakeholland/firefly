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
    'fpc_relief': {'x': (-6.2, 7.02), 'y': (71.44, 73.12), 'z': (21.83, 22.93)},

    # --- case screws (Bottom -> Top), M2 socket head ---
    'screw_head_dia': 3.8,
    'screw_head_h': 2.0,
    'screws_ABC': [
        {'name': 'A', 'xy': (-22.97, 25.04)},
        {'name': 'B', 'xy': (0.0, -24.0)},
        {'name': 'C', 'xy': (23.74, 25.2)},
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
        'battery': {'xyz': (8.0, 40.0, 30.0), 'x': (-20.0, 20.0), 'y': (-4.0, 26.0), 'z': (2.0, 10.0)},
        'battery_rail_w': 1.2, 'battery_rail_z': (2.0, 6.0), 'battery_rail_clear': 0.3,
        'battery_strap': {'w': 6.0, 'h': 1.5},  # slot through the rails, not the floor
        'l76k_wired': {'x': (-10.5, 10.5), 'y': (-23.5, -5.5), 'z': (2.0, 6.0)},
        'l76k_frame_wall': 1.0, 'l76k_frame_clear': 0.3, 'l76k_wire_notch_w': 3.0,
        # pass-5 fix: the PCB now rests on a 0.3mm-tall floor PAD from
        # z=2.0 (nominal cavity floor top) to z=2.3, not directly on z=2.0
        # -- the bare cavity floor and the PCB's own insertion tolerance
        # were landing the PCB body 0.17mm INSIDE the floor. 2.3 is the
        # PCB's intended resting height (bottom flush on the pad).
        'l76k_floor_pad_z': (2.0, 2.3),
        # NOTE (2026-09-06 pass 6): the coordinator raised the real kit
        # being board-to-board (B2B) with XIAO below Wio component-side
        # down, then further superseded that with a 3-board (L76K+XIAO+
        # Wio) direct-solder stack in a new cradle at the dome tip, which
        # in turn requires growing the case height (measured stack height
        # 18mm does not fit under the current ~23mm ceiling). That is a
        # substantial re-architecture (new cradle geometry, relocated
        # battery/GPS bay, and re-deriving every Z-dependent Top feature
        # off a parameterized case height) that was NOT completed in this
        # pass -- see the README's pass-6 section for what was verified
        # and what remains as follow-up work. Reverted here to the last
        # known-good (pin-header, Wio-bottom/XIAO-top) configuration
        # rather than ship a partially-applied, uncertain change.
        'stack': {
            'x': (-22.0, -4.2), 'y': (0.0, 22.3),
            'wio_pcb_bottom_z': 10.5, 'xiao_pcb_bottom_offset': 6.1, 'stack_top_z': 21.0,
        },
        'tray_wall': 1.2, 'tray_clear': 0.45, 'tray_z_bottom': 10.2,
        # pass-5 fix (coordinator diagnosis): the ORIGINAL 'Top x XIAO'
        # interference was an orientation bug, not a footprint-size
        # problem -- XIAO's native long axis (~22.5mm, including the
        # USB-C overhang) was being mapped onto world X (see
        # insert_comms_boards' xiao_doc block, now 'y90' instead of 'y'),
        # making the stack ~22.5mm wide in X against a tray sized for
        # Wio's ~17.8mm width. With XIAO correctly rotated so its long
        # axis lies along world Y (parallel to Wio's own long axis, both
        # ~22.3-22.5mm), XIAO's real world footprint is 17.78 x 22.48 --
        # a near-exact match for Wio's 17.78 x 22.32 -- so only a small
        # symmetric margin is needed, not the earlier asymmetric
        # multi-mm widening (which this replaces).
        'tray_x_extra': 0.0,
        'tray_x_extra_right': 0.0,
        'tray_ledge': {'w': 2.0, 'h': 2.0},  # z tray_z_bottom .. +h, at each short end (2x2mm 45deg wedge)
        'tray_gap': {'side': '+y', 'w': 6.0},  # for XIAO USB/wires
        'gps_patch': {'xyz': (25.0, 25.0, 8.3), 'x': (-2.8, 22.2), 'y': (-5.0, 20.0), 'z': (10.5, 18.8)},
        'gps_frame_wall': 1.0, 'gps_frame_clear': 0.3,
        # pass-5 fix: the GPS frame is a plain hanging ring with a clear
        # 25.5x25.5 opening CENTRED on the patch box (per Jake's spec --
        # "GPS frame inner = 25.5 x 25.5 with the patch box centred"),
        # replacing the old ledge-based frame whose shelf, oversized to
        # reach the Top ceiling, fully overlapped the patch box's z-range
        # (Top x GPS Patch Reference interference).
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
