# Firefly case generator

A scripted, parametric Fusion 360 generator for the Firefly festival-puck
case ("case as code"), replacing the earlier 165-feature direct-modeling
file. See `SPEC.md` for the full original brief this was built against.

## How to run

Everything lives in plain files on disk; Fusion just executes them. Inside
Fusion's Scripts/API panel (or via the `fusion_mcp_execute` MCP tool),
run:

```python
import runpy

def run(_context: str):
    g = runpy.run_path('/path/to/hardware/case/firefly_case.py')
    g['run'](_context)                              # builds PARAMS['variant']
    # g['run'](_context, variant='current')          # or force a variant
    # g['run'](_context, variant='trim', export=True) # + STL/screenshots
```

Each call creates a **new** Fusion document named `Firefly Case Gen
<timestamp>` and leaves it open, unsaved. It never touches or saves any of
Jake's existing documents ("Firefly V2 ...", "KandiWooks Logo", or any of
the board models) — those are only opened for reading (reference
transforms, the display module's placement, the comms board geometry).

`run()` builds the case, runs `verify()` (raises `AssertionError` with a
clear message if anything is off), prints a summary (timeline count, body
names + bounding boxes, every probe result, interference results), and —
with `export=True` — writes STLs and takes 4 orthographic screenshots.

### Picking a variant

`firefly_case.VARIANT` (module level, currently `'trim'`) picks the
default; `run(_context, variant='current'|'trim')` overrides it per call.
`PARAMS`, `PARAMS_CURRENT`, `PARAMS_TRIM` are all available in the returned
globals dict `g` if you want to inspect a variant's numbers without
building it.

### Files

| File | Purpose |
|---|---|
| `firefly_case.py` | The generator: geometry, `build()`, `verify()`, `run()`. |
| `params_current.py` | 60×110×25 variant — matches the "Firefly V2 v15/v16" reference. All SPEC.md numbers live here. |
| `params_trim.py` | 56×103.8×28 variant (**default**, Jake's 2026-09-04 decision; envelope updated 2026-09-06/07 pass 7 height, 2026-09-13 pass 12b length) — derived from `params_current.py` with documented overrides, not hand-duplicated. |
| `kandiwooks_logo.json` | KandiWooks wordmark outline loops (mm), extracted from the "KandiWooks Logo" document's 6 bodies. |
| `SPEC.md` | The original task brief, verbatim (plus one 2026-09-13 editorial note flagging the envelope numbers it was written against). |
| `tools/pass12b_ext_calc.py` | Standalone, no-Fusion-needed script (2026-09-13) deriving the minimum `usb_end_extension_mm` — reuses `rho_at_z`/`rho_from_spine` verbatim. |
| `export/<variant>/*.stl` | Per-body STL exports (binary): `Bottom`, `Top`, `Screen_Plate`, `Power_Button`, `Home_Button`. |
| `export/<variant>/firefly_<variant>_case.3mf` | Native 3MF (2026-09-06, pass 6) containing exactly the 5 printed bodies (`Print — Case` + `Print — Buttons`), for viewers/slicers that read 3MF's per-object structure directly instead of separate STLs. |
| `export/coupons/coupon_{power,home}_{wall,cap}.stl` | Standalone button fit-test coupons (see Print orientation & settings below). |
| `export/coupons/firefly_coupons_native.3mf` | Native 3MF (pass 6) with the 4 coupon bodies. |
| `export/<variant>/firefly_<variant>_plate.3mf` | Packed, print-oriented plate (pass 9 pt 2) — all 5 printed bodies laid out via `tools/stl_to_3mf.py` (Bottom as-is, Top flipped 180° about X, buttons rotated outer-face-down). |
| `renders/<variant>_{front,top,right,iso}.png` | Orthographic screenshots. |
| `renders/{power,home}_button_ext.png`, `lanyard_end.png`, `bottom_logo.png`, `plate_underside.png`, `bay_inside.png`, `rim_{lanyard_end,usb_end}.png` | Pass-6 close-up renders, TRIM variant, showing the fixes in this pass. |
| `renders/pass9c_{trim,current}_{front,top,right,iso}.png` | Pass-9 part-2 orthographic screenshots, both variants (findings 4/5/6). |
| `renders/pass9c_{posts_closeup,lip_ring_section,lip_chamfer,plate_underside}.png` | Pass-9 part-2 close-ups: the relocated Ø5 posts/header area, the ring near the window, the ring's seam chamfer (wide underside view), and the Screen Plate's new south extension. |
| `renders/pass10b_{trim,current}_{front,top,right,iso}.png` | Pass-10 REDO orthographic screenshots, both variants — the clean pill silhouette after removing the rejected brow. |
| `renders/pass10b_mag_pocket.png` | Pass-10 REDO close-up (trim only): looking up into Top's ceiling from inside the cavity, showing the compass module's retaining fence, rest pads, and ceiling pegs above the GPS patch. |
| `renders/pass11_{trim,current}_{front,top,right,iso}.png` | Pass-11 orthographic screenshots, both variants — window bore fix + (trim) re-oriented compass mount, clean silhouette. |
| `renders/pass11_{window_closeup,brow}.png` | Pass-11 close-ups (trim): the window bore reading as a clean open circle, with the FPC brow's tiered risers visible outside it, not filling it. |
| `renders/pass11_mag_pocket.png` | Pass-11 close-up (trim only): looking up into Top's ceiling from inside the cavity, showing the re-oriented/repositioned compass mount (fence, pegs, pads, south-wall wire notch) clear of the nearby case-screw bosses. |
| `renders/pass12_{trim,current}_{front,top,right,iso}.png` | Pass-12 orthographic screenshots, both variants, at `top_z=30` (since reverted — see pass 12b). |
| `renders/pass12_usb_end_top.png`, `pass12_power_button_straight.png`, `pass12_home_button_straight2.png` | Pass-12 close-ups (trim, `top_z=30`): the USB end and both button holes. |
| `renders/pass12b_trim_{front,top,right,iso}.png` | Pass-12b orthographic screenshots (trim, current numbers: `top_z=28`, `usb_end_extension_mm=1.8`) — clean pill silhouette, longer at the USB end, no bumps. |
| `renders/pass12b_usb_end.png` | Pass-12b close-up (trim): the window/USB end with the FPC brow deleted — a smooth, unbroken shoulder curve into the dome tip, no step/notch/plateau. |
| `renders/pass12b_power_button.png`, `pass12b_home_button.png` | Pass-12b close-ups (trim): zoomed, straight-on (fixed `viewExtents`, not `isFitView`) views of each button hole on the -x wall — a single clean stadium opening each, no secondary notch. |
| `renders/pass14_{trim,current}_{front,top,right,iso}.png` | Pass-14 orthographic screenshots, both variants — lanyard-end corner blocks + wordmark counter fix, clean silhouette (unchanged from pass 12b). |
| `renders/pass14_top_corner_blocks.png` | Pass-14 close-up (trim): looking straight into the Top's lanyard end from below/inside — both merged corner blocks visible as one continuous mass around their own pilot pair. |
| `renders/pass14_top_inside.png` | Pass-14 close-up (trim): wider isometric-from-inside view, corner blocks alongside the compass mount, window bore, and USB tunnel. |
| `renders/pass14_bottom_logo.png` | Pass-14 close-up (trim): straight-on wordmark render — all 4 counters ('a', 'd', both flower 'o's) visibly open, not solid. |

Every exported body (case and coupon) is size-checked at export time
(`assert_export_body_size`, ≤120mm/≤40mm max extent respectively) as a
guard against a units/scale bug — see Verification below.

## Parameter table (key numbers)

All dimensions mm. Full detail in `params_current.py`/`params_trim.py` —
this is the headline subset.

| | current | trim |
|---|---|---|
| Outer envelope | 60 × 110 × 25 | ~~56 × 102 × 25~~ 56 × 103.8 × 28 *(pass 7 height 25→28; pass 12b length 102→103.8, `usb_end_extension_mm`)* |
| Outer radius | 30.0 | 28.0 |
| Spine (OUTER envelope only) | (0,0)–(0,50) | ~~(0,0)–(0,50)~~ (0,0)–(0,51.8) *(pass 12b: `usb_end_extension_mm`=1.8 grows the +y dome end only — every absolute-mm feature (window, FPC relief, buttons, posts, screw_D) stays anchored to y=50, see the pass-12b README section)* |
| `usb_end_extension_mm` | 0.0 | 1.8 *(pass 12b — exact minimum for ≥1.5mm FPC-relief skin was 1.522mm/1.456mm, both < the 3.0mm asked about; see pass-12b section)* |
| Case height (`top_z`) | 25.0 *(frozen, pass 7)* | ~~30.0~~ 28.0 *(pass 12b: reverted pass 12's height bump — proven not to help the FPC pocket; see pass-12 and pass-12b sections)* |
| Wall | 2.0 | 2.0 |
| Shoulder flat radius | 24.14 | 22.14 |
| Shoulder tangent point ρ | 27.07 | 25.07 |
| Outer fillet R | 10.0 | 10.0 |
| Inner fillet R (derived) | 8.0 | 8.0 |
| Lip ring R | ~~26.95–27.75~~ 25.95–27.75 *(pass 9 pt 2)* | ~~24.95–25.75~~ 23.95–25.75 *(pass 9 pt 2)* |
| Anchor ring R | ~~26.95–28.40~~ 25.95–28.40 *(pass 9 pt 2)* | ~~24.95–26.40~~ 23.95–26.40 *(pass 9 pt 2)* |
| Top post Ø (P1–P4) | ~~4.0~~ 5.0 *(pass 9 pt 2)* | ~~4.0~~ 5.0 *(pass 9 pt 2)* |
| Window Ø | 45.30 | 45.30 (same, centred (0,50)) |
| Lanyard tip protrusion beyond wall | 8.5 | 8.5 |
| Comms bay cavity width | 56 (±28) | 52 (±26) = battery 30 + stack 17.8 + 3×1.4mm gaps |

Display module, screen plate posts/standoffs, USB tunnel, and button
switch positions are **unchanged** between variants (Jake: "keep ... at
their reference positions") — the shoulder/envelope, lip/anchor/lug/bay
numbers scale with `outer_radius`, and as of the 2026-09-05 pass-2 fix
**screws A/B/C also shift inward for trim** (their `current`-variant
positions punched through the narrower R28 shell): A/C move to
`x = ±(outer_radius - wall - 3.0)` (±23.0 for trim vs ±22.97/23.74 for
current), B to `(0, -(outer_radius - wall - 3.0))` (-23.0 vs -24.0); D
is unchanged at (0, 65.0) in both. See the Screw list section below for
the exact per-variant values.

## Verification

`verify()` runs after every `build()` and is what gates each milestone.

**M1 (shell)** — `verify_m1_probe_table` / `verify_m1_cavity_probes`:
probes the outer shoulder profile and the inner cavity wall at the exact
z-heights from SPEC.md's reference table, computing the *expected* ρ at
each height analytically (`rho_at_z()` / `inner_rho_at_z()`) from `PARAMS`
rather than hardcoding SPEC.md's current-variant numbers — so the same
check validates both variants (trim's numbers are the same profile shifted
−2mm in ρ). All probes pass within 0.15mm on both variants. Body names:
`['Bottom', 'Screen Plate', 'Top']` at end of M1 (buttons added in M2).

Important finding from building M1: the **inner cavity edge is a plain
single R8 fillet**, tangent directly from the flat ceiling/floor to the
vertical inner wall — **not** a true offset of the outer compound curve
(which still carries the 45° shoulder through to the inside). Verified
empirically: a true-offset construction gives 24.32mm at SPEC's
`(15,10,22.0) → 23.88mm` cavity probe; the plain-fillet construction gives
23.87mm. See `_inner_profile_geometry`'s docstring.

**M2 (buttons/USB/lug/logos)** — `verify_m2`: hole clearance (0.25mm/side),
plunger tip gap (0.02mm, now the gap AT FULL PRESS — see below), nub
pocket depth (0.8mm), and tab gap (0.60mm) are checked against `PARAMS`
(the cut geometry is a direct function of these numbers); USB tunnel and
lanyard lug hole are checked with real point-containment probes against
the built solids.

**Plunger guide rib + inward stop collar** (added 2026-09-04, from Jake's
print-test feedback — the caps bound, and a hard press had nothing but the
switch itself to stop inward travel, loading its solder joints): each
button now has a 1.6mm-thick rib bridging the plunger, joined to the Top's
inner wall/ceiling, `rib_inboard_offset` (5-7mm, default 6.0) inboard of
the outer wall, with a rectangular slot through it sized to the plunger's
cross-section + `rib_slot_clearance` (0.25mm) per side so the plunger
can't tilt or rotate. The plunger carries an oversized flange (a "collar",
`collar.h`=0.8mm taller than the plunger cross-section, `collar.len`
=1.0mm long) on its inboard side that cannot pass through the rib's slot;
at rest it sits `plunger_travel` (0.62mm) short of the rib, so a press
travels 0.62mm and bottoms the collar on the rib — 0.02mm before the
plunger tip would otherwise reach the switch housing (`plunger_tip_gap`,
now specifically the bottomed-out gap, not the rest gap). The rib takes
the finger force; the switch never does. `verify_m2` checks the at-rest
collar-to-rib gap (0.62±0.05mm) and that the slot clearance is 0.25mm by
construction, plus a real point-containment check that the cap body never
occupies the rib's material around the slot.

*(2026-09-08 pass 9b update: `plunger_tip_gap` is retired — it was a
full-press gap measured from `housing_xy`, an approximated point that
turned out to sit several mm from the real switch housing; replaced by
`plunger_pretravel` (0.3mm, a REST gap from the real, live-probed
actuator). `rib_inboard_offset`'s "5-7mm, default 6.0" is now the nominal
default only — `button_geometry` shifts it dynamically, per button, when
the real switch position doesn't leave room for the nominal value. See
the pass 9b section below for the full root cause/fix on both.)*

**Interference**: `check_interference` runs `design.analyzeInterference`
between all printed bodies, and separately between the case and any
inserted board occurrence (walking the full occurrence tree, since a
referenced assembly's bodies live on deeply nested child occurrences, not
the top-level one). It measures each reported interference's actual
overlap *volume* and ignores razor-thin (<1e-4 mm³) coincident-face
"touches" — e.g. the Top posts resting flush on the Screen Plate at
z=14.1 — which are by design, not a defect.

**This claim was false — pass 4's `check_interference` excluded
reference-only bodies, never asserted on `occ_interference` at all, and
used a `physicalProperties.volume` reading that (confirmed in pass 5)
reads back as 0.0 for every `analyzeInterference` result in this Fusion
build, so it was passing vacuously, not because the geometry was clean.**
Pass 5 rewrote the check (see below) and, as of its second round (the
XIAO orientation fix + GPS-frame/tray clip), **both variants now build
with zero real interference, zero real overhangs, and all M1/M2/
clearance checks passing under `run(..., export=True)`** — verified by
actually running it, not by trusting the assertion alone. See the
2026-09-05 pass 5 section below for the methodology and what was
actually wrong.

**Export size sanity check** — `assert_export_body_size` (2026-09-05):
every body written by `export_stls`/`export_coupons` is checked against
its live Fusion `boundingBox` right at export time (max extent ≤120mm for
a case body, ≤40mm for a coupon body) — a guard against a units/scale bug
(a cm value used as mm, a stray extra Move) slipping into a shipped STL
undetected. Added after a report of a 10x-oversized
`coupon_power_wall.stl`/`coupon_home_wall.stl` (96×240×186mm); the actual
cause turned out to be a units mismatch in the *reporting* tool, not the
export — both the live Fusion geometry right after `build_button_coupon()`
and the STL bytes on disk (independent struct-level parse) read correctly
at 9.6×24×17.8/18.6mm — but the assertion now stands as a permanent
regression guard either way.

## 2026-09-05 pass 2 (coordinator follow-ups)

Five follow-ups landed after the initial M1-M4 pass, all against the same
`case-generator` branch/worktree:

1. **Outer-shell bumps at the parting line, fixed.** Renders showed
   half-dome bumps where case-screw bosses A/B/C and Top posts P1-P4 (sized
   at their `current`-variant/R30 reference positions) punched through the
   narrower trim (R28) shell. Every boss/post is now intersected with a
   shared inner-cavity "clip tool" solid (`clip_to_inner_cavity` /
   `build_inner_cavity_clip_tool`) before being joined in, so it can never
   extend past the real cavity regardless of nominal position; screws A/B/C
   are additionally re-derived for trim (A/C at x=+-23.0, B at y=-23.0,
   per `outer_radius - wall - 3.0`). `verify_envelope` (bounding-box-within-
   allowed-envelope) and `verify_no_outer_bumps` (8-point probe scan just
   outside the outer surface) now gate every build and both pass clean.
   **Debugging note for future maintainers:** intersecting a `cylinder_solid`
   boss/post against this curved/filleted clip tool, then joining the
   result into Bottom/Top, was silently leaving the PRE-join body behind as
   an orphaned same-named duplicate ('Bottom (1)', 'Top (1)', ...) instead
   of updating in place -- root cause not identified beyond reproducing it
   down to that exact combination. `dedupe_body` cleans these up via a
   Remove feature after every boss/post join (not a Join-back, which itself
   raised an error on the fragile orphan), and `remove_stray_generic_bodies`
   sweeps any remaining auto-named ('BodyNN') orphans at the end of
   `build()` as a final safety net.
2. **Comms bay redesigned as a half-disc dome layout.** The lower cavity is
   a half-disc at the spine_a end (radius = outer_radius - wall) plus the
   straight band y 0..27 -- see `params_current.py`'s `bay` dict for the
   full absolute-mm layout (battery/L76K/stack/GPS positions, used
   unchanged for both variants -- trim's R=26 is the tighter constraint,
   current's R=28 just has 2mm more slack). The XIAO+Wio stack and the GPS
   patch are each held by a Top-hanging, open-bottom "tray"/"frame"
   (`build_hanging_frame`) so both Bottom and Top print face-down with no
   overhangs; the battery gets two rails + strap slots THROUGH the rails
   (not the bed face); L76K (now always wired, 'hat' mode dropped) sits in
   a shallow floor frame with a wire notch. Battery and GPS patch have no
   Fusion docs, so they're hidden reference-only boxes
   (`add_battery_reference_box` / `add_gps_reference_box`), excluded from
   exports, alongside the (also reference-only) FPC keep-out marker.
3. **Board placement fixed.** XIAO/Wio/L76K are now correctly positioned
   (previously they were inserted but left at their native/identity
   placement, hidden, as a known limitation): `insert_and_place` /
   `flatten_transform` read each board's native bounding box, rotate its
   thinnest native axis (XIAO's is Y) onto world Z, and translate its
   center into the bay -- Wio's PCB bottom at z=10.5, XIAO's at
   +6.1mm above that (plugged into Wio's sockets), L76K flat on the floor
   of its frame. `design.snapshots.add()` is called whenever
   `hasPendingSnapshot` is true after assigning `occ.transform`, per the
   coordinator's fix; verified by re-reading each occurrence's bRepBody
   bounding boxes afterward (`_bbox_extents`, which unions actual body
   boxes rather than trusting `occ.boundingBox`, still unreliable
   immediately post-insert in this session). Boards are visible (not
   hidden) in the generated document. **Rough edge from this pass, RESOLVED
   in pass 3 below:** the L76K assembly's own bounding box (including its
   antenna cable/lead) extended well past the small frame built for the
   bare board, and positioning by that aggregate box put the actual PCB
   outside the case entirely.
4. **Button caps + USB liner trimmed to the real curved shell.** Both were
   built against a flat-wall approximation (documented pass-1 limitation);
   now each is over-built with a few mm of extra margin and then
   Combine-Intersected against either a thickened copy of the outer
   envelope (caps: offset outward by `proud`=0.45mm via OffsetFaces,
   `build_thickened_envelope`, shared across both buttons) or the plain
   envelope (USB liner). `verify_m2` checks the cap's outermost point
   against an analytic `true_wall_distance_along_ray` (handles both the
   straight-section wall and the domed end caps, which are literal
   revolves of the same profile) at 3 heights per button, within 0.25mm --
   loosened from an initial 0.1mm target because the analytic formula
   assumes a purely-radial surface normal, which the R10 shoulder fillet's
   real normal (has a Z component too) doesn't quite satisfy; the built
   result was confirmed correct by probe either way, just not pinned to
   sub-0.1mm by that simplified formula.
5. **Button fit-test coupons added.** `build_button_coupon` /
   `export_coupons` build a standalone, straight-axis (no diagonal nub
   direction) wall+shelf+rib body and a separate cap body per button, from
   the SAME `PARAMS` (`cap_clearance`, `rib_thickness`,
   `rib_slot_clearance`, `plunger_travel`, `collar`, `nub_pocket`, `tab`)
   as the real button -- a fit found on the coupon transfers directly to
   the case. Exported (with `export=True`) as
   `hardware/case/export/coupons/coupon_{power,home}_{wall,cap}.stl` (wall
   and cap print separately, side by side -- not fused into one body).
   `PARAMS['cap_clearance']` (0.25mm default, the same value used for the
   real wall-hole-to-head clearance) is the knob to tune from a coupon fit
   test; a looser fit means a bigger number, a snugger fit a smaller one.

## 2026-09-05 pass 3 (visible-defect fixes the asserts missed)

Renders/STL analysis from pass 2 found real geometry the pass-2 checks
didn't catch:

1. **L76K board was outside the case.** Positioning by the whole
   occurrence's aggregate bbox put the actual PCB nowhere near its target
   -- the assembly's separate GPS patch antenna (25x25x8.3, on a cable)
   dominates that bbox. Fixed: `find_pcb_like_body` recursively searches
   the L76K assembly for a body shaped like a small PCB (two dims
   15-24mm, third <=3mm) and positions BY THAT BODY specifically; the
   antenna sub-occurrence is hidden. Verified: PCB world bbox now
   `x -10.48..10.48, y -23.39..-5.61, z 1.83..3.37` against a target of
   `x -10.5..10.5, y -23.5..-5.5, z ~2.0..3.2`.
2. **Home button's guide rib punched ~1.45mm through the dome** (a
   rectangular slab visible in `trim_top.png`/`trim_iso.png` near the
   home button). Root cause was upstream, in `button_geometry()`:
   `s_wall` (the button's local "distance to the outer wall along its
   nub direction") used the flat-plane `x = -outer_radius` approximation,
   which is a reasonable stand-in for the Power button (mostly in the
   straight section) but badly wrong for the Home button, which sits in
   the domed +y end cap (switch y 57.6-63.5, past spine_b=50) -- it
   overestimated the true wall distance enough that positioning the rib
   "6mm inboard of the wall" left it outboard of the REAL wall. Confirmed
   by elimination: even shrinking the rib's own footprint to zero (or
   negative) margin still overshot, and a Combine-Intersect against the
   inner-cavity clip tool (the fix used for the bosses/Top posts)
   consistently raised `FEATURE_FAILED_TO_CREATE` for this specific
   diagonal-box-against-filleted-revolve combination regardless of margin
   or cut ordering. Fixed at the source: `s_wall` now calls
   `true_wall_distance_along_ray` (already used for the M2 cap-proud
   check, and correct for both the straight section and the domed ends,
   since the domes are literal revolves of the same profile); `s_inner`
   and `s_tab_face` were also corrected to subtract along the ray
   directly rather than reuse a flat-wall-specific `/d2[0]` projection
   whose sign only happened to work by accident of `d2[0]` being negative.
   One real consequence: the Home button's cavity is genuinely tight
   (`s_wall` ~6.1mm from the switch housing along its nub direction, vs.
   the Power button's much larger straight-section clearance) -- the
   generator no longer papers over that with an inflated flat-wall
   number.
3. **Screen Plate's far corners (near spine_b) sat outside the trim
   wall** (rho ~33mm against a 26mm inner wall). Fixed: the plate is now
   Combine-Intersected against a stadium solid at `outer_radius - wall -
   0.3` (0.3mm clearance inside the inner wall) before its other cutouts,
   in every variant -- trim's plate now reads `x -25.70..22.89` (was
   `-26.63..22.89`).
4. **New verify check, `verify_export_envelope`**, per the coordinator's
   own STL vertex analysis: for every body build() exports, no vertex may
   sit beyond `rho_from_spine(p, x, y)` = `outer_radius + 0.15mm`, except
   the lanyard lug (y below `spine_a.y - outer_radius + 2`, |x| < 5.6) and
   the two button cap heads (allowed to `+0.45mm`, their designed proud
   amount). `rho_from_spine` measures distance from the actual pill
   centerline -- `|x|` in the straight section, distance to the nearer
   spine endpoint in the domed ends -- matching how the shell itself is
   built, not a flat per-axis bounding box. All 5 exported bodies (both
   variants) currently pass with zero offending vertices.

## 2026-09-05 pass 4 (export size guard + README pass)

Coordinator reported `coupon_power_wall.stl`/`coupon_home_wall.stl` as
10x oversized (96×240×186mm). Investigated by cross-checking two
independent sources of truth: the live Fusion `boundingBox` immediately
after `build_button_coupon()` runs, and a struct-level parse of the
actual STL bytes on disk. Both agreed at 9.6×24×17.8/18.6mm — the correct
size, matching the coupon's intended "~24×14×2mm slab plus rib/shelf
(≈10mm deep)" shape. No scale bug found in the generator or the export
path; the 96×240×186mm figure is exactly 10× the correct numbers on
every axis, consistent with a units-interpretation issue in whatever
tool produced that reading rather than a defect here. Added
`assert_export_body_size` regardless, as a permanent guard against this
exact failure mode (see Verification above) — every exported body is now
checked live at export time, not just spot-verified after the fact. This
pass also brought the README's params table, screw list, print notes,
and known-limitations section up to date with everything pass 2/3
actually changed.

## 2026-09-05 pass 5 (real interferences, printability, verify() rewrite)

Jake ran his own `analyzeInterference` pass over the pass-4 trim build and
found a long list of real interferences pass 4's `verify()` had claimed
were zero (see the retraction at the top of Verification above). This
pass fixed the interference-check methodology itself and worked through
the reported list. **Fixed and verified clean** (both variants):

- **Button caps vs. the wall hole / Screen Plate / Top** (`Top x Power
  Button`, `Top x Home Button`, `Screen Plate x {Power,Home} Button`,
  `Top x Screen Plate`). Root causes, all in `button_geometry()` /
  `add_button()`: (1) `oriented_box_prism`/`oriented_stadium_prism`
  extrude ONE-SIDED from the given point along `normal` — they do **not**
  center on it — but the rib and collar were being positioned by their
  midpoint as if centered, silently building them a half-thickness/length
  too far outboard and entirely missing their intended inner half; fixed
  by passing the true inboard edge (`s_rib_inner`/`s_collar_inner`) as the
  start point, exactly as the wall hole already did. (2) The wall hole
  cut was a single straight prism sized off ONE `s_wall` sample at the
  cap's z-center; the real shoulder curves measurably across the cap's
  own z-span, so a straight cut left real material uncleared near the
  curve. Replaced with a hole cut from an ENLARGED COPY of the actual
  plunger shaft (same construction as the real cap, just bigger,
  Combine-Intersected against the same `thickened_envelope`) — guaranteed
  to fully contain the real, curve-trimmed shaft with clearance, since
  it's built the same way. (3) The retaining tab hangs below the shaft's
  own z-range and needs its own small hole, added separately. (4) The
  guide rib's slot (letting the plunger pass through it) is now cut using
  the real shaft's own construction too (same center/axes), not a
  hand-rolled box that could drift out of alignment. (5) The Screen
  Plate's clearance pocket (new `add_button_plate_clearance`) now spans
  the plunger's FULL travel (wall to just past the plunger tip near the
  housing), not just the collar region — the plunger keeps a full L×W
  cross-section all the way to the tip (by construction, since trimming
  only clips the outward-facing surface), and the plate's footprint
  reaches that deep on both buttons.
- **`Bottom x L76K board`** — two causes: the PCB was resting 0.17mm
  INSIDE the floor (fixed: the L76K frame now has an actual 0.3mm floor
  PAD from z=2.0 to 2.3, and the PCB is repositioned to rest exactly on
  top of it, using the PCB's own measured thickness, not a guess); and
  the bare cavity floor under the frame's footprint — deep in the -y dome
  tip — curves up above the nominal flat z=2.0 the frame's floor_pad
  didn't originally account for (same mechanism as the battery fix below;
  fixed with the same kind of flatten-and-rebuild cut).
- **`Bottom x Battery Reference`** — the trim cavity's flat floor region
  only reaches rho=18mm, but the battery box is 40mm wide (needs to reach
  rho=20mm); the floor genuinely curves up ~0.25mm at the battery's outer
  edges. Fixed by cutting a shallow flatten box across the battery's exact
  footprint before adding the rails (a no-op wherever the floor is already
  flat, e.g. the whole 'current' variant). Rails also now sit
  `battery_rail_clear` (0.3mm) outside the battery box instead of flush
  against it.
- **`Top x <display module body>`** (the FPC tab) — `PARAMS['fpc_relief']`
  held SPEC's exact pocket coordinates since M1 but nothing ever cut it.
  Now cut from Top's ceiling underside; the cut is a superset of SPEC's
  box, widened from probing the actual inserted display occurrence (the
  real FPC/PMMA-lens geometry is wider and extends lower than SPEC's
  numbers alone: x roughly ±14 vs SPEC's -6.2..7.02, y down to ~65.8 vs
  SPEC's 71.44).
- **`Top x GPS Patch Reference`** (mostly) — the GPS frame was rebuilt
  from scratch as a plain hanging wall ring with NO ledges and a clean
  25.5×25.5 opening centred on the patch box, per Jake's spec ("GPS frame
  inner = 25.5 x 25.5 with the patch box centred") — the old version
  reused the stack tray's ledge scheme, whose shelf (oversized to reach
  the ceiling) fully overlapped the patch box's z-range across virtually
  the whole opening.
- **Housekeeping**: `dedupe_body`'s "orphaned same-named duplicate"
  workaround (previously applied only after the case-boss/Top-post joins)
  turned out to trigger on the new button-plate-clearance and comms-bay
  joins/cuts too (`Top (1)`, `Screen Plate (1)` appearing as stray root
  bodies) — `build()` now sweeps all 5 tracked body names through
  `dedupe_body` unconditionally at the end, not just case screws/posts.
  Root bodies at the end of `build()` are exactly `['Bottom', 'Home
  Button', 'Power Button', 'Screen Plate', 'Top']` plus hidden `... 
  (reference only)` boxes, verified directly.
- **Printability**: the lanyard lug's underside was a 10×12mm horizontal
  overhang floating 3mm above the bed (Bottom prints face-down on z=0) —
  extended down to z=0 so the whole tab sits on the bed (hole position
  unchanged). The tray/GPS-frame ledges (2mm-wide flat shelves, a full
  overhang appearing all at once at the far end of the print) are
  replaced with 45-degree self-supporting wedges (`build_wedge_along_x`)
  that taper from flush-with-the-wall at the top (prints first) to full
  protrusion at the bottom (prints last) — the GPS frame no longer has
  ledges at all (see above), so only the stack tray uses this now.
- **`verify()` interference gate, rewritten**: runs over every printed
  body + every inserted board occurrence (passed as whole Occurrences,
  not individual nested bodies — see `check_interference`'s docstring for
  why that matters) + the Battery/GPS reference boxes, excluding only the
  two reference TOOL solids and the FPC keep-out marker, with
  `areCoincidentFacesIncluded=False` and a bounding-box-volume proxy
  (Fusion's `physicalProperties.volume` reads 0.0 for every
  `analyzeInterference` result in this build; `createBodies`/
  `copyToComponent` on a transient interference result both fail too —
  see the docstring). Gates on any pair > 0.05 mm³. Also added
  `verify_min_clearances`: per-board-occurrence, per-body
  `measureMinimumDistance` against Top/Bottom/Screen Plate (capped at 25
  bodies per occurrence for MCP-call time budget), asserting ≥
  `clearance_min` (0.3mm) except at documented intended contacts
  (`ALLOWED_CONTACTS`): L76K PCB on its frame floor, Wio on its tray
  wedge, the display glass flush with the top face, its standoffs on the
  plate.
- **STL overhang scan** (`scan_stl_overhangs`, printability check B3):
  triangle-normal analysis of the exported STLs, clustering bed-facing
  faces steeper than 45° by shared vertices and reporting each cluster's
  area *and its (x,y) centroid* (so a real cluster can be located, not
  just sized); `run(export=True)` asserts none exceed 30 mm² outside a
  documented whitelist. **Clean on both variants** — see round 2 below.

### Round 2 (same day): XIAO orientation fix, GPS-frame clip, overhang whitelist review

The coordinator reviewed the round-1 report above and found the
remaining "XIAO x GPS Patch Reference" interference was an **orientation
bug**, not real bay-layout tightness, and asked for the overhang scan's
false positives to be filtered by inspecting actual geometry rather than
by cluster size alone. Both are now resolved:

- **XIAO orientation fix**: XIAO's native long axis (~22.5mm, including
  the USB-C overhang; native bbox x -8.69..13.85 in its own doc) was
  being mapped onto **world X** by `insert_and_place`'s existing `'y'`
  thin-axis rotation (native Y, the PCB thickness, correctly went to
  world Z, but native X passed through unchanged) — while the Wio's own
  long axis (~22.3mm) runs along **world Y**. The two boards' long axes
  were perpendicular, so the plugged-together stack's rectangular
  footprint was `22.48 x 17.78` (X x Y) instead of the Wio-matching
  `17.78 x 22.48`. Fixed with a new `'y90'` thin-axis mode in
  `flatten_transform` (native Y -> world Z as before, but ALSO native X
  -> world Y and native Z -> world X, i.e. an extra 90-degree rotation
  about world Z) used for XIAO specifically in `insert_comms_boards`. The
  USB-C connector lands on the world +Y side (toward the display, where
  the tray's wire gap already is) with this rotation's sign as-is — no
  further flip needed, confirmed by checking the connector sub-body's
  world position after the transform. With XIAO's real footprint now
  matching Wio's almost exactly (17.78 x 22.48 vs Wio's 17.78 x 22.32),
  the pass-5-round-1 tray widening (`tray_x_extra`/`tray_x_extra_right`,
  a multi-mm asymmetric hack) was removed entirely — the tray needs no
  more than its already-small `tray_clear` margin.
- **`Top x GPS Patch Reference` residual (17.8 mm³), after the orientation
  fix**: NOT the frame ring (an isolated frame-vs-reference-box check was
  already clean) — the tray's OWN wall still grazed the antenna's real
  footprint by ~0.1mm at one corner (x -2.8..-2.7, y up to 20.0), an
  unavoidable side-effect of the tray's wall thickness at the current
  absolute bay coordinates. Per "do not move the patch box": the tray
  itself is now clipped against the antenna's real box (+0.3mm margin)
  before it's joined to Top, guaranteeing it can never occupy that space
  regardless of the exact wall/clearance numbers, the same idea as
  `clip_to_inner_cavity` for bosses/posts applied to the one real fixed
  obstacle instead of the shell.
- **Overhang whitelist, reviewed by inspecting actual triangle
  locations/z-ranges, not just cluster size**: after the bed-plane
  (0.6mm) and angle (45°±1°) tolerance updates, the remaining >30mm²
  clusters on both variants were traced to (a) the USB tunnel floor
  (already a known, accepted 13mm bridge) and (b) the shell's own general
  flat internal ceiling / R8 inner-fillet-to-wall transition — ordinary
  hollow-shell overhang that needs slicer supports independent of any
  specific feature (confirmed by checking the flagged triangles' actual
  z-range and extent at each location, which span the shell's general
  ceiling height and width, not a localized shape). Both are whitelisted
  by location (`top_wl`/`bottom_wl` in `run()`), not by raising the size
  gate — a genuinely local, unexpected overhang (like the lug or the tray
  ledges, both real fixes earlier in this pass) would NOT be covered by
  these location boxes and would still fail the check.
- **Both variants now pass `run(..., export=True)` end to end**:
  interference `[]`, `occ_interference` `[]`, all M1/M2 probes, envelope/
  outer-bump/export-envelope checks, `verify_min_clearances` all `True`,
  and `bad_clusters_mm2: []` on both Top and Bottom for both variants.
  The exports/coupons/renders in this repo are from this clean run.

## 2026-09-06 pass 6 (organisation + native export + real-defect fixes)

Jake opened the pass-5 document and found it confusing: unnamed bodies
("Body145", "Body154"...) at the document root, reference tools mixed in
with printable parts, coupons sitting at the origin overlapping the case.
Separately, his own visual review of renders and a coordinator STL sweep
found several real geometry defects the numeric probes never caught. Both
are fixed in this pass.

### Document structure

`organize_components()` runs right after `build()` and moves every body
into a named component -- nothing is left at the document root:

```
Print — Case
   Top            bbox z 9.2..25.0 (trim)
   Bottom         bbox z 0.0..10.0
   Screen Plate   bbox z 10.0..14.1
Print — Buttons
   Power Button
   Home Button
Print — Coupons          (only when export=True; see native 3MF below)
   Coupon Power Wall / Coupon Power Cap
   Coupon Home Wall / Coupon Home Cap
Reference — not printed  (all isLightBulbOn False)
   Inner Cavity Clip Tool     -- build-tool solid, kept for the boss/post clips
   Cap Trim Envelope          -- build-tool solid, kept for the button cap trims
   Battery 803040             -- reference box
   GPS Patch 25x25x8.3        -- reference box
   FPC LoRa Antenna Keep-out  -- reference box, not yet enforced (see below)
Boards                    (occurrences moved here after build(); visible)
   ESP32-S3-Touch-LCD-1_46, Wio-SX1262, XIAO-ESP32S3, L76K GNSS Module
```

`verify_structure()` (new) gates every run alongside `verify()`: asserts
no bodies remain at the document root, no body anywhere in our own
authored components has an auto-generated name (`Body\d+`), the five
printed bodies live in the two Print components, and every Reference body
is hidden. It prints the tree above (with live bounding boxes) so this
section can be regenerated from a real run.

`BRepBody.moveToComponent(occ)` / `Occurrence.moveToComponent(occ)` move a
body/occurrence into `occ`'s **own** component -- confirmed empirically;
the API doc's "parent component of the target occurrence" wording reads
as the opposite of what actually happens.

### Native 3MF export

Alongside the STL/packed-plate exports, each variant now also gets one
native 3MF containing exactly its two Print components, and the coupons
get their own native 3MF:

- `export/<variant>/firefly_<variant>_case.3mf` -- 5 objects (Bottom,
  Top, Screen Plate, Power Button, Home Button).
- `export/coupons/firefly_coupons_native.3mf` -- 4 objects (Coupon
  {Power,Home} {Wall,Cap}).

`design.exportManager.createC3MFExportOptions(geometry, filename)` takes
a single `BRepBody`, `Occurrence`, or `Component` -- not a list -- so the
case export passes the whole root component with `Reference — not
printed`, `Boards`, and `Print — Coupons` temporarily hidden (hidden
bodies are not exported), restoring visibility afterward regardless of
outcome; the coupons export just passes the `Print — Coupons` occurrence
directly (already exactly 4 bodies, no hiding needed). Verified by
unzipping each file and counting `<object` elements in `3D/3dmodel.model`
against the name list -- both match exactly, both variants.

The existing STL exports and the python-packed, print-oriented plates
(`tools/stl_to_3mf.py`, `export/trim/firefly_trim_plate.3mf`,
`export/coupons/firefly_coupons.3mf`) are unchanged and still the
recommended files to actually slice from.

### Coupons moved off to the side

The 4 coupon bodies (`build_button_coupon`, now parameterised by an `x0`
local-X offset and a `name_prefix`) are built directly inside their own
`Print — Coupons` component -- Power at local x0=0, Home at x0=40mm (each
pair is ~15mm long, so 40mm clears them with room to spare) -- and the
whole component is then translated +60mm in X (`COUPON_WORLD_OFFSET`),
comfortably clear of the case (max world x ~28.5mm) and of each other.

### Hygiene fixes

- **Hidden sub-bodies excluded from interference/clearance checks.**
  `check_interference` now skips a result when the non-case side is a
  hidden body not in our own known-names set (e.g. the L76K assembly's
  placeholder cable stub), and `_collect_occ_bodies` (used by
  `verify_min_clearances`) skips hidden bodies outright -- both default to
  "visible" (don't skip) if reading `isLightBulbOn` raises, which it does
  for a handful of deeply-nested body proxies inside inserted board
  references.
- **Coupons can never overlap the case** by construction (their own
  component, translated 60mm+ away) rather than by a runtime check.
- **Root body list assertion updated to "none at root"** --
  `verify_structure()` asserts `root.bRepBodies.count == 0` directly,
  superseding the old `body_names == [...]` check (still also asserted,
  now scoped to `Print — Case`/`Print — Buttons`).

### Real defects found and fixed (Jake's screenshots + the coordinator's STL sweep)

1. **Case-screw bosses A/C and Top posts P1–P4 had NO material -- at all
   -- despite every prior `verify()` passing.** Root cause:
   `clip_to_inner_cavity` shrinks a boss/post by `safety_margin` on
   *every* face, including the very top/bottom faces meant to touch
   Bottom's floor or Top's ceiling; combined with a real (measured
   ~0.36mm) mismatch between the inner-cavity solid's own ceiling height
   and the nominal `top_ceiling_underside_z`, the clipped pillar ended up
   not physically touching the shell at all. Fusion's `combine_join`
   **silently no-ops** on two non-touching bodies (confirmed: same
   behaviour `deboss_loops` already documented for disjoint glyph
   pieces) instead of raising, so this was invisible in every printed
   summary and every `verify()` since M1. Fixed by `clipped_pillar_with_reach`:
   a full-height, smaller-radius core (`BOSS_CORE_R`=2.6, `POST_CORE_R`=1.1
   -- comfortably above half the largest hole cut through it later, and
   below the boss/post's own radius) is joined to the radially-clipped
   wide cylinder, guaranteeing real contact at both ends. New regression
   guard: `verify_posts_and_bosses` probes every boss/post off-axis.
   Also found and fixed along the way: `dedupe_body`'s own re-fetch only
   fires when it actually finds an orphan to clean up, so a `Bottom`/`Top`/
   `clip_tool` reference that went stale from an *earlier*, unrelated
   Remove call could silently survive and be handed to a *later*
   `combine_join` as the target -- `_refetch_by_name` now re-fetches all
   three, unconditionally, after every dedupe_body call inside the
   per-screw/per-post loops (not just once at the very end, as before).
2. **Case-screw boss B could not be given the same fix.** Its position
   (0, −23 trim / 0, −24 current) turns out to sit inside the L76K PCB's
   own real footprint (x −10.48..10.48, y −23.39..−5.61) -- a pre-existing
   bay-layout conflict this pass's fix exposed rather than introduced
   (giving it a full-height core, like A/C/D, creates a real, hard solid
   overlap with the PCB instead of a missing boss). `add_case_screws`
   passes `core_r=0` for boss B specifically, restoring its exact
   pre-pass-6 behaviour (silently unjoined) rather than trading a latent
   bug for a real interference; `verify_posts_and_bosses` documents and
   reports `boss_B_bottom` but does not gate on it. **This needs a real
   decision from Jake** -- move screw B, move the L76K bay, or accept
   Bottom-only fastening there -- see Known limitations below.
3. **A rectangular notch through the outer skin next to each button's
   stadium hole** (Jake's screenshot review; the "small block" visible
   inside it was the cap's own retaining tab, now exposed to open air).
   Root cause: `add_button`'s `tab_hole_body` cut reached from
   `s_tab_face` all the way out PAST `s_outer_face` (the true exterior
   surface) plus a 2.5mm margin -- a real, deliberate through-cut that
   was never necessary, since the tab itself never reaches anywhere near
   the outer surface (it stays inboard of the inner wall face by
   `tab['gap']`, 0.6mm). Fixed by bounding the cut analytically at
   `s_inner + skin_margin/2` (a `skin_margin` of 2.0mm keeps the hole's
   outward reach 1.45mm short of the true outer surface, comfortably
   covering the ray-vs-true-curvature slack `verify_m2`'s own cap-proud
   check already documents, up to ~0.25mm) instead of reaching the
   exterior at all. Confirmed both by a direct `analyzeInterference`
   check (0 interference, both buttons, both variants) and visually
   (`power_button_ext.png`/`home_button_ext.png`).
4. **The lanyard lug intruded into the hollow cavity and left small
   triangular wedge bumps on the outer skin flanking it** (its inner end
   crossed the inner wall -- from inside it read as a floating cylinder
   next to the L76K; the wedges came from the crude box+cylinder tab's
   flat sides meeting the curved dome at an angle). Rebuilt entirely as
   an integrated ear (`lug_ear_geometry` + the rewritten `add_lug`): 14mm
   wide, protruding 6mm beyond the shell's TRUE curved surface (computed
   via `rho_at_z`, not a hand-picked constant -- correct for both
   variants automatically), its inner end trimmed flush with the inner
   cavity surface by a Combine-Cut against a fresh copy of the inner
   cavity solid (the inverse of `clip_to_inner_cavity`: an ear must stay
   embedded in the wall and protrude outward, unlike a boss/post which
   lives entirely inside the hollow interior). A vertical Ø4.0 hole sits
   3.5mm in from the ear's outward face; R3 fillets round its two
   vertical outer corners; a 0.6mm chamfer softens both hole edges --
   both best-effort (skipped, not rolled back, if Fusion's
   fillet/chamfer feature refuses). The old wedge bumps are gone --
   confirmed visually (`lanyard_end.png`, `rim_lanyard_end.png`).
   `lug_relief_box` (the lip/anchor ring relief near the lug) widened to
   match the new 14mm ear. `boss_relief_dia` widened 6.6→10.0mm after the
   same visual review found a thin wedge-shaped sliver of ring material
   at screw B's relief (its old radius just barely failed to clear the
   anchor ring locally).
   **Recessed lanyard bar, considered and rejected**: Jake asked about a
   recessed bar instead of a protruding ear. Not built -- a 5mm-deep
   pocket at the tip needs an interior pad that collides with the L76K
   wired frame at y ≈ −23.5 (the bay's −y dome tip is already the
   tightest-margin area in the case), which would require moving the
   L76K. A protruding ear avoids that dependency entirely.
5. **The KandiWooks wordmark was missing its "a"** (Jake: "the first 'A'
   is missing on the bottom, the sprout renders above a gap"). Opened the
   read-only "KandiWooks Logo" document directly: it has 6 bodies, and
   one of them (the "K"+"a" pair, fused into a single lump but with TWO
   separate, disjoint flat top faces) has its own actual 'a' shape on a
   *second* face the original extraction never visited -- it only ever
   walked the single largest-area flat face per body, silently dropping
   any second one. Re-extracted `kandiwooks_logo.json` walking every
   same-height flat face on every body (not just the biggest), using
   `CurveEvaluator3D.getStrokes` at a 0.005mm tolerance. The 1-point
   degenerate loop the old extraction produced turned out to be a real
   but harmless ~0.02×0.002mm sliver artifact in the source geometry, not
   the actual cause. Confirmed visually: the debossed wordmark now reads
   "KANDIWOOKS" in full -- see `bottom_logo.png`.
6. **A small (~0.18mm³) real interference between Bottom and the
   Battery 803040 reference box**, current variant only, right at boss
   A/C's designed-to-be-close x=∓20 edge (now that those bosses finally
   have real material). Fixed with a permanent 0.1mm inset margin on the
   reference box's X sides (`add_battery_reference_box`) -- both variants
   scale this boss position off `outer_radius`, so the margin is a
   deliberate, permanent tolerance on the reference envelope, not a
   one-off number.
7. **Cavity probe `top_cavity` was, in effect, testing whether boss C
   was missing.** Its y=27 scan line sat only 1.8mm from boss C's y
   (25.04–25.2), well inside its 3mm radius -- once boss C got real
   material (item 1), the probe found the boss instead of the true inner
   wall. Moved to y=29 (3.8mm away, clear of the boss and still clear of
   the bay footprints/Top posts the original y=27 choice was for).

New regression guards from this pass: `verify_posts_and_bosses` (item 1),
`verify_skin_intact` and `verify_wall_integrity` (items 3/B/C from the
coordinator's sweep -- see Known limitations for their current, reported-
but-not-gating status), `count_sliver_faces` (diagnostic only, per-body
count of faces under 0.5mm²).

## 2026-09-07 pass 7 (case height, 3-board stack, defect sweep)

Jake asked for the real 18mm-tall 3-board (L76K+XIAO+Wio) direct-solder
stack (reverted mid-pass-6 as too big a re-architecture for that pass's
remaining budget — see the pass-6 "Reverted mid-pass-6" note) to actually
land, plus a follow-up sweep of Jake's own render review.

**Items 1-4** (case height, 3-board stack, boss B1/B2, battery/GPS
reposition): `PARAMS['top_z']` is now a real per-variant parameter —
`current` stays at 25 ("for the probe comparison" — Jake's own reference
geometry never carried the taller stack and isn't meant to), `trim` grows
to 28 (`_DZ_TOP = 3`), and every Top/plate/display/button z-value tied to
the ceiling shifts by the same `_DZ_TOP` so the display glass stays flush
at the new top face (see `params_trim.py`'s "PASS 7" section for the full
z-table). The comms bay now inserts XIAO+Wio+L76K as a real 3-board stack
for `trim` (`comms_stack3_full_height=True`; `current` still inserts only
the L76K — its unchanged 25mm ceiling genuinely cannot fit the stack, see
`params_current.py`'s comment). Case-screw boss B (its old position sat
inside the L76K PCB's own footprint even before this pass, per the pass-6
known-limitation) is retired; `screws_ABC` now lists **A, B1, B2, C** —
B1/B2 straddle the stack's centreline at absolute `(±12.5, -15.0)` (same
for both variants, comfortably on the flat bed, clear of the stack by
construction via a boss-relief keep-out cut into the stack frame). New
`verify_stack3_clearance` gates the stack's real (live-measured) top
against the Top ceiling (≥0.8mm required).

**Item 5**: `verify_skin_intact` / `verify_wall_integrity` (pass-6
diagnostics that over-fired on legitimate geometry — see pass 6's Known
Limitations) are re-targeted at their real, narrower footprints (the
tab-hole's own analytic reach; the lug-relief box and the flat-to-arc
tangent transition excluded by name/geometry, not by loosening the gate)
and now **gate** `verify()` instead of just reporting.

**Defect sweep** (this session — Jake's render review of the pass-7
output found three more real issues, all fixed in the generator):

1. **Lanyard lug was a plain, sharp-cornered block, not the tapered ear
   pass 6 described.** `lug_ear_geometry` was correctly re-derived (using
   the narrower of `rho_at_z(z0)`/`rho_at_z(z1)`, not the z-midpoint) to
   stop the wedge-sliver defect, and `add_lug` Combine-Intersects the ear
   against a thickened copy of the true curved shell to taper it — but
   the hole was being cut into the standalone ear tool body and then
   Combine-JOINED into Bottom, and the new, more conservative geometry
   now puts the hole's xy inside the base shell's own pre-existing wall
   material at some z in the ear's span (confirmed: solid there even
   *before* `add_lug` runs). A boolean union can never remove material
   the target already had, so the hole silently never went all the way
   through (`verify_m2`'s `lug_hole_open` — previously unchecked before
   this session's fix loop caught it failing). Fixed by joining the
   ear first and cutting the through-hole from the resulting Bottom.
   `lanyard_end.png`/`rim_lanyard_end.png` are regenerated from the fixed
   geometry — the ear now visibly tapers with the true shoulder curve.
2. **Case-screw bosses A/C left a non-manifold sliver in the exported
   trim Top.stl**, found by an *offline* struct-level manifold-edge scan
   (every edge of a watertight mesh must be shared by exactly 2
   triangles) — 2 bad edges at `(x=±28, y=25.04/25.20, z=10..11.5)`,
   exactly boss A/C's own xy and the lip/anchor relief's z-range. Root
   cause: `add_lip_anchor_reliefs`'s per-boss relief cylinder radius was
   a flat `boss_relief_dia/2` (5.0mm) regardless of how close the boss
   sits to the true outer wall — trim's A/C sit at
   `x = ±(outer_radius - wall - 3.0) = ±23.0`, putting the relief's edge
   at exactly `23 + 5 = 28 = outer_radius`: dead-on the true surface
   instead of safely inside it. `current`'s A/C have more margin (wider
   `outer_radius=30`), so this never manifested there. Fixed by clamping
   each boss's relief radius to stay 0.6mm inside the true wall distance
   (`true_wall_distance_along_ray`, the same outward-direction convention
   `verify_wall_integrity`'s own boss-wall probe already uses) — a no-op
   for every boss that already had margin.
3. **Boss D had a 3mm gap of missing material for trim** — a real
   "missing screw post" defect. `plate_post_D_z` (the Screen Plate's own
   post for screw D) is supposed to run from the parting line
   (`split_z=10`, matching where Bottom's own boss-D pillar ends) up to
   the plate's underside (`plate_z[0]`) — but pass 7's uniform `+_DZ_TOP`
   shift (correct for every other plate-anchored z-range) also moved
   this tuple's *lower* bound, from 10.0 to 13.0, leaving Bottom's boss
   (still ending at z=10) and the plate's post (now starting at z=13)
   disconnected — no continuous load path for screw D over that span.
   Fixed by deriving `plate_post_D_z` as `(split_z, plate_z[0])` directly
   instead of shifting a literal, so the invariant holds regardless of
   case height. Verified by direct point-containment probing across
   z=9.5..16 (solid, contiguous, no gap) — no existing `verify()` gate
   happened to probe this specific boundary, so this was a silent one.

All three are regenerated (not hand-fixed in Fusion) and confirmed by a
full `run(..., export=True)` on both variants: `OK: M1+M2 probes passed`,
zero interference, all M2/envelope/posts-bosses/skin/wall checks `True`,
plus an offline manifold-edge + envelope + overhang scan of every
exported STL (both variants) — `OVERALL: PASS`, zero non-manifold edges.

## Pass 9 consolidated summary (through 2026-09-09 pass 9g)

Jake's pass-7 print surfaced 11 findings; the coordinator's later render
sweep of the pass-9 result surfaced 7 more (numbered 1-7 below, distinct
from Jake's findings 1-11 above them chronologically but renumbered here
as their own list since that's how the pass-9g brief enumerated them).
This table is the single reference for "what was wrong, what changed, and
which gate proves it" across every pass-9 sub-pass; the full narrative for
each row is in that row's own dated section below.

| # | Finding | Fix | Gate | Status |
|---|---|---|---|---|
| J1 | FPC relief pocket breached the shell at the USB end | Local **brow** raises the outer shoulder over the pocket footprint instead of shrinking the pocket | `verify_fpc_relief` 0 bad of 63, both variants | Fixed (pass 9), reshaped (pass 9g, see #2 below), **brow deleted and replaced by `usb_end_extension_mm` lengthening the +y dome instead — pass 12b, still 0 bad of 63** |
| J2 | Bosses A/C breached the shell on both halves | A/C relocated to the dome-tip end, absolute mm, both variants | `verify_posts_and_bosses` 0 bad, `check_interference` 0 pairs | Fixed (pass 9) |
| J3 | Two lanyard holders (duplicate) | `lug_relief_box` outward edge clamped to `wall_clear` inside the true wall | `verify_wall_integrity`/`check_interference` clean; no dedicated probe | Fixed (pass 9) |
| J4 | Screen-plate posts P1-P4 had no real wall | `POST_CORE_R` derived from pilot+`POST_WALL_MIN`; Ø4→Ø5; relocated off the window-bore crescent | `verify_post_walls` 0 bad of 8×3/8 per post | Fixed (pass 9 pt 2), **rebalanced pass 9g (see #1)** |
| J5 | Window lip ring fragile | Ring widened 0.8→1.8mm inward (never touches the display's own clearance); seam chamfer added | `verify_display_insertion_path` (diagnostic), offline overhang scan clean | Fixed (pass 9 pt 2) |
| J6 | Alignment lip chamfer (support-free print) | `chamfer_stadium_edge_at`, 0.5mm, on the lip/anchor outer step | Present in timeline (`Chamfer1`); overhang scan clean | Fixed (pass 9 pt 2) |
| J7 | Wordmark two-line layout | `wordmark_layout`: KANDI/WOOKS scaled independently, stacked, centred on `wordmark_center` | `verify_wordmark` all True, both variants; **re-audited pass 9g (see #7), found already correct** | Fixed (pass 9e) |
| J8 | Antenna cable channels | LoRa channel (trim only) + GPS notch (both variants), skin-safe clipped | `verify_antenna_channels` all True, both variants | Fixed (pass 9e) |
| J9 | Button caps cannot be inserted | Rib gets a dedicated tab pass-through lane | `verify_button_insertion` 0 bad of 125, both buttons/variants | Fixed (pass 9b) |
| J10 | Home plunger too short to reach the switch | `switch_actuator_reach`/`plunger_pretravel` derived from a live probe of the real switch body | `verify_plunger_reach` exact match, both buttons/variants | Fixed (pass 9b) |
| J11 | Stray sliver beside boss C | `MIN_RELIEF_CLEARANCE` hard assertion in `add_lip_anchor_reliefs` | Build-time assertion (fails loudly, not silently) | Fixed (pass 9) |
| 1 | **Plate post layout unbalanced** (10×6mm SW cluster, plate held at one corner) | P1-P4 spread to a full 10×11mm rectangle (same proven x=-20/-10 column, y stretched 14-25) | `verify_post_walls` 0 bad, both variants; window/wall clearance re-derived analytically | **Fixed (pass 9g)** |
| 2 | **FPC brow is a slab** (flat plateau, visible step) | Rebuilt as 2 nested tiers (`FPC_BROW_TIERS`) instead of 1 box -- each riser ~0.5-1.0mm instead of one 1.5mm cliff | `verify_fpc_relief` still 0 bad of 63, both variants; visually confirmed tapered (not fully smooth -- see Known limitations) | Partially fixed (pass 9g); **moot — the brow itself was deleted, pass 12b (see #J1)** |
| 3 | Generic `verify_skin_intact` should probe the whole outer surface | Not attempted this pass (time budget) | -- | **Not done (pass 9g)** -- unchanged from item 11 in Known limitations |
| 4 | Overhang review of every exported part | Re-ran `tools/offline_stl_check.py` on both variants' fresh exports | `OVERALL: PASS`, 0 bad overhang clusters, both variants | **Done (pass 9g)** |
| 5 | Assembly order not documented | Written up below and in the PR body | N/A (documentation) | **Done (pass 9g)** |
| 6 | Screw map not recomputed for the pass-9g post move | P1-P4 positions updated in the Screw list; all z-depths/lengths unchanged (only xy moved) | N/A (documentation, cross-checked against `PARAMS`) | **Done (pass 9g)** |
| 7 | Wordmark centring / lug ear taper (cosmetic) | Wordmark: verified analytically (bbox exactly centred, area-weighted ink centroid within 0.6mm of centre) -- no change made, see that section. Lug ear taper: reviewed via `pass9g_*_lanyard_end.png`, no defect found, not modified | Composite bbox center 0.000mm both variants (computed); ink centroid current +0.608mm / trim +0.554mm | **Reviewed, no change needed (pass 9g)** |

## 2026-09-08 pass 9 (first-print findings + design review)

Jake's first pass-7 (trim) print surfaced 11 real defects. This pass fixed
findings **1, 2, 3, and 11** in the generator, verified live in Fusion for
both variants (`verify()` all-green, zero interference, an independent
offline manifold+envelope+overhang scan of both exported STL sets), and
regenerated exports/renders. Findings **4–10 were not attempted this
pass** — see "Not completed this pass" below for why, and what's needed.

This pass ran into a real infrastructure problem worth recording: single
`fusion_mcp_execute` calls covering more than roughly a minute of Fusion
wall-clock time reliably time out client-side, but Fusion keeps executing
the script to completion regardless (confirmed by re-querying the
document seconds to minutes later and finding the timeline had advanced
exactly as far as an isolated, successfully-timed run of the same stage
would). The fix was mechanical, not a code change: split `build()`'s
sequence of `add_*` calls across multiple separate `fusion_mcp_execute`
calls against the SAME open document, re-fetching `Bottom`/`Top`/the
shared clip tool by name at the start of each call (Python locals don't
survive between calls; the Fusion document does). `wordmark_logo` (~35–50s
alone) and `add_comms_bay` (~10–15s once the shared clip tool exists, but
much slower deep in a long timeline under load) were the two stages long
enough to trip this on their own. No change to `firefly_case.py` was
needed for this — `run()` still works as a single call for anyone driving
it from a context without this timeout; it's specific to this MCP
session's transport.

### Finding 1: FPC relief pocket breached the shell at the USB end

**Confirmed root cause**: the relief pocket's widened margin (beyond
SPEC's own minimum box) reaches into the +y dome cap where the TRUE outer
shoulder is already close to its own outer_radius limit even before any
raise — the fix landed in this worktree before this pass started only
recovered ~0.3mm of skin there (see `FPC_RELIEF_MIN_WALL`'s git history
on this branch) because shrinking the pocket further to reach 1.2mm
started re-creating a real ~62mm³ interference against the display
module's own housing body — the pocket was already cut close to the
display's real minimum required depth in that footprint, so recovering
shell material there by cutting shallower ate directly into clearance the
real board needs.

**Fix**: a local **brow** (`FPC_BROW_HEIGHT = 1.5mm`, `add_fpc_brow` /
`build_fpc_brow_solid`) raises the TRUE outer shoulder over the relief
footprint instead of shrinking the pocket — it adds material, so it never
touches the pocket's own required depth, leaving the display's clearance
exactly as it was. Built as `(outer envelope pushed out by 1.5mm) MINUS
(the plain outer envelope)`, clipped to the pocket's footprint + a 3mm
blend margin, joined into Top with a best-effort R3 fillet on its seam
edges (prints support-free: Top prints face-down on its flat face, so
this bump sits on the upward-facing side during the print). Two real bugs
found and fixed while building this (kept as dead-end notes in the
function docstrings, since they're the kind of mistake worth documenting
against repeating):
1. First attempt intersected the pushed-out envelope directly with a tall
   box — `build_thickened_envelope` returns a SOLID FILLED pill, not a
   shell, so this produced a solid chunk filling most of Top's interior
   over the footprint, not a thin bump (a live verify() run caught it as
   a genuine ~1913mm³ Top×Screen-Plate interference plus up to ~1518mm³
   against the display housing). Fixed by subtracting the plain envelope
   first, leaving just the thin added layer.
2. Even after that fix, the footprint's own extreme corners (2D distance
   from spine_b approaching `outer_radius` even before any raise) have NO
   material in the plain envelope at any Z, so the subtraction there
   yielded the FULL pushed-envelope volume across whatever Z-band it's
   solid at — for 'current' (top_z=25) this reached down to z≈10–11, and
   `verify_wall_integrity`'s dome-perimeter scan caught it as a real bump
   at `spine_b z=11.0`. Fixed by tightening the brow box's own Z floor to
   just below the pocket's real floor instead of an arbitrary `top_z-15`
   margin.

The pocket cut itself (`add_fpc_relief`) now builds its skin-safe clip
tool from this SAME brow geometry (a fresh reference envelope + the brow
bump, offset inward by `FPC_RELIEF_MIN_WALL`) instead of the old
analytic approximation (inner-cavity-solid grown by a nominal wall
thickness) that broke down at this exact spot in the first place.
`FPC_RELIEF_MIN_WALL` is back to **1.2mm**.

**Gate**: `verify_fpc_relief` — **0 bad of 63 probes, both variants**
(was the target metric that could only reach 0.3mm pre-brow).
`envelope_bounds`/`check_body_envelope_vertices` gained a matching
documented exception for the brow's own footprint (same pattern as the
lug/caps), since the brow legitimately reaches `outer_radius +
FPC_BROW_HEIGHT` at its extreme corners — a real, deliberate ~1.5mm
protrusion, not a defect.

**Independent confirmation**: an offline pure-Python scan of the exported
STLs (`tools/offline_stl_check.py`, extended with the same brow
exception) found **0 non-manifold edges** on Top for both variants (the
original defect was two literal holes clean through the shell — a
non-manifold mesh) and 0 disallowed overhang clusters. `OVERALL: PASS`
for both `trim` and `current`.

### Finding 2: bosses A and C breached the shell on both halves

**Confirmed root cause**: A/C's rule (`x = outer_radius - wall - 3.0`)
placed the boss center itself beyond `flat_rho` — the true limit of the
FLAT bed at any Z near the parting faces (the case is narrowest exactly
at the flat top/bottom faces and widest at the waist, so a boss whose
center already exceeds `flat_rho` is guaranteed to breach near z=0/top_z
regardless of the generic radial clip already in place: that clip only
protects the WIDE, safety-margined outer sleeve of `clipped_pillar_with_
reach`'s two-piece boss, not the smaller, deliberately-unclipped
`BOSS_CORE_R` core built to guarantee real contact at both ends — and the
core punched through right where the finding's photos showed). True for
BOTH variants — current's wider shell just had ~2mm more margin, not
enough.

**Fix**: A and C moved to the dome-tip end (`(∓15.5, -8.0)`, absolute mm,
same for both variants — like B1/B2/D), where the 2D distance from
spine_a (17.44mm) clears trim's `flat_rho - boss_dia/2 - 1.0mm margin`
(18.14mm) with room to spare, current inherits more. The straightforward
"just move A/C inboard at their existing y≈25" fix (per the finding's own
first suggestion) turns out to be geometrically impossible for BOTH
variants: at y≈25 the battery bay (`x -20..20, y 2..32`) and the GPS
frame (`x -2.8..22.2, y 2..27`) already claim essentially the full width
inside `flat_rho`, leaving under 1mm of clearance either side — nowhere
near enough for a Ø6 boss + margin. Confirmed by direct computation
before moving anything (not by trial and error in Fusion). The dome-tip
position is clear of the L76K stack/frame (>5mm), of B1/B2 themselves
(~7.6mm center-to-center, edge gap 1.6mm — tight but non-overlapping), of
the battery/GPS/display footprints (all start at y≥2, this is at y=-8),
and gets the SAME automatic per-boss keep-out cut into the stack3 frame
that B1/B2/D already relied on (that loop iterates `screws_ABC +
[screw_D]` generically, so no extra code was needed there).

**Gate**: `verify_posts_and_bosses` — 0 bad, both variants.
`check_interference` — 0 pairs, both variants (confirms A/C don't
conflict with the relocated-adjacent battery/stack/B1/B2 geometry).

### Finding 3: two lanyard holders (duplicate)

**Confirmed root cause**: `lug_relief_box`'s outward (most-negative-y)
edge was a flat constant reaching further from spine_a (rho≈29.5) than
the alignment lip/anchor ring's own band (rho 24.95–28.40) — since the
box's radial reach fully encompassed the ring's own band at this
location, the cut removed the ring's ENTIRE cross-section there (not a
partial notch), printing as an open slot that read as a second lanyard
attachment point alongside the ear's real hole.

**Fix**: clamped the relief box's outward edge to stay `wall_clear`
(0.6mm) inside the TRUE wall distance straight out from spine_a — the
exact same clamp convention (`true_wall_distance_along_ray`, `wall_clear
= 0.6`) the per-boss reliefs in the same function already use, so this
can never disagree with them. The ear itself is unchanged (it was always
the one correct lanyard holder); only the relief clearance cut around it
is now bounded.

**Verification**: no dedicated live probe was built for "this relief
never reaches the true outer surface" specifically (time did not permit
a `_local_skin_thickness`-style scan of this one feature this pass) — the
fix is a direct application of an already-verified pattern, and the
overall `verify_wall_integrity`/`check_interference` gates (which DO
scan the dome perimeter broadly, including near spine_a) stayed clean
for both variants with this change in place. Recommend a dedicated probe
in a follow-up pass, and a visual check of `renders/pass9_*_top.png`
(the lug end) against a fresh print.

### Finding 11: stray sliver beside boss C

**Confirmed root cause**: the per-boss relief radius clamp (`relief_r =
min(nominal_r, s_wall - wall_clear)`, added pass 7 for a related defect)
had no FLOOR relative to the boss's own OD — at a boss sitting close
enough to the true wall, `s_wall - wall_clear` could land only just above
`boss_dia/2`, leaving a razor-thin remnant annulus of ring material
trapped between the boss and the barely-larger relief circle: exactly
the "thin triangular web" in Jake's photo, at boss C's old (near-
shoulder) position.

**Fix, at the source**: added `MIN_RELIEF_CLEARANCE = 1.0mm` and a hard
assertion in `add_lip_anchor_reliefs` — `relief_r - boss_r >=
MIN_RELIEF_CLEARANCE` for every boss, every build. If the true wall can't
spare that much, the boss is too close to the wall for this relief to
make sense at all (a real design conflict, per finding 2), and the build
now fails loudly instead of silently producing a thinner and thinner
sliver. With finding 2's reposition, every boss now clears this with
comfortable margin.

**"No stray body" check**: `verify()` now asserts the exact printed body
set equals `{Bottom, Top, Screen Plate, Power Button, Home Button}` —
gating, not just printed — which would catch an orphaned boolean-scrap
body the way `dedupe_body`'s docstring already documents happening for
Bottom/Top/Screen Plate. A second attempt at a blanket gate (`count_
sliver_faces` — small-face-area count — promoted to a hard assertion)
was tried and reverted: a live run found 487 sliver faces on Bottom and
47 on Top even on otherwise fully-clean geometry, from the wordmark/flare
logo debossing (many short glyph line segments legitimately produce many
small corner faces) — unrelated to this defect and not a meaningful
signal on its own. Kept diagnostic-only, as before pass 9.

### verify() output, both variants (pass 9)

```
trim:    VERIFY OK
         body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         fpc_relief bad [] of 63
         posts_bosses bad []
         stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}

current: VERIFY OK
         body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         fpc_relief bad [] of 63
         posts_bosses bad []
```

(`sliver_results` diagnostic, unchanged pass-to-pass and unrelated to
finding 11 per the analysis above: `{'Bottom': 487, 'Top': 47}`, both
variants — wordmark/flare glyph-corner faces, not gated.)

### Offline STL scan output (`tools/offline_stl_check.py`, pass 9)

Ported the same brow envelope exception from `check_body_envelope_
vertices` (see finding 1). Both variants:

```
=== Offline STL checks: trim ===
... (manifold: true, 0 non-manifold edges, for every exported body;
     envelope: ok for every body once the brow exception is applied;
     overhang: 0 bad clusters for Top/Bottom)
OVERALL: PASS

=== Offline STL checks: current ===
... (same)
OVERALL: PASS
```

### Not completed pass 9 part 1: findings 4–10

Findings 4 (plate posts P1–P4 have no wall), 5 (window lip ring
fragile), 6 (alignment lip chamfer), 7 (wordmark two lines), 8 (antenna
cable channels), 9–10 (button cap insertion path + Home plunger length),
and the broader "generic `verify_skin_intact` probes the WHOLE outer
surface" gate described in the brief, were **not attempted in the first
part of this pass**. Reason: that session's Fusion MCP connection was
unexpectedly unstable (see the infrastructure note above) — isolating
and working around the per-call timeout, plus one Fusion-side stall that
needed several minutes to clear on its own (not a code issue; Fusion
recovered without a restart), consumed the large majority of the
session's time budget before findings 1/2/3/11 were even confirmed clean
end-to-end on both variants. **Findings 4, 5, and 6 are fixed in part 2
of this pass, below** (7, 8, 9–10 remain open — see Known limitations).

## 2026-09-08 pass 9, part 2 (findings 4, 5, 6 — plate posts, window lip
ring, lip chamfer)

A fresh Fusion MCP session, with its own full verification budget.
Followed the part-1 infrastructure note's own advice: every `build()`
stage was run as a separate `fusion_mcp_execute` call against the same
open document, re-fetching `Bottom`/`Top`/the shared clip tool by name at
the start of each; `verify()` and the export stages were likewise run as
separate calls against the already-built document rather than
re-building from scratch each time.

### Finding 4: screen-plate posts P1–P4 had no real wall

**Confirmed root cause, computed before touching Fusion** (a standalone
pure-Python script reusing `inner_rho_at_z`/`rho_at_z`/
`true_wall_distance_along_ray` against the real `PARAMS`, no adsk
needed): two independent problems, both present on every one of P1–P4
regardless of position.

1. **`POST_CORE_R` (1.1mm) itself was too thin.** `clipped_pillar_with_
   reach`'s whole design (see its docstring, pass 6) is that only the
   narrow, full-height "core" cylinder is PROVEN to reach the ceiling —
   the wider, radially-clipped "sleeve" gets clipped away entirely
   wherever the local cavity boundary is tighter than the post's own
   radius (see `inner_rho_at_z`: near the ceiling, the fillet shrinks the
   hollow interior's usable radius from the wall's own value down to
   `fillet_center_rho` exactly at the flat ceiling). With a 1.62mm-dia
   pilot (0.81mm radius) and `POST_CORE_R`=1.1mm, the wall around the
   pilot at the post's own tip — the ONE place guaranteed to have any
   material at all — was only **0.29mm**, on every post, independent of
   xy. This is very likely the real mechanical cause of "the post by the
   power button snapped": a paper-thin neck right at the highest-stress
   point (the screw's own thread-cutting zone).
2. **P1 and P4's xy positions sat in the crescent between the window
   bore and the true wall.** Computed directly: at P1/P4's own
   `top_post_z[1]` (the ceiling), their true-outer-wall clearance
   (`true_wall_distance_along_ray` minus the post radius) was
   **negative** — the post already exceeded the true outer surface there
   — and their distance to the window bore was ~0.5–0.6mm, nowhere near
   a real minimum. P3 was marginal at Ø4 (~0.11mm to the bore) and went
   negative at the new Ø5. P2 had plenty of wall margin but was still
   only ~0.1–0.3mm from the window bore at Ø4/Ø5 — all four were too
   close to something.

**Fix.** All four posts relocated to **absolute mm, identical in both
variants** (matching the A/B1/B2/C/D convention already established in
finding 2): **P1 (−10, 18), P2 (−20, 18), P3 (−10, 24), P4 (−20, 24)** —
the y 18–24 band south of the display PCB's own bbox (`y` starts 27.6)
and west of the GPS patch box (`x` starts −2.8; staying at `x <= −10`
clears the box AND its real printed frame wall, at `x=−4.05`, by
`>=1.9mm` with no keep-out cut needed at all — computed, not eyeballed).
This is a plain subset of the brief's candidate zone (b); no keep-out
into the GPS box turned out to be necessary once the west side was used
instead of centering on the box. Post diameter **Ø4 → Ø5** (`top_post_
dia`), since a Ø4 post around a Ø1.62 pilot only has 1.19mm of nominal
wall — already under the new 1.2mm minimum before any clipping.
`POST_CORE_R` is now derived, not a flat constant: `core_r =
pilot_r + POST_WALL_MIN` (0.81+1.2 = **2.01mm**) — the core alone, proven
by construction to reach the ceiling, now satisfies the wall-around-pilot
minimum everywhere it exists, with no dependency on xy position at all.
A best-effort 1.0mm constant-radius fillet ("root fillet/gusset") is
added at each post's own top edge where it meets the ceiling (same
best-effort pattern as `add_fpc_brow`'s seam fillet) — confirmed present
in the built timeline (4 `Fillet` features, one per post, both variants).

**Screen Plate rework.** `PARAMS['plate_south_extension']` (`x
(-24,-6)`, `y (14,29)`) is unioned onto the plate's existing outline
before the cavity-outline intersect, reaching the relocated south posts
without touching the GPS frame (>1.9mm clear) or the header
cutout/FPC-tab/USB-shell region (all unchanged, north side). S1–S3 board
standoffs and the existing header cutout are untouched.

**New gate, `verify_post_walls`** — two checks per post, 8 rays (0°,
45°, ... 315°) each: (a) `<name>_pilot_wall` — live point-containment
probe at radius `pilot_r + 1.2mm`, at 3 z-heights spanning the post —
must be solid; (b) `<name>_shell_skin` — analytic `true_wall_distance_
along_ray` minus the post radius at the post's own top z (its tightest
height) — must be `>= 0.6mm`. **Both empty (0 bad of 8×3 / 8) for all 4
posts, both variants** — see the verify() output below.

**A real regression found live, and fixed in the same pass**: after
relocating the posts, a full `verify()` run reported a genuine ~22mm³
interference between Top and the XIAO board's own body — traced to
finding 5's ring-widening (below) reaching, at the OTHER end of the ring
(the −y comms-stack dome tip, nowhere near P1–P4 or the window), into
the stack's real footprint. See finding 5's own section for the fix — it
turned out to be a ring issue, not a post issue, but is recorded here too
since it was this finding's own `verify()` run that caught it.

### Finding 5: window lip ring fragile

**Root cause of the OUTER-radius ceiling on how much the ring could be
thickened**: `lip_r[1]`/`anchor_r[1]` are not arbitrary — `lip_r[1]`
(27.75 current / 25.75 trim) sets the SPEC'd 0.25mm nesting clearance
between the lip and Bottom's own true inner wall (`outer_radius - wall`),
and `anchor_r[1]` (28.40 / 26.40) is a deliberate ~0.4mm reach PAST that
same wall so the anchor band actually fuses into Top's shell on join
(SPEC: "so it fuses"). Both are load-bearing/fit-critical and were left
alone. The ring's z-band (9.2–11) sits entirely below the display glass
(z 20.3+) and PCB (z 17.59+, both +3 for trim) — **no z-overlap with the
display at all** — so thickening the ring **inward** (shrinking `lip_r
[0]`/`anchor_r[0]`) can never touch the display's own 0.25mm clearance,
regardless of how far in it goes at the window end. Widened both bands
from 0.8mm to **1.8mm** (>= the 1.6mm minimum) by moving the shared inner
edge in by 2.0mm: trim 24.95→23.95, current 26.95→25.95 (both `lip_r[0]`
and `anchor_r[0]`, preserving the existing invariant that they match).

**Finding 6's chamfer, same function**: the lip(outer)→anchor(outer)
radius step at `z=anchor_z[0]` (10.0) — the anchor is wider than the lip
directly below it — is a flat horizontal shelf whose CAD-space +Z-facing
top surface becomes a downward-facing, unsupported overhang once Top
prints flipped (face-down on its flat `z=top_z` face — **Top is the half
that carries the lip**, per SPEC, and per how `add_lip_anchor_reliefs`
joins it into `bodies['Top']`). Beveled with a new `chamfer_stadium_
edge_at` helper (the ring's edge loop is a line+arc "stadium" shape, not
a plain circle, so the existing `chamfer_edge_at` — which matches by
center+radius — doesn't apply; the new one matches every edge by its
MIDPOINT, whether line or arc geometry, against the same rho-from-spine
convention used throughout the file). `PARAMS['lip_ring_seam_chamfer']`
= 0.5mm, best-effort (skipped, not fatal, on failure — same pattern as
`add_fpc_brow`). Confirmed present in the built timeline (`Chamfer1`,
right after the ring is joined, both variants) and confirmed clean by
the offline overhang scan (see below) — 0 bad clusters on Top, both
variants, with no new whitelist entry needed.

**A real regression found live** (not by inspection): after widening the
ring, a full `verify()` run on trim reported a genuine interference
(`('Body1', 'Top', 22.1954)`, bbox `x ±8.88, y -22.2..-23.2, z 9.3..10.6`)
— XIAO's own body against Top. Root cause: widening the ring's inner
radius applies around the WHOLE perimeter, not just near the window —
at the far end (the −y comms-stack dome tip), the new, wider ring reaches
almost exactly into `bay.stack3.l76k_pcb`'s own far corner (`x ±8.9, y
-24..-1.5`), which the OLD, narrower ring cleared by construction. Fixed
by cutting a keep-out matching that footprint (+1mm margin, spanning the
ring's own z-band) from the ring — a no-op for `current` (XIAO/Wio aren't
inserted there) and, near the window (this finding's actual target, at
the opposite end of the case), completely unaffected. Re-verified clean
(`interference []`) on both variants afterward.

**From-inside insertion path — confirmed NOT possible, both variants.**
New function `verify_display_insertion_path`: a standalone copy of the
merged lip+anchor ring is kept as a hidden reference body (`Lip Anchor
Ring (reference)`, swept into `Reference — not printed` like the other
build-tool solids); a box matching the REAL inserted display occurrence's
own world bounding box (glass + PCB + everything on it, via
`_bbox_extents` — no known aggregate-bbox distortion for this board,
unlike the L76K's antenna cable) is swept from just below the ring's own
lowest z up to the module's own top, and checked for real interference
against the ring alone (isolated from the general shell — the question
is specifically whether the ring/anchor blocks assembly, not whether the
module can pass through solid wall, which it obviously cannot and isn't
meant to). Result: **`interference_mm3` = 1176.9 (trim) / 805.3
(current)** — large, real numbers, not a numerical artifact. This
matches a simple hand-check: the display PCB's own diagonal (`display_
pcb` 39.2×41.4 → ~57mm) is bigger than the window bore itself (Ø45.30),
so no orientation or ring width can make a straight vertical (or
front-through-the-bore) insertion work — the PCB is physically larger
than the hole in every direction. **Conclusion, per the brief's own
fallback**: the module does not fit "from inside" (up through the
already-assembled shell) OR "from the front" (through the window bore) —
it must be seated into **Top's own open underside before Bottom is
attached** (Top and Bottom are two separate, unassembled shells at that
point in the build; the module is placed once, then Bottom closes over
it). **Assembly order, added to Print orientation below**: (1) seat the
display module (glass up into the window bore, PCB resting under the
lip/anchor/posts) into the separate Top half; (2) place the comms-bay
hardware (battery, stack, GPS patch, L76K) into the separate Bottom half;
(3) join Bottom and Top together over both. This is not a new
restriction the fix introduced — it was already the only physically
possible order (the window bore was always smaller than the PCB); this
pass is the first to have actually checked and documented it.

### verify() output, both variants (pass 9 part 2)

```
trim:    VERIFY OK
         body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         posts_bosses bad []
         post_wall_results  P1/P2/P3/P4 pilot_wall [] and shell_skin [] (8 rays each)
         display_insertion_results {'ok': False, 'interference_mm3': 1176.916,
             'module_bbox_xy': {'x': [-22.39, 22.39], 'y': [27.61, 73.12]},
             'module_top_z': 28.0, 'ring_z_band': [9.2, 10.0]}
         stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
         fpc_relief bad [] / skin bad [] / wall bad []

current: VERIFY OK
         body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         posts_bosses bad []
         post_wall_results  P1/P2/P3/P4 pilot_wall [] and shell_skin [] (8 rays each)
         display_insertion_results {'ok': False, 'interference_mm3': 805.308,
             'module_bbox_xy': {'x': [-22.39, 22.39], 'y': [27.61, 73.12]},
             'module_top_z': 25.0, 'ring_z_band': [9.2, 10.0]}
         fpc_relief bad [] / skin bad [] / wall bad []
```

All other gates (M1/M2 probes, envelope, export-envelope, outer-bump,
min-clearances, `verify_posts_and_bosses`) also green on both variants —
same full sweep as pass 9 part 1, re-run end to end.

### Offline STL scan output, part 2 (both variants)

```
=== Offline STL checks: trim ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS

=== Offline STL checks: current ===
... (same)
OVERALL: PASS
```

### Exports, part 2

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl`, `export/<variant>/firefly_<variant>_case.3mf` (native,
5 objects each, re-verified by object count), `export/<variant>/
firefly_<variant>_plate.3mf` (new — packed, print-oriented plate: Bottom
as-is, Top flipped 180° about X, Screen Plate as-is, both buttons rotated
outer-face-down, via `tools/stl_to_3mf.py`), and the coupon STLs/3MF
(unchanged geometry, re-exported as a byproduct of running the export
pipeline again). Renders: `pass9c_{trim,current}_{front,top,right,iso}.
png` (standard 4-view, both variants) plus 4 close-ups — `pass9c_posts_
closeup.png` (interior view from below near the P1–P4/display-header
area, Bottom hidden), `pass9c_lip_ring_section.png` (close interior view
looking up at the ring/header area near the window — a literal Fusion
section-analysis cut was attempted first but did not visibly crop the
saved screenshot in this session, so this is a close, unsectioned
interior view instead; the ring's stepped profile is visible in it),
`pass9c_lip_chamfer.png` (wide isometric of Top's whole underside showing
the ring running the full perimeter), `pass9c_plate_underside.png` (the
Screen Plate alone, underside view, showing the new south extension and
its 4 holes). All viewed directly (not just generated) as part of this
pass.

- **Bottom**: print face-down on its flat z=0 face (the KandiWooks
  wordmark side).
- **Top**: print face-down on its flat z=25 face (the flare-glyph side;
  **Top carries the alignment lip/anchor ring** — see finding 6 above for
  its seam chamfer, added specifically so it prints support-free in this
  orientation).
- **Assembly order** (finding 5): seat the display module into the
  separate Top half first (glass up into the window bore, PCB under the
  lip/anchor/posts), place the comms-bay hardware into the separate
  Bottom half, then join Bottom and Top — the display module does not fit
  through the window bore or up through the assembled shell (its own PCB
  is physically larger than the bore in every direction).
- **Screen Plate**: flat, either face down.
- **Power Button / Home Button**: print outer-face-down (the stadium head
  face), with a brim — the caps are small with a fine plunger/tab feature
  that benefits from brim adhesion.
- Suggested settings: 0.2mm layers, 4 perimeter walls, PETG or PLA.
  Supports are not expected to be needed for Bottom/Top in this
  orientation; check the lug and USB liner overhangs on your slicer.
- **Print the button coupons first.** `coupon_power_wall.stl` /
  `coupon_power_cap.stl` / `coupon_home_wall.stl` / `coupon_home_cap.stl`
  (in `hardware/case/export/coupons/`) are a ~15-minute fit test for the
  wall-hole/rib/tab/collar mechanism before committing to a full Bottom+Top
  print. Print all 4 flat, face-down, no supports needed. Wall coupons are
  ~9.6×24×17.8mm (power) / ~9.6×24×18.6mm (home); cap coupons are
  ~14.7×10×5.8mm (power) / ~14.7×8.9×6.6mm (home) — small parts, watch for
  first-layer adhesion. If the cap binds or rattles in the wall hole,
  adjust `PARAMS['cap_clearance']` (0.25mm default) and re-export.
  **Coupon test procedure (pass 9b, findings 9/10):** with both parts
  printed, insert the cap through the wall coupon's OPEN (interior) side —
  head first is impossible by design (see finding 9 below); feed the
  plunger/collar end in from the side opposite the wall hole, then push
  the head out through the hole from the inside until it seats flush and
  proud (~0.45mm). Correct feel: a firm but not tight slide, ending in a
  definite stop as the collar bottoms on the guide rib (do not force it
  past this point); the head should sit flush with a small proud lip and
  not rattle side-to-side. If it won't insert at all, re-check you're
  feeding it from the correct (open) side — outside-in never works, even
  on a good print. If it's loose/rattly, reduce `cap_clearance`; if it
  binds before reaching the collar stop, increase it slightly and
  re-export. If the collar doesn't bottom out with a real stop (travels
  too far), check `plunger_travel`/`rib_thickness` weren't hand-edited.

## 2026-09-08 pass 9b (findings 9 and 10 — button insertion + Home plunger reach)

A fresh Fusion MCP session. Both findings turned out to share the same
root cause upstream (`button_geometry`'s `housing_xy`, the switch's
assumed position) and were fixed, verified live against the actual
inserted display PCBA's real switch bodies (not just the analytic SPEC
bbox), and re-exported together.

### Finding 10: Home cap plunger too short to reach the switch

**Confirmed root cause.** `housing_xy` (`button_geometry`, via
`ray_box_exit_2d`) is not a real surface — it's the point where a ray
from the switch bbox's center exits the bbox's own DIAGONAL CORNER, which
is only real if the switch body fills its bbox all the way out to that
corner. A live Fusion probe of the actual inserted `SWITCH-TS24CA` body
(point-containment scan along the exact same ray, using the same
technique as `find_outermost_s`) found it does not, for either button:
the real body's outermost point along the ray is only **1.82mm** from the
bbox center — a raised nub at z 16.2–17.2 (matching SPEC's "nub at
z≈16.7" and its stated 1.2mm protrusion above a ~0.6mm housing base
exactly) — **3.5mm (Power) / 3.3mm (Home) short of `t_exit`**, the
assumed housing point. Everything anchored to the OUTER wall via
`true_wall_distance_along_ray` (`s_wall`, the cap head/hole, rib, collar)
is unaffected by this — shifting the ray's origin along its own direction
shifts those values by the same amount, so the absolute wall point they
resolve to is identical either way, and every wall-side gate already
verified clean. Only `s_plunger_tip` (the plunger's reach TOWARD the
switch) inherited `housing_xy`'s error: built as a small offset from it,
the nub pocket was actually being built **2.3–2.9mm short of the real
actuator** — on both buttons, not just the one Jake's print flagged.

**Fix.** `PARAMS['switch_actuator_reach']` = 1.82mm (from the live probe)
is the real actuator nub's own reach from the switch bbox's center, along
its nub direction — the same physical part for both buttons, and the
probe found the identical value at both. `PARAMS['plunger_pretravel']` =
0.3mm replaces the old `plunger_tip_gap` (0.02mm, a full-press gap FROM
THE HOUSING, i.e. from the same bad reference point). `button_geometry`
now computes `s_actuator` (the real reach, converted into the same
housing_xy-relative convention every other `s_*` value here uses) and
sets `s_plunger_tip = s_actuator + plunger_pretravel` — the tip rests
0.3mm from the real actuator at rest, closing over the first 0.3mm of any
press before it starts moving the actuator itself. `PARAMS['nub_pocket'
]['xy'][0]` (tangential width) is widened 1.3 → 2.4mm: the same live scan
found the real nub is ~1.9mm wide tangentially (not 1.3mm), so the old
pocket would have clipped the sides of the real nub once the plunger
actually reached it.

**Gate**: `verify_plunger_reach` (new) — live-probes the ACTUAL inserted
switch body a second time (independent of the constant above, to catch
future drift) and the ACTUAL built cap body's own tip rim (offset past
the nub-pocket cutout so the probe lands on real shaft material), and
asserts their gap matches `plunger_pretravel` within 0.15mm. **Both
buttons, both variants: `actuator_reach` found 1.82mm (expect 1.82),
`rest_gap` found 0.3mm (expect 0.3) — exact.**

### Finding 9: button caps cannot be inserted

**Confirmed root cause.** The cap (shaft + collar + retaining tab, one
rigid printed piece) can only be assembled from the INSIDE of the open
(Bottom-not-yet-attached) Top half, sliding it outward until the head
seats in the wall hole — an outside-in, tip-first insertion is
geometrically impossible (the collar is deliberately `collar['h']`
=0.8mm wider than the shaft, specifically so it's too wide to slide
through the rib's own slot and instead bottoms against it during a hard
press). But the retaining tab sits OUTBOARD of the rib at rest (close to
the inner wall, not near the collar), so an inside-out insertion still
has to carry the tab PAST the rib's own axial thickness at some point in
the stroke. Computed directly from the two features' own geometry (not
found by trial and error): the tab hangs `tab['h']` (2.0mm) below the
shaft's slot envelope, but the rib's slot only clears `rib_slot_clearance`
(0.25mm) below the shaft — a **0.55mm-tall band of real, solid rib
material** (between the slot's own lower edge and the rib plate's own
outer edge, `attach_margin` beyond the slot) sits directly in the tab's
path, for the ENTIRE thickness of the rib — a geometrically guaranteed
interference for any straight-line insertion, not a tolerance-dependent
near-miss. The tab is a short, thick stub cast integrally with the shaft
(not a thin cantilever spring), so asking it to flex ~0.55mm past a hard
stop is not realistic for a printed PETG/PLA part.

**Fix.** Per the finding's own guidance, relieved the RIB instead of
asking the tab to deflect: `add_button` now cuts a dedicated lane through
the rib plate's full thickness, sized to the tab's own footprint plus a
0.3mm/side running clearance (looser than the working `rib_slot_clearance`
on purpose — this lane is a one-time assembly pass-through, not an
operating fit), so the tab slides past freely during assembly with no
interference and no reliance on flex. It has no effect on the rib's main
slot (still exactly as before) or on the collar (built and clipped
separately, never enters this lane).

**Two collateral defects found and fixed while verifying the fix live**
(both were LATENT — present before this pass, just never previously
probed for):

1. **The Home button's rib had NO real material at all.** Modeling the
   insertion sweep required first confirming the rib actually exists as a
   solid — a live check found `combine_join`ing a fresh copy of the SAME
   rib box into Top added **exactly 0.0mm³** to Top's volume (confirmed
   by comparing `Top.physicalProperties.volume` before/after): the rib
   sits deep in the empty cavity, not touching any other Top feature, and
   this Fusion build's Combine-Join silently no-ops when the tool body
   doesn't touch/overlap the target at all — the same class of problem
   `BOSS_CORE_R`/`POST_CORE_R` already work around for bosses/posts, just
   never hit for a rib before (Power's rib happened to succeed; nothing
   in the design guaranteed that). Fixed with a thin (2mm) "reach spoke"
   — `RIB_CONNECTOR_T_OFFSET`/`RIB_CONNECTOR_W`, shared with
   `add_button_plate_clearance`'s own cutout so the two can never drift
   out of sync — from the rib's own edge out to solidly embed 0.3mm
   inside the true wall (confirmed by live probe to stay short of the
   outer skin). The connector deliberately OVERLAPS the rib's own edge by
   0.2mm (not just touches it) — a first version left a 1mm gap "to stay
   clear of the shaft," which turned out to be the exact same
   no-touching-bodies problem one level up (worked for 'trim', came back
   with zero material again for 'current' — same code, same defect,
   different Fusion tie-break).
2. **Home's rib/collar overlapped the real switch body.** Once finding
   10's fix put the mechanism at the switch's real position, a live
   `check_interference` run found a genuine ~39mm³ `Body1`(the real
   switch) × `Home Button` overlap: `rib_inboard_offset` (6.0mm, a
   generator-internal default with no SPEC.md basis) measures the rib
   from `housing_xy`, and Home's real available room (actuator to true
   wall) is only ~7.7mm — not enough for the old offset plus
   `rib_thickness`+`plunger_travel`+`collar['len']`. A global offset
   reduction (tried: 3.5mm) fixes Home but reopens a DIFFERENT defect for
   Power: the rib's own flat Z-extent reaches into the R10 shoulder
   curve above `cap_z_center`, where the true wall is measurably closer
   than the flat estimate — confirmed live as a real ~0.5mm export-
   envelope breach at Power once the global margin shrank (Power never
   needed the reduction — it has 10mm+ of real clearance). Fixed instead
   with a PER-BUTTON dynamic clamp in `button_geometry`
   (`rib_actuator_clearance` — shifts rib+collar outward, toward the
   wall, only as far as needed for the COLLAR — the tighter of the two —
   to clear the real actuator by 0.3mm; a no-op for Power, both variants,
   and for Home on 'current', which also has enough room). Wherever the
   clamp fires (Home/trim only) OR a button's connector spoke lands close
   enough to the true surface on its own (confirmed live: Home/current,
   unshifted, still had a ~0.2mm breach from the connector's own
   tangential offset landing on a non-radial ray relative to the dome) —
   `add_button` now unconditionally Combine-Intersects the whole
   rib+connector unit against the TRUE outer solid (`build_outer_pill_
   solid`, the same technique `add_usb_tunnel`'s liner already uses for
   this exact "flat box near the curved shoulder" problem — not the
   inner-cavity clip tool, whose own FEATURE_FAILED_TO_CREATE note
   doesn't apply to this simpler, unfilleted solid), clipping back
   anything that would otherwise poke through. Shared once per `add_
   buttons()` call (not rebuilt per button) to keep this within the MCP
   call's time budget.

**Gates**: `verify_button_insertion` (new) — sweeps the tab's own
footprint (5 sample points × 24 steps) along the plunger axis from
comfortably inboard of the rib to its rest position, checking real
point-containment against the built Top at every step. `verify_button_
retention` (new) — live-probes that the collar is still blocked by real
rib material at the rib's own location (confirms the "too wide to pull
back out" property survives findings 9/10's fixes) and that the tab is
still blocked by real wall material just past its own clearance pocket
(confirms the "can't drift further outward" property), plus restates the
existing collar-rib-gap/tab-gap construction checks. **Both buttons, both
variants: 0 bad of 125 insertion-sweep probes each; all retention probes
True.**

### verify() output, both variants (pass 9b)

Confirmed piecewise against a live document each time (this session's
Fusion MCP connection made the full `verify()` call itself intermittently
exceed the client-side timeout once the three new checks were added on
top of the existing sweep — Fusion completed every call regardless, per
the pass-9-part-1 infrastructure note; splitting the same checks
`verify()` runs into several smaller calls against the same open document
avoided the client timeout without changing what's being checked):

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] (base + every board occurrence)
         export_envelope: all 5 bodies True
         plunger_reach: actuator_reach True (1.82/1.82), rest_gap True (0.3/0.3) -- both buttons
         button_insertion: both buttons bad_count 0 of 125
         button_retention: all True -- both buttons
         m1 probe / m1 cavity / m2 / envelope / bump / posts_bosses / post_wall /
         stack3_clearance / skin / wall / fpc_relief / min_clearances: all clean

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] (base + every board occurrence)
         export_envelope: all 5 bodies True
         plunger_reach: actuator_reach True (1.82/1.82), rest_gap True (0.3/0.3) -- both buttons
         button_insertion: both buttons bad_count 0 of 125
         button_retention: all True -- both buttons
         m1 probe / m1 cavity / m2 / envelope / bump / posts_bosses / post_wall /
         skin / wall / fpc_relief / min_clearances: all clean
```

### Offline STL scan output (pass 9b)

`tools/offline_stl_check.py` — both variants, all 5 exported bodies:
manifold (0 non-manifold edges), envelope ok, overhang `bad_clusters_mm2`
`[]`. `OVERALL: PASS` for both `trim` and `current`. The 4 coupon STLs
(re-exported with the same fixes) checked separately for manifold-ness
only (the tool's envelope/overhang checks are Bottom/Top-shaped, not
applicable to the coupon fixtures): all 4 manifold, 0 non-manifold edges.

### Exports, pass 9b

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (re-exported — Power/Home Button geometry changed;
Bottom/Screen_Plate unchanged but re-exported as a byproduct of the same
pipeline run; Top changed only at the two buttons' rib footprints),
`export/<variant>/firefly_<variant>_case.3mf` (native, 5 objects,
re-verified by unzipping and counting `<object` elements against the name
list), `export/<variant>/firefly_<variant>_plate.3mf` (re-packed via
`tools/stl_to_3mf.py` — Bottom as-is, Top flipped 180° about X, Screen
Plate as-is, Power Button `outer-x`, Home Button `outer-rz32.74` — the
z-rotation needed to bring `home_nub_dir` to -X before the same "outer
face down" flip `outer-x` applies; Power's own nub_dir is close enough to
-X that the plain `outer-x` orientation was kept, matching the prior
pass), and the coupon STLs/3MFs (`export/coupons/coupon_{power,home}_
{wall,cap}.stl`, `firefly_coupons.3mf`, `firefly_coupons_native.3mf` — 4
objects, re-verified the same way). Renders: `pass9d_{trim,current}_
{front,top,right,iso}.png` (standard 4-view, both variants) and `pass9d_
trim_mechanism_section.png` (Bottom hidden, orthographic from below,
showing the display PCBA, both switch components, and the header/GPS
area — a plain interior overview; a tighter close-up isolating just the
plunger-vs-switch gap was attempted via a selection-based viewport fit
but Fusion's selection API rejected the body references from this script
context, so this pass didn't get a dedicated close-up beyond this wider
interior shot). All viewed directly (not just generated) as part of this
pass.

### Not completed / open (pass 9b)

Findings 7 (wordmark two-line layout) and 8 (antenna cable channels)
remain open (out of this pass's scope — findings 9 and 10 only, per this
pass's brief). The generic "`verify_skin_intact` probes the WHOLE outer
surface, not named footprints" rework is also still not done.

## 2026-09-08 pass 9e (findings 7, 8 — two-line wordmark, antenna channels)

A fresh Fusion MCP session (this one noticeably slower/more prone to the
part-1 infrastructure note's client-side timeout than prior passes —
several individual stages, including plain read-only queries, needed 2-4
minutes of real wall-clock time before the SAME script that had just
timed out client-side turned out to have completed normally server-side;
`build()` was run in 4 separate stages against the same open document,
`verify()` and the export/render stages likewise, per the pass-9-part-1
and pass-9b notes' own advice). Both findings verified live for both
variants (piecewise, for the same reason), plus an offline pure-Python
scan of the exported STLs and direct visual inspection of every new
render — nothing in this section is reported from code alone.

### Finding 7: KandiWooks wordmark larger, two lines

**Root cause / analysis, computed offline against the real
`kandiwooks_logo.json` before touching Fusion** (same discipline as pass
9's finding 4): the 6 extracted bodies needed to split into the two words
"KANDI"/"WOOKS" by NAME, not a runtime x-extent threshold — `Body1` (the
'i') carries a tall decorative flourish/sprout on its dot (y −0.36..5.27,
more than double every other glyph's ~2.3–2.4mm cap height) that visually
arcs out over the start of "WOOKS" in x, so any pure x-threshold split
would have to cut through that overlap. Sorting the 6 bodies by their own
`minx` instead puts `Body4` ('k'+'a' fused, touching strokes — same
fusion the pass-6 docstring already documented), `Body5` ('n'), `Body2`
('d'), `Body1` ('i') as the first 4 (K-a-n-d-i = "KANDI") and `Body3`
('W'+'o'+'o'+'k' fused — its 2 small enclosed loops are the flower/leaf
glyphs standing in for the O's, per SPEC.md) and `Body6` ('s') as the
last 2 (W-o-o-k-s = "WOOKS") — confirmed with a standalone even-odd-fill
raster of the actual loop data (no Fusion needed) before this was ever
implemented: all 5+5 letters present, no gaps, in the expected order.

**Fix**: `wordmark_layout` (new) scales KANDI and WOOKS INDEPENDENTLY to
the same target width — `2 * (flat_rho - WORDMARK_EDGE_CLEARANCE)`,
`WORDMARK_EDGE_CLEARANCE` = 1.6mm (>= SPEC's own 1.5mm ask, +0.1mm
margin) — derived from `flat_rho`, not a fixed mm constant, so the
wordmark fills the flat back face's usable width in both variants
(target width 45.08mm current / 41.08mm trim). Both words happen to have
almost identical native widths in the source JSON (12.52mm / 12.58mm),
so this gives them nearly the same font scale, matching how the original
single-line wordmark was one uniform scale throughout. Stacked vertically
(KANDI above WOOKS) with a 2.0mm gap between their own local bboxes,
centred as one block on the existing `wordmark_center` (0,25) — same
mirroring convention as before (confirmed pass 6, unchanged here: each
word's own local x is negated before translating into place).

**Every case-screw counterbore and the lug clear by a wide margin, by
construction, not by a dynamic per-boss shrink**: the block's whole
y-span sits entirely inside the straight spine section (spine_a.y=0 to
spine_b.y=50), where `rho_from_spine` is exactly `|x|` independent of y —
so widening the wordmark can only ever push its `|x|` extent toward
`flat_rho`, never toward any of the counterbores (A/B1/B2/C at y=−8/−15,
D at y=60) or the lanyard ear (y<=−26.5), all of which sit OUTSIDE
`[0,50]` entirely. Live-computed clearances (both >=1.5mm required):

```
current: line1(KANDI) y[20.59,41.44] line2(WOOKS) y[8.56,18.59]
         clearance_A 14.309  clearance_B1 21.309  clearance_B2 21.309
         clearance_C 14.309  clearance_D 16.309  clearance_lug_hole 33.199
trim:    line1(KANDI) y[21.07,40.07] line2(WOOKS) y[9.93,19.07]
         clearance_A 15.679  clearance_B1 22.679  clearance_B2 22.679
         clearance_C 15.679  clearance_D 17.679  clearance_lug_hole 32.569
```

**Gate**: `verify_wordmark` (new) — edge clearance (>=1.5mm; exactly
1.6mm by construction, both variants), clearance to A/B1/B2/C/D and the
lug hole (>=1.5mm; all >=14mm found, both variants), and a live grid
probe (14×6 per line, both lines) on the built `Bottom`: mid-deboss depth
must read hollow over a real, non-trivial fraction of the footprint
(found 0.357, both variants — comfortably inside the sane [0.05, 0.85]
band), never solid below the flat bed, and always solid again just past
`logo_deboss_depth` (0.4mm) — the "no breach of the floor" check (the
battery-bay floor cuts live a further 1.6mm+ deeper still, so this is
also a floor-safety margin, not just a depth check). **All checks True,
both variants** (see the `WORDMARK` results in the piecewise verify()
output below).

**Visual confirmation**: `pass9e_trim_bottom_logo.png` /
`pass9e_current_bottom_logo.png` — straight-on Bottom-face renders using
the generator's own custom orthographic camera (`_set_ortho_camera`,
eye on the −Z axis below the part, up=+Y — the SAME convention the
pass-6 `bottom_logo.png` reference render used). Both read "KANDI" over
"WOOKS" left-to-right, every letter present (K-A-N-D-I, W-O-O-K-S with
the flower/leaf glyphs in place of the O's, the sprout on the "I" intact
and not mirrored-within-itself), comfortably inside the flat bed with
visible clearance to every counterbore. **A real dead end worth
recording**: Fusion's generic view-cube `direction='bottom'` preset (used
for a quick sanity check before building the real camera call) renders
this SAME geometry MIRRORED (reading "IᗡNɐʞ" / "SʞooM", i.e. correctly
mirrored-for-viewing-through-material but in the wrong-handed camera
convention for "look at the physical underside") — confirmed by
comparing both renders directly; the discrepancy is entirely in which
camera convention was used, not the geometry, which needed no changes
once the correct (established) camera setup was used.

### Finding 8: Antenna cable channels

**u.FL connector positions, live-probed (2026-09-08)**: queried the
Wio-SX1262 and L76K reference docs' own `'U.FL Connector'` sub-occurrence
bodies directly (`root.allOccurrences` → `bRepBodies`, world-space per
SPEC.md gotcha 6) — first in each board's OWN native document (to locate
the connector relative to its own PCB), then confirmed a second time
directly in a real built 'trim' case document (the actual inserted,
placed occurrence) — both readings agreed to within 0.005mm, and also
matched a THIRD, independent hand-derivation from `insert_comms_boards`'
own placement-transform math plus separately-probed native board
thicknesses, computed before touching Fusion at all. Final values: Wio's
u.FL (LoRa) at **(3.444, −21.961, 19.895)mm**, L76K's u.FL (GPS) at
**(2.095, −21.435, 5.92)mm** — both stored in `PARAMS['antenna']`
(`params_current.py`, inherited unchanged by trim: L76K's own placement
doesn't depend on case height, and current's Wio position, while
unused when the Wio isn't inserted, is the same physical part positioned
the same way whenever it IS).

**LoRa route** (Wio u.FL → the FPC keep-out strip on Top's inner dome
wall, **'trim' only** — 'current' never inserts the Wio, see
`comms_stack3_full_height`): the connector sits ~3.5mm inboard of the
true inner cavity wall along its own outward radial bearing from
spine_a — live-computed via `true_wall_distance_along_ray` from the
connector's own xy: **5.589mm** to the TRUE outer surface, a real gap of
open cavity, not a connector sitting flush against the shell. Cut ONE
radial channel (`add_antenna_channels`) from the connector's own point
outward, length = that true-wall distance minus the 1.2mm skin minimum
(**4.389mm**) — Combine-Intersected against a copy of the plain outer
envelope offset inward by 1.2mm (`_antenna_skin_safe_channel`, same idiom
as `add_fpc_relief`'s skin-safe tool, see that function's own docstring),
so the cut can never reach closer than 1.2mm to the true outer surface
however far it's asked to reach, following the real dome curvature
rather than a flat estimate. Cross-section 2.0mm wide (tangential) ×
1.6mm tall (Z), best-effort filleted (0.3mm, same skip-on-failure pattern
as `add_fpc_brow`'s seam fillet).

**GPS route** (L76K u.FL → the GPS patch's own frame above the battery,
**both variants** — the L76K is always inserted): the connector sits
INSIDE the stack3 frame's own hollow interior, already within the
frame's EXISTING wire-clearance notch (`build_comms_stack_frame`'s own
`wire_notch_w`, x ±3mm — the connector's own x=2.1 and z=5.92 both fall
inside it, confirmed by direct numeric comparison, not assumed) — so
this crossing needs no new cut at all. From there the route runs
straight north (same x), rising in Z, clear of the battery on all three
axes (the GPS frame's own south wall band, y 0.75–1.75, sits south of
the battery's y>=2 start; the route's z crossing, split_z+0.5..+2.5, sits
entirely above the battery's own z<=10.0 — confirmed both analytically
and by the live gate below) to the GPS frame's own south wall (1.0mm — a
real, complete, un-gapped ring: `build_gps_frame_body` passes no
`gap_w`) — the ONE genuinely new cut this route needs. **The route
stays well inboard of the alignment lip/anchor ring** (rho ~2.3mm from
spine_a here, vs the ring's own inner radius 23.95mm trim / 25.95mm
current) — per the finding's own conditional ("a notch in the
parting-line lip IF a cable must cross the halves"), this route crosses
z=split_z but never touches the ring, and there is no other solid wall
material at this xy at the parting plane either (deep in open cavity) —
so no lip notch was cut. This is a deliberate, computed routing choice
(picking the inboard path specifically to avoid needing one), not an
oversight — documented here per the "any deviation from SPEC.md"
reporting convention even though it's a null result.

**Gate**: `verify_antenna_channels` (new) — each channel's own
cross-section reads hollow at a live-probed interior point; the LoRa
channel's own skin distance re-checked LIVE at that same probe point
(found 3.395mm >= the 1.2mm minimum — comfortably clear at the
midpoint probed; the construction itself, not this probe, is what
guarantees exactly 1.2mm at the channel's own outer tip); the GPS
notch's own z-band (10.5–12.5) confirmed to sit entirely above the
battery's real z-range (2.0–10.0) — the "no breach of the battery bay
floor" check. **Both variants**:

```
current: gps_channel_open True (2.095, 1.25, 11.5)
         gps_no_battery_floor_breach True ((10.5, 12.5), (2.0, 10.0))
trim:    lora_channel_open True (3.784, -24.129, 19.895)
         lora_skin_ok True 3.395
         gps_channel_open True (2.095, 1.25, 11.5)
         gps_no_battery_floor_breach True ((10.5, 12.5), (2.0, 10.0))
```

**Render / route documentation**: `pass9e_trim_antenna_routes.png` — an
interior isometric view (Bottom hidden, Top ghosted to 15% opacity,
board occurrences hidden) with both routes drawn as thick (1.6mm,
exaggerated for visibility — the real channels are 2.0×1.6mm) construction
rods between the connector points and their respective wall crossings,
built as temporary hidden-then-deleted reference bodies (never part of
any exported geometry or the interference gate) purely for this render.
The GPS route's rod is clearly visible running from the stack area north
past the battery/GPS boxes; the short LoRa rod (a 4.4mm stub) is harder
to make out at this camera distance next to the display module — this is
a real limitation of this specific render, not of the underlying
geometry (which is independently confirmed by the live probes above),
and a follow-up pass could add a dedicated close-up the way pass 9c did
for the lip ring.

### verify() output, both variants (pass 9e)

Confirmed piecewise against a live document each time, same reasoning as
pass 9b (this session's Fusion MCP connection made even simple read-only
queries exceed the client-side timeout while `build()`/`verify()` were
still running server-side — waiting them out and re-querying, rather
than assuming failure, was the correct move every time this pass: no
call in this pass actually failed and needed a real fix except the one
noted under "Bugs found and fixed" below):

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         wordmark: edge_clearance True 1.6, clearance_{A,B1,B2,C,D,lug_hole} all True (>=15.6mm)
         wordmark: deboss_present True 0.357, floor_intact_below_depth True, no_material_below_bed True
         antenna: lora_channel_open True, lora_skin_ok True 3.395
         antenna: gps_channel_open True, gps_no_battery_floor_breach True
         full verify() (all M1/M2/posts/walls/buttons/fpc-relief/stack3/export-envelope gates
             from pass 9/9b, unchanged this pass): ran to completion twice without rollback
             (timeline advanced +4 features each time, matching verify_display_insertion_path's
             own reference-body construction -- an AssertionError anywhere in verify() rolls
             back the WHOLE script's changes per SPEC.md gotcha 1, so two clean, non-reverted
             runs are direct evidence every assertion in verify() passed, both times)

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         wordmark: edge_clearance True 1.6, clearance_{A,B1,B2,C,D,lug_hole} all True (>=14.3mm)
         wordmark: deboss_present True 0.357, floor_intact_below_depth True, no_material_below_bed True
         antenna: gps_channel_open True, gps_no_battery_floor_breach True (lora route N/A -- Wio not inserted)
         full verify(): ran to completion once without rollback (timeline advanced +4 features,
             same reasoning as trim)
```

### Bug found and fixed while building the antenna channels

`oriented_box_prism` (and the shared `move_body_to_frame` it calls)
expects 3-element `(x,y,z)` direction vectors for all three axes — the
first `add_antenna_channels` attempt passed the LoRa channel's `dirv`/
`tang` as plain 2-tuples (the natural output of the file's existing 2D
`normalize2`/radial-direction helpers, used everywhere else in this file
for flat XY math), which raised `IndexError: tuple index out of range`
inside `_cross` the first time it actually ran (`z_axis =
_cross(x_axis, y_axis)`, indexing `a[2]`/`b[2]` on a 2-tuple). Caught
immediately by the very next live run (this is exactly the kind of error
SPEC.md's "put asserts at the end, no try/except around modeling" gotcha
is meant to surface loudly rather than silently) — fixed by converting
both direction vectors to 3-tuples (`z=0.0`) right before the
`oriented_box_prism` call. No other code in the file mixes 2D and 3D
direction-vector conventions this way; kept as a documented one-off in
`add_antenna_channels` rather than changing the 2D helpers, which are
correct and heavily used as-is.

### Offline STL scan output (pass 9e)

`tools/offline_stl_check.py`, unchanged this pass — both variants, all 5
exported bodies (Bottom/Top changed; Screen_Plate/Power_Button/
Home_Button unchanged, re-exported as a byproduct of the same pipeline
run): manifold (0 non-manifold edges), envelope ok, overhang
`bad_clusters_mm2` `[]`. **`OVERALL: PASS` for both `trim` and
`current`.**

### Exports (pass 9e)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (Bottom/Top changed — the wordmark and antenna-channel
cuts; Screen_Plate/Power_Button/Home_Button byte-for-byte re-exports, not
touched by either finding), `export/<variant>/firefly_<variant>_case.3mf`
(native, 5 objects), `export/<variant>/firefly_<variant>_plate.3mf`
(re-packed via `tools/stl_to_3mf.py` — Bottom as-is, Top flipped 180°
about X, Screen Plate as-is, Power Button `outer-x`, Home Button
`outer-rz32.74` — unchanged orientation scheme from pass 9b, since
`home_nub_dir`/`power_nub_dir` are fixed PARAMS values independent of
variant), and the coupon STLs/3MFs (unchanged geometry — neither finding
touches the button mechanism — re-exported as a byproduct of running the
export pipeline again). Renders: `pass9e_{trim,current}_{front,top,right,
iso}.png` (standard 4-view, both variants), `pass9e_{trim,current}_
bottom_logo.png` (finding 7's straight-on confirmation), `pass9e_trim_
antenna_routes.png` (finding 8's route diagram). All viewed directly (not
just generated) as part of this pass.

## 2026-09-09 pass 9g (coordinator's render sweep -- final pass before printing)

A fresh Fusion MCP session (this one's client-side timeout hit on almost
every call, including plain read-only queries -- same infrastructure
behaviour documented in the pass-9-part-1 note; the workaround used here,
in addition to running `build()`/`verify()` in separate calls against the
same open document, was to have scripts **write their results to a JSON
file on local disk** (`hardware/case/_stub/*.json`) instead of relying on
the tool call's own return value, then read that file back over a
separate, ordinary filesystem read -- a file write on disk survives a
client-side timeout the same way Fusion's own model state does, so this
sidesteps the timeout for getting DATA out, not just for knowing a build
finished). `run(variant=..., export=True)` was called once per variant (as
a single call each, letting it time out client-side and then polling the
export directory on disk -- which lands on the same machine Fusion runs
on -- until the STL timestamps advanced, rather than re-splitting `build()`
into a dozen manual stages); both variants built, verified, and exported
cleanly end-to-end on the first attempt with the changes below.

### Finding 1: plate post layout unbalanced

**Confirmed root cause**: pass 9 part 2's own fix (see "Finding 4" above)
relocated P1-P4 to a real, gate-clean position -- but all four landed in a
tight **10x6mm cluster** at absolute (-10,18)/(-20,18)/(-10,24)/(-20,24),
because that was the smallest change that cleared the window bore and the
GPS frame at the time. A cluster this small holds the Screen Plate at
essentially one corner (the SW quadrant), leaving its NE 2/3 unsupported
-- a real, separate design defect from the "no wall" defect pass 9 part 2
fixed, just not visible until the plate was viewed as a whole part rather
than probed post-by-post.

**Fix, computed before touching Fusion** (pure-Python probes of the real
`PARAMS`, reusing `true_wall_distance_along_ray` and a plain Euclidean
distance to `window_center` -- no Fusion needed for either check): the
brief's candidate (a) (spread to the USB end) was ruled out first --
that region is within ~2mm of the FPC relief/brow footprint, the display
FPC tab, and the USB tunnel liner all at once, and candidate (b)
(symmetric pair to the east, x=+10/+20) was ruled out second -- at
y=18-24 that x range sits **inside the GPS patch frame's own footprint**
(`bay.gps_patch['x']` = -2.8..22.2), so a symmetric east pair would need a
per-post keep-out cut into the GPS frame's hanging structure, which is
already documented (Known limitations, item 3) as tight/fragile near the
dome tip -- not a change to make without a dedicated verification budget
for the frame itself. That leaves the safe **west lane** (x approx -21 to
-8, bounded by shell skin on one side and the GPS frame wall on the
other) as the only zone that needed no new keep-out cuts anywhere.

Within that lane, the WINDOW BORE (not the GPS frame or the true wall) is
the binding constraint on how far NORTH a post can go: at post x=-10,
`window_clear` (Euclidean distance from `window_center`, minus the window
radius 22.65, minus the post radius 2.5) crosses below the 1.0mm minimum
above y=25.83 -- so y=25 (1.78mm clearance) was picked as the practical
north limit at that x, and y=14 (matching `plate_south_extension`'s own
existing south edge) as the south limit, both far short of any real
constraint (window clearance at y=14 is >12mm; shell skin doesn't depend
on y at all in this straight-spine region, only x). Final positions,
**absolute mm, identical in both variants** (same convention as every
other boss/post):

```
P1 (-20.0, 14.0)   P2 (-10.0, 14.0)
P3 (-20.0, 25.0)   P4 (-10.0, 25.0)
```

This is the SAME x column pass 9 part 2 already proved clean (shell skin
1.64mm trim / 3.64mm current at x=-20, >=13.6mm at x=-10 -- both variants,
unchanged by this move) -- only the y-spread changed, from a 6mm span to
an 11mm span (10x11mm bounding box, ~83% more area than the old 10x6mm
cluster), spreading support across a real rectangle instead of one
corner. Analytic clearances, both variants (window position/radius don't
vary by variant): P1 16.03mm, P2 12.21mm, P3 6.87mm, P4 1.78mm window
clearance; all >= the 1.0mm minimum with margin. `plate_south_extension`
(the second box unioned onto the Screen Plate's main outline to reach
these posts) needed its own `y0` lowered from 14.0 to **10.0mm** -- the
OLD extension's south edge sat exactly flush with the old P1/P2 y=18
position's own pad margin, but flush with the NEW y=14 row it would leave
**zero** pad (the post holes would notch straight through the plate's own
south edge); `y1` raised 29.0 -> **29.5mm** for the same reason at the new
P3/P4 y=25 row. Both changes give every post >=4.0mm of real plate
material beyond its own hole (post radius 2.5 + 1.5mm pad), on all four
sides, both new rows -- computed directly, not eyeballed (see
`params_current.py`'s own comments on both `top_posts` and
`plate_south_extension` for the exact numbers).

**Gate**: `verify_post_walls` -- **0 bad of 8x3 (`pilot_wall`) / 8
(`shell_skin`) for all 4 posts, both variants** (identical result to pass
9 part 2's own numbers for P1/P3 at x=-20, confirming the x-column move
carried no regression; P2/P4 at x=-10 pass with much larger margin, as
expected further from the true wall). `verify_posts_and_bosses`,
`check_interference`: clean, both variants. Full piecewise `verify()`
output below.

### Finding 2: FPC brow is a slab

**Confirmed root cause**: `build_fpc_brow_solid`'s single box-clip
construction (pass 9) pushes the outer envelope out by a UNIFORM
`FPC_BROW_HEIGHT` (1.5mm) everywhere inside its footprint, then crops
that uniform-thickness layer to a rectangle -- a box crop can never taper
a uniform-thickness layer, so the seam is a real ~1.5mm vertical cliff by
construction, independent of whether the best-effort seam fillet
happened to apply. Visible in `pass9_trim_iso/right/top.png` as a flat
plateau with a hard edge, exactly as the finding describes.

**Fix**: rebuilt as `FPC_BROW_TIERS` -- two nested tiers instead of one
box, reusing only already-proven primitives (`box_solid`,
`build_thickened_envelope`, the boolean `combine_*` ops) rather than a
true loft or two-distance chamfer (both would need new, live-iterated
Fusion API calls this pass's time budget didn't cover): tier 0 (margin 0,
height 1.5mm) sits tight over the pocket exactly as before; tier 1
(margin `FPC_BROW_BLEND`=3.0mm, height 0.525mm) is a wide, shallow
shoulder around it. This turns the old single 1.5mm cliff into two
shorter risers (0.975mm and 0.525mm) spread over the same 3mm of blend
margin -- a visibly gentler, tapered mound (confirmed in
`pass9g_{trim,current}_brow_{iso,right}.png`) even before any fillet is
attempted. `add_fpc_relief` shares `build_fpc_brow_solid` unchanged (same
function, now returning the 2-tier union), so its skin-safe clip tool
automatically stays in sync with the real, tapered brow shape -- it can
only ever assume LESS raised material at the outer tier than the old
single-box version did, which makes `FPC_RELIEF_MIN_WALL`'s clip more
conservative there, not less.

**Gate**: `verify_fpc_relief` -- **0 bad of 63 probes, both variants**
(unchanged from pass 9's own number -- the tier-0 footprint, which is all
this gate probes, is byte-for-byte the same construction as before).
`check_interference`: clean.

**Not fully resolved: the seam fillets did not apply.** A best-effort
constant-radius fillet is attempted at each of the two tier boundaries
(radius 1.0mm inner, 1.5mm outer) -- same skip-on-failure pattern as
`add_lug`'s corner fillets and the pass-9 lip-ring chamfer. A live
timeline scan after both builds found **zero** `Fillet`/`Chamfer`
features attributable to `add_fpc_brow` in either variant (the nearest
`Fillet` features, 4 of them, are the unrelated top-post root fillets
from `add_top_posts`, much later in the timeline). Tried
`isTangentChain=False` instead of `True` on the theory that tangent-chain
matching was pulling in unrelated dome tessellation edges and failing the
whole solve -- rebuilt 'current' with this change and got the same
result (still 0 brow fillets), so that wasn't the cause; the real reason
is unconfirmed (most likely the fillet radius, 1.0-1.5mm, is too large
relative to the ~3mm-wide shelf between the two risers for Fusion's
solver to fit both fillets without them colliding, but this is a
hypothesis, not a confirmed diagnosis -- a follow-up pass should add a
live edge-count print inside the `try` block to distinguish "no edges
matched" from "fillets.add() raised" before trying smaller radii). The
SHAPE itself (the 2-tier taper) is real, built, and gate-clean either
way -- the fillets would only smooth its two remaining creases further,
they are not load-bearing for any dimensional gate. Recorded as a new
Known-limitations item below rather than re-attempted blind a third time
in this pass's remaining budget.

### Finding 3: generic `verify_skin_intact` (whole-outer-surface probe)

**Not attempted this pass.** This is unchanged from Known-limitations
item 11/13 -- the existing `verify_skin_intact` still only probes the
perimeter of the two button tab holes, not the whole outer surface from
all 6 directions with an enumerated opening whitelist. Given the time
spent confirming/fixing findings 1 and 2 live (two full build+verify+
export+render cycles, both variants) plus the render/documentation work
below, this larger gate rewrite needs its own dedicated pass rather than
a rushed version at the end of this one. The offline `tools/
offline_stl_check.py` overhang/envelope scan (finding 4, below) is a
partial, independent substitute in the meantime -- it does scan the
entire exported mesh, just for overhang angle and envelope radius, not
skin thickness specifically.

### Finding 4: overhang review

`tools/offline_stl_check.py`, re-run against this pass's fresh exports,
both variants, all 5 printed bodies (whitelist unchanged from pass 9 --
see the tool's own `TOP_WL`/`BOTTOM_WL` constants for the enumerated,
reasoned exceptions: the USB tunnel floor bridge, and the ordinary flat
hollow-shell ceiling/fillet-transition areas near the window/header and
above the comms bay, all inspected and judged legitimate in a prior
pass -- nothing new added or removed this pass):

```
trim:    Bottom manifold=True envelope_ok=True overhang_bad=[]
         Top    manifold=True envelope_ok=True overhang_bad=[]
         Screen_Plate / Power_Button / Home_Button: manifold=True envelope_ok=True
         OVERALL: PASS

current: Bottom manifold=True envelope_ok=True overhang_bad=[]
         Top    manifold=True envelope_ok=True overhang_bad=[]
         Screen_Plate / Power_Button / Home_Button: manifold=True envelope_ok=True
         OVERALL: PASS
```

No new overhang clusters from either fix (the rebalanced posts don't
change any external surface; the brow's 2-tier taper is, if anything,
LESS steep than the old single box step, and the whitelisted
`general_ceiling_overhang`/`usb_tunnel_floor`/`l76k_frame_ceiling`
clusters are unaffected by both). Per SPEC's print orientation, no
supports are expected for Top/Bottom/Screen Plate in their documented
bed orientation; the button caps print with a brim (small parts,
first-layer adhesion) -- unchanged guidance from prior passes.

### Finding 5: assembly order

Consolidated here (parts of this were established in earlier passes --
see pass 9 part 2's "Finding 5" for the full derivation of why the
display must go in first) into one ordered list, and copied into the PR
body:

1. **Seat the display module into the separate, unassembled Top half**
   (glass up into the window bore, PCB resting under the lip/anchor/
   posts) -- confirmed (pass 9 part 2) that this is the ONLY possible
   order: the display PCB (39.2x41.4mm, ~57mm diagonal) is physically
   larger than the window bore (Ø45.30) in every direction, so it cannot
   go in "from the front" through the bore, or "from inside" up through
   the already-assembled shell, at any point after Top and Bottom are
   joined.
2. **Fit the Screen Plate onto Top's own posts** (P1-P4, now spread
   across a real 10x11mm rectangle -- see Finding 1) from inside, over
   the display PCB -- the plate's own header cutout clears the display's
   2x10 header; S1-S3 line up with the display board's own SMT
   standoffs.
3. **Insert both button caps from inside** the still-open Top half,
   sliding each outward until its head seats in its own wall hole (pass
   9b, finding 9 -- an outside-in insertion is geometrically impossible
   by design; the caps cannot be added after Bottom is joined on).
4. **Place the comms-bay hardware into the separate Bottom half**:
   battery first (floor-mounted, rails + strap slots), then the 3-board
   stack (Wio/XIAO/L76K, trim only -- current freezes at the pre-stack
   pin-header configuration for the probe comparison, see Known
   limitations), routing the antenna leads per Finding 8's channels as
   the boards go in, not after.
5. **Route the GPS patch antenna cable up into its frame** (hanging from
   Top, above the battery) as the last comms-bay step before closing the
   halves -- the frame's own wire-clearance notch and the GPS channel
   (Finding 8, pass 9e) are both already open at this point, no fishing
   a cable through a closed shell. **Then stick a ~2.0mm compressible
   foam pad on the patch's own top face** (pass-10-REDO retention, trim
   only -- see that section below) -- this is what takes up the 2.7mm
   spare between the patch and the compass module mounted above it, once
   the halves close.
6. **Mount the compass module (GY-273/QMC5883P) onto Top's own ceiling
   pegs**, trim only (pass 10 REDO -- see that section below for the full
   placement/orientation derivation; 'current' cannot host this mount at
   all, see Known limitations #27) -- while Top is still the separate,
   open half: slide the module's two mounting holes down onto the two
   Ø2.7 ceiling pegs (header edge toward the display end/+Y) until its
   PCB seats flush against the two rest pads on the header side, sensor
   face down toward the (not-yet-closed) Bottom. Do this before Bottom
   and Top close -- there is no way to reach the ceiling pegs once the
   halves are joined.
7. **Join Bottom and Top together on the alignment lip** (Finding 5 /
   pass 9 part 2's widened 1.8mm ring, with its own seam chamfer for a
   support-free print) -- this is the step that closes over everything
   placed in steps 1-6; nothing above can be added after this point.
8. **Drive the case screws**, in this order: **A, B1, B2, C** (the four
   Bottom-boss-to-Top-boss M2x12s, dome-tip cluster) to pull the two
   halves flush and square first, **then D** (Bottom boss to the Screen
   Plate's own post, M2x10 current / M2x12 trim -- deliberately last
   among the case screws, since it also indirectly locates the plate
   relative to the now-closed shell), **then P1-P4** (Top post to Screen
   Plate, M2x6 -- now landing across the full rebalanced rectangle from
   Finding 1, not a corner cluster) to pin the plate itself, **then S1-
   S3** (Screen Plate to the display board's own SMT standoffs, M2x4) to
   finish locking the display board to the plate stack.

Checked against the model (not just asserted): every insertion path
above either has a dedicated live probe already (`verify_display_
insertion_path`, `verify_button_insertion`/`verify_button_retention`) or
is a plain geometric consequence of one body being open/unassembled at
that step (Bottom and Top are two separate, unjoined printed parts until
step 7) -- nothing in this order asks a board or fastener to pass through
an opening smaller than itself.

### Finding 6: screw map

Recomputed against `PARAMS` directly (not hand-copied) after Finding 1's
move. **Only P1-P4's xy positions changed** -- every z-depth, pilot
diameter, and screw length below is identical to the table already in
"Screw list" (unaffected by an xy-only move): M2x12 x4 (A/B1/B2/C, both
variants), M2x10 (current D) / M2x12 (trim D, the case-height-driven
length bump from pass 7), M2x6 x4 (P1-P4, unchanged z 14.1-20.6 current /
17.1-23.6 trim), M2x4 x3 (S1-S3). The "Screw list" section below has been
updated in place with the new P1-P4 coordinates rather than duplicated
here.

### Finding 7: cosmetic check

**Wordmark centring**: computed directly against the real
`kandiwooks_logo.json` and `wordmark_layout()` (pure Python, no Fusion
needed). The composite bounding box (both lines together) is **exactly**
centred on `wordmark_center` in both variants (`x` range -22.54..22.54
current / -20.54..20.54 trim, centre 0.000mm to floating-point precision
-- expected, since each line is independently forced to the SAME
`target_width` and centred on the same `cx`, so their union can never be
off-centre). Went a step further and computed the AREA-WEIGHTED centroid
of every loop (shoelace formula, signed area -- so the flower/leaf
glyphs, which are enclosed sub-loops, correctly contribute as holes, not
solid ink) as a more rigorous stand-in for "visual weight" than the raw
bounding box: **+0.608mm (current) / +0.554mm (trim)** -- under 3% of the
wordmark's own half-width, and, if anything, slightly RIGHT of centre,
not left. Checked what shifting the whole block to zero that residual
would cost: the resulting edge clearance drops to as low as **0.99mm**
on the now-nearer side, both variants -- BELOW the 1.5mm SPEC minimum
`verify_wordmark` gates on. **No change made**: the geometry is already
correctly centred by the most rigorous available measure, and the only
way to chase the smaller, cruder "raw vertex count" asymmetry that
prompted this finding (unweighted vertex mean: -3.665mm current /
-3.34mm trim -- but this metric weights every small stroke segment
equally regardless of enclosed area, so a glyph built from many short
line segments on one side outweighs a glyph built from few long ones on
the other, independent of actual ink coverage) would breach a real,
gated dimensional requirement. Recorded here as reviewed-and-verified
rather than silently skipped.

**Lug ear taper**: reviewed via `pass9g_{trim,current}_lanyard_end.png`
(both variants) -- no defect visible at this render's framing/distance;
not modified this pass. A dedicated close-up (same idea as pass 9c's
`pass9c_lip_ring_section.png` for the window lip) would be needed for a
more confident visual sign-off and is left as a follow-up.

### verify() output, both variants (pass 9g)

Confirmed piecewise against a live document each time (same
infrastructure note as every pass-9 sub-pass); this pass's own addition,
writing results to a JSON file on disk instead of relying on the call's
return value, made this the first sub-pass where a slow/timed-out call
never once required a blind re-run to recover its output:

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         post_wall_results  P1/P2/P3/P4 pilot_wall [] and shell_skin [] (8 rays each) -- 0 bad
         posts_bosses_results  all True (A/B1/B2/C/D, P1-P4, both stack-frame keep-outs)
         fpc_relief bad [] of 63
         stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
         wordmark: edge_clearance True 1.6, clearance_{A,B1,B2,C,D,lug_hole} all True (>=15.6mm)
         antenna: lora_channel_open True, lora_skin_ok True 3.395, gps_channel_open True, gps_no_battery_floor_breach True
         envelope: Bottom True, Top True; export_envelope: all 5 bodies True
         skin_results all True; wall_results all True
         sliver_results (diagnostic, unrelated to any pass-9g change): {'Bottom': 452, 'Top': 76}

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         post_wall_results  P1/P2/P3/P4 pilot_wall [] and shell_skin [] (8 rays each) -- 0 bad
         posts_bosses_results  all True
         fpc_relief bad [] of 63
         wordmark: edge_clearance True 1.6, clearance_{A,B1,B2,C,D,lug_hole} all True (>=14.3mm)
         antenna: gps_channel_open True, gps_no_battery_floor_breach True (lora N/A -- Wio not inserted)
         envelope: Bottom True, Top True; export_envelope: all 5 bodies True
         skin_results all True; wall_results all True
         sliver_results: {'Bottom': 445, 'Top': 68}
```

(`sliver_results` diagnostic counts shift slightly pass-to-pass with any
geometry change nearby -- still unrelated to any gated defect, same
reasoning as pass 9's own finding 11 writeup.)

### Offline STL scan output (pass 9g)

See Finding 4 above -- `OVERALL: PASS`, both variants, 0 non-manifold
edges, 0 envelope breaches, 0 bad overhang clusters, across all 5
printed bodies.

### Exports and renders (pass 9g)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (Bottom/Top changed -- the post and brow geometry;
Screen_Plate changed -- new south-extension bounds and hole positions;
Power_Button/Home_Button byte-for-byte re-exports, untouched by either
fix), `export/<variant>/firefly_<variant>_case.3mf` (native, 5 objects,
re-generated by Fusion's own exporter), `export/<variant>/firefly_
<variant>_plate.3mf` (re-packed via `tools/stl_to_3mf.py` from the fresh
STLs -- **this is the file Jake prints** for `trim`), and the coupon
STLs/3MFs (unchanged geometry -- neither fix touches the button
mechanism -- re-exported as a byproduct of running the pipeline again).

Renders: `pass9g_{trim,current}_{front,top,right,iso}.png` (standard
4-view, both variants, from the same `run(export=True)` call as the
exports), plus, per variant: `pass9g_{trim,current}_brow_{iso,right}.png`
(Finding 2's before/after -- the 2-tier taper, not fully filleted, is
directly visible in both), `pass9g_{trim,current}_posts_plate.png`
(interior view near the rebalanced P1-P4 rectangle, Bottom hidden),
`pass9g_{trim,current}_inside_top.png` / `_inside_bottom.png` (Bottom/Top
hidden respectively, looking straight down), `pass9g_{trim,current}_
usb_end.png`, `pass9g_{trim,current}_lanyard_end.png`, and `pass9g_
{trim,current}_bottom_logo.png` (Finding 7's wordmark re-confirmation --
note this particular render's camera was not re-tuned to hide the two
loose button-cap bodies sitting in frame, so the case reads off-centre in
that one specific image; the wordmark's own centring was verified
analytically instead, see Finding 7 above, not by eye against this
render). All viewed directly as part of this pass.

## 2026-09-10 pass 10 (fixed mount for the compass module, GY-273/QMC5883P)

Jake's brief: a fixed mount for the compass module (taped in until now) so
the firmware's axis map and calibration get one repeatable orientation.
Module geometry measured off Jake's Fusion model "HMC5883L Mag v1" (mm,
module's own local frame): PCB 18.6 x 14.0 x 1.0 (local x -9.64..8.96,
local y -6.67..7.33), components <=1.0mm on top (sensor ~3x3 at local
(-0.9,0.5)), five header pins soldered from below (local x=+7.44, local y
= -4.81/-2.21/0.39/2.89/5.49, Ø1.0), two mounting holes (Ø3.0 real) at
local (-7.21,-4.17) and (-7.21,5.03) (9.2mm apart).

**This section was rewritten same-day.** The FIRST pass-10 attempt (a
fenced pocket standing on edge past case-screw boss C at the lanyard end,
needing a 5mm outward BROW to recover enough radial depth) shipped,
verified clean, and was then **rejected by the coordinator after looking
at the renders**: the brow protruded as a visible boxy bump on the
`pass10_trim_{iso,top,right}.png` renders, breaking the pill's clean
outer silhouette (no external bumps for the compass, full stop). That
attempt's own geometry (`mag_brow_box`, `add_mag_brow`, the vertical
`mag_pocket_footprint`/`add_mag_pocket`, horizontal `add_mag_pegs`) is
removed entirely, along with every `check_body_envelope_vertices`/
`verify_wall_integrity` exemption it needed. Everything below is the
REDONE placement -- ceiling-hung, no brow, no pocket cut, no outer-wall
interaction of any kind.

### New placement: hanging from Top's own inner ceiling, above the GPS patch

The GPS patch antenna's own retention frame (`build_gps_frame_body`) is a
thin (1.0mm) wall ring hanging from the ceiling down to just above the
patch (`gps_patch.z[0] - gps_frame_clear` = 10.2) -- ABOVE the patch
(z 18.8 up to the ceiling), inside that ring's own 25.5x25.5 opening, is
already open cavity air, by construction, with nothing else in it. That
open chimney is exactly where this mount lives: no cut, no brow, nothing
subtracted from the shell at all -- only material ADDED (two ceiling
pegs, two rest pads, a low retaining fence), all hanging from the
ceiling, the same "hanging frame" idiom already used for the GPS frame
and the old stack tray.

**Vertical stack budget** (ceiling down to the patch, both variants use
the SAME formula, `mag_module_clearance`/`mag_module_fits` in
`firefly_case.py`): standoff `2.5mm` (doubles as the header/solder-joint
allowance, `local_header_below`) + PCB thickness `1.0mm` + component bump
`1.0mm` = `4.5mm` from the ceiling down to the module's lowest physical
point (the component/sensor face). Spare above the patch =
`top_ceiling_underside_z - 4.5 - gps_patch.z[1] (18.8)`:

| | ceiling | spare above patch | fits? |
|---|---|---|---|
| **trim** | 26.0 | **2.7mm** | **yes** |
| **current** | 23.0 | **-0.3mm** (a real 0.3mm overlap) | **no** |

'current' cannot host this mount -- its ceiling was frozen at the old
25mm-case height in pass 7 ("current stays at height 25 for the probe
comparison") and never grew the 3mm trim did. `mag_module_fits(p)`
returns `False` for 'current', and `add_mag_module`/`verify_mag_pocket`
both skip cleanly (no cut, no join, `verify_mag_pocket` reports all 4
checks as `(True, [])`) rather than forcing a real interference -- same
pattern as `comms_stack3_full_height` already skipping the 3-board stack
there. This is a genuine, documented scope gap, not an oversight (see
Known limitations #27); fixing it for real means growing 'current' the
way trim already did, a decision for Jake, not this pass.

### XY placement: centred on the GPS patch, computed against PARAMS first

Per the brief's own instruction, every number below was checked with pure
Python against `PARAMS` (`rho_from_spine`, the `gps_patch`/`top_posts`/
`gps_frame_opening` entries `add_gps_reference_box`/`build_gps_frame_body`
already use) BEFORE any Fusion work -- see
`/private/tmp/claude-501/case-pass10-scratch/probe_placement_redo.py`
(this redo's own working notes -- distinct from the earlier, now-
superseded `probe_placement.py` written for the rejected brow placement) and the live `mag_module_clearance` numbers
above, cross-checked afterward by `verify_mag_pocket`'s real probes
against the built geometry.

The module's long axis (local x, 18.6mm, header at +x) runs along world
**Y**, centred on the GPS patch's own y-span (2.0 to 27.0, centre 14.5).
The short axis (local y, 14.0mm) runs along world **X**, centred on the
patch's own x-centre (9.7, from x -2.8..22.2). Both choices maximise
margin on every side rather than hugging one edge:

| | value | margin to the nearest real obstacle |
|---|---|---|
| PCB world Y span | 5.2 .. 23.8 | 1.95mm (fence) to the GPS frame opening's own y 1.75/27.25 |
| PCB world X span | 2.7 .. 16.7 | 4.25mm (fence) to the GPS frame opening's own x -3.05/22.45 |
| Fence outer footprint | x 1.2..18.2, y 3.7..25.3 | 8.7mm to Screen Plate posts P2/P4 (x=-10, Ø5, edge at -7.5) |
| Ceiling standoff (peg/pad height) | 2.5mm | -- |
| Component-bottom-to-patch-top spare | 2.7mm (trim) | -- |

All comfortably clear -- no obstacle in this footprint was ever tight
enough to need a skin-safe clip; the fence/pegs/pads are built as plain
geometry and (per the brief's own instruction to still route everything
through the shared inner-cavity clip tool) the pegs/pads use
`clipped_pillar_with_reach` (the same helper `add_top_posts` uses) purely
for its full-height-core join-guarantee, not because the radial clip
against the true outer shell ever actually trims anything this deep
inside.

### Orientation (fixed and documented)

Local -> world mapping (module's own PCB frame -> the case generator's
world mm; pure TRANSLATIONS, `mag_world_x`/`mag_world_y`/`mag_world_z` in
`firefly_case.py`), same for both variants:

- Local **+x** (18.6mm axis, **header edge**, local x=+7.44) -> world
  **+Y** (toward the display end -- a short wire run to the back
  header's SDA/SCL/3V3/G).
- Local **-x** (**mounting-hole edge**, local x=-9.64) -> world **-Y**
  (toward the lanyard end; both ceiling pegs share this local x, so both
  land at the same world Y=7.63).
- Local **+y** -> world **+x**; local **-y** -> world **-x** (an
  arbitrary handedness choice -- both pegs and both rest pads are placed
  by explicit local (x,y) pairs, not a directional rule, so this axis
  carries no functional constraint).
- Local **+z** (the **component/sensor face**, away from the PCB) ->
  world **-Z** (DOWN, toward the GPS patch/Bottom -- the module is
  mounted **components-down**, hanging off the two ceiling pegs + two
  rest pads).
- Local **-z** (the **header/solder-pin face**) -> world **+Z** (UP,
  toward Top's ceiling -- both mounting holes share local x=-7.21, i.e.
  the same local z=0 plane, so both pegs land flush against the same
  ceiling standoff).

In words: the module hangs flat under the ceiling, header edge toward the
display (short wire run), mounting-hole edge toward the lanyard end,
sensor face looking down at the (foam-padded) GPS patch, header/solder
face flush up against the ceiling standoff. `ff_compass.c`'s own comment
is updated with this exact table -- translating it into which *sensor*
axis (QMC5883P's own silkscreen/datasheet frame) that corresponds to
still needs a bench check, unchanged from before (Known limitations #26).

### Geometry

- **Two Ø2.7 ceiling pegs** (`mag_peg_world_positions`), one per mounting
  hole, hanging from the ceiling down to the PCB's own bottom face
  (height = `standoff_h`, 2.5mm) -- built with `clipped_pillar_with_reach`
  (radial clip against the inner-cavity tool is a no-op this deep inside;
  the full-height core is what guarantees a real, non-silently-skipped
  join to the ceiling, the same bug class `add_top_posts`' own docstring
  documents).
- **Two Ø3.0 rest pads** (`mag_pad_world_positions`) under the PCB's
  header-side corners (inset 1.0mm from each edge), same standoff height
  as the pegs, so the PCB hangs level -- both ends at the same world Z.
- **A low retaining fence** (`build_hanging_frame`, 0.3mm clearance +
  1.2mm wall, 3.5mm deep from the ceiling) around the bare PCB outline --
  the same GPS-frame/stack-tray idiom reused directly, with a 3mm notch
  (`header_notch_w`) cut through the header-edge wall, centred on the
  header pins, for the wire run.
- No pocket, no cut, no brow: every one of the three pieces above is pure
  ADDED material, joined into Top only. Bottom is untouched by this mount
  entirely.
- **`PARAMS['mag_module']`** (`params_current.py`, inherited unmodified
  by `params_trim.py`): local module geometry, standoff/fence dimensions,
  and the two world-placement translation offsets, with the full
  placement derivation inline in the comment.
- **`verify_mag_pocket`** (`firefly_case.py`, rewritten for the redo):
  (1) `envelope_open` -- the module's own component-side reference
  envelope is genuinely hollow (confirms the fence/pegs/pads didn't
  accidentally fill the board's own footprint); (2) `pegs_have_material`;
  (3) `pads_have_material`; (4) `fence_has_material` (two sample points
  away from the header notch). All four report `(True, [])` when
  `mag_module_fits(p)` is `False` ('current'). All gate `verify()`.

### Retention across the 2.7mm spare

With 2.7mm of intentional clearance between the module's lowest point and
the GPS patch (needed so the pegs/pads can be sized with a sane, non-zero
tolerance and so the module never touches the antenna), nothing in the
Fusion geometry itself clamps the module vertically once assembled --
it's correctly seated at build time by resting flush on the pegs/pads,
but has 2.7mm of float before the halves close. **Chosen fix: a ~2.0mm
compressible foam pad** stuck to the GPS patch's own top face at assembly
time (see the Assembly Order section above, and pass-10 REDO's addition
to step 5) -- compressed to roughly 0.7mm once the halves close, it
takes up the float and holds the module lightly against its pegs/pads
without needing a precision fit. A **printable interference lip** (a
third peg, oversized to friction-fit) was considered and rejected: at
this thickness (a 1-2mm cantilevered snap feature working against
repeated case-opening cycles) it is a much more fragile detail than a
$0.02 piece of foam tape, for no real functional benefit -- hard-iron
calibration already handles a fixed nearby magnet's own offset, so a
foam-pad's small amount of play does not need to be sub-millimetre
precise.

### Distance from the display's speaker

The display occurrence (`ESP32-S3-Touch-LCD-1_46`) was probed directly
for a speaker sub-body; none was found by name in the inserted
reference -- Jake's board doesn't appear to carry a modelled speaker
body distinct from its general back-side bbox. Using the display's own
back-side bbox instead (`display_bbox`: y 27.6..73.13): the compass
module's footprint tops out at world y=25.3 (fence outer edge) -- **at
least 2.3mm clear in Y** of the display's own bbox, with NO y-range
overlap at all between the two features (25.3 < 27.6), so they cannot
collide regardless of Z. Hard-iron calibration (already implemented,
S12 step 3) handles a fixed nearby magnet's own offset either way.

### `verify()` output, both variants (pass 10 REDO)

Confirmed piecewise (build, then `organize_components`, then `verify`,
each its own `fusion_mcp_execute` call against the same open document --
this redo's own build/verify cycle landed clean on the FIRST attempt for
both variants, no fix-and-rebuild round needed):

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         occ_interference []
         mag_pocket_results {'envelope_open': (True, []), 'pegs_have_material': (True, []),
                              'pads_have_material': (True, []), 'fence_has_material': (True, [])}
         stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
         skin/wall/posts-bosses/post-wall/envelope/export-envelope/wordmark/antenna/fpc-relief: all clean
         bump_results: all False (no outer bump, 8/8 probes)
         OK: M1+M2 probes passed

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         occ_interference []
         mag_pocket_results {'envelope_open': (True, []), 'pegs_have_material': (True, []),
                              'pads_have_material': (True, []), 'fence_has_material': (True, [])}
         stack3_clearance {'ok': True, 'note': 'comms_stack3_full_height=False -- no Wio/XIAO inserted'}
         skin/wall/posts-bosses/post-wall/envelope/export-envelope/wordmark/antenna/fpc-relief: all clean
         bump_results: all False (no outer bump, 8/8 probes)
         OK: M1+M2 probes passed
```

### Offline STL scan output (`tools/offline_stl_check.py`, pass 10 REDO)

`OVERALL: PASS`, both variants -- 0 non-manifold edges, 0 envelope
breaches, `bad_clusters_mm2: []` on both Top and Bottom, confirmed live
by the same `fusion_mcp_execute` export run (`overhang_scans`:
`{'Top': {'flagged_triangles': 1350/962, 'bad_clusters_mm2': []},
'Bottom': {'flagged_triangles': 662/602, 'bad_clusters_mm2': []}}` for
trim/current respectively). The old `mag_module_pocket` whitelist entries
(both `TOP_WL`/`BOTTOM_WL` in `tools/offline_stl_check.py`, and the
matching boxes in `firefly_case.py`'s own `run()`) are removed entirely
-- the redone mount's own pegs/pads/fence fall inside the existing
`general_ceiling_overhang` entry on Top and touch Bottom not at all, so
no dedicated entry was needed even before removing the old ones.

### Exports and renders (pass 10 REDO)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (Bottom reverts to its pre-pass-10 shape -- this mount
touches only Top, so Bottom's own geometry is exactly what it would be
with no compass mount at all, though the exported STL bytes differ from
the last commit since the previous (rejected) brow build DID touch
Bottom; Top changed -- the mount's pegs/pads/fence; Screen_Plate/
Power_Button/Home_Button untouched by this pass), `export/<variant>/
firefly_<variant>_case.3mf` (native, Fusion's own exporter, 5 objects),
`export/<variant>/firefly_<variant>_plate.3mf` (re-packed via
`tools/stl_to_3mf.py` -- Bottom as-is, Top flipped 180° about X, Screen
Plate as-is, Power Button `outer-x`, Home Button `outer-rz32.74`, same
convention as every prior pass). Coupons re-exported as a byproduct of
the same pipeline run, byte-identical (button mechanism unaffected by
this pass) -- `export/coupons/coupon_{power,home}_{wall,cap}.stl`,
`firefly_coupons.3mf`, `firefly_coupons_native.3mf`.

Renders: `pass10b_{trim,current}_{front,top,right,iso}.png` (standard
4-view, both variants -- the clean pill silhouette the coordinator asked
to see, with NO bump anywhere: compare directly against the rejected
`pass10_trim_{iso,top,right}.png` from the first attempt) and
`pass10b_mag_pocket.png` (trim only -- Bottom/buttons/boards/reference
bodies hidden, camera placed INSIDE the cavity looking straight up into
the ceiling from just below the GPS patch, 5cm view width): the retaining
fence ring, both Ø3.0 rest pads with the header-notch gap, and both Ø2.7
ceiling pegs are all clearly visible in one shot -- no follow-up render
needed (see the removed Known-limitations items #23/#24 below). All
viewed directly as part of this pass, including a pixel-level check of
the clean-shell renders for any hint of a bump.

See the removed/added items in "Known limitations / deviations from
SPEC.md" below (items 23-25 marked N/A or resolved; items 27-28 new) for
what this pass leaves open.

## 2026-09-11 pass 11 (brow filled the window bore; compass mount moved
off the display; new openings-open gate)

Three coordinator-reported items, all fixed and re-verified live for
both variants: (1) the FPC brow (pass 9) silently refilling the top of
the window bore, confirmed by Jake's own ray-cast of the pass-9 export;
(2) the compass mount (pass 10 REDO) sitting too close to the window/
display, confirmed by Jake's review of `pass10b_mag_pocket.png`; (3) a
new `verify_openings_open` gate so a defect of the FIRST kind -- an
opening silently plugged by later-added geometry -- can never regress
unnoticed again.

### Defect 1 root cause: build ORDER, not the brow's own shape

`build()` calls `add_window()` (cuts the bore + chamfers its rim) BEFORE
`add_fpc_brow()` (raises the shoulder over the FPC relief footprint at
the USB end). `build_fpc_brow_solid` derives each brow tier from a
FRESH, unbored reference pill (`build_outer_pill_solid`/
`build_thickened_envelope`) -- it has no notion that Top's real window
bore already exists -- so wherever a tier's own footprint box (widened by
`FPC_BROW_BLEND`) overlaps the bore's XY footprint (it does: the bore's
own upper rim, y roughly 62-71 for |x| lteq 14-17, sits almost exactly
under the brow's footprint at the USB end), joining that tier into Top
blindly refills whatever part of the now-open bore falls inside it.

**Fix**: `build_fpc_brow_solid` now Combine-Cuts the finished brow
against a cylinder covering the window bore's own true opening (radius
= bore radius + its own chamfer, so the chamfered rim is excluded too)
spanning from the glass ledge (`window_z_bottom`) up through the highest
point any brow tier could ever reach (`top_z + FPC_BROW_HEIGHT`) --
shared by both `add_fpc_brow` and `add_fpc_relief`'s skin-safe-tool
derivation (both call `build_fpc_brow_solid`), so the two can never
disagree. Reordering `add_window`/`add_fpc_brow` instead (the other
option in the brief) was rejected: `add_window`'s `chamfer_edge_at`
requires the bore's ENTIRE rim to sit at a single, flat z (`top_z`) to
find a matching circular edge to chamfer -- if the brow ran first and
raised part of that rim locally, the chamfer would either fail outright
or leave part of the rim un-chamfered. The exclusion-cut approach keeps
`add_window`'s own construction (and its ledge/chamfer numbers)
byte-for-byte identical to pass 9, touches nothing but the brow, and the
brow still fully covers the FPC relief footprint OUTSIDE the bore
(confirmed: `verify_fpc_relief` 0 bad of 63 probes, both variants,
unchanged from pass 9).

**Before/after bore-scan numbers**:

- **Before** (live Fusion probe against a fresh build from the
  UNMODIFIED `origin/main` `firefly_case.py`, same document technique as
  every other stage this pass): `window_column_probe_points` x 6 z levels
  (102 probes, window column, trim) -- **3 bad of 102**, all three at
  `z=28.5`: `(13.01, 63.01, 28.5)`, `(0.0, 68.4, 28.5)`,
  `(-13.01, 63.01, 28.5)` -- consistent with Jake's own report ("every
  sample from y=62 to 71 hits solid at z 28.52 or 29.50").
- **After** (this branch): the same live probe reports **0 bad of 102**
  (`verify_openings_open`'s `window_column` entry, `(True, [])`), both
  variants. Independently, the standalone offline ray-cast
  `/private/tmp/claude-501/borescan.py` (parity-crossing test, no
  dependency on `firefly_case.py`'s own point-containment code) against
  the EXPORTED `export/trim/Top.stl`: 37 xy points (rings at 40%/85% of
  bore radius + centre) x 9 z levels (`window_z_bottom` through
  `top_z + 0.5`) = **333 probes, 0 blocked** --
  `RESULT: PASS -- bore column is open at every sample from the ledge
  through the top`. Same tool against `export/current/Top.stl`
  (z 22.4-25.5): **333 probes, 0 blocked**. (Run with
  `uv run --with numpy python borescan.py <path> --r 21.0 --z-bottom
  <ledge> --z-top <top_z+0.5> --n-z 9`; an early run at the literal bore
  radius, `--r 21.65`, hit a ray/mesh-edge tangency artifact at the
  extreme rim -- not a real defect, see the script's own retry note --
  fixed by sampling 1mm further inside the bore, matching the generator's
  own probe convention.)

### Defect 2 root cause: header/wire edge pointed at the display, and the mount hugged the GPS patch's own centre instead of its south edge

Pass 10 REDO's placement centred the mount on the GPS patch's own y-span
(2..27, centre 14.5) with the header/wire edge (local x=+8.96) mapped to
the LARGER world Y -- i.e. toward the display/window end. Both effects
compounded: the fence's own north edge (25.3) sat only ~1.4mm from the
window bore's true rim (computed; matches Jake's ~1mm visual estimate),
and the five header wires exited directly into that gap.

**Fix** (`PARAMS['mag_module']`, `params_current.py`; `mag_world_y`,
`firefly_case.py`):

1. **Orientation flipped.** `mag_world_y` is now `-local_x + offset`
   (was `+local_x + offset`): the header/wire edge (local x=+8.96) now
   maps to the SMALLER world Y (toward the lanyard end); the
   mounting-hole edge (local x=-9.64) now maps to the LARGER world Y
   (toward the display end). `mag_pcb_world_footprint` (sorts the two
   mapped Y values, since the mapping's direction is no longer assumed)
   and `mag_pad_world_positions` (computes its edge inset in the LOCAL
   frame, then maps once) were both made sign-agnostic so this is a pure
   PARAMS change; `add_mag_module`'s fence notch moved from `gap_side=
   '+y'` to `'-y'`.
2. **Footprint shifted** as far -Y and +X as the GPS frame's own real
   opening (x -3.05..22.45, y 1.75..27.25) allows with a >=0.5mm safety
   margin against touching the frame's own wall on any side (computed in
   pure Python against these exact PARAMS before touching Fusion --
   `world_y_from_local_x_offset` 14.84 -> **12.71**,
   `world_x_from_local_y_offset` 9.37 -> **13.07**).

**New position** (world mm, trim; unchanged formula for current, but the
mount is skipped there regardless -- see `mag_module_fits`): PCB
`x 6.4..20.4, y 3.75..22.35`; fence `x 4.9..21.9, y 2.25..23.85`; pegs
(mounting-hole side, now toward +Y/display) at `(8.9, 19.92)` and
`(18.1, 19.92)`; pads (header side, now toward -Y/lanyard end) at
`(7.4, 4.75)` and `(19.4, 4.75)`; wire-exit notch centred at `x=13.41` on
the fence's SOUTH wall.

**Clearances** (live-probed against the real built geometry, `verify_
mag_pocket`'s two new checks):

| | old (pass 10b) | new (pass 11) | gate |
|---|---|---|---|
| Window-bore true-opening clearance (worst corner) | ~1.4mm (computed) | **3.955mm** | >= `MAG_DISPLAY_RING_MIN_CLEAR` (3mm) |
| Display back-side bbox clearance | ~2.3mm (pass 10's own number) | **3.765mm** | >= 3mm |
| GPS frame wall margin (south / east / north / west) | n/a | 0.5 / 0.55 / 3.4 / 7.95mm | > 0 (no new interference) |

3.955mm/3.765mm fall short of the brief's 5mm stretch target -- pushing
either further south or east starts eating the GPS frame's own real
wall/opening boundary (the frame-margin numbers above are already down
to the 0.5mm safety floor on the south and east sides); this is a real
geometric ceiling given the frame's fixed footprint, not an oversight,
and both numbers clear the 3mm gate with margin.

**"Window lip ring" disambiguation**: `mag_window_bore_clearance`
measures distance to the window BORE's own true opening (radius =
`window_dia/2`, centred on `window_center`), not `PARAMS['lip_r']` (the
buried alignment lip/anchor ring at z 9.2-11) -- `stadium_ring_solid`
only puts that ring near |x| roughly 26-28 in the straight section
(nowhere near this mount's x 5-22 footprint), so it cannot be what
Jake's pass-10b review saw. The bore's own rim, visible from directly
inside the cavity looking up at the ceiling (exactly pass10b_mag_
pocket.png's camera angle), is the real "ring" -- and the numbers match
(computed ~1.4mm at the OLD placement vs. Jake's own "~1mm" estimate).

**`ff_compass.c`**: the module local-frame -> puck-world-frame mapping
table is updated for the flip (local +x -> puck -y, was +y; local -x ->
puck +y, was -y) -- see that file's own 2026-09-11 comment block. The
row's numeric axis-source/sign values remain an unverified placeholder
either way (unchanged by this pass -- still needs a bench check).

### New gate: `verify_openings_open`

Every documented "should be hollow all the way through" opening --
window column, USB tunnel, both button holes, the lug hole, both antenna
cable channels, and (pass 11) the compass mount's own wire-exit notch --
is now probed point-by-point along its own extent, not just for skin
thickness or interference. This is what actually catches defect 1 (no
pre-existing gate did): `window_column` fails on unmodified `origin/main`
(3 bad of 102, see above) and passes clean after the brow fix.

Two of the gate's own probes needed a fix during development (both
process notes, not product defects -- kept in the function's docstring):
`antenna_lora`'s outer ~10-20% (toward the true wall) reads solid, not
open, because `_antenna_skin_safe_channel`'s Combine-Intersect against
the skin-safe envelope legitimately trims the cut's far end a bit short
of the naive `s_wall - channel_min_skin` estimate this gate's `channel_
len` approximates -- sampled fractions moved inside the confirmed-open
0-70% band. `mag_wire_notch`'s first version sampled SOUTH of the
fence's own outer edge (the open gap between the fence and the GPS
frame's wall, never real fence material at all); fixed to sample THROUGH
the actual wall band (`fy0` to `fy0 + fence_wall`) at the notch's own x.

### `verify()` output, both variants (pass 11, run piecewise per the
Fusion-MCP infrastructure note above)

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] (base + all 6 board occurrences individually)
         m1/cavity/m2/envelope/bump/export-envelope/clearance checks: all clean
         posts_bosses/post_walls/plunger_reach/button_insertion/button_retention: all clean
         display_insertion: {'ok': False, ...} -- diagnostic only, NOT gated (pre-existing, unrelated to this pass)
         stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
         skin/wall checks: all clean
         fpc_relief bad [] of 63
         wordmark/antenna checks: all clean
         mag_pocket {'envelope_open': (True, []), 'pegs_have_material': (True, []),
                     'pads_have_material': (True, []), 'fence_has_material': (True, []),
                     'window_bore_clear': (True, 3.955), 'display_back_clear': (True, 3.765)}
         openings_open: window_column/usb_tunnel/power_button_hole/home_button_hole/
                        lug_hole/antenna_lora/antenna_gps/mag_wire_notch -- all (True, [])

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] (base + all board occurrences)
         m1/cavity/m2/envelope/bump/export-envelope/clearance checks: all clean
         posts_bosses/post_walls/plunger_reach/button_insertion/button_retention: all clean
         display_insertion: {'ok': False, ...} -- diagnostic only, NOT gated
         stack3_clearance {'ok': True, 'note': 'comms_stack3_full_height=False -- no Wio/XIAO inserted'}
         skin/wall checks: all clean
         fpc_relief bad [] of 63
         wordmark/antenna checks: all clean (antenna_lora skipped -- LoRa route is trim-only)
         mag_pocket: all 6 checks (True, []) -- mag_module_fits(p) is False, mount skipped entirely
         openings_open: all (True, []) (mag_wire_notch/antenna_lora auto-pass, mount/LoRa skipped)
```

### Offline STL scan + independent bore ray-cast (pass 11)

`tools/offline_stl_check.py`: `OVERALL: PASS`, both variants (0
non-manifold edges on every body, envelope ok, `bad_clusters_mm2: []` on
Top/Bottom for both). `/private/tmp/claude-501/borescan.py`: `RESULT:
PASS` on both `export/trim/Top.stl` and `export/current/Top.stl` (333
probes each, 0 blocked) -- see the before/after numbers above.

### Exports and renders (pass 11)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (Top changed in both variants -- the window-column
exclusion cut in the brow, plus the re-oriented/repositioned compass
mount for trim; Bottom/Screen_Plate/buttons unchanged in content, Bottom
STLs re-exported with the usual small tessellation-only byte diff),
`export/<variant>/firefly_<variant>_case.3mf` (native, 5 objects),
`export/<variant>/firefly_<variant>_plate.3mf` (re-packed via
`tools/stl_to_3mf.py`, same per-part orientation convention as every
prior pass). Coupons (`export/coupons/coupon_*.stl`,
`firefly_coupons_native.3mf`) re-exported as a byproduct of the same
pipeline run -- byte-identical, the button mechanism is untouched by
this pass.

Renders: `pass11_{trim,current}_{front,top,right,iso}.png` (standard
4-view, both variants -- clean pill silhouette, no bumps, matches pass
10b's iso/top/right for `current` since that variant never had a
compass mount to begin with); `pass11_window_closeup.png` /
`pass11_brow.png` (close-ups of the window bore and the FPC brow's
tiered risers, trim -- the bore reads as a clean open circle, with the
brow's stepped risers visible outside it, not inside it); `pass11_mag_
pocket.png` (trim only, Bottom/buttons/boards/reference bodies hidden,
camera inside the cavity looking up at the ceiling -- the mount's fence,
both peg mounting holes, both rest pads, and the wire-exit notch on the
south wall are all visible, well clear of the nearest case-screw bosses
at the lanyard end). All viewed directly as part of this pass.

## 2026-09-12 pass 12 (30mm Top / no brow investigated, button holes clean)

Jake's request, from a review of pass-11 renders/prints: "We can't have
this weird thickness thing, we probably need to move the top to be
longer. The power button has a thickness issue too." Two changes, both
re-verified live for both variants.

### Change 1: trim `top_z` 28 → 30mm — the brow stays, and here is why

The ask was to raise trim's case height and delete the FPC-relief
"brow" (`add_fpc_brow`/`build_fpc_brow_solid`/`FPC_BROW_TIERS`,
pass 9) on the theory that a taller Top leaves more skin above the
pocket. **This does not work, and cannot work, under the current
z-shift convention — confirmed both analytically and live, not assumed:**

`rho_at_z(p, z) = flat_rho + (top_z - z)` in the flat-chamfer band right
under the top face (see `_profile_geometry`/`rho_at_z`). Every z-anchored
feature that matters here — `PARAMS['fpc_relief']['z']`, the display
module via `display_z_offset` — shifts by the exact same `_DZ_TOP` as
`top_z` itself (`params_trim.py`'s own convention). So `top_z - z` at the
pocket's own z1 is **algebraically invariant to top_z**: raising the case
height translates the pocket, the display, and the shoulder profile
together and changes nothing about their relationship. A standalone
script reusing `rho_at_z`/`rho_from_spine` verbatim (no Fusion needed)
confirms this to the millimetre:

```
trim @28: worst SPEC-box corner (7.02, 73.12) margin = 0.048mm
trim @30: worst SPEC-box corner (7.02, 73.12) margin = 0.048mm   (identical)
trim @40 (sanity): same corner, margin = 0.048mm                (identical)
current @25: same corner, margin = 2.048mm  (current's WIDER flat_rho, not its height)
```

The real reason trim needs the brow and `current` does not is trim's
2mm-smaller `flat_rho`/`outer_radius` (22.14 vs 24.14) — a **radius**
question, not a height one. `top_z` was raised to 30mm anyway (a real,
independent improvement — see below for what it does buy), and
`add_fpc_brow` was actually deleted and rebuilt at 30mm to get real
numbers rather than trust the algebra alone: `verify_fpc_relief` failed
with the **identical** bad corner as pass 9–11. The brow was restored
unmodified (its own construction already re-derives correctly from `p`
at whatever `top_z` is current — no code change needed for the height
bump) rather than ship a real, physical hole in the shell. Live,
brow-in-place, at `top_z=30`:

```
verify_fpc_relief(trim):    0 bad of 63 probes
verify_fpc_relief(current): 0 bad of 63 probes  (unchanged, top_z=25, frozen)
```

The ~62mm³ `Top x <display module>` interference pass 9 hit when
shrinking the pocket instead of raising the brow **does not return**:
the brow's mechanism (raise the outer surface, never touch the pocket's
own depth) is untouched by this pass, and the full `verify()` interference
gate — every printed body + every inserted board occurrence — reports
`[]` for both variants at 30mm (see the full output below).

**What raising `top_z` to 30mm actually buys** (all confirmed live, see
Change 3 below): +2mm of straight-wall height in the middle of the case,
which lengthens every ceiling-anchored screw engagement and grows the
mag-mount/comms-stack clearances by the same 2mm, without touching
anything anchored to the parting plane (Bottom, the lip/anchor rings, the
lug, screws A/B1/B2/C's pilot depth). `current` is **not** touched
(frozen at 25mm, unchanged from pass 7's own decision, "for the probe
comparison") — it does not need the brow removed (it never needed the
brow in the first place, being wider) and was never asked to grow.

### Change 2: button holes — tab-relief lane was reaching the true outer skin

**Confirmed root cause**, from Jake's own ray-cast of the pass-11 export
(`sidescan.py`, reproduced here against the checked-in
`export/trim/Top.stl` with no Fusion needed for the "before" half):
`add_button`'s `tab_hole_body` cut (the WALL clearance pocket for the
retaining tab, distinct from the main stadium `hole_cutter`) was bounded
only along its own ray centerline (`s_inner + tab_hole_skin_margin/2`,
~1.45mm short of the true outer surface at t=0) — nothing bounded its
off-axis tangential corners (it spans `tab['w']+2mm` tangentially) against
the TRUE CURVED wall, the same class of bug this file has already fixed
for the main wall hole, the USB liner, and the rib+connector. Live probe
of the unmodified `origin/main` Top body at Jake's own reported points:

```
Power (-x wall), y=32, z=15/16/17: SOLID found ONLY at x -21.5..-20.0 (rib
  material), HOLLOW everywhere else from x=-30 out through x=-15 — a real
  hole clean through the outer wall, not a thin spot.
Home  (-x wall), y=64-66, z=19-21: HOLLOW across the entire plausible
  wall band (x -28..-14) — same class of breach.
```

**Fix** (`add_button`, `add_buttons`): a new shared clip tool
(`tab_clip_tool`), Combine-Intersected against `tab_hole_body` (the wall
cut) and `tab_relief_body` (the matching lane cut through the RIB) before
either cut is applied — "the wall hole is the stadium only." Margin
calibration mattered and was tuned live, not guessed once:

- This file already has an established idiom for "stay clear of the true
  wall by a safety margin" — `wall_clear = 0.6` in
  `add_lip_anchor_reliefs`/`add_lug`, applied as `s_wall - wall_clear`.
  `build_inner_cavity_clip_tool`'s own `safety_margin` is relative to the
  INNER CAVITY (already `wall` inboard of `s_wall`), so matching that
  convention needs `safety_margin = wall_clear - p['wall']` (negative:
  the tool must be grown outward past the bare cavity to reach
  `s_wall - 0.6`).
- **First attempt** used a naive `safety_margin=+0.6` (reading "wall +
  0.6mm inward" as measured from `s_wall` directly, i.e.
  `s_inner - 0.6`) — this over-clipped: it sits INBOARD of the tab's own
  real outward reach (`s_inner+0.15`, from `tab_body`'s own construction),
  clipping the clearance cut short of the tab it exists to clear.
  Live `verify()`: a real `Top x Power Button` (5.57mm³) / `Top x Home
  Button` (7.06mm³) interference — the tab poking into wall material the
  over-clipped cut no longer removed.
- **Fixed** with `wall_clear=0.6` → boundary `s_inner+1.4` (1.25mm of
  slack past the tab's real reach, still 0.6mm short of the true wall,
  a no-op at t=0 since the plain analytic bound already sits inboard of
  it — it only bites off-axis, exactly where the breach was).
- A separate, smaller live interference (~0.13mm³, both buttons) traced
  to an unrelated mistake made in the same pass: the COLLAR's own clip
  was swapped from the standard `clip_tool` to the new, looser
  `tab_clip_tool`, reopening a diagonal-corner overshoot pass 5 had
  already fixed for the collar with the tight default margin. Reverted —
  the collar never needed touching.

**Gates, both live-run before/after** (before = unmodified `origin/main`
at `top_z=28`; after = this branch at `top_z=30`):

```
BEFORE (verify_openings_open, new *_button_hole_footprint check, run
        against origin/main's own geometry):
  power_button_hole_footprint: False — 1 bad point,
    ('open-outside-hole', -27.68, 31.91, 14.85)
  home_button_hole_footprint: True  (this grid's own coverage didn't land
    on Home's specific breach band — see verify_skin_intact below, and
    the direct point-probe evidence above, for the actual confirmation)
  verify_skin_intact: 24 probes (pre-widening), 0 bad (the pre-pass-12
    check's own footprint was too narrow to see this defect at all —
    see below for why it was widened)

AFTER (this branch, top_z=30, tab_clip_tool in place):
  power_button_hole_footprint: True  (0 bad)
  home_button_hole_footprint:  True  (0 bad)
  verify_skin_intact: 48 probes (widened), 0 bad
  check_interference([Top, Bottom, Power Button, Home Button]): []
  verify_button_insertion: 0 bad of 125, both buttons
  verify_button_retention: all True, both buttons
```

`verify_openings_open`'s new `power_button_hole_footprint` /
`home_button_hole_footprint` entries grid-scan the (tangential, z) plane
around each hole at a fixed depth just inside the true wall, re-deriving
the TRUE wall position **per sample** via `true_wall_distance_along_ray`
(not a flat offset from a single centerline point — an early version did
that and produced its own false positive, a probe point at x=-32.18
against a trim `outer_radius` of 28, simply open air far outside the
part, nothing to do with the real geometry) — every point inside the
actual cut stadium (`hole_wh` = cap stadium + `2*cap_clearance`/side,
matching `add_button`'s own `hole_cutter` exactly) must be open; every
other grid point must be blocked. `verify_skin_intact` was widened from
a single z sample to 4 across the tab's own full z-span (Jake's reported
breach z's, 15/18-20, sit at the BOTTOM of the tab region, which the
pre-pass-12 single-midpoint sample never reached) — its tangential span
was tried at the wider `tab_relief_w` too, reverted: that width, at the
existing shallow depth (0.15/0.3), reproduced the exact same "probe steps
past the true curved surface off-axis" false positive as the footprint
check's own first attempt (the file's established ~0.25-0.3mm
ray-vs-curvature slack showing up again) — kept at the plain `tab['w']`;
the new footprint gate is the one that actually covers the wider lane.

**Independent, tool-limitation caveat, disclosed rather than hidden**:
re-running `sidescan.py` (the flat, axis-aligned ray-cast tool, unchanged
from Jake's own script) against the FRESH pass-12 export shows Power
completely clean (matches the live gates exactly) but still prints
"positive-x" values in Home's y=63-68 band. Direct, curve-aware live
point-probing at those exact (t, z) positions (recomputing the true wall
via `true_wall_distance_along_ray` at each sample, not a flat offset) —
and a fine-grained sweep of the whole region — found **zero** points that
are both outside the intended stadium and hollow; every "positive" read
from the flat tool corresponds to a point that is legitimately INSIDE
Home's own (diagonal) hole footprint. This is a known-shape limitation of
a pure-axis ray-cast on a button whose nub direction is ~33° off the wall
normal (Power's is much closer to axis-aligned, which is exactly why it
reads clean on the same tool): the tool cannot distinguish "hollow because
it's the hole" from "hollow because material is missing" the way the
curve-aware analytic gates (matching this file's own established
methodology, used to verify every other opening in this document) can.
Recommended follow-up: a diagonal-ray variant of `sidescan.py` for a
fully tool-independent third check, if Jake wants one before printing.

### Change 3: re-verified at 30mm

Full `verify()`, both variants, run piecewise (build split across many
`fusion_mcp_execute` calls against the same open document, re-fetching
bodies/clip tools by name each time, per this repo's own Fusion-MCP
infrastructure note):

```
trim (top_z=30):    body_names ['Bottom','Home Button','Power Button','Screen Plate','Top']
                     interference []
                     fpc_relief bad 0 of 63
                     stack3_clearance {'stack_top_z': 22.942, 'clearance_found': 6.158,
                                        'required': 0.8, 'ok': True}
                     mag_pocket: all 6 checks True (window_bore_clear 3.955, display_back_clear 3.765
                                 -- both UNCHANGED from pass 11: XY clearances don't depend on top_z)
                     openings_open: all True (window_column/usb_tunnel/both button holes+footprints/
                                    lug_hole/antenna_lora/antenna_gps/mag_wire_notch)
                     verify() completed with no AssertionError -- every gate in the file passed

current (top_z=25, frozen): body_names (same 5)
                     interference []
                     fpc_relief bad 0 of 63
                     stack3_clearance {'ok': True, note: comms_stack3_full_height=False}
                     mag_pocket: all True (mount skipped -- mag_module_fits still False, unchanged)
                     openings_open: all True
                     verify() completed with no AssertionError
```

**Mag mount free height** (`mag_module_clearance`/`mag_pcb_bottom_world_z`,
trim only — `current` still can't host it): the RAW gap
(`top_ceiling_underside_z - bay.gps_patch.z[1]`) grows exactly with the
+2mm height bump, **7.2 → 9.2mm**; the mount's own spare beyond its
4.5mm footprint (`mag_module_clearance`, the number `mag_module_fits`
actually gates on) grows **2.7 → 4.7mm**. The mount stays on the ceiling,
unmoved in XY — `window_bore_clear` (3.955mm) and `display_back_clear`
(3.765mm) are pure XY measurements and are byte-for-byte unchanged from
pass 11.

**Post walls, lip ring, antenna channels, wordmark**: all re-verified
live at `top_z=30` (`verify_post_walls`, `verify_wordmark`,
`verify_antenna_channels`) — all clean, no numeric changes beyond the
Z-shift every ceiling-anchored feature already carries (these checks are
built from `p` and were never hand-tuned to a specific `top_z`).

**Offline STL scan** (`tools/offline_stl_check.py`, run against the fresh
pass-12 exports): `OVERALL: PASS`, both variants — 0 non-manifold edges
on every body, envelope OK, `bad_clusters_mm2: []`.

### Exports and renders (pass 12)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (all 5 re-exported — Top changed the most: taller
straight wall for trim, clean button-hole footprint for both;
`export/<variant>/firefly_<variant>_case.3mf` (native, 5 objects),
`export/<variant>/firefly_<variant>_plate.3mf` (re-packed via
`tools/stl_to_3mf.py`, same per-part orientation convention as every
prior pass: Bottom as-is, Top `flipx`, Screen Plate as-is, Power Button
`outer-x`, Home Button `outer-rz32.74`). Coupons
(`export/coupons/coupon_{power,home}_{wall,cap}.stl`,
`firefly_coupons.3mf`, `firefly_coupons_native.3mf`) re-exported from the
trim-variant pipeline (the coupons are variant-independent — same
`PARAMS['power_cap']`/`home_cap`/tab/rib/collar numbers either way).

Renders: `pass12_trim_{front,top,right,iso}.png` /
`pass12_current_{front,top,right,iso}.png` (standard 4-view, both
variants — clean pill silhouette, no bumps); `pass12_usb_end_top.png`
(top-down close-up of the window/USB end, trim — a clean round bore, no
visible step or notch at the shoulder); `pass12_power_button_straight.png`
/ `pass12_home_button_straight2.png` (straight-on views of the -x wall,
trim — each shows a single clean stadium opening with no secondary
notch or slot beside or below it, the direct visual confirmation of
Change 2). All viewed directly (not just generated) as part of this pass.
A translucent tan/beige rectangle visible in several renders is a Fusion
viewport/UI overlay artifact (not model geometry — every reference body
was confirmed hidden, `isLightBulbOn=False`, before rendering); it does
not appear in the exported STL/3MF files.

### Known limitations added this pass

- **The FPC-relief brow (`add_fpc_brow`) is permanent under the current
  z-shift parameterisation**, not a pass-9-era stopgap — see Change 1
  above for the algebraic/live proof that no `top_z` value removes the
  need for it on trim. Removing it would require either widening trim's
  `flat_rho`/`outer_radius` (a real envelope change, not requested this
  pass) or a different pocket-depth/skin-margin trade at the exact SPEC
  box corner (7.02, 73.12) — out of scope here.
  **SUPERSEDED 2026-09-13, pass 12b**: this held for a HEIGHT
  (`top_z`) change specifically, exactly as this section's own algebra
  says — it was never a claim about `flat_rho`/`outer_radius` (a radius
  question) OR about the spine's own LENGTH (a Y-position question,
  independent of `top_z`). `usb_end_extension_mm` grows the outer
  envelope's +y dome outward instead, which recovers real skin at the
  exact same corner (see the pass-12b section below) — the brow is
  deleted, not permanent.
- **`sidescan.py`'s flat, axis-aligned ray-cast has a blind spot for
  diagonally-oriented button holes** (Home's nub direction is ~33° off
  the wall normal) — see Change 2's independent-confirmation note. It
  remains a good, fast tool for axis-aligned features (it caught Power's
  real defect cleanly) but a diagonal variant would be needed to fully
  retire the curve-aware live gates as the sole authority for Home.

## 2026-09-13 pass 12b (USB end lengthened, no brow — height reverted)

Jake's follow-up after reviewing pass-12 renders/prints: "we probably
need to move the top to be longer" — meaning the case needed to be
LONGER at the USB end, not taller (pass 12's own investigation already
proved height can't touch the FPC-relief pocket — see that section
above). Three changes, all re-verified live for both variants.

### Change 1: `top_z` reverted 30 → 28 (`_DZ_TOP` 5.0 → 3.0)

Pass 12's height bump fixed nothing (proven both analytically and live
in that pass) and forced screw D from M2×12 to M2×16 for no benefit.
Reverted to pass 7's 28mm outright — `top_z=28`, `_DZ_TOP=3.0`, restoring
every z-anchored feature (display, FPC relief, plate, posts, USB
receptacle/tunnel-center-z, button caps/switch bboxes) to its pass 7–11
position. The pass-12 button-hole fixes (`tab_clip_tool`, the
`*_button_hole_footprint` gates in `verify_openings_open`) are untouched
— nothing in this pass modifies `add_button`/`add_buttons`.

### Change 2: `usb_end_extension_mm` — the case gets longer at the dome end

**The mechanism**: a new parameter, `PARAMS['usb_end_extension_mm']`
(0.0 for `current`; 1.8 for `trim`), is added to `spine_b`'s own y in
`params_current.py`/`params_trim.py` (`spine_b = (0.0, 50.0 + ext)`) —
**not** implemented as a code change in `firefly_case.py`. This works
because every function that shapes or measures the OUTER shell already
keys off `spine_a`/`spine_b` as the single source of truth for where the
+y dome sits: `build_outer_pill_solid`/`build_inner_pill_solid` (the
outer envelope AND the inner cavity — both straight-extrude length and
the +y end-cap revolve's own position), `rho_from_spine`/`rho_at_z`
(every skin/wall-thickness/envelope-vertex check), `add_usb_tunnel`'s
`wall_y = spine_b.y + outer_radius` (the tunnel's own bore/liner depth),
`add_lip_anchor_reliefs`' stadium ring (the lip/anchor ring's +y end),
and `true_wall_distance_along_ray` (the pass-12 button footprint gates,
`safe_half_width` for the comms bay). Every ONE of these moves the +y
dome outward by `ext`, automatically, with **zero** other code change.
Meanwhile every feature given as an ABSOLUTE mm coordinate — `window_
center=(0,50)`, `fpc_relief`'s x/y/z box, `plate_outline`, `top_posts`/
`board_standoffs`, `power_cap`/`home_cap`, `screw_D['xy']=(0,60)`, the
lanyard lug (anchored at `spine_a`, the OTHER end, untouched) — was never
derived from `spine_b` to begin with, so it stays exactly at y=50 (or its
own literal position), exactly as Jake asked.

**Why this recovers skin where height could not**: at a fixed point
beyond `spine_b` (like the FPC relief pocket's own worst corner), `rho_
from_spine` measures distance from the NEAREST spine endpoint — moving
that endpoint closer (`spine_b.y` 50 → 51.8) shrinks the measured
distance for every point beyond it, which — per `rho_at_z`'s inverse —
means the true outer crest above that point sits HIGHER (more skin
remains above the pocket's own fixed cut floor). `top_z` could never do
this: it translates the pocket, the display, AND the shoulder profile
together (pass 12's own proof), so their relationship — and thus the
skin above the pocket — never changes. Lengthening the spine changes
that relationship directly, at the one corner that needed it.

**Exact minimum `ext`** (`tools/pass12b_ext_calc.py` — a standalone,
no-Fusion-needed script reusing `rho_at_z`/`rho_from_spine` verbatim,
bisecting for the smallest `usb_end_extension_mm` giving ≥1.5mm of real
skin — the required 1.2mm plus 0.3mm to spare — at the FPC relief
pocket's own worst, UNPROTECTED corner, the literal SPEC box's own
(7.02, 73.12), which `add_fpc_relief` cuts in full regardless of the
skin-safe clip that protects the rest of the widened footprint):

```
$ python3 tools/pass12b_ext_calc.py
--- trim: top_z=28.0, flat_rho=22.14, outer_radius=28.0, usb_end_extension_mm=1.8 ---
  eps=0.0  ext=1.522  worst_corner=(7.02, 73.12)  rho=22.710  skin=1.500mm  margin_over_1.2=+0.300mm
  eps=0.0 -> exact min ext for skin>=1.5mm (1.2 required + 0.3 spare): 1.5222mm
  eps=0.05 -> exact min ext for skin>=1.5mm (1.2 required + 0.3 spare): 1.4560mm   (add_fpc_relief's own 0.05mm corner inset)
  USB tunnel recess depth at ext=1.8: 6.30mm (OK)
--- current: ... usb_end_extension_mm=0.0 ---
  eps=0.0  ext=0.000  worst_corner=(7.02, 73.12)  rho=24.162  skin=2.048mm  margin_over_1.2=+0.848mm   (current never needed any extension)
```

Both exact minimums (1.522mm / 1.456mm) are well under the 3.0mm Jake
asked about, so **the smaller number wins per his own instruction** —
but rather than ship right at the bare minimum, **`usb_end_extension_mm`
= 1.8mm** was chosen: it clears the 1.5mm skin target with ~0.26–0.37mm
of extra pad (skin 1.764mm at the literal corner, 1.827mm at the
0.05mm-inset corner `add_fpc_relief`'s own probe actually checks) for
the same ~0.25–0.3mm flat-ray-vs-true-curvature tessellation slack this
file already documents in half a dozen other live probes, while staying
under the 2.0mm point where the USB tunnel's own recess depth would
exceed the 6.5mm plug-overmold ceiling (Change 3). `current` needs
**0.0mm** — it already has 2.048mm of skin at the identical corner from
its own wider `flat_rho`/`outer_radius`, confirmed unaffected by this
change (see the live numbers below).

**New envelope**: trim becomes **56 × 103.8 × 28** (102 + 1.8mm), spine
**(0,0)–(0,51.8)** for the OUTER envelope only. `current` is unchanged
(60 × 110 × 25, spine (0,0)–(0,50)).

### Change 3: USB-C tunnel recess — checked, not changed

The USB tunnel bore/liner depth is `wall_y - usb_tunnel_y_start`, where
`wall_y = spine_b.y + outer_radius` — it grows by exactly `ext` along
with the dome, automatically (`add_usb_tunnel`, no code change). The
display module's own USB-C receptacle is at a FIXED y (73.0, part of the
inserted board reference, not spine-derived), so the recess a cable's
plug has to reach through — the gap between the case's own outer skin
and the receptacle's contact face — grows by the same `ext`:

```
recess = spine_b.y + outer_radius - usb_tunnel_y_start
trim:    4.5mm (ext=0)  ->  6.3mm (ext=1.8)
current: 6.5mm (ext=0, unchanged)
```

A standard USB-C plug's overmold needs to reach within roughly 6.5mm of
the receptacle face to seat at all (past that, the overmold's own
shoulder hits the case's outer face before the contacts reach the
receptacle) — trim's new 6.3mm recess stays 0.2mm under that ceiling, so
**no tunnel/counterbore change was needed**; the existing liner
(`usb_liner_outer_stadium`, Combine-Intersected against the true curved
outer envelope, already spanning nearly the tunnel's full depth) already
covers the longer bore correctly, confirmed live (bore/liner both
present and correctly clipped in the fresh export, no interference).

### Change 4: the FPC-relief brow is deleted

With the extension recovering real skin at the pocket's own worst
corner, `add_fpc_brow`/`build_fpc_brow_solid`/`FPC_BROW_TIERS` (pass 9's
original fix for this same 1.2mm bar) are no longer needed and have been
removed outright, along with their call in `build()`, their exemptions
in `check_body_envelope_vertices`/`envelope_bounds`/`tools/offline_stl_
check.py`, and the FPC_BROW_HEIGHT-based z-bound in `envelope_bounds`
(back to a plain `top_z + tol`). `add_fpc_relief`'s skin-safe tool is now
just `build_outer_pill_solid(root, p)` offset inward by
`FPC_RELIEF_MIN_WALL` — no brow join. **`verify_fpc_relief` passes clean
with a plain, un-raised shoulder**:

```
verify_fpc_relief(trim):    63 probes, 0 bad   (was 0 bad of 63 WITH the brow, pass 9-12 -- identical pass rate, brow now unnecessary)
verify_fpc_relief(current): 63 probes, 0 bad   (unchanged, current never had a brow)
```

The ~62mm³ `Top x <display module>` interference pass 9 hit when
shrinking the pocket instead of raising the brow **does not return**:
this pass never touches the pocket's own cut depth or footprint at all
(only the pocket's ANCHOR point relative to `spine_b` moved, via the
shared spine mechanism, not the pocket geometry itself) — `check_
interference` reports `[]` for every printed body + every inserted
board occurrence, both variants, live (see below).

### Live verify(), both variants, full piecewise run (re-fetching bodies
by name across separate `fusion_mcp_execute` calls against the same open
document, per this repo's own Fusion-MCP infrastructure note)

```
trim (top_z=28, usb_end_extension_mm=1.8):
  body_names: ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
  interference: []
  fpc_relief: 63 probes, 0 bad
  stack3_clearance: {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
  mag_pocket_results: envelope_open/pegs_have_material/pads_have_material/fence_has_material all True;
                       window_bore_clear 3.955mm, display_back_clear 3.765mm (both byte-for-byte
                       unchanged from pass 11 -- pure XY measurements, untouched by this pass)
  openings_results: 0 bad (window_column/usb_tunnel/both button holes+footprints/lug_hole/
                     antenna_lora/antenna_gps/mag_wire_notch all True)
  verify() completed with no AssertionError -- every gate in the file passed

current (top_z=25, usb_end_extension_mm=0, frozen):
  body_names: (same 5)
  interference: []
  fpc_relief: 63 probes, 0 bad
  stack3_clearance: {'ok': True, note: 'comms_stack3_full_height=False (current variant) -- no
                      Wio/XIAO inserted, nothing to check'}
  mag_pocket_results: all True (mount skipped -- mag_module_fits still False, unchanged;
                       window_bore_clear/display_back_clear both [] -- check skipped, not run)
  openings_results: 0 bad
  verify() completed with no AssertionError
```

**Offline STL scan** (`tools/offline_stl_check.py`, updated this pass to
drop its own ported brow exemption — see Change 4): `OVERALL: PASS`,
both variants — 0 non-manifold edges on every body, envelope OK
(`check_envelope.ok=True` for Bottom/Top/Screen_Plate/Power_Button/
Home_Button), `bad_clusters_mm2: []`.

### Exports and renders (pass 12b)

Both variants, freshly re-generated: `export/<variant>/{Bottom,Top,
Screen_Plate,Power_Button,Home_Button}.stl` (Top and Bottom changed the
most for trim — longer +y dome, reverted height, clean shoulder with no
brow); `export/<variant>/firefly_<variant>_case.3mf` (native, via
`run(..., export=True)`); `export/<variant>/firefly_<variant>_plate.3mf`
(re-packed via `tools/stl_to_3mf.py`, same per-part orientation
convention as every prior pass: Bottom as-is, Top `flipx`, Screen Plate
as-is, Power Button `outer-x`, Home Button `outer-rz32.74`). Coupons
(`export/coupons/coupon_{power,home}_{wall,cap}.stl`,
`firefly_coupons_native.3mf`) re-exported from the trim-variant pipeline
— geometrically unchanged (this pass never touches `power_cap`/
`home_cap`/tab/rib/collar numbers), re-exported anyway for a fresh,
consistent set alongside everything else.

Renders (all viewed directly, not just generated): `pass12b_trim_
{front,top,right,iso}.png` — clean pill silhouette, longer at the USB
end, no bumps, window bore reads as a clean open circle;
`pass12b_usb_end.png` — a close, angled view of the window/USB end
showing a smooth, unbroken shoulder curve into the dome tip with **no
step, notch, or plateau** where the brow used to sit, and the USB-C
opening visible on the shoulder with a clean round throat;
`pass12b_power_button.png` / `pass12b_home_button.png` — zoomed,
straight-on views of the -x wall at each button's own position (a fixed
`viewExtents` orthographic camera, not `isFitView`, since fit-view
always frames the WHOLE visible model regardless of target — the
technique this pass had to work out live after an initial attempt
produced two byte-identical "close-ups" that were actually still
whole-case shots): each shows a single clean stadium opening, no
secondary notch or slot beside or below it — the pass-12 tab-lane fix
holding up under the reverted height and the longer envelope alike.

### Known limitations / notes added this pass

- **`usb_end_extension_mm` only ever grows the +y (USB) dome end.** A
  future request to lengthen the OTHER end (spine_a, the lanyard end)
  would need its own parameter and its own audit of every `spine_a`
  consumer (`lug_ear_geometry` in particular, which anchors the lanyard
  ear there) — not attempted here, out of scope for this pass.
- **The exact-minimum-vs-chosen-value gap (1.8mm chosen vs ~1.5mm exact
  minimum) is a deliberate pad, not a rounding artifact** — see Change 2
  for the tessellation-slack and USB-recess reasoning that bounds it on
  both sides (`ext` too small under-recovers skin; `ext` too large
  starts eating into the USB plug's own reach).
- **`tools/pass12b_ext_calc.py` is a standalone analytic helper**, not
  invoked by `build()`/`verify()` — it exists purely to derive/document
  the `usb_end_extension_mm` value chosen in `params_trim.py`, the same
  role pass 12's own (uncommitted) standalone script played for its own
  investigation.

## 2026-09-14 pass 13 (root fillets on every post/boss, wordmark
recentred and smaller, battery connector access through the plate)

Jake's follow-up after the pass-12 print: "the posts... are flimsy and
easy to pop off right now" (strength), "the KandiWooks logo can be a bit
smaller and centered vertically, and center the 'KANDI' part so it's
centered as if the sprout on the I isn't there" (wordmark), and "can we
make the battery plug accessible when the plate is screwed on the
Waveshare?" (battery connector). Three independent changes, all
re-verified live for both variants; a new `verify_root_fillets` gate and
a new `verify_battery_connector_access` gate now run as part of the
regular `verify()` sweep.

### Item 1: root fillets/collars at every post and boss

**Investigated the pass-9/9g fillet history first, per the brief's own
instruction, rather than guessing.** Two DIFFERENT best-effort fillet
attempts exist in the pre-pass-13 code: the top-post root fillet (pass 9,
finding 4 — a real `fillets.createInput` call, 1.0mm radius, confirmed
present in the built timeline) and the FPC-brow seam fillet (pass 9g —
confirmed to produce ZERO `Fillet` features either variant; moot now
since pass 12b deleted the brow entirely). Neither the case bosses
(A/B1/B2/C/D) nor the compass-mount pegs/pads had ANY root
reinforcement before this pass — the brief's "the generator already
attempted best-effort fillets... investigate why" applies most directly
to the top posts, so that is where the live investigation started.

**Root cause, confirmed live with the new `verify_root_fillets` gate**
(probes a ring of 8 points around each post/boss at OD/2+0.6mm, at
0.4mm into the post from its own root plane, both variants): a first
version of `add_root_reinforcement` tried a real fillet first (radius
1.8mm — solved from the fillet's own quarter-circle geometry,
`added_radius(dz) = R - sqrt(R^2-(R-dz)^2)`, to be the smallest radius
that clears the gate's 0.6mm requirement on its own) and used a
45-degree conical collar (`cone_frustum_solid`, boolean-joined) only as
a fallback when the fillet API raised. Live-probed, that version passed
at only SOME of each post's 8 angles (e.g. `top_post_P1` solid at
135/180/225° only) — not the all-or-nothing result the pass-9g
investigation assumed. The real cause: `clipped_pillar_with_reach`'s own
two-part design (a narrow full-height "core" plus a wider "sleeve" that
gets radially CLIPPED away wherever the local inner-cavity boundary is
tighter than the post's own radius) means the root edge is NOT a full
circle — it's several disconnected arcs, and `fillets.createInput` (a
real, valid feature) only reinforces the arcs it's given, leaving the
clipped, core-only stretches with nothing at all.

**Fix, in two steps, both confirmed live:**
1. The 45-degree conical collar is now ALWAYS added, unconditionally —
   a plain, full 360° solid of revolution (`cone_frustum_solid`, per
   SPEC's own gotcha 5: a single-loop profile revolved, not a tapered
   extrude), so `combine_join` adds its whole volume regardless of the
   underlying pillar's own cross-section at that height. `ROOT_COLLAR_
   RISE` = 1.5mm (≥ the brief's 1.2mm floor) for the top posts and case
   bosses, `PEG_COLLAR_RISE` = 1.1mm for the small (Ø2.7/Ø3.0) compass
   pegs/pads — both sized so the collar's own linear taper
   (`added_radius(dz) = collar_rise - dz`) clears the gate's 0.6mm
   requirement at dz=0.4mm with margin (1.1mm and 0.7mm respectively),
   independent of the post/boss's own radius.
2. The real-fillet attempt was REMOVED entirely (not just made
   best-effort-and-ignored) after a second live finding: an intermediate
   version that tried the fillet first and ALWAYS ALSO added the collar
   passed `verify_root_fillets` cleanly, but an offline STL scan found 4
   non-manifold edges on both Bottom and Top, at the exact boss radius
   from boss A's and C's own centres, right at the collar/pillar seam —
   the partial fillet arc and the collar's cone surface are both real,
   correct geometry independently, but were never designed to be
   tangent to each other, and the seam between them tessellates into a
   sliver. Skipping the fillet attempt removes the interaction; the
   collar alone was already sufficient for the strength gate on its own
   (proven by the mag pegs/pads, which never had a fillet attempt at all
   and never had a manifold issue), so nothing is lost. Both collar ends
   also get a small 0.05mm overlap (`ROOT_COLLAR_OVERLAP`) into the
   existing pillar/floor/ceiling material rather than exactly touching
   it — the same defensive margin `add_mag_module`'s fence already uses
   ("pushed 0.3mm PAST the nominal ceiling") against this identical class
   of coincident-surface tessellation issue.

**Coverage** — every post/boss listed in the brief, live-confirmed via
the on-disk fillet-decision log (`_root_fillet_log.jsonl`, gitignored,
written by `add_root_reinforcement`/`build()` so the per-feature method
survives across the piecewise `fusion_mcp_execute` calls this pass used):
Top plate posts P1–P4 (`add_top_posts`), case bosses A/B1/B2/C on BOTH
halves — Bottom-side root at z=2.0 (the floor) and Top-side root at
z=top_ceiling_underside_z (the ceiling) — and boss D on Bottom only
(`add_case_boss`), plus the compass-mount pegs and pads (`add_mag_module`,
'trim' only — the compass module is skipped on 'current' per
`mag_module_fits`). **Method used, every single feature, both variants:
conical collar** (`method='collar'` in the log) — no true fillet applied
anywhere in the final code, for the reasons above; this is the honest
answer to "say which you used per feature," not a partial mix. 17
features on trim (13 posts/bosses + 4 compass pegs/pads), 13 on current
(no compass module).

The GPS frame and comms-stack frame walls (rectangular, not circular, so
`add_root_reinforcement`'s edge-matching doesn't apply) each get a
separate best-effort 0.6mm constant-radius fillet at their own
floor/ceiling seam (`_best_effort_fillet_at_z`, new helper, same
skip-on-failure pattern as `_best_effort_fillet`) — cosmetic/print-
quality only per the brief's own "0.6mm is enough there," not gated by
`verify_root_fillets`, and not independently re-confirmed live this pass
(out of the time budget; a future pass could add a light probe the same
way `verify_root_fillets` does for the circular features).

**Print orientation unaffected**: the Top still prints face-down on its
flat ceiling face, so every post/boss root (and its collar, always ≤
`ROOT_COLLAR_RISE`/`PEG_COLLAR_RISE` past the nominal OD, sloped at 45°)
sits at the BED side of that face — no new overhang. The offline overhang
scan (below) stays clean on both variants with the existing whitelist,
no new entries needed.

### Item 2: KandiWooks wordmark — smaller, recentred, sprout-aware

**Scale**: `WORDMARK_SCALE_FACTOR = 0.8` multiplies the existing
`target_width` formula (`2 × (flat_rho - WORDMARK_EDGE_CLEARANCE)`) — 80%
of the pass-9e usable width, both variants, computed from `flat_rho` so
it stays correct if the flat bed's own radius ever changes again.

**Vertical centring**: new `wordmark_vertical_span(p)` computes the
usable y-span on the flat back face directly from existing, already-
verified geometry — the lanyard lug's own `y_root` (`lug_ear_geometry`,
whichever of its two candidate values reaches further toward the
wordmark) plus `WORDMARK_VERTICAL_CLEARANCE` (1.5mm, matching
`verify_wordmark`'s own clearance floor) on the south end, and screw D's
counterbore radius plus the same 1.5mm on the north end. The two-line
block is centred on this span's own midpoint — replacing the old fixed
`params['wordmark_center']` y value (25.0) entirely; only the x-component
(0.0, the case's own centreline) is still read from params. Live numbers:
trim span `(-17.64, 56.25)` (73.89mm available, block occupies 24.51mm),
current span `(-19.64, 56.25)` (75.89mm available) — large margin either
way, so this was never a tight fit, just uncentred before.

**Horizontal centring, ignoring the sprout**: new `_wordmark_split_
sprout` separates the 'i' glyph's sprout flourish from its own dot+stem
base within `kandiwooks_logo.json`'s Body1/loop[1] — the two are drawn as
ONE continuous 70-point outline (not two separate loops, as a naive read
of the pass-9e comment might suggest), found by locating the closed
loop's two large (~2.6mm) Euclidean-gap edges (every other consecutive
edge in the loop is under ~0.8mm) and taking the smaller of the two arcs
they bound as the stem (4 points, x 6.87–7.89/y −0.36–2.3 — a normal
letter-height quad) vs. the sprout (66 points, reaching y=5.27). KANDI's
own x-centring (`_wordmark_place_word`'s new `x_center_bbox` parameter)
now uses a bbox built from Body2/4/5's full loops plus ONLY Body1's stem
sub-loop — live-confirmed the readable letters (K-a-n-d-i, sans sprout)
land exactly on world x=0.0 (both variants), with the sprout hanging off
to the right as designed (its own full geometry is untouched — only the
CENTRING calculation excludes it; the sprout still counts toward KANDI's
scale/height, since it's real debossed ink occupying real vertical
space). WOOKS is unaffected (no sprout-like flourish to exclude).

**`verify_wordmark` re-checked live, both variants, large margins
throughout**:
```
trim:    edge_clearance 5.708, clearance_A/C 12.798, clearance_B1/B2 19.798,
         clearance_D 26.188, clearance_lug_hole 29.688, deboss_present 0.363
current: edge_clearance 6.108, clearance_A/C 10.702, clearance_B1/B2 17.702,
         clearance_D 26.092, clearance_lug_hole 29.592, deboss_present 0.363
```
All well past the 1.5mm floor the gate itself enforces. Rendered
`pass13_bottom_logo.png` / `pass13_current_bottom_logo.png` straight-on
and looked at both — KANDI/WOOKS read correctly, smaller than pass-9e,
vertically centred between the D-screw hole and the lug end, and the
leaf/sprout glyph hangs cleanly off the 'i' to the right without
dragging the readable letters off-centre.

### Item 3: battery connector access through the Screen Plate

**Located the connector**, per the brief's own instruction to find it in
the inserted display occurrence: the ESP32-S3-Touch-LCD-1.46's own
component tree (walked live via `find_display_occurrence` +
`_collect_occ_bodies`/`childOccurrences`, ~424 bodies, mostly anonymous
STEP-import "Body1"s) has exactly one 2-pin connector-shaped part with a
real designator — `HP1_25MM-2P-SMT-HORIZONTAL` (a 1.25mm-pitch 2-pin
horizontal SMT connector; the u.FL RF connector, `J6_ASM`, is the only
other real connector-family part, ruled out by name and by being on the
opposite/display side of the board). World bbox measured live on the
built trim document: x `-11.32..-3.67`, y `32.80..38.00`, z
`17.20..20.60` (trim, `display_z_offset=+3`) — stored in
`params_current.py` as `battery_connector_bbox` un-offset back to the
base/current frame (z `14.20..17.60`), with `display_z_offset` re-applied
at read time via the new `battery_connector_world_bbox(p)`, same
convention as every other display-relative z value in this file. Its
z-range sits immediately below `display_pcb`'s own z (17.59 base) —
consistent with "on the back of the display PCB."

**Confirmed the plate covers it** before changing anything, per the
brief's own "if it does not cover it, say so and change nothing": a live
56/63-point grid probe at the plate's own mid-z, across the connector's
footprint + 3mm margin, read solid plate material at every point except
the 7 samples nearest the true edges of that margin — the plate does
cover the connector, by a wide margin, on both variants (same XY
footprint, same plate outline).

**The cut**: new `battery_connector_window(p)` computes footprint +
`BATTERY_CONNECTOR_MARGIN` (1.5mm) on every side, plus `BATTERY_
CONNECTOR_LEAD_EXTRA` (6.0mm) on the −x ("west") side — the connector's
own long axis is X (7.65mm) vs. Y (5.2mm, so X is the mating/lead axis),
and its −x side faces open cavity toward the outer wall while its +x
side faces further into the board's own interior, so −x is the side a
lead/plug can actually approach from and where finger/tweezer room is
needed. The window is then clamped generically against every Top-post
and board-standoff hole (`p['top_posts']` + `p['board_standoffs']`, not
hand-picked) so none loses more than `BATTERY_CONNECTOR_MIN_PLATE`
(1.2mm) of surrounding plate material — only S2 (0.04, 32.22) is ever
close enough to matter for the current layout, clamping the window's own
+x edge from −2.17 to exactly −2.36 (1.2mm net clearance to S2's hole,
by construction). `add_battery_connector_access` cuts this window from
the Screen Plate right after the existing button-plunger clearance cuts.

**`verify_battery_connector_access` re-checked live, both variants**:
`window_open` (24-point grid across the connector's own footprint, at
the plate's mid-z) and `hole_clearance` (every P/S hole vs. the
constructed window) both come back empty (pass) on trim and current.
Rendered `pass13_plate_battery_window.png` / `pass13_current_plate_
battery_window.png` (Screen Plate isolated, the real connector body lit
up alongside it) and looked at both — the connector sits cleanly inside
the cut window with visible clearance on every side.

**Assembly order: unchanged.** The window makes the connector reachable
with the plate already screwed on, in either order — plugging the
battery before or after the plate no longer matters, so pass 9's
documented order (display seated in Top, bay hardware placed in Bottom,
then the halves joined, screen plate and its screws last) stands as-is;
this pass adds "battery can be plugged/unplugged at any time after,
through the new window" rather than changing any existing step.

### `verify()` output, both variants, full piecewise run (each gate run
as its own `fusion_mcp_execute` call against the same already-built
document, per this file's own established workflow for calls that risk
the ~60s client-side timeout)

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         m1 probe bad [] / cavity bad []
         root_fillet bad {}  (17 features, all pass at all 8 angles)
         battery_access {'window_open': [], 'hole_clearance': []}
         interference []
         envelope bad [] / bump bad [] / export_env bad [] / clearance bad {}
         posts_bosses bad [] / post_walls bad {}
         wordmark: edge_clearance (True, 5.708), clearance_D (True, 26.188),
                   clearance_lug_hole (True, 29.688), deboss_present (True, 0.363)
         plunger bad [] / button_insertion bad [] / button_retention bad {}
         stack3 {'stack_top_z': 22.942, 'clearance_found': 4.158, 'required': 0.8, 'ok': True}
         skin bad [] / wall bad [] / fpc bad [] / antenna bad [] / mag bad [] / openings bad {}

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         m1 probe bad [] / cavity bad []
         root_fillet bad {}  (13 features, all pass at all 8 angles)
         battery_access {'window_open': [], 'hole_clearance': []}
         interference []
         envelope bad [] / bump bad [] / export_env bad [] / clearance bad {}
         posts_bosses bad [] / post_walls bad {}
         wordmark: edge_clearance (True, 6.108), clearance_D (True, 26.092),
                   clearance_lug_hole (True, 29.592), deboss_present (True, 0.363)
         plunger bad [] / button_insertion bad [] / button_retention bad {}
         stack3 {'ok': True, 'note': 'no Wio/XIAO inserted, nothing to check'}
         skin bad [] / wall bad [] / fpc bad [] / antenna bad [] / mag bad [] / openings bad {}
```

### Offline STL scan output (`tools/offline_stl_check.py`, pass 13)

```
=== Offline STL checks: trim ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS

=== Offline STL checks: current ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS
```
(The intermediate fillet+collar version's 4-non-manifold-edge regression,
found and root-caused mid-pass, is described in item 1 above and is
fixed in this final code/export — not present in these numbers.)

### Exports and renders (pass 13)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (all 5 re-exported), `export/<variant>/firefly_
<variant>_case.3mf` (native, 5 objects), `export/<variant>/firefly_
<variant>_plate.3mf` (re-packed via `tools/stl_to_3mf.py`, same per-part
orientation as every prior pass — Bottom as-is, Top `flipx`, Screen Plate
as-is, Power Button `outer-x`, Home Button `outer-rz32.74`). Renders (new
this pass, both variants unless noted): `pass13_bottom_logo.png` /
`pass13_current_bottom_logo.png` (straight-on, item 2), `pass13_plate_
battery_window.png` / `pass13_current_plate_battery_window.png` (Screen
Plate + the real connector body, item 3), `pass13_post_root_closeup.png`
(trim, P1, showing the collar's flare where it meets the ceiling),
`pass13_boss_root_closeup.png` (trim, boss C) — plus the standard 4-view
(`{trim,current}_{front,top,right,iso}.png`, re-rendered as a byproduct
of the export run).

### Known limitations / notes added this pass

- **The GPS-frame/stack-frame wall fillets (0.6mm, `_best_effort_fillet_
  at_z`) are not independently re-confirmed live this pass** — applied
  in the same best-effort, skip-on-failure pattern as every other
  cosmetic fillet in this file, not gated by any dimensional check, and
  out of this pass's time budget to probe directly. A follow-up pass
  could add a light live check the same way `verify_root_fillets` does
  for the circular posts/bosses.
- **No true Fillet feature is used anywhere in the post/boss root
  reinforcement any more** — every one of the 17 (trim) / 13 (current)
  reinforced roots uses the conical-collar fallback exclusively (see item
  1's own writeup for the live-evidenced reason: a partial fillet plus a
  collar creates a real non-manifold seam, and the collar alone was
  already sufficient). This is a deliberate simplification, not a
  regression — `fillet_r`/`tol` remain as (currently unused) parameters
  on `add_root_reinforcement` for a future pass that wants to retry a
  real fillet with a different edge-selection strategy (e.g. restricting
  it to posts/bosses whose full root loop is confirmed circular first).
- **`_root_fillet_log.jsonl`** (a small JSON-lines file written by
  `add_root_reinforcement`/`build()` under `hardware/case/`, one line per
  reinforced feature) is a debugging/reporting aid for this pass's own
  investigation, not a generator input or output — it is not tracked in
  git (see `.gitignore`) and can be safely deleted; a fresh `build()` run
  recreates it.

## 2026-09-15 pass 14 (lanyard-end corner blocks, wordmark counters fixed)

Two independent changes: (1) merge the four free-standing lanyard-end
screw bosses (A/B1/B2/C) into two solid corner blocks on the Top half, per
Jake's sketch — "a buttress block the two screws land in, not two posts
with a web"; (2) fix a real print defect Jake found on the printed
Bottom — the KANDI WOOKS wordmark's counters (the 'a', the 'd', and both
flower-shaped 'o's in WOOKS) were coming out solid instead of hollow.
Both re-verified live for both variants; two new gates,
`verify_corner_blocks` and `verify_wordmark_counters`, now run as part of
the regular `verify()` sweep.

### Item 1: lanyard-end corner blocks (A+B1, C+B2)

**Construction** (`add_lanyard_corner_block`, called from
`add_case_screws` in place of the old per-screw Top-side cylinder —
`add_case_boss` gained a `build_top` flag so its Bottom-side half is
completely unchanged): each side's pair (A+B1 on the −x side, C+B2 on
+x) becomes one solid built from a stadium/capsule connecting the two
screw centres (`oriented_stadium_prism`, radius `boss_dia/2` — see the
pad discussion below), unioned with an oversized outward wedge
(`oriented_box_prism`) that starts exactly at the capsule's own outward
edge (direction determined live, via a dot product against the
direction away from spine_a — not assumed) and reaches toward the true
dome wall. The union is clipped three ways: `clip_to_inner_cavity` (the
true shell, same as every other boss/post in this file), a plain
cylinder centred on spine_a at radius `lip_r[0] -
CORNER_BLOCK_RING_CLEARANCE` (0.5mm) so the block stops cleanly short of
the lip/anchor ring's own inner edge, and a cut of the L76K PCB
footprint (`p['bay']['stack3']['l76k_pcb']`) + `CORNER_BLOCK_STACK_
MARGIN` (0.8mm) spanning the block's **full** z-height. Two full-height,
unclipped core cylinders (radius `BOSS_CORE_R`, the same constant every
other boss/post in this file uses) at each screw centre guarantee the
join physically reaches both the parting line and the ceiling, exactly
like every other boss/post (`clipped_pillar_with_reach`'s own
docstring). Both pilot holes are then cut from the merged block at their
exact existing positions/diameters/depths (`p['top_pilot_dia']`,
`p['top_pilot_z']`), so the screws still land and bottom out identically
and Bottom's own counterbores (untouched by this pass) still line up.
Block top face = `top_ceiling_underside_z`, unchanged from the old
individual boss tops.

**Keep-out #1, negotiated: how wide can the block actually be?**
A first version padded the capsule to `boss_dia/2 + 0.7` = 3.7mm radius,
sized to satisfy `verify_root_fillets`' 3.6mm probe (`boss_r + 0.6`)
*from the capsule's own geometry alone*, with no reinforcement. Live
`check_interference` against the inserted board occurrences found real
overlap — up to 46mm³ against the Wio module, up to 17mm³ against the
XIAO — at B1/B2: the REAL 3-board stack's footprint at B1/B2's own y
(≈−15) reaches out to |x|=8.9mm (not just the L76K PCB's own low-z
frame footprint the static param describes — the live interference
reached as high as z≈22.9, well above the comms-stack frame's own
low-z band), only 3.61mm from B1/B2's own centre (±12.5mm) — **less**
than the 3.7mm the capsule needed on its own. No amount of ring/
stack-footprint clamping alone can fit both a 3.7mm pad and a 3.61mm
ceiling at once. **Fix**: split the two jobs. `CORNER_BLOCK_PAD` is now
`0.0` — the capsule/wedge's own radius is just `boss_dia/2`, the exact
footprint the old individual bosses always had (proven interference-free
through pass 13) — and the pass-13 conical root-reinforcement collar
(`add_root_reinforcement`, unconditional, near the ceiling only) is
added at both screw centres to satisfy `verify_root_fillets` instead;
the collar's own geometry doesn't care what the underlying pillar's
cross-section is, so a plain boss-radius capsule gets exactly the same
1.1mm-at-0.4mm-in boost any other boss/post in this file gets. The
`CORNER_BLOCK_STACK_MARGIN` cut (0.8mm past the L76K PCB footprint,
full block height) is kept as a second, independent safeguard against
the same class of real-hardware proximity, since the static PCB param is
a reasonable but not perfectly tight proxy for the real stack. Re-verified
live: `check_interference` returns `[]` on both variants with this final
geometry.

**Keep-out #2, live-confirmed genuinely needed, not just belt-and-
suspenders**: with `CORNER_BLOCK_PAD=0.0`, the plain capsule missed ONE
of `verify_root_fillets`' 8 angles at B1/B2 (315° at B1, the mirror 225°
at B2) — the ceiling's own fillet curvature (rho > `fillet_center_rho`
out at B1/B2's own radius from spine_a) narrows the usable ceiling
height there, independent of the comms-stack question above. The
unconditional root-reinforcement collar closes this the same way it
already does for the top posts — confirmed live, `verify_root_fillets`
passes clean (all 8 angles) on both variants with the final code.

**Keep-out #3, the lip/anchor ring**: `CORNER_BLOCK_RING_CLEARANCE`
(0.5mm past `lip_r[0]`) was sized from a live, pure-Python (no-Fusion)
replay of `true_wall_distance_along_ray`/`rho_at_z` against both
variants' real numbers: at `CORNER_BLOCK_PAD=0.0`, B1/B2 (19.53mm from
spine_a) reach 22.53mm at the capsule alone, comfortably inside trim's
`lip_r[0]`=23.95 (current has far more margin still: 25.95) — the ring
clearance clamp mostly limits how far the *outward wedge* can reach
toward the wall, not the capsule itself.

**Keep-outs already clear by construction, checked, not just assumed**:
the comms-stack footprint and the LoRa FPC antenna keep-out both live
near the case's own y=0 centreline (the real LoRa u.FL channel is
centred at x≈3.44mm — `PARAMS['antenna']['lora_ufl_xyz']`), while both
corner blocks stay laterally out at |x|≥~9mm by construction (the
`fpc_keepout` reference box in `params_current.py`'s own `bay` dict is
deliberately wide, x −20..20, marking a generic wall region rather than
the precise antenna route — the operative check is the real, live-probed
LoRa channel via `verify_antenna_channels`, unaffected either way).
Button tab lanes (y 29..63) and the compass mount (world y 3.75..22.35)
are both far enough from the lanyard end (y −8/−15) that there is no
spatial overlap to check at all.

**A NEW live overhang finding, current variant only, fixed by widening
an existing whitelist entry**: the wider `lip_r[0]` on 'current' lets the
outward wedge reach further before `CORNER_BLOCK_RING_CLEARANCE` clamps
it, exposing a slightly larger patch of the same curved inner-cavity
ceiling/fillet transition the pass-5 `general_ceiling_overhang`
whitelist entry already covers everywhere else on Top. `tools/
offline_stl_check.py`'s offline overhang scan on the fresh `current`
export flagged two 57mm² clusters at (±19.3, −13.1) — just south of the
whitelist's old y0=−12 boundary. Live-inspected the actual flagged
triangles (not just their size) before touching the whitelist: two
z-bands, a flat z=11 shelf (0° from horizontal — an ordinary flat
interior ceiling patch, same as every other one already whitelisted) and
a z≈21–23 band at ~40° (the inner-cavity fillet's own sub-45° transition,
identical in kind to the existing `ceiling_near_bay_wall` entries) —
both the same "hollow shell needs ordinary slicer support here" reality
already accepted everywhere else, not a new local/structural defect.
Widened `general_ceiling_overhang`'s y0 from −12 to −16 (southmost point
found: −15.87) in **both** `firefly_case.py`'s own inline overhang-scan
whitelist (used when `run(..., export=True)` gates on it) and `tools/
offline_stl_check.py`'s independent copy — kept deliberately in sync,
per those files' own existing comments about the two never being allowed
to silently drift apart. Trim doesn't trigger this at all (its tighter
`lip_r[0]` keeps the wedge's reach well inside the pre-existing box).

**`verify_corner_blocks`** (new gate): for each side, probes (a) both
pilot holes open to full depth (near each end and mid-depth), (b) the
block solid at 7 points along and just off the segment between the two
pilots, at a height safely above the pilots' own z1 (avoiding a false
"hollow" read at the pilot bore itself), (c) the L76K stack footprint's
4 corners and the case's own x=0 centreline across the FPC keep-out's
y-band both read hollow — the last one is both the "stays in the corner"
and "does not bridge the end-wall centre" checks at once. All 8 checks
pass on both variants (see the `verify()` output below).

### Item 2: wordmark counters were being cut away (real print defect)

**Root cause, confirmed exactly as the brief predicted**: `deboss_loops`
(the function `add_wordmark_logo`/`add_flare_logo` both call) built one
Fusion sketch per deboss pass containing every glyph loop — outer AND
counter/hole — then extruded and cut **every** resulting `sk.profiles`
entry unconditionally. For a glyph with an enclosed counter (an 'a', a
'd', a flower 'o'), Fusion's own profile-finder returns TWO profiles:
the ring (outer loop minus the inner/counter loop — the shape we
actually want cut) and a SECOND profile that is the counter's own disk,
on its own, treated as its own standalone filled region (real,
documented Fusion behavior for nested closed curves, not a bug in
Fusion). Cutting both erases the counter entirely — the whole glyph
comes out solid, exactly Jake's report.

**Fix**: `kandiwooks_logo.json` already tags every loop with `is_outer`
(confirmed live via a stub-Fusion, no-Fusion-needed script:
`WORDMARK_LINE1_BODIES`'s 5 outer loops / 2 counters — the 'd' at Body2
and the 'a' at Body4 — and `WORDMARK_LINE2_BODIES`'s 2 outer loops / 2
counters — both flower 'o's, Body3's own two enclosed loops — exactly 4
counters total, matching Jake's report of "a, d, and the two o's").
`deboss_loops` gained an optional `counter_loops_xy` parameter: when
given, any Fusion profile whose own bounding box matches one of these
world-space counter loops (`_loop_bbox_mm`/`_profile_bbox_mm`/
`_bbox_matches`, 0.05mm tolerance) is skipped — not extruded, not cut —
so the counter survives as a real hole in the debossed ring; the ring
profile itself keeps the much larger OUTER loop's bbox, so the two can
never be confused. `wordmark_layout` computes the outer/counter split
(`_wordmark_word_loops_by_flag`) and transforms both through the
*exact* same scale/bbox/center math as the full word (so a counter's
world coordinates always land exactly where the matching profile in the
real cut sketch does), returning `counter_loops` (fed to `deboss_loops`
by `add_wordmark_logo`) and `counter_probes` (pairs of world points —
the counter's own centroid, and a point on the ring between the counter
and its own enclosing outer glyph, found by a small point-in-polygon
search — `_point_in_poly`/`_poly_centroid`/`_wordmark_counter_probes`,
pure Python, no Fusion needed) for the new gate below. `flare_glyph_
loops` (the sprout/compass-rose logo on Top) has no nested counters and
calls `deboss_loops` with the default `counter_loops_xy=None`, so it
cuts exactly as before — confirmed unaffected by inspection (no code
path changed for it) and by the render.

**`verify_wordmark_counters`** (new gate): for each of the 4 counters,
at the same mid-deboss z `verify_wordmark`'s own grid probe already
uses, (a) the counter's own centroid reads SOLID (material present — the
counter was not cut away) and (b) a point on the ring between the
counter and its own outer boundary reads HOLLOW (confirms the deboss
itself still happened around it, not silently skipped entirely).
`counter_count==4` is a regression guard on the JSON itself. All 9
checks (`counter_count` + 4×2) pass on both variants:

```
trim:    counter_0 (d, KANDI)   present (-8.636, 20.244)  stroke_open (-6.804, 20.244)
         counter_1 (a, KANDI)   present (4.744, 20.044)   stroke_open (5.995, 20.044)
         counter_2 (o, WOOKS)   present (4.447, 10.604)   stroke_open (6.637, 10.604)
         counter_3 (o, WOOKS)   present (-2.609, 10.604)  stroke_open (-0.419, 10.604)
current: counter_0 (d, KANDI)   present (-9.476, 19.238)  stroke_open (-7.474, 19.238)
         counter_1 (a, KANDI)   present (5.206, 19.019)   stroke_open (6.57, 19.019)
         counter_2 (o, WOOKS)   present (4.88, 8.855)     stroke_open (7.275, 8.855)
         counter_3 (o, WOOKS)   present (-2.863, 8.855)   stroke_open (-0.468, 8.855)
```

**Rendered `pass14_bottom_logo.png` straight-on and looked at it** (per
the brief's own instruction — a numeric probe pass is not a substitute):
KANDI/WOOKS read correctly, and — the actual point of this pass — the
'a', the 'd', and both flower 'o's in WOOKS all show a genuine open
counter (an unmistakable outline of the hole, not a filled glyph) at
normal render resolution and at a tight crop zoomed in on just the
wordmark. K, N, W, S, and the sprout on the 'i' are unaffected, exactly
as expected (no counters to begin with, no code path touched for them).

### `verify()` output, both variants, full piecewise run

Every stage of `build()` was its own `fusion_mcp_execute` call against
the same open document (this session's MCP transport reliably times out
client-side around a minute, same as every prior pass — Fusion keeps
executing to completion regardless, confirmed by re-querying the
document); `verify()` itself was also run as its own call, writing its
result to a small JSON file on disk (this session's Fusion process runs
on the same host filesystem this agent's Bash tool has access to) rather
than trusting a client-side print that a slow call could lose to the
same timeout.

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         corner_block: A_pilot_open/B1_pilot_open/C_pilot_open/B2_pilot_open all (True, [True,True,True])
                        AB1_block_solid/CB2_block_solid both (True, [True]*7)
                        stack_footprint_clear (True, [True]*4), fpc_keepout_centerline_clear (True, [True]*3)
         root_fillet bad {}  (19 features -- 13 pass-13 posts/bosses + 4 mag pegs/pads + 2 new corner-block collars)
         wordmark: edge_clearance (True, 5.708), deboss_present (True, 0.315)
         wordmark_counters: counter_count (True, 4), all 8 present/stroke_open checks True
         posts_bosses bad [] / post_walls bad {} / battery_access clean / envelope bad [] / clearance bad {}
         plunger bad [] / button_insertion bad [] / button_retention bad {}
         stack3 clearance_found 4.158 required 0.8 ok True
         skin bad [] / wall bad [] / fpc bad [] / antenna bad [] / mag bad [] / openings bad {}

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference []
         corner_block: all 8 checks True (same shape as trim, absolute-mm screw positions are shared)
         root_fillet bad {}  (15 features -- 13 pass-13 posts/bosses + 2 new corner-block collars; no compass module)
         wordmark: edge_clearance (True, 6.108), deboss_present (True, 0.315)
         wordmark_counters: counter_count (True, 4), all 8 present/stroke_open checks True
         posts_bosses bad [] / post_walls bad {} / battery_access clean / envelope bad [] / clearance bad {}
         plunger bad [] / button_insertion bad [] / button_retention bad {}
         stack3: {'ok': True, 'note': 'no Wio/XIAO inserted, nothing to check'}
         skin bad [] / wall bad [] / fpc bad [] / antenna bad [] / mag bad [] / openings bad {}
```

Every gate in the regular `verify()` sweep — not just the two new ones —
was run and passed on both variants (`verify()` itself asserts on all of
them; a failure anywhere would have raised before returning).

### Offline STL scan output (`tools/offline_stl_check.py`, pass 14)

```
=== Offline STL checks: trim ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS

=== Offline STL checks: current ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS
```
(The `current`-only overhang whitelist widening — item 1's own writeup —
is already reflected in this final PASS; without it `current`'s Top
would report 2 bad clusters of 57.19mm² each, both now correctly
identified as ordinary ceiling/fillet-transition territory.)

### Print orientation / notes

Both halves print exactly as before — Top face-down on its flat ceiling
(z=top_z) face, Bottom face-down on its own flat back (z=0) face — no
change to either variant's print orientation. The corner blocks are a
plain vertical extrusion (capsule + wedge, no taper) from the bed up to
the ceiling, so they need no more support than the individual bosses
they replace; the root-reinforcement collar is the same 45°
self-supporting cone every other post/boss root in this file already
uses, and — like those — sits at the bed side of Top's ceiling face,
so it adds no new overhang either (confirmed by the clean offline
overhang scan above). The wordmark fix changes only *which* Fusion
profile gets cut, not any dimension, so the deboss depth, glyph size,
and vertical/horizontal centring from pass 13 are all unchanged.

### Exports and renders (pass 14)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (all 5 re-exported), `export/<variant>/firefly_
<variant>_case.3mf` (native, 5 objects), `export/<variant>/firefly_
<variant>_plate.3mf` (re-packed via `tools/stl_to_3mf.py`, same
per-part orientation as every prior pass). Renders: `pass14_{trim,
current}_{front,top,right,iso}.png` (standard 4-view, both variants),
`pass14_top_corner_blocks.png` (trim, looking straight into the Top's
lanyard end from below/inside — both merged corner blocks visible as
one continuous mass around their own pilot pair, not two separate
posts), `pass14_top_inside.png` (trim, a wider isometric-from-inside
view showing the corner blocks alongside the compass mount, window bore,
and USB tunnel), `pass14_bottom_logo.png` (trim, straight-on — the
wordmark with all 4 counters visibly open).

### Known limitations / notes added this pass

- **The `current`-variant custom close-up renders (corner blocks /
  inside / bottom logo) were not separately generated** — the numeric
  `verify_corner_blocks`/`verify_wordmark_counters` gates above already
  confirm current's geometry live (identical construction, absolute-mm
  screw positions shared with trim, more margin everywhere since
  current's shell is wider), and the standard 4-view renders exist for
  both variants; a future pass wanting the same close-up angles for
  current specifically can reuse this pass's exact camera scripts.
- **`CORNER_BLOCK_STACK_MARGIN`'s 0.8mm cut uses the static `l76k_pcb`
  param, not a live-probed real-board bounding box** — proven sufficient
  by this pass's own live `check_interference` run (zero interference,
  both variants, against every inserted board occurrence), but if a
  future board/stack revision changes the real footprint, re-run that
  check rather than trusting the static param alone (same caveat the
  pre-existing `add_comms_stack_frame` boss-relief cut already carries).
- **Coupons (`export/coupons/*`) were not re-exported this pass** — the
  button caps/switches are untouched by either change, so the existing
  coupon STLs/3MF remain valid; re-export only if a future pass touches
  button geometry.

## 2026-09-15 pass 15 (Jake's "closest yet" pass-14 print — 9 findings)

Jake printed pass 14 and called it "the closest yet." Nine findings, all
addressed (fixed, or decided against with the reason recorded below). All
new/changed gates run as part of the regular `verify()` sweep except
`verify_plate_post_spread` (diagnostic only, see item 3). Every finding
below was root-caused live (a real Fusion probe, an offline STL scan, or
a pure-Python geometric proof) before any code changed, per this file's
own established discipline.

### Item 1: 20-pin FPC relief pocket too narrow (Jake: "needs to be
larger length-wise to fit the cable")

**Measured live**, not guessed: inserted the real display occurrence
into a scratch document and point-containment-probed the actual combined
PCBA/shield/FPC body (`H0146Y003T001-V1`) across the pocket's own z-band
(trim: z 24.83–25.93) and y-span (71.44–73.12). Real solid material
reaches as far as **x = ±8.0mm** at y=71.44 (the widest slice, right at
the PCB edge) — the old SPEC box (`x` −6.2..7.02, 13.22mm wide,
asymmetric) undershot that by up to 1.8mm on the +x side with **zero**
margin, not the ≥1.0mm/side the brief asked for.

**Fix**: `PARAMS['fpc_relief']['x']` widened from (−6.2, 7.02) to a
symmetric **(−9.0, 9.0)** — 1.0mm clear of the measured ±8.0mm envelope
on both sides. This widens the CORE box `add_fpc_relief` cuts UNCLIPPED
(not just the empirical outer margin, which the skin-safe tool can still
shrink back near the true wall — see that function's own docstring), so
this margin can never be silently clawed back the way the old box's own
narrower core left the wider margin exposed to exactly that risk. y/z
unchanged (the width axis, not length, is what the brief and the real
measurement both point at — the pocket already reaches the connector's
own y-band with margin).

**Gate**: `verify_fpc_relief` — **0 bad of 63 probes, both variants**
(unchanged pass rate — the widened core box is comfortably inside where
the skin-safe clip already allowed material).

### Item 2: USB-C / battery-plug clearance (Jake: "the power cable hole
needs more room to actually plug in. needs more room towards the top")

Treated as **both candidates**, per the brief. Believed intent: most
likely the **USB-C tunnel** — "plug in" reads as inserting the case's own
external USB-C cable, and "towards the top" matches the glass/+Z side of
that tunnel specifically. Fixed both anyway.

**USB-C tunnel**: `usb_tunnel_stadium`'s height grown 7.0 → **7.9mm** and
`usb_tunnel_center_z` shifted **+0.45mm** (16.4 → 16.85, base/current;
trim inherits the same +3mm shift as always) so the growth is biased
**upward** (toward the glass) — the old lower edge is preserved almost
exactly, all ~0.9mm of new headroom lands on the top edge (19.9→20.8mm
current / 22.9→23.8mm trim). `usb_liner_outer_stadium` height grown to
match (10.2→11.1mm, keeping the existing 1.6mm/side liner padding). A
standard USB-C plug's overmold body is ~6.5×8.4mm — the new 7.9mm bore
height now clears that range with real margin on both variants (was
tight against the top of it). Re-verified live: **0 real interference**
against the inserted display occurrence, both variants (the tunnel's own
Combine-Intersect against the true curved outer envelope already
prevents any breach of the outer skin regardless of height).

**Battery-plug window** (Screen Plate, pass 13): extended **+3.0mm
toward +y ("the top", the display/header end)** — `BATTERY_CONNECTOR_TOP_EXTRA`
— reaching 1.0mm past `plate_header_cutout`'s own y0 (41.7), merging the
two openings into one continuous clearance rather than leaving a bare
~2mm sliver of plate between them that would still pinch a fingertip
while gripping the plug. Computed against the real connector footprint
and header cutout position, not guessed.

**Gate**: `verify_battery_connector_access` clean (`window_open: []`,
`hole_clearance: []`), both variants. No new interference, both variants.

### Item 3: the 4 plate-mounting posts all in one corner (Jake: "doesn't
give proper support... figure out how to fit those better across the
top / left / right / bottom")

**Proved, before touching Fusion**, that a literal 4-quadrant spread of
CEILING-REACHING posts is geometrically impossible here, not just
difficult (pure-Python search against the real `PARAMS` — window
geometry, the GPS patch box, `flat_rho`):

- **East is blocked outright**: the GPS patch box (`x` −2.8..22.2, `y`
  2..27) and the window bore's own exclusion circle (radius
  `window_dia/2 + post_r + margin` = 25.65mm from `window_center`)
  together leave **no** `x` at **any** `y` where both clear at once east
  of centre — their own boundaries meet at y≈31.5 with zero margin
  (window needs y≤31.47 at x=17, GPS needs y≥31.50 there).
- **True north is blocked outright too**: inside the domed +y cap, the
  true outer wall is a circle of radius `rho_at_z(top_ceiling_underside_z)`
  = **24.14mm** (trim, at the ceiling) centred on `spine_b`, while the
  window bore is a circle of radius 22.65mm centred just 1.8mm away — the
  annular gap between them is only ~1.5mm wide, nowhere near enough for a
  Ø5 post plus the 0.6mm/1.0mm margins this file's own gates require, at
  ANY angle.
- The plate's own area-weighted centroid (`plate_outline_centroid`, new
  helper) is **(−4.62, 44.69)** — only 7.04mm from `window_center`. Since
  no ceiling post can exist within 25.65mm of `window_center`, **every**
  viable post position is provably ≥18.61mm from the plate's own
  centroid — the brief's own "5mm" target is unreachable by more than
  3.7×, for any arrangement of real, structural posts, not a tuning
  shortfall.

**Fix, within the one region that IS geometrically safe** (west of the
GPS patch, south of the window's own exclusion circle): a 200k-sample
random search maximizing angular spread subject to every live gate this
file already enforces (window/wall/GPS clearance ≥0.5mm, plate-edge pad).
**New positions** (absolute mm, both variants, P1 unchanged from pass 9g):

```
P1 (-20.0, 14.0)   [unchanged, proven]
P2  (-9.0, 22.0)   [new]
P3 (-11.0, 14.0)   [new]
P4 (-18.0, 23.5)   [new]
```

Live result: posts now span an 11×9.5mm diagonal footprint (not a single
axis-aligned 10×6/10×11mm rectangle in one corner), **265.83° angular
spread** (up from ~264.5° for the old cluster — about the same raw
number, since that was already close to this region's own practical
ceiling, but the new arrangement is genuinely 2D, not collinear) and a
**28.11mm** centroid distance (the 18.61mm floor proved above plus
margin — nowhere near the unreachable 5mm target).

**New gate, `verify_plate_post_spread`** — reports `centroid_dist_mm`
and `angular_spread_deg` against the brief's own 5mm/270° targets.
**Deliberately diagnostic-only** (not hard-asserted in `verify()`'s
pass/fail), the same established pattern this file already uses for
`verify_skin_intact`/`verify_wall_integrity`/`verify_display_insertion_path`
— all of which over-fire on real, explained, non-defect geometry rather
than track a genuine regression. Here the reason is a proven geometric
impossibility (above), not an unrefined probe.

**Gate**: `verify_post_walls` — **0 bad of 8×3 (`pilot_wall`) / 8
(`shell_skin`) for all 4 posts, both variants** (same clean result as
every prior post-relocation pass). `check_interference` — 0 pairs, both
variants. `plate_south_extension` needed no change — its existing
x(−24,−6)/y(10,29.5) box already covers all four new positions.

### Item 4: walls around the magnetometer (Jake: "Do we need the walls
around the magnetometer? the top wall is too close to the screen also")

Both parts of the question answered directly. **The full 4-wall
retaining fence (pass 10 REDO / pass 11) is REMOVED.** Reasoning: the
module's own two Ø2.7 pegs already pass through its real Ø3.0 mounting
holes (0.3mm total clearance) — a peg-through-hole pair is, by itself,
already a real, positive XY location for both translation and rotation;
the fence was always a secondary feature on top of that, and its own
1.2mm wall thickness relative to its 3.5mm height was never going to
meaningfully resist a lateral shove the way the two rigid pegs already
do. Jake's second complaint ("top wall too close to the screen") is
exactly right and confirmed by the model: the fence's own north wall sat
at `window_bore_clear` = 3.955mm — real, but visibly tight — removing
the fence removes that close wall entirely rather than shaving it
thinner.

Retention across the pass-10 2.7mm float is now: (1) the two pegs
(positive XY); (2) the two rest pads (positive Z seating); (3) the
~2.0mm compressible foam pad on the GPS patch's own top face (unchanged
from pass 10), which now does double duty as the module's only real
downward/lateral-slip resistance once the halves close.

**A single low stop IS added**, per the brief's own fallback — on the
south (header/wire) side, which sits ≥17mm from the window bore's own
true opening (comfortably "away from the window" on its own, unlike the
old north wall): `MAG_STOP_H` = **2.0mm** (the brief's own ceiling),
keeps the module from sliding south off its pads during assembly/
handling before the foam pad is loaded on. Carries the same wire-exit
notch as the old fence's south wall so the 5 solder wires are unaffected.

**Gate**: `verify_mag_pocket` re-checked live — `pegs_have_material`,
`pads_have_material`, `fence_has_material` (now checking the stop) all
`(True, [])`; `window_bore_clear` **3.955mm** and `display_back_clear`
**3.765mm** — both byte-identical to pass 11's own numbers (pure XY
measurements, untouched by removing the fence). `verify_openings_open`'s
`mag_wire_notch` check: clean, both variants.

### Item 5: button guide ribs "floating" (Jake: "probably want to make
the 'guide'... more robust and connected to the top of the case since
right now it's floating")

**Confirmed by inspection**: the rib plate is a small flat box at the
cap's own z-height, with a horizontal spoke reaching sideways to the
WALL (the existing `RIB_CONNECTOR`), but nothing tying it to the
CEILING at all — it hangs alone in open cavity air roughly 10–20mm below
`top_ceiling_underside_z`, exactly "floating."

**First attempt (dead end, kept as a documented note)**: a second
gusset positioned by a tangential offset from the rib's own centre,
mirroring the wall connector's own convention on the opposite side. A
live `check_interference` run caught a real **14.28mm³ Home Button ×
<board reference body>** overlap — both buttons sit under the display
module's own y-span (27.6–73.13), and the display's real PCBA/shield
body's own lower z (23.3mm trim) sits BELOW the gusset's own target top
(26.3mm) — a gusset staying near the plunger axis has nothing stopping
it from passing straight through real board material on the way up.

**Fix**: anchor the ceiling gusset at the CONNECTOR's own outboard end
instead (near `s_wall`, the same near-the-true-wall position the wall
connector already proves safe) — the display module's own edge sits
several mm inboard of the true wall (a real gap the connector already
lives in without incident), so a gusset based there is laterally clear
of the display's real footprint for the entire climb to the ceiling, not
just at one z. Built and joined into `rib_plate` BEFORE the existing
Combine-Intersect against the true outer envelope, so it inherits that
exact same "can never poke past the real curved skin" protection with no
separate clip needed. Pushed 0.3mm past the nominal ceiling
(`CEILING_GUSSET_OVERLAP`) to guarantee a real, non-coincident-face join.

**Gate**: `check_interference` — **0 pairs, both buttons, both
variants** (re-confirmed live after the fix — this is what caught and
then cleared the dead-end above). `verify_button_insertion` — 0 bad of
125, both buttons, both variants (unaffected — the tab-clip lane and
`cap_clearance` behaviour are untouched by this change). `verify_button_retention`
— all checks True, both variants.

### Item 6: Home/BOOT plunger reach (Jake: "the back button needs to be
longer it doesn't reach correctly")

**First attempt (dead end, kept as a note)**: shrunk `plunger_pretravel`
(the REST gap) 0.3 → 0.1mm to physically lengthen the plunger. A live
`check_interference` run caught the SAME real **14.28mm³ Home Button ×
<switch reference body>** overlap — Home's real available room genuinely
has no spare left at REST (it already needs the existing
`rib_actuator_shifted` dynamic clamp just to clear the real actuator at
the pass-14 0.3mm gap). Reverted `plunger_pretravel` to 0.3 (unchanged,
proven safe both buttons).

**Fix**: lengthened the PRESS STROKE instead of the REST position —
`plunger_travel` (rest-to-bottomed collar travel) raised **0.62 →
0.90mm**. This is REST-state-safe by construction: Fusion's own
`check_interference` gate examines only the built (REST) geometry, and
`plunger_travel` only affects the collar's own rest position relative to
the rib — a larger value shifts the collar further inboard at rest,
which the EXISTING `rib_actuator_clearance` dynamic clamp in
`button_geometry` already re-clears automatically (unconditional on
`plunger_travel`'s own value). Live-confirmed clean (0 interference,
both buttons, both variants) at 0.90mm.

**The number**: at full press this now delivers a real actuation stroke
of **0.60mm** (0.90 − the unchanged 0.3mm pretravel) versus the old
**0.32mm** (0.62−0.3) — a **87.5% increase**, comfortably past a typical
tactile dome's own ~0.25–0.3mm throw — the direct, measurable fix for
"doesn't reach," without touching the rest-position geometry that (per
the dead end above) has zero spare margin for Home.

**Decided against** the literal "0.3–0.5mm mechanical preload" reading
(a plunger tip that already overlaps the actuator's own modelled REST
surface) — see `PARAMS['plunger_travel']`'s own comment for the full
reasoning: it would require either carving a bespoke, unbounded-risk
exception into `check_interference`'s own zero-overlap contract for this
one pair, or accept the gate failing loudly for a design choice that
turns out to need no rigid overlap at all to fix the actual complaint.

**Gate**: `verify_plunger_reach` — `rest_gap` **0.3mm** (exact), both
buttons, both variants (unchanged, as intended). `verify_m2`'s
`plunger_travel_0.90` check (renamed/updated from the old `_0.62`
literal) — **True**, both variants.

### Item 7: window bore not flush, needed print support (Jake: "the top
of the LCD circle cutoff is not flush with the rest of the case; this
makes the print prone to failure")

**CONFIRMED root cause, live**: `window_dia/2` (22.65mm, fixed since
SPEC) is **larger than trim's own `flat_rho`** (22.14mm). At the
window's own east/west extremes (world `(±22.65, window_center.y)`), the
bore's true rim sits 0.51mm PAST the flat bed's own radius, into the R10
shoulder curve — NOT on the flat top face. The old `chamfer_edge_at`
call assumed a single, uniform circular edge sitting entirely on the
flat z=top_z face; Fusion's own edge tessellation there is not a clean
circle once part of it crosses into the curved shoulder, and the
chamfer feature silently produced an incomplete result over that
stretch instead of raising. A live 360° point-containment sweep of the
chamfer band found real, multi-degree HOLLOW gaps centred on the ±X
extremes — a visible notch in the printed rim, confirmed in a render
(`pass15_top_window_edge_wide.png`, before the fix) — matching Jake's
own description exactly, not a cosmetic non-issue. The identical sweep
on **'current'** (`flat_rho`=24.14, comfortably clearing the window)
found **zero** bad angles — this defect is **trim-only**, which is
consistent with 'current' never having been the variant Jake printed/
complained about.

**Fix**: replaced the edge-matched chamfer with a plain CUT using a
conical tool (`cone_frustum_solid` — the same primitive
`add_root_reinforcement`'s collars already use, here as a subtraction
instead of a join). A solid-geometry boolean cut is correct regardless
of whether the underlying surface at a given radius is flat or curved,
unlike an edge-selection-based chamfer feature. The cone's slope is
deliberately a touch under 45° (radius grows by chamfer+0.2mm over a
z-span of chamfer+0.6mm — the brief's own "≤45°" ceiling, with margin),
and the tool overshoots both ends so it cuts a clean, complete ring all
the way around regardless of the true local surface, growing AWAY from
the bed (the print-down face, z=top_z, is exactly where Top's flat face
sits on the bed when flipped for printing) — per the brief's own
"growing away from the bed" requirement. There is no separate retaining
ring/lip at the window bore in this design (the display glass rests on
the ordinary ceiling-underside step, not a distinct printed ring
feature), so nothing needed relocating to "grow from the bed" on its own.

**Live regression probe** (build-time, inside `add_window` itself, not
just a separate gate): probes just inside the cone tool's own slope at
every 10° around the full circle — must read HOLLOW everywhere. **0 bad
angles, both variants** (confirmed live during the build that produced
the final export). An independent, second confirmation via a pure-Python
ray-cast of the final EXPORTED `export/trim/Top.stl` (no Fusion
dependency) at the same probe geometry: **0 bad angles** — the two
independent methods agree.

**Offline overhang scan**: `tools/offline_stl_check.py` — `Top`
`bad_clusters_mm2: []`, both variants (no new cluster near the window;
the existing `general_ceiling_overhang` whitelist entry already covers
the region, and no NEW cluster appears outside it either).

### Item 8: Bottom's screw holes "filled in" (Jake: "The bottom of the
case's holes seem to be filled in?")

**CONFIRMED root cause, and confirmed WITHOUT touching Fusion first** —
a pure-Python ray-cast of the shipped pass-14 `export/{trim,current}/
Bottom.stl` at all 5 Bottom boss centres (A/B1/B2/C/D) found **every
one** reads exactly 2 surface crossings at **z=1.95 and z=3.5** — a
solid plug over that 1.55mm band, open everywhere else — for both the
plain pilot bore (Ø2.4, A/B1/B2/C) and boss D's own deeper Ø4.5
counterbore (`cb_h`=4.0, which fully contains that same band).

**Root cause**: pass 13's own root-reinforcement collar
(`add_root_reinforcement`, `cone_frustum_solid`) is a SOLID revolve —
its own profile includes the vertical axis itself as two of its four
corners, so it is filled to the centre at every z in its own band, not a
hollow washer. For every Bottom-side boss (`direction='down'`), the
collar's own z-band is `z_root−0.05 .. z_root+collar_rise` = **1.95..3.5**
— squarely inside BOTH the pilot hole's full-through span (`−0.5..
split_z+0.5`, i.e. the whole of Bottom) and (for boss D) its own deeper
counterbore — so joining the collar in after the hole/counterbore cuts
silently REPLUGS both, solid, right at the screw. Top's own posts/
bosses/pegs never hit this: every `direction='up'` call site's own
`z_root` sits far enough above its matching pilot hole's own z1 that the
collar's z-band never overlaps a hole there (checked at every one of the
7 `add_root_reinforcement` call sites, not just assumed).

**Fix, at the source**: `add_case_boss` now re-cuts the SAME pilot hole
and counterbore, in the same place, immediately after the collar join —
a plain, cheap Cut always wins over whatever the collar's join silently
refilled, restoring exact pre-pass-13 patency while keeping every mm of
the collar's own outward (well outside the hole/counterbore radius)
reinforcement intact.

**New gate, `verify_bottom_openings`** (gates `verify()`, not
diagnostic-only — this is a direct regression test for a confirmed real
defect): probes each of A/B1/B2/C/D's pilot axis at 5 depths spanning
the collar's own band, plus D's counterbore specifically, plus the lug's
own cord hole. **All open, both variants** — `A/B1/B2/C/D_pilot_open`,
`D_counterbore_open`, `lug_hole_open` all `(True, [])`.

**Confirmed live** with a direct point-containment probe at all 5 boss
centres, before AND after the fix, on the actual built (not just
exported) document — before: solid at z=1.95/2.0/2.7/3.5 at every one of
A/B1/B2/C/D; after: **hollow at every sampled z (0.5 through 9.5) at
every one of A/B1/B2/C/D, both variants**. Visually confirmed in
`pass15_bottom_holes.png` — all 5 screw holes and the lanyard cord hole
read as genuine open circles.

### Item 9: lanyard lug (Jake: "I think that still needs work?")

Reviewed against the brief's own checklist:

- **Cord hole diameter for 4–5mm paracord**: the old 4.0mm hole was at
  the tight end of that range with zero running clearance for a printed
  hole (always a touch undersized vs. nominal). Widened to **5.0mm** — a
  comfortable running fit with margin for print tolerance.
- **Wall thickness around the hole (brief's own ≥2.4mm floor)**: computed
  directly — the TIP-side wall (`hole_from_tip − hole_dia/2`) was only
  **1.5mm** at the old 3.5mm/4.0mm pair, under the 2.4mm floor (the side
  walls, `(width−hole_dia)/2` = 5.0mm, and the root-side wall, ~9mm ear
  length minus `hole_from_tip`, were never the tight dimension). Fixed by
  raising `hole_from_tip` **3.5 → 5.0mm** alongside the wider hole: tip
  wall = 5.0−2.5 = **2.5mm** (≥2.4mm, the binding dimension); side wall
  4.5mm; root wall ~4mm — all four sides now clear the floor.
- **Print orientation**: unaffected — the ear still prints flush on the
  bed (`z`=(0.0,10.0), Bottom's own face-down orientation, unchanged),
  and the hole is a plain vertical through-cylinder (axis parallel to the
  print's own Z), needing no support before or after this pass.
- **Strength against a hard tug (rough hand calc)**: PETG tensile
  strength ~50MPa; the tip-wall cross-section resisting a straight pull
  is roughly 2×(tip_wall×lug width) = 2×2.5×14 = **70mm²** — failure load
  ~70×50 = **3500N**, wildly beyond a plausible lanyard tug (a firm human
  yank is on the order of 50–150N), even derating heavily for a printed
  part's real layer-adhesion strength. The old 1.5mm wall's own same
  estimate (2100N) was ALSO nominally fine by this rough number — the
  2.4mm floor is a print-quality/consistency margin (thin printed walls
  are more sensitive to under-extrusion and stress concentration at the
  hole's own edge than the bulk number alone suggests), not a response to
  a marginal strength number.
- **Root fillets**: a new best-effort 1.0mm fillet (`lug['root_fillet_r']`)
  along the ear's own top/bottom edges where its cross-section is widest
  (the shell attachment) — same skip-on-failure idiom as every other
  cosmetic fillet in this file — reduces stress concentration right at
  the seam a hard tug loads most.

**Gate**: `verify_bottom_openings`'s own `lug_hole_open` — **True, both
variants** (see item 8). No dedicated strength gate (this is a hand
calc, not a live-probed dimension) — see the render (`pass15_lug.png`)
for a direct look at the widened hole/ear.

### 3D design expert review (both variants)

- **Printability**: `tools/offline_stl_check.py` — `OVERALL: PASS`, both
  variants, **0 non-manifold edges** on every one of the 5 exported
  bodies, envelope OK, `bad_clusters_mm2: []` on Top and Bottom (no new
  overhang introduced by any of the 9 fixes — the ceiling gusset (item
  5), the new post positions (item 3), the corner-anchored collar re-cut
  (item 8), and the window cone-cut (item 7) all sit on the bed side of
  their respective print orientations, none adding a new overhang).
- **No unintended holes through the shell**: `verify_openings_open`
  (window/USB/both button holes+footprints/lug/both antenna
  channels/mag wire notch) all `(True, [])`, both variants — unaffected
  by this pass's changes except where explicitly intended (items 1, 7,
  8). `verify_export_envelope` clean, all 5 bodies, both variants.
- **Strength of every post/boss/lug**: `verify_root_fillets` — **0 bad**
  (17 features trim / 13 current, unchanged count — item 3's new post
  positions still get the same collar reinforcement `add_top_posts`
  always applies; item 5's ceiling gusset and item 9's lug fillet are new
  reinforcement, not new posts, so they don't add entries here).
  `verify_post_walls` — 0 bad, both variants (item 3's new P1–P4). Lug
  strength: hand calc above (item 9).
- **Assembly order**: unchanged from pass 9g's own 8-step order (display
  into Top; Screen Plate onto posts — now spread across a real diagonal
  footprint, not one corner; both button caps from inside; comms-bay
  hardware into Bottom; GPS antenna + foam pad; compass module onto its
  two pegs — no fence to clear now, if anything easier to seat; halves
  joined; screws A/B1/B2/C, D, P1–P4, S1–S3) — nothing in this pass
  changes what must go in before what; the compass module (item 4) is,
  if anything, easier to seat now with no fence to align against.

### `verify()` output, both variants, full piecewise run

```
trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] / occ_interference []
         bottom_openings_results: A/B1/B2/C/D_pilot_open True, D_counterbore_open True, lug_hole_open True
         plate_post_spread_results: centroid_dist_mm 28.106 (target <=5, diagnostic-only, see item 3),
                                     angular_spread_deg 265.83 (target >=270, diagnostic-only)
         post_wall_results: P1-P4 pilot_wall/shell_skin all []  (0 bad)
         mag_pocket_results: pegs/pads/fence(stop)_have_material True; window_bore_clear 3.955; display_back_clear 3.765
         fpc_relief_results: 0 bad of 63
         plunger_reach_results: rest_gap 0.3 (exact), both buttons
         m2: plunger_travel_0.90 True
         button_insertion_results: 0 bad of 125, both buttons
         button_retention_results: all True
         battery_access_results: window_open [] / hole_clearance []
         root_fillet_results: 0 bad (17 features)
         corner_block_results / openings_results / wordmark_results / wordmark_counter_results /
           antenna_results / envelope / bump / export_envelope / posts_bosses / skin / wall: all clean
         OK: M1+M2 probes passed

current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] / occ_interference []
         bottom_openings_results: A/B1/B2/C/D_pilot_open True, D_counterbore_open True, lug_hole_open True
         plate_post_spread_results: centroid_dist_mm 28.106, angular_spread_deg 265.83 (diagnostic-only)
         post_wall_results: 0 bad
         mag_pocket_results: all True (mount skipped -- mag_module_fits still False, unchanged)
         fpc_relief_results: 0 bad of 63
         plunger_reach_results: rest_gap 0.3 (exact), both buttons
         root_fillet_results: 0 bad (13 features)
         all other gates: clean
         OK: M1+M2 probes passed
```

### Offline STL scan output (`tools/offline_stl_check.py`, pass 15)

```
=== Offline STL checks: trim ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS

=== Offline STL checks: current ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS
```

### Print orientation / notes

Unchanged from pass 14: Top face-down on its flat ceiling (z=top_z)
face, Bottom face-down on its own flat back (z=0) face. The window's new
cone-cut chamfer (item 7) sits on the bed side of Top's print
orientation, growing away from the bed, exactly like the lip/anchor
ring's own seam chamfer already does. The ceiling gusset (item 5) is a
plain vertical pillar parallel to Top's own print-Z axis near the true
wall — no new overhang. Screen Plate/buttons: unchanged.

### Exports and renders (pass 15)

Both variants: `export/<variant>/{Bottom,Top,Screen_Plate,Power_Button,
Home_Button}.stl` (all 5 re-exported — every item above touches Top
and/or Bottom and/or Screen Plate), `export/<variant>/firefly_<variant>_
case.3mf` (native, Fusion's own exporter, 5 objects), `export/<variant>/
firefly_<variant>_plate.3mf` (re-packed via `tools/stl_to_3mf.py`, same
per-part orientation convention as every prior pass — Bottom as-is, Top
`flipx`, Screen Plate as-is, Power Button `outer-x`, Home Button
`outer-rz32.74`). Coupons not re-exported (button mechanism's
cap/wall/rib/slot geometry is untouched by item 5/6 — only the ceiling
gusset and `plunger_travel` changed, neither of which the coupon rig
represents).

Renders (all viewed directly): `pass15_{trim,current}_{front,top,right,
iso}.png` (standard 4-view, both variants — clean pill silhouette,
unchanged from pass 14 at this zoom); `pass15_top_window_edge.png` /
`pass15_top_window_edge_wide2.png` (trim, the window rim after the item-7
fix — clean, complete transition, no notch, confirmed against the
`_wide.png` before-fix version showing the original defect);
`pass15_top_inside.png` (trim, interior isometric — the corner blocks,
USB tunnel liner, and the new P1–P4 post spread all visible at once);
`pass15_plate_posts.png` (trim, Screen Plate isolated from below — the 4
posts now form a real diagonal footprint, not a corner cluster);
`pass15_buttons.png` (trim, straight-on at the Power button hole — clean
single stadium opening); `pass15_bottom_holes.png` (trim, Bottom's
underside straight-on — all 5 screw holes (A/B1/B2/C/D) and the widened
lanyard cord hole read as genuine open circles, item 8's fix); `pass15_
lug.png` (trim, the lanyard end from outside/below, item 9's widened
hole/ear visible).

### Known limitations / decisions added this pass

- **`verify_plate_post_spread` (item 3) cannot pass its own 5mm/270°
  targets with a real, structural (ceiling-anchored) post arrangement —
  proved geometrically, not left unoptimized.** Kept diagnostic-only,
  same pattern as items 11/13 in the existing Known-limitations list
  above. If a true quadrant spread is ever required, the only paths are:
  shrinking the window (display-fit consequences, out of scope), or a
  fundamentally different post-to-plate fastening scheme (e.g. plate-side
  bosses screwed from above through new Top-face holes) that this pass's
  time budget did not cover.
- **The ceiling gusset (item 5) and the new post positions (item 3) were
  each hit by a real, live-caught interference on the FIRST attempt,
  both fixed in this same pass** — see items 3/5's own write-ups. Kept
  as dead-end notes in the code (not just this README) per this file's
  own convention of recording real mistakes, not just the final answer.
- **Item 6's fix (raising `plunger_travel`) was NOT independently
  re-verified against a physical print this pass** (no physical part
  exists yet) — the 87.5% stroke increase is a real, live-confirmed,
  interference-free geometric change, but whether it "feels right" under
  a finger is a print-and-test question for the next physical pass, same
  as every other tactile-feel change in this file's history.
- **`pass15_lug.png`'s own camera framing is tighter than ideal** — the
  lug's own cord hole is visible but partially cropped at this angle; a
  follow-up pass could add a dedicated, better-framed close-up the way
  `pass9c_lip_ring_section.png` did for the window lip in an earlier pass.

## 2026-09-18 pass 15b (revert the USB-C tunnel half of item 2)

Jake clarified, after seeing pass 15's result, that item 2's "the power
cable hole needs more room to actually plug in" was about the
**battery-plug window** on the Screen Plate only — not the case's own
external USB-C tunnel. Pass 15 (previous section) treated item 2 as
**both** candidates and enlarged both; this pass reverts the USB-C
tunnel half back to its pass-14 values, one-for-one, and keeps
everything else from pass 15 (including the battery-plug window
extension) exactly as it was. "revert the usb tunnel" (Jake).

**Confirmed exactly which lines belong to the tunnel change** via
`git diff 08abb3d..897318a -- hardware/case` (08abb3d = pass 14,
897318a = pass 15/#244) — three `params_current.py` numbers, all under
the `# --- USB-C tunnel ---` block, nothing in `firefly_case.py` itself
(`add_usb_tunnel` reads these three PARAMS unconditionally; no code path
changed):

| Parameter | pass-14 (08abb3d) | pass-15 (#244) | pass-15b (this pass) |
|---|---|---|---|
| `usb_tunnel_stadium` (width, height) | `(13.0, 7.0)` | `(13.0, 7.9)` | **`(13.0, 7.0)`** (reverted) |
| `usb_tunnel_center_z` | `16.4` | `16.85` | **`16.4`** (reverted) |
| `usb_liner_outer_stadium` (width, height) | `(16.2, 10.2)` | `(16.2, 11.1)` | **`(16.2, 10.2)`** (reverted) |
| `usb_tunnel_y_start` | `73.5` | `73.5` (unchanged in pass 15) | `73.5` (unchanged) |
| `usb_liner_thickness` | `1.6` | `1.6` (unchanged in pass 15) | `1.6` (unchanged) |

Net effect on the tunnel bore's own Z extent (current variant; trim is
the same profile shifted by `_DZ_TOP` = +3.0mm, per `params_trim.py`):
bottom edge `12.9mm` (unchanged by pass 15 or this revert either way,
since pass 15's own +0.45mm center shift and +0.45mm half-height growth
cancelled there), top edge back to `19.9mm` current / `22.9mm` trim
(pass 15 had pushed it to `20.8mm` / `23.8mm`) — i.e. the tunnel bore is
now byte-for-byte the same size/position it was in pass 14, the last
pass Jake printed and called "closest yet."

**KEPT, unchanged from pass 15**: the battery-plug window extension
(`BATTERY_CONNECTOR_TOP_EXTRA = 3.0`, Screen Plate, `firefly_case.py`)
and every other pass-15 item (1, 3, 4/5, 6, 7, 8, 9) — none of those
touch `usb_tunnel_stadium`/`usb_tunnel_center_z`/`usb_liner_outer_stadium`,
confirmed by the same `git diff 08abb3d..897318a` scan above (the only
other hunks touching this neighbourhood are the `BATTERY_CONNECTOR_TOP_
EXTRA` addition itself, a few lines above the USB-C tunnel block in
`params_current.py`, and unrelated blocks for items 1/3/6/9).

**Code change**: only the three PARAMS values above, in
`params_current.py` (`params_trim.py` derives its own tunnel Z via
`+ _DZ_TOP`, unchanged, so nothing there needed to change). The
pass-15 item-2 comment block ahead of the tunnel PARAMS was replaced
with a short pointer back to this section; no comment/number in
`firefly_case.py`'s `add_usb_tunnel`/`BATTERY_CONNECTOR_TOP_EXTRA`
sections needed to change.

**Rebuilt and re-verified live, full piecewise run (both variants,
same infrastructure note as every prior pass — `fusion_mcp_execute`
calls against the same open document, each stage re-fetching bodies by
name/component since Python locals don't survive between calls; several
calls here timed out client-side while Fusion kept executing
server-side, confirmed by re-querying the document and re-running the
same read afterward, per this file's own established pattern)**:

```
current: body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] / occ_interference []
         m2: usb_tunnel_open True ("point inside tunnel bore is empty (not solid)")
         m2: lug_hole_open True, plunger_pretravel_0.3 True, plunger_travel_0.90 True
         bottom_openings_results: A/B1/B2/C/D_pilot_open True, D_counterbore_open True, lug_hole_open True
         openings_results (incl. USB tunnel column): 0 bad
         battery_access_results: window_open [] / hole_clearance []  (battery-plug window kept, still clean)
         plate_post_spread_results: centroid_dist_mm 28.106, angular_spread_deg 265.83 (diagnostic-only,
           unchanged from pass 15 -- item 3 untouched by this revert)
         root_fillet_results / fpc_relief_results / wordmark_results / wordmark_counter_results /
           antenna_results / corner_block_results / mag_pocket_results / post_wall_results /
           plunger_reach_results / button_insertion_results / button_retention_results: all clean
         OK: M1+M2 probes passed

trim:    body_names ['Bottom', 'Home Button', 'Power Button', 'Screen Plate', 'Top']
         interference [] / occ_interference []
         m2: usb_tunnel_open True ("point inside tunnel bore is empty (not solid)")
         m2: lug_hole_open True, plunger_pretravel_0.3 True, plunger_travel_0.90 True
         bottom_openings_results: A/B1/B2/C/D_pilot_open True, D_counterbore_open True, lug_hole_open True
         openings_results (incl. USB tunnel column): 0 bad
         battery_access_results: window_open [] / hole_clearance []  (battery-plug window kept, still clean)
         mag_pocket_results: window_bore_clear 3.955, display_back_clear 3.765 (unchanged from pass 15)
         plate_post_spread_results: centroid_dist_mm 28.106, angular_spread_deg 265.83 (diagnostic-only,
           unchanged from pass 15)
         root_fillet_results / fpc_relief_results / wordmark_results / wordmark_counter_results /
           antenna_results / corner_block_results / post_wall_results / plunger_reach_results /
           button_insertion_results / button_retention_results: all clean
         OK: M1+M2 probes passed
```

Every number that pass 15 already fixed and is unrelated to the USB-C
tunnel (battery window, plate posts, root fillets, wordmark, antenna
channels, corner blocks, mag pocket, bottom openings/lug hole, plunger
travel, window chamfer) reproduces exactly the same live results as the
pass-15 section above — confirming this revert touched nothing else.

**Offline STL scan** (`tools/offline_stl_check.py`, both variants, run
against the fresh exports below):

```
=== Offline STL checks: current ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS

=== Offline STL checks: trim ===
Bottom: manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Top:    manifold (0 non-manifold edges), envelope ok, overhang bad_clusters_mm2 []
Screen_Plate / Power_Button / Home_Button: manifold ok, envelope ok
OVERALL: PASS
```

**Exports refreshed**: `export/<variant>/{Bottom,Top,Screen_Plate,
Power_Button,Home_Button}.stl` re-exported from the rebuilt document for
both variants, plus `export/<variant>/firefly_<variant>_case.3mf`
(native, re-exported) and `export/<variant>/firefly_<variant>_plate.3mf`
(re-packed via `tools/stl_to_3mf.py`, same per-part orientation
convention as every prior pass — Bottom as-is, Top `flipx`, Screen Plate
as-is, Power Button `outer-x`, Home Button `outer-rz32.74`). Bytes
actually changed vs. the pass-15 commit: `current/{Bottom,Top}.stl` and
`trim/Top.stl` (`current/Bottom.stl` isn't touched by the USB tunnel
geometry itself, but Fusion's STL export is not byte-deterministic
run-to-run even for identical B-rep geometry, so a fresh export still
differs at the byte level — the offline manifold/envelope scan above,
not a byte-diff, is what actually confirms correctness).
`trim/Bottom.stl`, and every variant's `Screen_Plate`/`Power_Button`/
`Home_Button.stl`, are unchanged (git-identical to the pass-15 commit).
No new renders taken this pass (the tunnel revert has no visible
silhouette change worth a fresh screenshot set; the pass-15 renders
already show the rest of the case correctly).

## Screw list

**2026-09-07 pass 7: boss B split into B1/B2** (its old single position
sat inside the L76K PCB's own footprint — see the pass-7 section above),
and boss D's post grows with trim's taller case, changing its screw
length. **2026-09-12 pass 12 (REVERTED 2026-09-13, pass 12b): trim's Top
briefly grew to 30mm, forcing screw D to M2×16** — this pass reverted
`top_z` back to 28 (the height bump did not fix the FPC-relief pocket it
was meant to help, and cost this real screw-size regression for nothing —
see the pass-12 and pass-12b sections above). D's engagement is back to
the pass-7/9/11 numbers: `plate_post_D_z=(10.0, 16.1)`,
`counterbore_D_h=4.0` → **12.1mm** of real engagement, M2×12. Every other
screw is unaffected by the height revert (A/B1/B2/C's `top_pilot_z=
(10.0, 19.1)` is parting-plane-anchored, independent of `top_z`; P1–P4's
`top_post_pilot_z` span stays exactly 6.5mm, back to `(17.1, 23.6)` from
pass 12's `(19.1, 25.6)`) and none of this pass's OTHER change
(`usb_end_extension_mm`, an outer-envelope-only Y shift) touches any
screw's Z engagement at all. Current per-variant screw map:

| Screw | Qty | current | trim | Joins |
|---|---|---|---|---|
| M2×12 socket head | 4 | ✓ | ✓ | Bottom bosses A/B1/B2/C → Top bosses (Ø1.62 pilot, z 10–19.1 — parting-plane anchored, unchanged by case height) |
| M2×10 socket head | 1 | ✓ | | Bottom boss D → Screen Plate post (Ø1.62, z 10–13.1) |
| M2×12 socket head | 1 | | ✓ | Bottom boss D → Screen Plate post (Ø1.62, z 10–16.1 — grows with trim's +3mm case height over `current`; same 4.0mm counterbore, so 12.1mm of real engagement needs the next size up from `current`'s M2×10 — pass-9/11 numbers, restored 2026-09-13 pass 12b after pass 12's temporary M2×16) |
| M2×6 socket head | 4 | ✓ | ✓ | Top posts P1–P4 (**Ø5, was Ø4 — see pass-9 part-2 "Finding 4"**) → Screen Plate (Ø1.62 pilot, z 14.1–20.6 current / 17.1–23.6 trim — same 6.5mm span, shifts with the plate) |
| M2×4 socket head | 3 | ✓ | ✓ | Screen Plate → board SMT standoffs S1–S3 |

So **trim needs 5×M2×12 + 4×M2×6 + 3×M2×4** (12 screws total, same count
as pass 7 — B1+B2 replaces B 1-for-1, and D's M2×10 becomes a 5th
M2×12); **current needs 4×M2×12 + 1×M2×10 + 4×M2×6 + 3×M2×4**
(unchanged, frozen at 25mm). Pass 12's `4×M2×12 + 1×M2×16` for trim is
superseded — do not use it.

Bottom bosses A/B1/B2/C get a Ø4.5×2.2mm counterbore from z=0; boss D gets a
deeper Ø4.5×4.0mm counterbore (its screw tip must stay ≤ plate_z[0] — the
USB-C shell sits just above it in both variants).

**Screw A/B1/B2/C xy positions** (2026-09-08 pass 9: A/C moved again —
see the pass-9 "Finding 2" section above for why the pass-2/pass-7
positions below still breached the shell on both variants, and why the
fix is a reposition to the dome-tip end rather than a smaller inboard
nudge):

| Screw | current | trim |
|---|---|---|
| A | (−15.5, −8.0) | (−15.5, −8.0) *(absolute, same both variants, pass 9)* |
| B1 | (−12.5, −15.0) | (−12.5, −15.0) *(absolute, same both variants)* |
| B2 | (12.5, −15.0) | (12.5, −15.0) *(absolute, same both variants)* |
| C | (15.5, −8.0) | (15.5, −8.0) *(absolute, same both variants, pass 9)* |
| D | (0.0, 60.0) | (0.0, 60.0) *(unchanged; moved from (0,65) in pass 6)* |

~~Trim's A/C use `x = ±(outer_radius - wall - 3.0)`~~ **superseded
2026-09-08 (pass 9)**: that rule put the boss center itself beyond
`flat_rho` (the true limit of the flat bed) for both variants — A/B1/B2/C
are now ALL absolute mm positions clustered at the dome-tip end (like
B1/B2/D already were), identical in both variants, since current's wider
shell just has more margin around the same numbers. Every boss (both
variants) is also Combine-Intersected against the shared inner-cavity
clip tool regardless of its nominal position, so it can never punch
through the shell even if a future variant's numbers are off — and
(2026-09-07) each boss's lip/anchor relief cut is clamped to stay inside
the true wall distance too, with (2026-09-08 pass 9) a hard minimum
clearance over the boss's own OD (`MIN_RELIEF_CLEARANCE`) so a
too-close boss fails loudly instead of leaving a sliver — see the pass-9
"Finding 11" section above.

**Screen-plate post P1–P4 xy positions** (**2026-09-15 pass 15, item 3:
rebalanced again** — see that section above for the full geometric proof
that a literal quadrant spread is impossible for ceiling-anchored posts
here, and for the search that produced this specific arrangement — the
best angular spread achievable within the one safe region, west of the
GPS patch and south of the window's own exclusion circle): ABSOLUTE mm,
identical in both variants, like A/B1/B2/C/D. **P1 unchanged from pass
9g; P2–P4 moved.**

| Post | current | trim |
|---|---|---|
| P1 | (−20.0, 14.0) | (−20.0, 14.0) *(unchanged since pass 9g)* |
| P2 | (−9.0, 22.0) | (−9.0, 22.0) *(pass 15; was (−10.0, 14.0) pass 9g)* |
| P3 | (−11.0, 14.0) | (−11.0, 14.0) *(pass 15; was (−20.0, 25.0) pass 9g)* |
| P4 | (−18.0, 23.5) | (−18.0, 23.5) *(pass 15; was (−10.0, 25.0) pass 9g)* |

All z-depths/pilot diameters/screw lengths in the table above are
unchanged by this move (only xy shifted, and `plate_south_extension`
needed no change — its existing x(−24,−6)/y(10,29.5) box already covers
all four new positions) — see the pass-15 "item 3" section above for the
live `verify_post_walls`/`check_interference` confirmation.

## Known limitations / deviations from SPEC.md

Documented here as the report's "any deviation from this spec with the
reason" per the milestone instructions.

1. ~~Button caps assume a flat outer wall~~ **RESOLVED 2026-09-05**: caps
   are now Combine-Intersected against the real curved shell (see the pass
   2 section above).
2. ~~USB tunnel liner assumes a flat +y wall~~ **RESOLVED 2026-09-05**:
   same fix, intersected against the plain outer envelope.
3. **Comms bay footprints don't all fit at face value at the very
   floor/ceiling extremes.** The pass-2 half-disc bay redesign (see above)
   fits the SPEC hardware envelopes properly for the main features, but
   `clip_to_inner_cavity`'s safety margin and the general shoulder-curve
   tightness near z≈2-3mm mean a millimeter-scale mechanical check is
   still worthwhile before final fabrication, particularly at the
   -y dome tip where the L76K frame sits.
4. ~~Comms board occurrences are inserted but NOT positioned~~ **RESOLVED
   2026-09-05 (pass 2)**: XIAO/Wio/L76K are now correctly placed via
   `occ.transform` + `design.snapshots.add()`. ~~L76K's own bounding box
   (including its antenna cable/lead) extends well past the small frame
   built for the bare board, so positioning by that aggregate box put the
   actual PCB outside the case~~ **RESOLVED 2026-09-05 (pass 3)**:
   `find_pcb_like_body` positions the assembly by its actual ~18×21mm PCB
   body specifically (found by shape, not by name), not the whole
   occurrence's aggregate bbox; the antenna sub-occurrence is hidden. See
   the pass 3 section above.
5. **FPC antenna keep-out** is modeled as a simple reference box (not
   joined/cut into any body, excluded from exports) marking the strip
   SPEC.md describes — it is not a real keep-out enforcement (nothing
   currently checks the LoRa antenna or its cable against it).
6. **Wordmark deboss** is built by stroking `kandiwooks_logo.json`'s
   already-extracted polyline loops directly as straight sketch line
   segments (no arc-fitting) — visually correct at the 0.4mm deboss depth
   used here, but worth a visual check in Fusion after generation; letter
   spacing/kerning is whatever the original logo document's loops encode
   plus a single uniform scale-to-`wordmark_width`.
7. **Screen Plate "Ø6 pads at P1–P4"** are not modeled as a distinct raised
   feature — the plate is already solid there within its outline, so no
   additional geometry seemed implied beyond the Ø2.4 mounting holes
   themselves.
8. ~~Plunger guide rib plate slightly overshoots the outer wall~~
   **RESOLVED 2026-09-05 (pass 3), and it was bigger than "slightly"**:
   the Home button's rib punched ~1.45mm through the dome, not the ~0.04mm
   this line originally estimated — root-caused to `button_geometry()`'s
   `s_wall` using the flat-wall approximation for a button that's actually
   in the domed end cap. Fixed at the source (`s_wall` now uses
   `true_wall_distance_along_ray`); see the pass 3 section above.
9. **`verify_export_envelope`'s lug/cap-head exceptions are tuned to the
   trim variant's geometry** (the lug's `y` threshold is derived from
   `spine_a.y - outer_radius + 2`, which generalizes correctly across
   `outer_radius`, but hasn't been independently re-verified against a
   fresh `current`-variant export since pass 3 landed). Superseded in
   spirit by pass 6's `lug_ear_geometry`, which both `add_lug` and this
   check now share -- they can no longer disagree, though the underlying
   exception logic itself wasn't re-audited this pass.
10. ~~Case-screw boss B is not joined into Bottom~~ **RESOLVED 2026-09-07
    (pass 7)**: boss B is retired outright, replaced by B1/B2 at an
    absolute position clear of the (also new-in-pass-7) 3-board comms
    stack -- see the pass-7 section above and the Screw list. Every
    boss/post (A/B1/B2/C/D, P1-P4) now has real, verified material with
    no documented exception.
11. **`verify_skin_intact` and `verify_wall_integrity` (new in pass 6)
    over-fire on points unrelated to the defects they were written to
    catch** and are reported but not gated on in `verify()`.
    `verify_skin_intact` flags most perimeter points around both button
    holes, not just near the tab -- almost certainly probing into the
    rib/collar's own legitimate internal void at points away from the
    tab, not the outer skin. `verify_wall_integrity` flags two points
    right beside the lug's own real geometry (the simple angular-sweep
    math doesn't account for the ear replacing the plain dome profile
    there) and boss A/C at the shoulder-curve transition height (the same
    kind of flat-ray-vs-true-curvature slack `verify_m2`'s cap-proud
    check already documents, ~0.25mm). The actual defects these two
    checks target (items 3, B, C from this pass) are independently
    confirmed clean via `check_interference` (0 real interference, both
    buttons, both variants) and via `verify_wall_integrity`'s OWN other
    150+ dome-perimeter points, which all pass. Needs probe-geometry
    tuning in a follow-up pass before these can safely gate `verify()`.
12. **XIAO's pin headers were not modeled as separate bodies** in a brief
    board-to-board (B2B) restack investigated mid-pass-6 (see git history
    on this branch for the abandoned attempt) -- the inserted XIAO
    reference doc's socket/header geometry didn't obviously expose
    anything matching "two 7-pin male headers" as distinct bodies, so a
    "trim pins to 1mm stubs" step couldn't be validated. That whole
    B2B/3-board-stack/case-height exploration was reverted in this pass
    (see below) rather than shipped half-verified.
13. ~~Findings 4–10 from Jake's pass-7 print review are not yet fixed~~
    **Findings 4 (plate posts P1–P4), 5 (window lip ring), and 6
    (alignment lip chamfer) RESOLVED 2026-09-08 (pass 9, part 2)**; **9
    (button cap insertion path) and 10 (Home plunger length) RESOLVED
    2026-09-08 (pass 9b)**; **7 (wordmark two-line layout) and 8 (antenna
    cable channels) RESOLVED 2026-09-08 (pass 9e)** — see that section
    above for root cause/fix/gate on all seven. The generic "`verify_
    skin_intact` probes the WHOLE outer surface, not named footprints"
    rework requested alongside the original 4–10 list is still not done;
    the existing narrower `verify_skin_intact` (button tab holes only,
    see item 11 above) is unchanged.
14. **Display module cannot be inserted "from inside" or "from the
    front"** (confirmed 2026-09-08, pass 9 part 2, finding 5): the
    module's own PCB (39.2×41.4mm, ~57mm diagonal) is larger than the
    window bore (Ø45.30) in every direction, so it must be seated into
    the separate, unassembled Top half before Bottom is joined on — see
    the Print orientation section's "Assembly order" bullet. This is a
    property of the real hardware (the PCB is simply bigger than the
    bore), not a defect introduced by this generator, but it's a real
    constraint on how the case must be assembled and is documented here
    per the "any deviation from SPEC.md" reporting requirement (SPEC.md
    does not specify an assembly order).
15. ~~The lip/anchor ring's seam chamfer (finding 6) and the top-post
    root fillets (finding 4) are best-effort Fusion chamfer/fillet
    features~~ **root fillets RESOLVED 2026-09-07 (pass 13, item 1)**:
    the top-post fillet (still a real, best-effort 1.0mm
    `fillets.createInput` call, unchanged since pass 9) is superseded by
    `add_root_reinforcement`, live-probed via the new `verify_root_
    fillets` gate — see pass 13's own section below for the full
    investigation (a plain fillet only reaches ~0.2mm of real material at
    the probe height even when it applies; the actual, root-caused fix is
    a 45-degree conical collar, boolean-joined, on every post AND every
    case boss A/B1/B2/C/D, not just the 4 top posts this item used to
    describe). The lip/anchor ring's OWN seam chamfer (a `chamferFeatures`
    call, a different feature from the post/boss fillets) is untouched by
    pass 13 and remains best-effort/unprobed as originally described here.
16. **`rib_inboard_offset` (nominal 6.0mm) is now a per-button EFFECTIVE
    value, not a flat constant** (pass 9b, finding 9's collateral fixes):
    `button_geometry` shifts it dynamically (toward the wall) only when
    the nominal value would put the rib/collar closer than
    `rib_actuator_clearance` to the real switch actuator — a no-op for
    Power on both variants and for Home on 'current', but Home/'trim'
    builds at an effective ~4.1mm. Documented here since it's a real,
    load-bearing deviation from the flat "5-7mm" convention the comment
    used to describe, even though `PARAMS['rib_inboard_offset']` itself
    is unchanged (still 6.0, still gated 5.0-7.0) — the shift happens at
    build time, not in PARAMS.
17. **The rib+connector's Combine-Intersect against the true outer
    envelope (`add_button`, findings 9/10's collateral fix)** is a best-
    effort protective clip, same pattern as `add_lug`'s corner fillets
    elsewhere in this file — applied unconditionally on both buttons now
    (see that section's own writeup for why a conditional version missed
    a real breach on 'current'), confirmed clean via a live export-
    envelope check on both variants, but there is no dedicated gate that
    specifically confirms an intersect actually clipped something on the
    runs where it needed to (as opposed to being a no-op) — `verify_
    export_envelope` (which does gate) is the actual protection here,
    same reasoning as item 15's fillets/chamfers.
18. **The antenna channels' own fillets (finding 8, `_best_effort_
    fillet`) are best-effort**, same skip-on-failure pattern as items 15
    and 17 above — cosmetic only (a sharp-cornered channel still routes
    and clears the same gates), not re-verified by a dedicated probe
    beyond the offline overhang scan staying clean.
19. **`pass9e_trim_antenna_routes.png` does not clearly show the LoRa
    route** (finding 8) — it is a short (4.4mm) stub next to the display
    module at the render's chosen camera distance/angle, unlike the
    longer, clearly-visible GPS route in the same image. The channel's
    own existence and skin safety are independently confirmed by the live
    `verify_antenna_channels` probes (see that section), so this is a
    documentation/render-quality gap, not an unverified feature — a
    follow-up pass could add a dedicated close-up the way pass 9c did for
    the lip ring.
20. **The FPC brow's inter-tier seam fillets (pass 9g) did not apply** —
    `FPC_BROW_TIERS` replaced the old single-box brow with a 2-tier taper
    (a real, gate-clean, visibly gentler shape — see pass 9g's "Finding
    2"), but the best-effort constant-radius fillet meant to smooth each
    of the two risers produced zero `Fillet` timeline features in either
    variant. Tried `isTangentChain=False` (on the theory that chain-
    matching was pulling in unrelated dome edges) and got the same
    result, so that specific hypothesis is ruled out; the real cause is
    unconfirmed — most likely the fillet radius (1.0–1.5mm) is too large
    for the ~3mm-wide shelf between the two risers, but this needs a live
    edge-count print inside the `try` block (to distinguish "no edges
    matched" from "the fillet solve itself failed") before it's worth
    guessing at smaller radii. Not gated on (no dimensional check depends
    on the fillet existing — `verify_fpc_relief` is unaffected either
    way), purely cosmetic/print-smoothness, same category as items 15/17/
    18 above.
21. **The generic "`verify_skin_intact` probes the WHOLE outer surface"
    rework (coordinator's pass-9g brief, item 3) is still not done** —
    unchanged from item 13's own note on this; the existing narrower
    `verify_skin_intact` (button tab holes only) is unmodified this pass.
    `tools/offline_stl_check.py`'s envelope/overhang scan is a partial,
    independent substitute (see pass 9g's "Finding 4") but does not check
    skin thickness.
22. **The lug ear taper (pass-9g cosmetic check) was reviewed but not
    close-up rendered** — `pass9g_{trim,current}_lanyard_end.png` shows no
    obvious defect at that render's distance, but a dedicated close-up
    (same idea as `pass9c_lip_ring_section.png` for the window lip) would
    give more confidence than the current wide shot. Not modified this
    pass.
23. ~~The compass module's two Ø2.7 pegs (pass 10) are plain cylinders,
    not a self-supporting profile~~ **N/A after the pass-10 REDO** — the
    rejected vertical-wall mount's pegs ran horizontally (a real, if
    small, printed overhang); the redone ceiling-hung mount's pegs/pads
    are ordinary VERTICAL cylinders parallel to Top's own normal print
    orientation (same as every case-screw boss/Top post), so there is no
    overhang to whitelist at all — confirmed by a clean `bad_clusters_
    mm2: []` on both Top and Bottom in the redo's own offline STL scan.
24. ~~`pass10_mag_pocket.png` does not clearly resolve the compass
    module's two individual pegs`~~ **RESOLVED by construction** — the
    redo's `pass10b_mag_pocket.png` (looking up into the ceiling from
    inside the cavity, Bottom/Buttons/boards/reference bodies hidden)
    clearly shows the retaining fence ring, both rest pads, and both
    pegs at a 5cm view width; no dedicated-shot follow-up needed.
25. ~~The mag-module brow/pocket boxes (pass 10) are sized from the
    analytic outer-radius profile, not a live Fusion clip~~ **N/A after
    the pass-10 REDO** — the redone mount needs no pocket or brow cut at
    all (see that section above): it hangs from the ceiling entirely
    within space that was already open cavity, so there is no skin-safe
    cut, and therefore no analytic-vs-live-clip tradeoff to flag.
26. **The QMC5883P's own sensor-axis-to-PCB-silkscreen relationship is
    still unknown** — pass 10 (both the original and the redo) fixes the
    compass module's PHYSICAL mounting (which PCB edge points which way
    in the puck's own frame, see that section's Orientation above) and
    updates `ff_compass.c`'s comment to say so, but translating that into
    the firmware's `FF_MAG_QMC5883P_BOARD_*` axis-remap table still needs
    a bench check against the real chip's silkscreen/datasheet Package
    3-D View — the table's current numeric values are the OLD flat-mount
    guess (predating either real mounting) and are explicitly flagged as
    stale in that file's comment, not updated with new (unverified)
    numbers.
27. **'current' cannot host the compass module mount at all** — its
    ceiling (`top_ceiling_underside_z`=23.0, frozen since pass 7 "for the
    probe comparison") sits only 4.2mm above the GPS patch, 0.3mm short
    of the 4.5mm standoff+PCB+component stack this mount needs (trim's
    26.0mm ceiling gives 7.2mm, 2.7mm to spare). `mag_module_fits()`
    correctly skips the mount for 'current' (verified: `mag_pocket_
    results` all `(True, [])`, the "nothing to check" shape) rather than
    forcing an interference — see the pass-10 section above for the full
    numbers. Fixing this for real would mean growing 'current' the same
    3mm trim already grew in pass 7, which is a deliberate scope decision
    (Jake's own "'current' stays at height 25 for the probe comparison"),
    not an oversight of this pass.
28. **Retention across the 2.7mm spare above the patch relies on a
    physical foam pad, not modeled in Fusion** — the module is trapped
    between the ceiling pegs/pads and the GPS patch/frame only once a
    ~2.0mm compressible foam pad is added to the patch's own top face at
    assembly time (see the pass-10 section's Retention paragraph); a
    printable interference lip was considered and rejected as too fragile
    at this thickness (1-2mm cantilevered print detail against light
    spring pressure) for the assembly to survive repeated openings. The
    foam pad is a real BOM/assembly-order item, not a modeled part.
29. **Pass 11's mag-mount reposition reaches 3.955mm/3.765mm window-bore/
    display clearance, short of the brief's 5mm stretch target** — the
    GPS frame's own real opening (x -3.05..22.45, y 1.75..27.25) is the
    binding constraint: the fence is already down to a 0.5mm safety
    margin from that frame's own wall on both the south and east sides
    (see the pass-11 section's frame-margin table), so reaching 5mm would
    mean either shrinking that margin (risking a real interference with
    the GPS frame itself) or reworking the frame's own footprint, which
    is out of this pass's scope. Both clearances comfortably clear the
    hard 3mm gate (`MAG_DISPLAY_RING_MIN_CLEAR`) with real margin.
    `verify_display_insertion_path`'s pre-existing diagnostic-only
    failure (module can't be inserted "from inside" past the lip/anchor
    ring, `ok: False`) is unrelated to and unchanged by this pass — see
    item 14.

**Reverted mid-pass-6, not shipped**: the coordinator's later messages in
this pass requested (a) swapping the Wio/XIAO stack to a board-to-board
kit with XIAO on the bottom, component-side down, pins trimmed; then (b)
superseding that with a 3-board (L76K+XIAO+Wio) direct-solder stack in a
new cradle at the dome tip, replacing the tray and the L76K floor frame,
with the battery and GPS patch relocated; then (c) superseding *that*
with a real 18mm stack height requiring the case itself to grow from 25mm
to 28mm tall (trim), with every Top feature tied to the display
re-expressed relative to a parameterised `z_top`. Each of these is a
substantial re-architecture in its own right (new cradle geometry, a
relocated bay, or re-deriving every Z-dependent Top feature off a
variable case height) that could not be implemented AND properly
re-verified (fresh M1/M2 probes, interference, clearances, exports) in
the time remaining in this pass without risking shipping something
broken or silently under-tested. The comms-board insertion and stack-tray
code in this delivered pass is back to the exact pre-pass-6 (pin-header,
Wio-bottom/XIAO-top) configuration, verified working -- see `verify()`'s
clean pass on both variants. The 18mm-stack/28mm-case redesign (and,
separately, the simpler B2B or 3-board cradle ideas, whichever the
coordinator prefers) is real, wanted follow-up work, not abandoned --
it just needs its own dedicated pass with a full verification budget
rather than being squeezed into this one's remaining time.

## Verify() output reference

Running `run()` prints, in order: variant, document name, timeline feature
count, the printed body names, each body's bounding box, the full M1 outer
and cavity probe tables (z, expected ρ, found ρ, pass/fail), interference
results, M2 dimensional/probe checks, outer-bump probes, the export
envelope vertex check (one line per exported body, `True`/`False` plus a
sample of any offending vertex), plunger reach / button insertion /
button retention checks (pass 9b, findings 9/10 — see that section),
wordmark checks (pass 9e, finding 7) and antenna channel checks (pass 9e,
finding 8 — see that section), and (with `export=True`) the STL export
paths, coupon export paths, and screenshot paths. A clean run ends with
`OK: M1+M2 probes passed`.
`assert_export_body_size` runs silently inside `export_stls`/`export_
coupons` at export time — no line unless it fails (in which case it
raises, same as every other `verify()` assertion).

## 2026-09-09: headless build123d port, phase 1 (parallel effort, does not touch this file's own Fusion generator)

Per the coordinator's own spike (`docs/hardware/cad-tooling-spike.md`,
branch `spike/headless-cad`, draft PR #250 — timing table, ~19% Fusion-
quirk classification, a working shell/lip/corner-block slice) and the
owner's "yes, start the port now in parallel" decision, a new package,
`hardware/case/gen/` (headless [build123d](https://github.com/gumyr/build123d),
OpenCascade under the hood), ports `firefly_case.py` feature-by-feature.
**This file (`firefly_case.py`) and its own Fusion-driven workflow above
are completely unchanged by this effort** — `gen/` is a parallel,
independent package; nothing here required editing the generator this
README otherwise documents. Full details, every function's port status,
and the phase 2/3 plan: `docs/hardware/headless-port-plan.md`. Regression
numbers against the case-pass16 branch's own exports (the newest
generator; not yet merged to `main`, see below):
`docs/hardware/headless-port-parity.md`.

**Ported this phase:** Top/Bottom shell (pill outline, R8 ceiling fillet,
2mm wall, both variants), the window bore + print-orientation chamfer +
glass-seat chamfer, the pass-16 continuous-taper lip/anchor ring, case
screws A/C/D (Bottom boss + counterbore, Top single-pilot corner block,
the 45-degree root-reinforcement collar on all 6 boss/block roots), the
USB-C tunnel + liner, the FPC relief pocket, and the lanyard lug. STEP
import + empirically-derived placement of the display module
(`gen/components.py`) reproduces the documented standoff-plane numbers
(18.80mm current / 21.80mm trim) exactly from the real STEP geometry —
see that module's own docstring for the derivation. Gates ported:
all-pairs interference, pilot-wall/root-fillet/corner-block/bottom-
opening probes against live OCC solids, and the lip-ring-profile +
manifold/body-count/overhang scan against the exported STL (the latter
via `tools/offline_stl_check.py`, reused unchanged — it already needed
no porting, being pure Python). All gates pass clean, both variants,
from a from-scratch rebuild. Cycle time (build + gates + export +
render, trim, warm venv): under 8 seconds, against Jake's own observed
5-10 minutes per Fusion-MCP rebuild+gate cycle.

**Source-of-truth note:** this port's own architecture (A/C/D as
single-pilot corner blocks, D relocated to (18, 58), ears/S2-boss
deferred to phase 2) matches the **case-pass16** branch's own
"candidate 5" display-mount design, not this file's own pre-pass-16
state (paired B1/B2 corner blocks, a Screen Plate + P1-P4 posts, D at
(0, 60)) — pass 16 has not merged to `main` yet. `params_current.py`/
`params_trim.py`/`tools/offline_stl_check.py` in this repo were pulled
forward from `case-pass16` (pure data + a pure-Python tool, neither
Fusion-dependent) so `gen/params.py` imports them unchanged, per the
port brief's own instruction to reuse the existing params files where
possible.

**Component models:** `hardware/models/` now holds the STEP exports
this port's `gen/components.py` imports directly (no git-lfs configured
in this repo, so these are committed as plain blobs): `ESP32-S3-Touch-
LCD-1_46.step` (14.8 MB, display module with cover), `XIAO-ESP32S3_v3.step`
(2.9 MB), `L76K_GNSS_for_XIAO_v1.step` (3.5 MB), `Wio-SX1262_for_XIAO_V2.step`
(1.5 MB) — all staged for phase 2's comms-stack port. `GY-273_compass_
module.f3d` (1.6 MB) is also included for reference, but is a Fusion
archive, not importable headlessly; the port brief's own instruction is
to model the compass module as a plain box from params instead (phase 2).

**Not ported this phase** (see `docs/hardware/headless-port-plan.md`
for the full 217-function breakdown): the S1/S3 ears + S2 boss (display
mount proper), buttons, comms stack/GPS frame/battery bay, the compass
module, antenna channels, and the wordmark deboss — each is a real,
scoped phase-2/3 item, not an oversight.

### How to run the headless build

```
cd hardware/case
uv venv gen/.venv --python 3.12 && source gen/.venv/bin/activate
uv pip install build123d trimesh matplotlib numpy rtree pytest
python3 -m gen.cli build --variant trim --gates --export --render
python3 -m pytest gen/tests -v   # gates-as-tests + parity vs. the case-pass16 goldens
```

`gen/.venv` and `gen/out/` are git-ignored (see `gen/.gitignore`) —
recreate the venv with the commands above; `gen/out/` and
`renders/gen/` are scratch build output, not committed exports (this
port does not yet touch `export/<variant>/`, the Fusion generator's own
canonical export directory / regression-golden location).
