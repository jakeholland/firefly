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
    # 2026-09-08 pass 9 (finding 5, window lip ring fragile): the ring was
    # only 0.8mm wide (lip) -- a chunk broke out at the bore in Jake's
    # print. The OUTER edges (27.75 lip / 28.40 anchor) are load-bearing
    # geometry (27.75 sets the 0.25mm nesting clearance against Bottom's
    # own true inner wall -- outer_radius - wall = 28 for current, 26 for
    # trim -- and 28.40 is the deliberate ~0.4mm reach PAST the true wall
    # that makes the anchor band actually fuse into Top's shell on join,
    # per SPEC's own "so it fuses"), so they're UNCHANGED; the ring is
    # thickened INWARD instead, widening the band from 0.8mm to 1.8mm
    # (>= the 1.6mm minimum). This is safe with a wide margin: the ring's
    # z-span (9.2-11) sits well below the display glass (z 20.3+) and PCB
    # (z 17.59-18.81 current / +3 shift trim) -- there is NO z-overlap
    # between the ring and the display module at all, so thickening
    # inward cannot touch its 0.25mm clearance regardless of how far in
    # it goes; 25.95 (current) / 23.95 (trim, see params_trim.py) still
    # leaves >3mm/1.3mm to the window bore (r=22.65) as a bonus margin.
    # See add_lip_anchor_reliefs' new seam chamfer (finding 6) for the
    # step this widening does NOT remove: the lip (27.75) to anchor
    # (28.40) OUTER radius step at z=10 is untouched by this fix and
    # still needs its own overhang treatment.
    'lip_r': (25.95, 27.75),
    'lip_z': (9.2, 10.0),
    'anchor_r': (25.95, 28.40),
    'anchor_z': (10.0, 11.0),
    'lip_ring_seam_chamfer': 0.5,  # mm -- finding 6: 45-deg-ish bevel on the lip/anchor outer step (z=10, r=lip_r[1]->anchor_r[1]) so Top prints support-free there
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
    # 2026-09-08 pass 9 (finding 4): the relocated P1/P2 (y=18/24) sit
    # south of the main outline's own y0 (28.8) -- a second, narrower
    # rectangle unioned onto the main outline before the cavity-outline
    # intersect reaches down to cover them. X range (-24..-6) clears both
    # new west-side posts (x=-10/-20, each with >=1.5mm pad beyond its
    # own Ø5 hole) while staying west of the GPS frame's real wall
    # (inner opening edge at x=-3.05, +1mm frame wall = -4.05) with
    # ~1.9mm to spare -- confirmed by direct computation (see README),
    # not just visual inspection, since the GPS frame is a separate
    # printed feature (on Top) the plate must never touch.
    'plate_south_extension': {'x': (-24.0, -6.0), 'y': (14.0, 29.0)},
    'plate_header_cutout': {'x': (11.5 - 1.0, 17.0 + 1.0), 'y': (43.7 - 1.0, 56.1 + 1.0)},
    'plate_hole_dia': 2.4,
    'plate_pad_dia': 6.0,
    # 2026-09-08 pass 9 (finding 4, screen-plate posts P1-P4 snapped):
    # the P1-P4 positions above (pass-6 P2 fix included) put P1/P4 in the
    # 3.35mm crescent between the window bore (r=22.65 at spine_b) and the
    # true inner wall (r=26 trim / 28 current) -- computed directly (not
    # just observed from the print): at their own top_post_z[1] (the
    # ceiling), P1/P4's OWN true-outer-wall clearance (`true_wall_
    # distance_along_ray` minus the post radius) is NEGATIVE (the post
    # already exceeds the true outer surface there) and their distance to
    # the window bore is ~0.5-0.6mm -- nowhere near the required 1.0mm.
    # P2/P3 (well inboard) have plenty of shell clearance but P3's
    # distance to the window bore is only ~0.1mm at Ø4 and goes NEGATIVE
    # at the new Ø5 (a real cut-through by the window bore's own z 25.4+
    # extent, which overlaps the post's own top ~0.6mm). Separately (and
    # probably the REAL cause of "the post by the power button snapped"):
    # POST_CORE_R (the narrow full-height "reach" cylinder guaranteeing
    # the post physically touches the ceiling, see clipped_pillar_with_
    # reach) was only 1.1mm radius against a 1.62mm-dia (0.81mm-radius)
    # pilot -- a 0.29mm wall at the post's own tip, regardless of xy
    # position, on EVERY one of P1-P4 -- see POST_CORE_R's new derivation
    # below.
    #
    # Fix (computed in a standalone script against these exact PARAMS
    # before touching Fusion -- see hardware/case/README.md's pass-9
    # section for the full numeric derivation): relocate all 4 posts,
    # ABSOLUTE mm, SAME for both variants (matching the A/B1/B2/C/D
    # pattern), to the y 18-24 band south of the display PCB (bbox y
    # starts 27.6) and clear of the GPS patch box/frame (box x -2.8..22.2
    # -- staying west of x=-6 clears it with >1.9mm to spare, no keep-out
    # cut needed at all) and the true wall (rho0=|x| <= 20 keeps >=1.6mm
    # skin to the true outer wall at the flat-ceiling height, both
    # variants). Verified (both variants): outer-wall clearance
    # >= 1.64mm (>= the required 0.6mm), window-bore clearance
    # >= 2.36mm (>= the required 1.0mm), for all 4 posts.
    'top_posts': {
        'P1': (-10.0, 18.0),
        'P2': (-20.0, 18.0),
        'P3': (-10.0, 24.0),
        'P4': (-20.0, 24.0),
    },
    'board_standoffs': {
        'S1': (-12.0, 65.0),
        'S2': (0.04, 32.22),
        'S3': (11.6, 65.46),
    },
    # 2026-09-08 pass 9 (finding 4): Ø4 -> Ø5 -- a plain Ø4 post around
    # a Ø1.62 pilot only has a 1.19mm nominal wall (before any clipping),
    # already under the new 1.2mm minimum; Ø5 gives 1.69mm nominal.
    'top_post_dia': 5.0,
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
    # 2026-09-08 pass 9b/finding 10: replaces the old 'plunger_tip_gap'
    # (0.02mm, defined as a full-press gap FROM THE SWITCH HOUSING via
    # `housing_xy` -- an approximated bbox-corner point that turned out to
    # sit 3.3-3.5mm outboard of the real switch body, per a live Fusion
    # probe of the actual 'SWITCH-TS24CA' body -- see button_geometry's
    # docstring). `switch_actuator_reach` is the real, measured actuator
    # nub's own outward reach (mm, from the switch's own bbox center,
    # along its nub direction) -- an intrinsic property of the switch
    # part itself, identical for both buttons (same physical part; the
    # live probe found 1.82mm for both). `plunger_pretravel` is the gap
    # (mm) between the plunger tip and that REAL actuator, at rest, before
    # any press starts closing it -- this is what actually gates the
    # button's reach now; see verify_plunger_reach.
    'switch_actuator_reach': 1.82,
    'plunger_pretravel': 0.3,
    # nub_pocket['xy'][0] (tangential width) widened 1.3 -> 2.4mm
    # (2026-09-08, same probe): the real actuator nub is ~1.9mm wide
    # tangentially (z 16.2-17.2 at the housing, per the live scan), not
    # the 1.3mm the pocket used to assume -- 2.4 keeps the SPEC's 0.25mm/
    # side clearance convention (1.9 + 2*0.25 rounded up). Height (1.6mm)
    # is untouched -- the real nub is only ~1.0mm tall, already clear.
    'nub_pocket': {'xy': (2.4, 1.6), 'depth': 0.8},
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
    # 2026-09-08 pass 9b, finding 9 (collateral discovery): kept at 6.0 --
    # a live probe of the REAL inserted switch body (see button_geometry's
    # docstring) found the Home button's rib/collar, at this offset,
    # genuinely overlapped the real switch (once the reach fix, finding
    # 10, put the mechanism at its real measured position). A SMALLER
    # global offset (tried: 3.5) fixes that but trades it for a DIFFERENT
    # real defect: the rib's own flat-box Z-extent reaches well past
    # cap_z_center (the one height s_wall is computed at) into the R10
    # shoulder curve, where the true wall is measurably closer -- a
    # smaller offset leaves less margin to absorb that gap, and a live
    # export-envelope check confirmed a real ~0.5mm breach at Power's
    # rib once the global offset shrank (Power never needed the shrink in
    # the first place -- 16mm+ of real clearance). Fixed instead with a
    # PER-BUTTON dynamic clamp in button_geometry (s_rib_inner/s_collar_
    # inner shift outward only when the nominal 6.0mm offset would
    # violate real-actuator clearance -- a no-op for Power) plus an
    # explicit Combine-Intersect of the (now closer-to-wall) rib against
    # the true outer envelope specifically when that clamp fires, so a
    # smaller effective offset can never re-open the same breach add_
    # button's rib_plate/connector code was fixed against here. See
    # button_geometry's and add_button's own comments for both halves.
    'rib_inboard_offset': 6.0,   # rib's outboard face, mm inboard of the outer wall (nominal; see comment)
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

    # --- antenna cable channels (finding 8, 2026-09-08 pass 9e) ---
    # Live-probed world-mm centers of the two u.FL connectors this design
    # actually routes cables from: queried directly off the Wio-SX1262/
    # L76K reference docs' own 'U.FL Connector' sub-occurrence bodies in
    # a real built 'trim' document (root.allOccurrences -> bRepBodies,
    # world-space per SPEC.md gotcha 6) -- see README's pass-9e section
    # for the query and its independent cross-check (hand-derived from
    # insert_comms_boards' own placement transforms + separately-probed
    # native board thicknesses, matching this live read to within
    # 0.005mm). `gps_ufl_xyz` is the SAME in both variants -- the L76K's
    # placement (l76k_bottom_z, the stack3 PCB footprint) doesn't depend
    # on case height, and L76K is always inserted (see insert_comms_
    # boards). `lora_ufl_xyz` is only meaningful when the Wio is actually
    # inserted (comms_stack3_full_height=True, i.e. 'trim') -- add_
    # antenna_channels skips the LoRa route entirely for 'current'.
    'antenna': {
        'gps_ufl_xyz': (2.095, -21.435, 5.92),
        'lora_ufl_xyz': (3.444, -21.961, 19.895),
        'channel_width': 2.0, 'channel_depth': 1.6, 'channel_fillet': 0.3,
        'channel_min_skin': 1.2,
    },
}
