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
| `params_trim.py` | 56×102×25 variant (**default**, Jake's 2026-09-04 decision) — derived from `params_current.py` with documented overrides, not hand-duplicated. |
| `kandiwooks_logo.json` | KandiWooks wordmark outline loops (mm), extracted from the "KandiWooks Logo" document's 6 bodies. |
| `SPEC.md` | The original task brief, verbatim. |
| `export/<variant>/*.stl` | Per-body STL exports (binary): `Bottom`, `Top`, `Screen_Plate`, `Power_Button`, `Home_Button`. |
| `export/<variant>/firefly_<variant>_case.3mf` | Native 3MF (2026-09-06, pass 6) containing exactly the 5 printed bodies (`Print — Case` + `Print — Buttons`), for viewers/slicers that read 3MF's per-object structure directly instead of separate STLs. |
| `export/coupons/coupon_{power,home}_{wall,cap}.stl` | Standalone button fit-test coupons (see Print orientation & settings below). |
| `export/coupons/firefly_coupons_native.3mf` | Native 3MF (pass 6) with the 4 coupon bodies. |
| `renders/<variant>_{front,top,right,iso}.png` | Orthographic screenshots. |
| `renders/{power,home}_button_ext.png`, `lanyard_end.png`, `bottom_logo.png`, `plate_underside.png`, `bay_inside.png`, `rim_{lanyard_end,usb_end}.png` | Pass-6 close-up renders, TRIM variant, showing the fixes in this pass. |

Every exported body (case and coupon) is size-checked at export time
(`assert_export_body_size`, ≤120mm/≤40mm max extent respectively) as a
guard against a units/scale bug — see Verification below.

## Parameter table (key numbers)

All dimensions mm. Full detail in `params_current.py`/`params_trim.py` —
this is the headline subset.

| | current | trim |
|---|---|---|
| Outer envelope | 60 × 110 × 25 | 56 × 102 × 25 |
| Outer radius | 30.0 | 28.0 |
| Spine | (0,0)–(0,50) | (0,0)–(0,50) *(unchanged)* |
| Wall | 2.0 | 2.0 |
| Shoulder flat radius | 24.14 | 22.14 |
| Shoulder tangent point ρ | 27.07 | 25.07 |
| Outer fillet R | 10.0 | 10.0 |
| Inner fillet R (derived) | 8.0 | 8.0 |
| Lip ring R | 26.95–27.75 | 24.95–25.75 |
| Anchor ring R | 26.95–28.40 | 24.95–26.40 |
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

### Not completed this pass: findings 4–10

Findings 4 (plate posts P1–P4 have no wall), 5 (window lip ring
fragile), 6 (alignment lip chamfer), 7 (wordmark two lines), 8 (antenna
cable channels), 9–10 (button cap insertion path + Home plunger length),
and the broader "generic `verify_skin_intact` probes the WHOLE outer
surface" gate described in the brief, were **not attempted this pass**.
Reason: this pass's Fusion MCP session was unexpectedly unstable (see
the infrastructure note above) — isolating and working around the
per-call timeout, plus one Fusion-side stall that needed several minutes
to clear on its own (not a code issue; Fusion recovered without a
restart), consumed the large majority of the session's time budget
before findings 1/2/3/11 were even confirmed clean end-to-end on both
variants. Rather than make unverified geometry changes for the remaining
findings under time pressure — which this project's own history (see the
pass-6/7 sections above) shows is exactly how silent regressions get
shipped — they're left for a follow-up pass with its own full
verification budget. Findings 4 and 5 are related (both concern the
plate/lip-ring region near the display window) and should likely be
tackled together; 9/10 (button mechanism) are independent and probably
the next-easiest to verify in isolation via the existing button coupons.



- **Bottom**: print face-down on its flat z=0 face (the KandiWooks
  wordmark side).
- **Top**: print face-down on its flat z=25 face (the flare-glyph side).
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

## Screw list

**2026-09-07 pass 7: boss B split into B1/B2** (its old single position
sat inside the L76K PCB's own footprint — see the pass-7 section above),
and boss D's post grows with trim's taller case, changing its screw
length. Current per-variant screw map:

| Screw | Qty | current | trim | Joins |
|---|---|---|---|---|
| M2×12 socket head | 4 | ✓ | ✓ | Bottom bosses A/B1/B2/C → Top bosses (Ø1.62 pilot, z 10–19.1 — parting-plane anchored, unchanged by case height) |
| M2×10 socket head | 1 | ✓ | | Bottom boss D → Screen Plate post (Ø1.62, z 10–13.1) |
| M2×12 socket head | 1 | | ✓ | Bottom boss D → Screen Plate post (Ø1.62, z 10–16.1 — grows with trim's +3mm case height; same 4.0mm counterbore, so ~12.1mm of real engagement now needs the next size up from M2×10) |
| M2×6 socket head | 4 | ✓ | ✓ | Top posts P1–P4 → Screen Plate (Ø1.62 pilot, z 14.1–20.6 current / 17.1–23.6 trim — same 6.5mm span, shifts with the plate) |
| M2×4 socket head | 3 | ✓ | ✓ | Screen Plate → board SMT standoffs S1–S3 |

So **trim now needs 5×M2×12 + 4×M2×6 + 3×M2×4** (12 screws total, same
count as before pass 7 — B1+B2 replaces B 1-for-1, and D's M2×10 becomes
a 5th M2×12); **current needs 4×M2×12 + 1×M2×10 + 4×M2×6 + 3×M2×4**.

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
13. **Findings 4–10 from Jake's pass-7 print review are not yet fixed**
    (plate posts P1–P4, window lip ring, alignment lip chamfer, wordmark
    two-line layout, antenna cable channels, button cap insertion path,
    Home plunger length) — see the pass-9 section's "Not completed this
    pass" note for why and suggested grouping for a follow-up pass. The
    generic "`verify_skin_intact` probes the WHOLE outer surface, not
    named footprints" rework requested alongside them is also not done;
    the existing narrower `verify_skin_intact` (button tab holes only,
    see item 11 above) is unchanged.

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
sample of any offending vertex), and (with `export=True`) the STL export
paths, coupon export paths, and screenshot paths. A clean run ends with
`OK: M1+M2 probes passed`. `assert_export_body_size` runs silently inside
`export_stls`/`export_coupons` at export time — no line unless it fails
(in which case it raises, same as every other `verify()` assertion).
