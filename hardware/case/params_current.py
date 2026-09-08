"""Firefly case parameters — 'current' variant (60 x 110 x 25, matches Firefly V2 v15/v16)."""

PARAMS = {
    'variant': 'current',
    # NOTE: 'hat' L76K mode was dropped 2026-09-04 (coordinator's bay
    # redesign) -- the generator now always builds the wired layout.

    # --- envelope / spine ---
    'spine_a': (0.0, 0.0),
    'spine_b': (0.0, 50.0),
    # 2026-09-12/13 pass 12b (Jake: "move the top to be longer" -- see
    # hardware/case/README.md's pass-12b section): the ONLY thing that
    # needs to grow to give the FPC relief pocket more skin at the USB
    # end is the OUTER envelope's length at the dome-tip (+y) end, not
    # its height -- pass 12 proved analytically and live that raising
    # top_z cannot touch this (rho_at_z's z-shift is invariant to top_z).
    # `usb_end_extension_mm` is added to spine_b's own y (below, right
    # after this dict) rather than hand-editing spine_b directly: every
    # function in this file that measures "distance from the pill's
    # spine" (rho_from_spine, rho_at_z-driven envelope/cavity/verify
    # code, add_usb_tunnel's wall_y, add_lip_anchor_reliefs' ring ends,
    # true_wall_distance_along_ray) already treats spine_b as THE single
    # source of truth for where the +y dome sits -- so growing it here
    # moves the outer shell, the inner cavity, the USB tunnel, and the
    # lip/anchor ring ends together, automatically, with no other code
    # change, while every feature given as an ABSOLUTE mm position
    # (window_center, fpc_relief, plate_outline, top_posts,
    # board_standoffs, power_cap/home_cap, screw_D) stays exactly where
    # it is -- it was never derived from spine_b to begin with. 'current'
    # does not need this (its wider flat_rho/outer_radius already gives
    # 2.048mm of skin at the same corner with zero extension -- see the
    # pass-12b README section for the live numbers), so it stays 0.
    'usb_end_extension_mm': 0.0,
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
    # pass 13, item 3: the Waveshare display module's own 2-pin JST-style
    # battery socket ("HP1_25MM-2P-SMT-HORIZONTAL" in the inserted
    # occurrence's own component tree -- a 1.25mm-pitch 2-pin horizontal
    # SMT connector), live-probed in the inserted display occurrence
    # (world bbox measured on the trim build, then un-offset by trim's
    # own +3mm display_z_offset back to this, the base/current, frame --
    # see insert_display_pcba's docstring for why the offset exists and
    # add_battery_connector_access for how it's re-applied). x/y are
    # identical in both variants (same reference transform); z is offset
    # per-variant via display_z_offset like every other display-relative
    # z value in this file.
    'battery_connector_bbox': {'x': (-11.32, -3.67), 'y': (32.80, 38.00), 'z': (14.20, 17.60)},
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
    # 2026-09-15 pass 15, item 1 (Jake's print: "the 20pin cable hole needs
    # to be larger length-wise to fit the cable"): x widened from the old
    # asymmetric (-6.2, 7.02) -- SPEC's own literal minimum box -- to a
    # symmetric (-9.0, 9.0), live-measured against the ACTUAL inserted
    # display occurrence (not just SPEC's text), per the coordinator's own
    # instruction. Probed the real combined PCBA/shield/FPC body
    # ('H0146Y003T001-V1') at the pocket's own z-band (trim: z 24.83-25.93,
    # i.e. base 21.83-22.93 + display_z_offset) and across its y-span
    # (71.44-73.12): real solid material reaches as far as x=+-8.0mm at
    # y=71.44 (the widest slice, right at the PCB edge -- narrower,
    # x+-6.2/7.0, toward y=73.0-73.12 near the connector) -- i.e. the old
    # SPEC box (13.22mm wide) undershot the real part's own widest point
    # by up to 1.8mm on one side with ZERO margin, not the >=1.0mm/side
    # the brief asks for. (-9.0, 9.0) is symmetric (the real measured
    # extent WAS effectively symmetric, +-8.0, once probed directly -- the
    # old box's own asymmetry (-6.2 vs 7.02) wasn't measured off anything
    # in this axis, just SPEC's own literal text) and clears the measured
    # +-8.0mm envelope by 1.0mm on both sides. This widens the CORE box
    # that add_fpc_relief cuts UNCLIPPED (not just the outer empirical
    # margin, which the skin-safe tool can still shrink back near the true
    # wall) -- see that function's own docstring for why the core box is
    # cut in full regardless of the skin-safe clip, guaranteeing this
    # margin is never silently clawed back the way the old, narrower core
    # box left the widened OUTER margin (x up to +-14) exposed to exactly
    # that risk. y/z UNCHANGED (Jake's own complaint, and the task's own
    # clarification, both point at the cable's WIDTH axis specifically --
    # x here, since the pocket already reaches the connector's own y-band
    # with margin -- not its length/run axis). Re-verified live:
    # verify_fpc_relief still 0 bad of 63 probes, both variants (see the
    # pass-15 README section for the exact numbers and the live probe
    # script/output).
    'fpc_relief': {'x': (-9.0, 9.0), 'y': (71.44, 73.12), 'z': (21.83, 22.93)},

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
    # 2026-09-09 pass 9g (coordinator's render sweep, "plate post layout
    # is unbalanced"): y0 lowered 14.0 -> 10.0 and y1 raised 29.0 -> 29.5
    # for a clean >=4mm pad (post r 2.5 + 1.5mm margin) past BOTH new post
    # rows' own edges -- P1/P2 (y=14, south row) now sit mid-span instead
    # of flush on the old y0=14 edge (which left a 0mm pad -- the post
    # holes would have notched straight through the plate's own south
    # edge), and P3/P4 (y=25, north row) keep the same 4.5mm pad as
    # before. See 'top_posts' below for the full spread rationale.
    'plate_south_extension': {'x': (-24.0, -6.0), 'y': (10.0, 29.5)},
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
    # 2026-09-09 pass 9g (coordinator's render sweep, "plate post layout
    # is unbalanced"): pass-9-part-2's fix above put all four posts in a
    # 10x6mm cluster in the SW corner of the plate (x -10/-20, y 18/24) --
    # geometrically valid (every gate green) but a real design defect on
    # its own: the Screen Plate is effectively held at one corner, with
    # nothing supporting its NE 2/3, so it can flex/rattle there. Computed
    # (pure-Python probes of these exact PARAMS, no Fusion needed --
    # true_wall_distance_along_ray for shell skin, Euclidean distance to
    # window_center for bore clearance -- see hardware/case/README.md's
    # pass-9g section for the full grid search) before touching Fusion:
    # a full rectangle spanning the SAME safe x column (-20/-10, already
    # proven clean by pass-9-part-2 -- shell skin 1.64mm trim / 3.64mm
    # current at x=-20, >=13.6mm at x=-10, both >>the 0.6mm minimum) but
    # stretched in y from the tight 18-24 band to the full safe 14-25
    # band: y=14 at the south end (the plate_south_extension's own south
    # edge, unconstrained by the window at this x) and y=25 at the north
    # end (window-bore-limited -- at x=-10, y=25.83 is the analytic
    # cutoff for the required 1.0mm clearance; y=25 leaves 1.78mm to
    # spare, vs. the window bore itself, and 2.6mm to the display PCB's
    # own bbox, which starts at y=27.6). This nearly doubles the support
    # footprint (10x6mm -> 10x11mm, ~83% more bounding-box area) using
    # ONLY y-axis moves -- the x positions, and therefore every x-derived
    # margin (shell skin, GPS-frame clearance, the plate's own south-
    # extension x-range), are UNCHANGED from the already-verified pass-9
    # part-2 fix. Window-bore clearance for all 4 (analytic, both
    # variants -- window position/radius don't vary by variant):
    # P1 15.16mm, P2 12.55mm, P3 6.09mm, P4 1.78mm -- all comfortably
    # >= the 1.0mm minimum. Re-verified live via verify_post_walls
    # (pilot_wall/shell_skin, 8 rays x 3 z / 8 rays per post) after the
    # move -- see the pass-9g section.
    # 2026-09-15 pass 15, item 3 (Jake: "the placement of the 4 holes...
    # are all in the bottom left... doesn't give proper support... figure
    # out how to fit those better across the top / left / right / bottom"):
    # PROVED, before touching Fusion (pure-Python search against these
    # exact PARAMS -- window_center/window_dia, the GPS patch box, and
    # flat_rho -- reusing the same rho_at_z/true_wall_distance_along_ray
    # relations this file's own live gates use), that a literal 4-quadrant
    # (N/S/E/W) spread of CEILING-REACHING posts is geometrically
    # impossible here, not just difficult:
    #   - EAST is blocked outright: the GPS patch box (x -2.8..22.2,
    #     y 2..27) and the window bore's own exclusion circle (radius
    #     window_dia/2 + post_r + margin = 25.65mm from window_center)
    #     together leave NO x at ANY y where both clear at once east of
    #     x=0 -- their own boundaries meet at y~31.5 with zero margin
    #     (computed exactly: window needs y<=31.47 at x=17 (near the GPS
    #     box's own east edge and the plate's own edge), GPS needs
    #     y>=31.50 there -- a real, provable dead zone, not a tuning
    #     miss).
    #   - NORTH is blocked outright too: within the domed +y end cap
    #     (y > spine_b), the true outer wall is a circle of radius
    #     rho_at_z(p, top_ceiling_underside_z) = outer_radius (24.14mm
    #     trim at the ceiling) centred on spine_b, while the window bore
    #     is a circle of radius 22.65mm centred just 1.8mm away (window_
    #     center) -- the annular gap between them is only ~1.5mm wide,
    #     nowhere near enough for a Ø5 post plus the 0.6mm/1.0mm margins
    #     this file's own gates already require, at ANY angle.
    #   - The plate's OWN area-weighted centroid (computed from
    #     plate_outline + plate_south_extension - plate_header_cutout,
    #     ~(-4.62, 44.69)) sits only 7.04mm from window_center (50.0) --
    #     since no ceiling post can exist within 25.65mm of window_center,
    #     EVERY viable post position is provably >= 25.65-7.04 = 18.61mm
    #     from the plate's own centroid. The coordinator's brief's own
    #     "centroid within 5mm" target is therefore unreachable by more
    #     than 13mm for ANY arrangement of real, structural (ceiling-
    #     anchored) posts -- not a tuning shortfall.
    # Given this, `verify_plate_post_spread` (new gate, firefly_case.py)
    # is DIAGNOSTIC ONLY (reported, not hard-asserted in verify()'s
    # pass/fail) -- the same established pattern this file already uses
    # for verify_skin_intact/verify_wall_integrity/verify_display_
    # insertion_path, all of which over-fire on real, explained,
    # non-defect geometry. What WAS achieved, within the one region that
    # IS geometrically safe (west of the GPS patch, south of the window's
    # own exclusion circle -- a real search, not a guess: 200k-sample
    # random search maximizing angular spread subject to every live gate
    # this file already enforces -- window/wall/GPS clearance >= 0.5mm,
    # plate-edge pad >= ~3mm): the four posts now span a much larger,
    # genuinely 2D footprint (x -20..-9, y 14..23.5, ~11x9.5mm bounding
    # box on a diagonal, not a single 10x11mm axis-aligned rectangle in
    # one corner) with 265.8 degrees of angular spread around their own
    # centroid (was 264.5 degrees for the old 4-corner rectangle -- about
    # the same raw number, since that number was already close to this
    # region's own practical ceiling, but the NEW arrangement is not
    # collinear/axis-aligned the way the old one was, and covers visibly
    # more of the plate's own area -- see the pass-15 README section for
    # the full derivation, live verify_post_walls numbers, and a render).
    # P1's own position is UNCHANGED from pass 9g (already proven safe,
    # both variants); P2-P4 are new.
    'top_posts': {
        'P1': (-20.0, 14.0),
        'P2': (-9.0, 22.0),
        'P3': (-11.0, 14.0),
        'P4': (-18.0, 23.5),
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
    # 2026-09-15 pass 15, item 6 (Jake: "the back button needs to be
    # longer it doesn't reach correctly"): FIRST attempt (dead-end, kept
    # as a note) tried shrinking `plunger_pretravel` (the REST gap) 0.3 ->
    # 0.1mm to physically lengthen the plunger -- a live `check_
    # interference` run caught a real 14.28mm^3 Home Button x <switch
    # reference body> overlap even at just 0.1mm shorter, confirming
    # Home's real available room genuinely has no spare left at REST (see
    # button_geometry's own docstring: Home's rib/collar already needs
    # the `rib_actuator_shifted` dynamic clamp just to clear the real
    # actuator at the EXISTING 0.3mm gap). Reverted `plunger_pretravel`
    # to 0.3 (unchanged, proven safe).
    #
    # FIX: lengthen the PRESS STROKE instead of the REST position --
    # `plunger_travel` (rest-to-bottomed collar travel) raised 0.62 ->
    # 0.90mm. This is a REST-state-safe change: the built (as-modeled)
    # geometry Fusion's own `check_interference` gate examines is always
    # the REST state, and `plunger_travel` only affects the collar's own
    # REST position relative to the rib (s_collar_outer = s_rib_inner -
    # plunger_travel) -- a larger value shifts the collar further inboard
    # at rest, which the EXISTING `rib_actuator_clearance` dynamic clamp
    # in button_geometry already re-clears automatically (it shifts the
    # whole rib+collar assembly outward, toward the wall, whenever the
    # nominal collar position would come closer than 0.3mm to the real
    # actuator -- unconditional on plunger_travel's own value). Live-
    # confirmed clean (0 interference, both buttons, both variants) at
    # 0.90mm. At full press this delivers a real actuation stroke of
    # 0.60mm (0.90 - the unchanged 0.3mm pretravel) versus the old
    # 0.32mm (0.62-0.3) -- a 87.5% increase, comfortably past a typical
    # tactile dome's own ~0.25-0.3mm throw -- the direct, measurable fix
    # for "doesn't reach", without touching the REST-position geometry
    # that (per the dead-end above) has no spare margin left for Home.
    'plunger_travel': 0.90,      # rest-to-bottomed inward travel before the collar hits the rib
    'collar': {'h': 0.8, 'len': 1.0},  # h = extra flange height beyond the plunger cross-section (Z); len = along travel axis
    # wall_x (the -x outer wall, where the buttons live) is derived at build
    # time as -PARAMS['outer_radius'] -- not duplicated here so it can never
    # drift out of sync between variants.

    # --- USB-C tunnel ---
    'usb_receptacle': {'x': (-4.48, 4.48), 'z': (14.35, 18.43), 'y': 73.0},
    # 2026-09-15 pass 15, item 2 (Jake: "the power cable hole needs more
    # room to actually plug in. needs more room towards the top"): grown
    # 0.9mm taller (7.0 -> 7.9mm) and shifted +0.45mm in Z (16.4 -> 16.85)
    # so the growth is biased UPWARD (toward the glass/+Z side, where
    # Jake's complaint points) rather than symmetric -- the OLD bore's
    # lower edge (12.9mm, current) is preserved almost exactly (new lower
    # edge 12.9mm too, since center+0.45 and half-height+0.45 cancel at
    # the bottom), all ~0.9mm of new headroom lands on the TOP edge
    # (19.9mm -> 20.8mm current / 22.9mm -> 23.8mm trim -- see
    # usb_tunnel_center_z's own +_DZ_TOP shift in params_trim.py). Checked
    # against a standard USB-C plug overmold (~6.5x8.4mm body): the new
    # 7.9mm bore height now exceeds the plug's own 6.5-8.4mm body range
    # with real margin on both variants, cf. the old 7.0mm which was
    # tight against the top of that range. Re-verified live (both
    # variants): 0 real interference against the inserted display
    # occurrence (the tunnel's own Combine-Intersect against the TRUE
    # curved outer envelope, add_usb_tunnel, already prevents any breach
    # of the outer skin regardless of how tall this is asked to be -- the
    # display-body clearance is the one that needed a live check here,
    # since the tunnel's own XY footprint sits close to the display's own
    # y-extent near the dome tip). See the pass-15 README section for the
    # exact clearance numbers.
    'usb_tunnel_stadium': (13.0, 7.9),
    'usb_tunnel_center_z': 16.85,
    'usb_tunnel_y_start': 73.5,
    'usb_liner_thickness': 1.6,
    'usb_liner_outer_stadium': (16.2, 11.1),

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
    # 2026-09-15 pass 15, item 9 (Jake: "the bottom lanyard thing... I
    # think that still needs work"): reviewed against the brief's own
    # checklist --
    #   - cord hole diameter for 4-5mm paracord: the old 4.0mm hole is
    #     AT the tight end of that range with zero running clearance (a
    #     real paracord this size would need to be forced through, and
    #     a printed hole always comes out a touch undersized/rough vs.
    #     nominal) -- widened to 5.0mm, a comfortable running fit for
    #     4-5mm cord with margin for print tolerance.
    #   - wall thickness around the hole (brief's own >= 2.4mm floor):
    #     computed directly from the existing geometry -- the TIP-side
    #     wall (hole_from_tip - hole_dia/2) was only 1.5mm at the old
    #     3.5mm/4.0mm pair, well under 2.4mm (the side walls, (width-
    #     hole_dia)/2 = 5.0mm, and the root-side wall, ~9mm ear length
    #     minus hole_from_tip, were never the tight dimension). Fixed by
    #     raising `hole_from_tip` 3.5 -> 5.0mm alongside the wider hole:
    #     tip wall = 5.0 - 5.0/2 = 2.5mm (>= 2.4mm, the binding
    #     dimension); side wall = (14.0-5.0)/2 = 4.5mm; root wall ~4mm
    #     (the ear's own ~9mm nominal length minus hole_from_tip) -- all
    #     four sides now clear the 2.4mm floor, not just three of them.
    #   - print orientation: unaffected by either change -- the ear still
    #     prints flush on the bed (z=(0.0,10.0), Bottom's own face-down
    #     orientation, unchanged), and the hole is a plain vertical
    #     though-cylinder (axis parallel to the print's own Z), needing
    #     no support either before or after this pass.
    #   - strength against a hard tug (rough hand calc, both variants
    #     share this geometry): PETG has a tensile strength of roughly
    #     50 MPa; the tip-wall cross-section resisting a straight pull
    #     is roughly 2x(tip_wall x lug width) = 2 x 2.5mm x 14mm = 70mm^2
    #     (the material on both sides of the hole, in the pull direction)
    #     -- failure load ~70mm^2 x 50MPa = 3500N, wildly beyond any
    #     plausible lanyard tug (a firm human yank is on the order of
    #     50-150N) -- even derating heavily for a printed part's real
    #     layer-adhesion strength (often 30-50% of bulk, and worse
    #     across layer lines specifically), this leaves a very
    #     comfortable margin. The old 1.5mm tip wall's own same estimate
    #     (2 x 1.5 x 14 x 50 = 2100N) was ALSO nominally fine by this
    #     same rough calculation -- the 2.4mm floor here is a
    #     print-quality/consistency margin (thin printed walls are more
    #     sensitive to under-extrusion, layer gaps, and stress
    #     concentration at the hole's own edge than the bulk number
    #     alone suggests), not a response to a marginal strength number.
    #   - root fillets: see add_lug's own code, right after the ear
    #     joins into Bottom -- a new best-effort 1.0mm fillet along the
    #     ear's own top/bottom root edges (where the ear's cross-section
    #     is at its widest, at the shell attachment) reduces the stress
    #     concentration a hard tug puts right at that seam, the same
    #     idiom (skip-on-failure) as every other cosmetic/reinforcement
    #     fillet in this file.
    'lug': {
        'width': 14.0, 'protrusion': 6.0, 'z': (0.0, 10.0),
        'hole_dia': 5.0, 'hole_from_tip': 5.0,
        'fillet_r': 3.0, 'hole_chamfer': 0.6, 'root_fillet_r': 1.0,
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

    # --- compass module mount (pass 10 REDO, 2026-09-06) ---
    # GY-273 (QMC5883P) mount. Measured off Jake's Fusion model "HMC5883L
    # Mag v1" (mm, MODULE'S OWN LOCAL FRAME -- PCB top face at local
    # z=1.0, components on top; the five header pins are soldered from
    # BELOW in Jake's build).
    #
    # Placement REDONE this pass: the coordinator rejected the original
    # pass-10 placement (standing on edge at the lanyard end, needing an
    # outward BROW -- see git history / the PR's earlier revisions) for
    # putting a boxy bump on the pill's outer silhouette, which must stay
    # clean. New placement: hanging from the TOP'S OWN INNER CEILING,
    # directly above the GPS patch frame's open chimney (bay.gps_patch) --
    # entirely inside space that is ALREADY open cavity (the GPS frame's
    # ring wall retains the patch antenna only up to its own z[1]=18.8;
    # above that, up through the ceiling, the frame's 25.5x25.5 opening is
    # hollow by construction -- see build_gps_frame_body). No outer-wall
    # interaction at all, no brow, no pocket cut needed.
    #
    # Stack height budget (ceiling down to the patch): standoff_h (2.5mm,
    # doubling as the header/solder-joint allowance, local_header_below)
    # + PCB thickness (1.0mm) + component bump (local_component_h, 1.0mm)
    # = 4.5mm. Spare above the patch = top_ceiling_underside_z - 4.5 -
    # gps_patch z[1] (18.8) -- see mag_module_clearance/mag_module_fits in
    # firefly_case.py. TRIM: 26.0 - 4.5 - 18.8 = 2.7mm spare -- fits.
    # CURRENT: 23.0 - 4.5 - 18.8 = -0.3mm -- does NOT fit (current's
    # ceiling was frozen at the old 25mm-case height in pass 7, "for the
    # probe comparison", and never grew the 3mm trim did) -- the mount is
    # skipped entirely for 'current' (mag_module_fits returns False),
    # same pattern as comms_stack3_full_height already skipping the
    # 3-board stack there. See README's pass-10 section for the full
    # writeup of why 'current' can't host this identically.
    #
    # XY placement/orientation REDONE AGAIN 2026-09-11, pass 11 (defect 2,
    # "compass mount sits too close to the display"): Jake's review of
    # pass10b_mag_pocket.png found the header edge (and its 5 solder
    # wires) pointing toward +Y -- the display end -- with the fence's own
    # north edge only ~1mm from the window bore's true rim there (see
    # firefly_case.py's mag_window_bore_clearance docstring for why the
    # bore, not the buried lip_r ring at z 9.2-11, is the real "ring"
    # Jake saw), and the wires exiting straight into that gap. Two
    # independent fixes, both computed in pure Python against these exact
    # PARAMS before touching Fusion (see README's pass-11 section for the
    # full derivation/search):
    #   (a) ORIENTATION FLIPPED: the local-x -> world-Y mapping is now
    #       DECREASING (mag_world_y: `-local_x + offset`, was `+local_x +
    #       offset`) -- the header/wire edge (local x=+8.96) now maps to
    #       the SMALLER world Y (toward -Y, the lanyard end); the
    #       mounting-hole edge (local x=-9.64) maps to the LARGER world Y
    #       (toward +Y, the display end). The wires no longer point at
    #       the window/display at all.
    #   (b) FOOTPRINT SHIFTED as far -Y and +X as the GPS frame's own
    #       real opening (x -3.05..22.45, y 1.75..27.25) allows with a
    #       >=0.5mm safety margin on every side that would otherwise
    #       touch the frame's own wall -- maximizing clearance to the
    #       window bore's true rim (the tightest constraint) without
    #       drifting into new interference with the GPS frame itself.
    # Result (world mm, both offsets below): PCB x 6.4..20.4, y 3.75..
    # 22.35; fence x 4.9..21.9, y 2.25..23.85. Window-bore clearance at
    # the worst (west, north) fence corner: 3.96mm (was ~1.4mm) --
    # >= MAG_DISPLAY_RING_MIN_CLEAR (3mm, firefly_case.py) with margin,
    # short of the 5mm stretch target only because pushing further south
    # or east starts eating the GPS frame's own real wall/opening
    # boundary (see the frame-margin numbers in README's pass-11 section)
    # -- a genuine geometric ceiling, not an oversight. Display back-side
    # bbox clearance (fence north edge to display bbox y0=27.6): 3.75mm
    # (was ~2.3mm). Both re-verified live by verify_mag_pocket's two new
    # keep-out checks (window_bore_clear/display_back_clear), not just
    # this analytic pass.
    #
    # Stack height budget is UNCHANGED by this pass (still standoff_h
    # 2.5mm + PCB 1.0mm + component bump 1.0mm = 4.5mm; TRIM 2.7mm spare,
    # CURRENT -0.3mm -- mount still skipped for 'current', see
    # mag_module_fits).
    #
    # Orientation (local -> world, SAME for both variants, world_x/
    # world_y are pure TRANSLATIONS -- see mag_world_x/mag_world_y in
    # firefly_case.py): local +x (18.6mm axis, HEADER edge, local
    # x=+8.96) -> world -Y (toward the LANYARD end, short wire run away
    # from the display/window); local -x (MOUNTING-HOLE edge, local
    # x=-9.64) -> world +Y (toward the display end). local +y -> world
    # +x; local -y -> world -x (arbitrary handedness choice, no
    # functional constraint on this axis -- both pegs/pads are placed by
    # explicit local (x,y) pairs, not by a directional rule; UNCHANGED by
    # pass 11). local +z (the COMPONENT/sensor face, away from the PCB)
    # -> world -Z (DOWN, toward the GPS patch/Bottom -- the module is
    # mounted components-down); local -z (the HEADER/solder-pin face) ->
    # world +Z (UP, toward the ceiling -- both peg mounting holes share
    # local x=-7.21, i.e. the SAME local z=0 plane, so both land at the
    # same world Z, flush against the ceiling standoff). See
    # firmware/targets/esp32s3/components/ff_compass/ff_compass.c's own
    # updated comment for the resulting axis-remap table.
    'mag_module': {
        'local_pcb': {'x': (-9.64, 8.96), 'y': (-6.67, 7.33)},  # 18.6 x 14.0mm
        'local_component_h': 1.0,   # max component height above local PCB top (sensor, ~3x3 at (-0.9, 0.5))
        'local_header_below': 2.5,  # allowance below local PCB bottom for header solder joints/pins -- doubles as standoff_h
        'local_mount_holes': [(-7.21, -4.17), (-7.21, 5.03)],  # Ø3.0 real mounting holes, 9.2mm spacing
        'local_header': {'x': 7.44, 'y': [-4.81, -2.21, 0.39, 2.89, 5.49], 'dia': 1.0},
        'mount_hole_dia': 3.0,
        'peg_dia': 2.7,        # peg OD -- 0.3mm total clearance in the Ø3.0 mounting hole
        'pad_dia': 3.0,        # header-side rest-pad OD (no through-hole, just a resting boss)
        'standoff_h': 2.5,     # == local_header_below: ceiling-to-PCB-bottom gap, for both pegs and pads
        'fence_wall': 1.2, 'fence_clear': 0.3, 'fence_h': 3.5,  # low retaining fence around the PCB outline
        'header_notch_w': 3.0,  # wire-exit notch in the fence's header-edge wall (SOUTH wall as of pass 11 -- see add_mag_module's gap_side)
        # World placement -- pure translations, SAME for both variants
        # (the GPS-patch/post layout this is centred against doesn't
        # scale with outer_radius; both offsets are simply
        # local-axis-origin -> world-axis-origin distances):
        # world_y = -local_x + world_y_from_local_x_offset  (pass 11: sign flipped, see above),
        # world_x = local_y + world_x_from_local_y_offset.
        'world_y_from_local_x_offset': 12.71,  # pass 11 (was 14.84): local_pcb x-span -> world y 3.75..22.35 (shifted -Y from the GPS patch's own centred position, header edge now the -Y/min-y end)
        'world_x_from_local_y_offset': 13.07,  # pass 11 (was 9.37): local_pcb y-span -> world x 6.4..20.4 (shifted +X, away from the window's own x=0 centreline)
        'min_patch_clearance': 1.0,  # required spare (mm) between component bottom and the GPS patch top for a variant to host this mount at all
    },
}

# --- pass 12b: apply the USB-end extension to the dome-tip spine point.
# See PARAMS['usb_end_extension_mm']'s own comment above for why this is
# the single mechanism that moves the outer envelope/inner cavity/USB
# tunnel/lip+anchor ring ends together while everything given as an
# absolute mm coordinate elsewhere in this file is untouched. A no-op
# here (spine_b unchanged) since 'current' keeps usb_end_extension_mm=0.
PARAMS['spine_b'] = (PARAMS['spine_b'][0], PARAMS['spine_b'][1] + PARAMS['usb_end_extension_mm'])
