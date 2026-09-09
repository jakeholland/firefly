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
    # 2026-09-2x pass 16 (owner call, item B): B1/B2 are RETIRED outright
    # (not just merged -- deleted) along with the old screw_D -- see
    # mech review F1 ("4 lanyard-end screws well past what pull-out
    # needs... A/B1/B2/C only 7.6mm apart, center to center") and the
    # README's pass-16 section for the pull-out math this decision
    # reuses verbatim (695-926N per M2x12 pilot at 9.1mm engagement vs a
    # 50-150N worst-case lanyard tug). Only A and C remain from the old
    # 4; D1/D2 (below) replace the old single D, for 4 case-closure
    # screws total (A, C, D1, D2) -- down from 5.
    'screws_ABC': [
        {'name': 'A', 'xy': (-15.5, -8.0)},
        {'name': 'C', 'xy': (15.5, -8.0)},
    ],
    'boss_dia': 6.0,
    'screw_hole_dia': 2.4,
    'counterbore_ABC_dia': 4.5,
    'counterbore_ABC_h': 2.2,
    'top_pilot_dia': 1.62,
    'top_pilot_z': (10.0, 19.1),   # -> M2x12

    # pass 16, item A/B (candidate 5, owner's chosen display mount):
    # screw D (0,60), its Screen Plate post, and its deeper 4.0mm
    # counterbore are all RETIRED -- there is no Screen Plate any more.
    # D1/D2 ORIGINALLY sat at the two EARS' own wall roots (+-19, 64) --
    # see git history on this branch. Same M2x12 pilot spec as A/C
    # (top_pilot_dia/top_pilot_z, unchanged) and the SAME shallow
    # counterbore_ABC_dia/h (2.2mm) -- there is no plate post any more to
    # need a deeper counterbore, so D1/D2's Bottom-side boss is IDENTICAL
    # in construction to A/C's (see add_case_boss, which has no is_D
    # branch).
    #
    # RELOCATED pass 16 (resumed, owner call on Finding 1): at (+-19, 64)
    # the D1 wall-root pillar was found, live, to be a genuine ~304mm^3
    # geometric enclosure with the Home button's own real cap/shaft body
    # -- the button's real solid completely fills the exact wall
    # cross-section `verify_post_walls` requires around the pilot, at
    # every sampled angle/z within the pilot's own engagement depth (see
    # README's pass-16 Finding 1 for the full live-probe trail). No
    # keepout radius/z-band can satisfy both the pilot-wall requirement
    # and the button's real geometry at that xy. Owner decision: D1/D2
    # LEAVE the ear roots (the ears keep their own independent wall
    # anchor at the old (+-19, 64) point -- see 'ears', below -- now with
    # no pilot through it).
    #
    # FALLBACK TAKEN (this pass): the owner's own first-choice relocation
    # -- two screws NORTH of the ears, hugging the dome wall around
    # (+-14, 71) -- was searched exhaustively, pure-Python, BEFORE any
    # Fusion build (per the brief's own instruction), against every
    # required clearance: real wall material behind the boss
    # (`true_wall_distance_along_ray`), the FPC relief pocket, the ears'
    # own wall-root/standoff footprints, and both switch housing bboxes.
    # That search caught a REAL constraint the brief's own clearance list
    # didn't name explicitly: `add_lip_anchor_reliefs`' own
    # `MIN_RELIEF_CLEARANCE` (every case screw needs a clean relief cut
    # through the lip/anchor ring, which independently requires >=1.6mm
    # of real wall skin at the boss's own OD, not just the 0.6mm
    # `verify_post_walls` alone would ask for) -- found the HARD WAY, by
    # a live `add_lip_anchor_reliefs` assertion failure on the first
    # build attempt at the brief's own (+-14, 71) target. Re-running the
    # full joint search (both variants) with the CORRECT constraint:
    # north of the ears (y >= 64, respecting every clearance above)
    # has ZERO feasible positions on the east/S3 side at all, and the
    # west/S1 side's own best joint position ((-12.6, 71.5)) clears by
    # only ~0.015mm -- not a real, buildable margin, effectively also
    # zero. Per the brief's own instruction ("If NO position on either
    # side satisfies every clearance, fall back to ONE dome-end screw D
    # ... report why, do not leave an interference"): D1/D2 are RETIRED,
    # replaced by a SINGLE screw 'D', keeping the list name 'screws_D12'
    # unchanged (every gate/helper in firefly_case.py already iterates
    # this list generically -- see the README's pass-16 section for the
    # full list of call sites confirmed to need no further change beyond
    # this one now holding a single entry).
    #
    # 'D' position: the literal dome TIP (the true apex, y approaching
    # spine_b + outer_radius) turns out to have the SAME failure mode --
    # the same search at x=0, y=73-80 (past the FPC pocket) finds
    # `MIN_RELIEF_CLEARANCE` goes negative well before the FPC pocket's
    # own 0.5mm clearance is satisfied, for the same underlying reason
    # (the profile's own top chamfer starts curving the wall inward well
    # before the true apex) -- so "the tip" cannot mean the literal apex
    # either. The real thickest-wall margin near the north end, live-
    # searched (both variants) over the full dome-cap region, is on the
    # EAST FLANK south of the FPC pocket and clear of both buttons (which
    # are west-only) and both ears: (18.0, 58.0) -- `MIN_RELIEF_CLEARANCE`
    # margin saturates at its own cap (both variants), `verify_post_
    # walls`' shell-skin margin >=5.3mm (trim) / 5.8mm (current), >=12mm
    # to the FPC pocket, >=27mm to either switch housing bbox, and 3.8mm
    # (trim/current, xy-identical) to the nearer ear (S3) -- comfortably
    # real margins, not a razor's edge like the north-of-the-ears
    # attempt. Also confirmed clear of the comms stack (`bay.stack3`,
    # y <= -1.5), the GPS patch/battery (`bay.gps_patch`/`bay.battery`,
    # y <= 31), and screws A/C (y=-8) -- no XY overlap with any of them.
    'screws_D12': [
        {'name': 'D', 'xy': (18.0, 58.0)},
    ],
    'usb_shell_z': 14.35,             # screw tip must stay <= 14.1

    # --- pass 16: candidate 5 display mount (ears + S2 boss), no Screen
    # Plate, no P1-P4 ceiling posts. See hardware/case/README.md's pass-16
    # section for the full derivation; summary here:
    #   - Two EARS (S1, S3): each a wall-anchored wedge from its own D1/D2
    #     root (embedded into the true dome wall, same proven
    #     capsule+wedge+core+collar pattern as add_lanyard_corner_block --
    #     mech review F7) reaching inward to the display's own SMT
    #     standoff at S1/S3 (board_standoffs, below) -- generalized as
    #     add_ear() in firefly_case.py.
    #   - One BOSS (S2): a shorter wedge from the WEST wall (NOT a
    #     wall-to-wall crossbar -- mech review F6 found the crossbar
    #     version overlapped the battery connector's own clearance window
    #     by 3.2mm in Y and fully in Z) reaching to S2 -- add_s2_boss().
    #   - `ear_seat_offset`: each seat is printed this much SHORT of the
    #     display module's own real standoff plane.
    #
    #     RE-DERIVED pass 16 (resumed, owner call on Finding 2): the
    #     original number above (`plate_z[1]`, inherited unmodified from
    #     the old Screen Plate across 9 prior passes) was live-probed by
    #     `verify_seat_heights` to be 4.9mm off the display module's own
    #     REAL standoff plane -- the plate had its own separate standoff
    #     posts bridging up to the board, so its top face was never
    #     actually required to equal this plane; the ears/S2-boss seat
    #     DIRECTLY under the board's own standoffs, so they must. Owner
    #     decision: stop inheriting `plate_z[1]`, derive `ear_seat_z`
    #     straight from a live measurement of the real standoff plane
    #     instead.
    #
    #     Live-measured (this pass): `insert_display_pcba()` + a
    #     `find_ceiling_z_at` downward scan (0.002mm step, the same
    #     technique `verify_seat_heights` uses) directly above S1/S2/S3 --
    #     all three read IDENTICALLY, both variants (confirming a flat
    #     standoff plane, as the old comment assumed): **18.80mm current /
    #     21.80mm trim** (world frame; exactly 3.00mm apart, matching
    #     `_DZ_TOP` -- the plane is a fixed property of the display module
    #     itself, shifted by the same amount as everything else tied to
    #     it). This supersedes the coarser 21.75mm figure Finding 2's
    #     own write-up quoted (a 0.1mm-step scan starting from a
    #     seat_z-relative offset landed one grid point low; the finer
    #     0.002mm scan used here, run from a fixed absolute window, is
    #     the trustworthy number).
    #
    #     `ear_seat_z` = standoff plane - `ear_seat_offset` (unchanged
    #     0.25mm convention, mech review F5): 18.80 - 0.25 = 18.55
    #     current; trim overrides below. Moving the seat up ~4.7mm (from
    #     the old 13.85/16.85 numbers) also moves the ears/S2-boss arm up
    #     by the same amount (`ear_arm_thickness` is unchanged, so the
    #     arm's own z-band -- and therefore `_ear_root_z1`'s/
    #     `PILOT_PROTECT_MARGIN`'s downstream caps -- shift with it; see
    #     firefly_case.py's own re-run of those checks this pass).
    'ear_seat_z': 18.80 - 0.25,   # live-measured standoff plane - ear_seat_offset (current); trim overrides below
    'ear_seat_offset': 0.25,
    'ear_foam_disc_mm': (0.3, 0.5),   # BOM note only -- not modeled in Fusion
    'ear_standoff_hole_dia': 2.4,     # M2 clearance -- M2x4 screwed UP from below into the display's own SMT standoff
    'ear_arm_thickness': 3.0,         # mm, the wall-to-standoff arm's own z-thickness below its seat face
    # pass 16 (resumed, Finding 1 fix): each ear's own wall-root anchor is
    # now DECOUPLED from the D1/D2 screws (which moved north, see
    # 'screws_D12' above) -- 'root_xy' keeps the ear physically anchored
    # at the SAME point the old D1/D2 screws used to sit ((+-19, 64), the
    # mechanical review's own live-recomputed rho_from_spine numbers,
    # review-mechanical.md S3.1: reaches the true wall by 3.42mm trim /
    # 4.40mm current, comfortably inside CORNER_BLOCK_REACH=10mm) -- only
    # the SCREW moved, not the ear's own structural attachment point.
    # add_ear() no longer cuts an M2x12 pilot here at all.
    'ears': {
        'S1': {'root_xy': (-19.0, 64.0), 'target': 'S1'},
        'S3': {'root_xy': (19.0, 64.0), 'target': 'S3'},
    },
    's2_boss': {'target': 'S2', 'wall_side': 'west'},
    # mech review F6: keep the S2 boss's own arm a full `s2_battery_clear`
    # (0.6mm) below the display's real battery-connector bbox z0, so the
    # arm's own bulk never reaches the connector's z-band at all --
    # add_s2_boss clamps the arm's own top face to
    # min(ear_seat_z, battery_connector_z0 - s2_battery_clear) and adds a
    # short local riser pad AT S2 itself (radius boss_dia/2, well clear of
    # the connector's own x-range -3.67 max) back up to the true seat.
    's2_battery_clear': 0.6,
    # glass seat (item E, mech review F9): 0.4mm chamfer/fillet at the
    # window step where the display glass's own edge meets the PETG
    # ceiling-underside step it rests on.
    'glass_seat_chamfer': 0.4,
    # battery Y end-stop (item F, mech review F13): a short rib across the
    # rails at the battery's own +y end, stopping shock-load travel along
    # Y (previously only rail friction + strap tension).
    'battery_endstop_w': 1.0,  # see bay.battery's own comment -- matches the 1mm trimmed off battery.y[1]

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

    # --- screen plate: RETIRED, pass 16 (owner call, item A) ---
    # Candidate 5 (see plate-mounting-round2.md / review-mechanical.md /
    # review-id-assembly.md, copied into docs/hardware/ this pass) removes
    # the Screen Plate entirely: the display's own standoffs (S1-S3,
    # `board_standoffs` below) screw directly into two case-integral EARS
    # (S1, S3) and a west-wall BOSS (S2) instead. Every param that only
    # existed to build/verify the plate ('plate_z', 'plate_outline',
    # 'plate_south_extension', 'plate_header_cutout', 'plate_hole_dia',
    # 'plate_pad_dia') and the P1-P4 ceiling posts ('top_posts',
    # 'top_post_dia/z', 'top_post_pilot_dia/z') is deleted along with
    # them -- see firefly_case.py's `add_ear`/`add_s2_boss` (replacing
    # `build_screen_plate`/`add_top_posts`) and the README's pass-16
    # section for the full history (pass 9/9g/15's own P1-P4 rebalancing
    # work, preserved in git history, is superseded by this removal, not
    # contradicted -- it was already diagnostic-only per its own
    # `verify_plate_post_spread` gate). `board_standoffs` (S1/S2/S3) is
    # the one param this whole section shared with the plate design that
    # SURVIVES unchanged below -- same absolute mm positions, now the
    # ears'/boss's own target points instead of plate-hole centres.
    'board_standoffs': {
        'S1': (-12.0, 65.0),
        'S2': (0.04, 32.22),
        'S3': (11.6, 65.46),
    },
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
    # room to actually plug in. needs more room towards the top") was
    # treated as BOTH candidates -- this USB-C tunnel half (usb_tunnel_
    # stadium 7.0->7.9mm, usb_tunnel_center_z 16.4->16.85,
    # usb_liner_outer_stadium 10.2->11.1) AND the Screen Plate battery-
    # connector window (BATTERY_CONNECTOR_TOP_EXTRA, above) -- but Jake
    # clarified afterward that item 2 meant the battery-plug window only
    # ("revert the usb tunnel"). Pass 15b (2026-09-18) reverts this USB-C
    # tunnel half back to its pass-14 values below; the battery-connector
    # window extension (BATTERY_CONNECTOR_TOP_EXTRA = 3.0) is UNCHANGED
    # and kept. See the pass-15b README section.
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
        # pass 16, item F (mech review F13, "nice": no positive Y end-stop):
        # y1 trimmed 32.0 -> 31.0 (1mm) to make honest room for a real
        # end-stop rib (`battery_endstop_w`, add_battery_bay) WITHOUT it
        # overlapping this reference box's own nominal footprint -- a live
        # check_interference run confirmed a real 318mm^3 Bottom-vs-
        # Battery overlap when the rib was added without this trim (the
        # box's own (2,32) span already assumed the cell fills the bay
        # end-to-end with zero slack, leaving no room for any stop at
        # either end without shrinking it). A 30x40x8mm 803040 cell still
        # fits with 1mm to spare (31mm resting length vs. the cell's own
        # 30mm) -- the same margin convention this file already applies
        # everywhere else (e.g. the 0.3mm battery_rail_clear).
        'battery': {'xyz': (8.0, 40.0, 30.0), 'x': (-20.0, 20.0), 'y': (2.0, 31.0), 'z': (2.0, 10.0)},
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
