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
| `export/<variant>/*.stl` | Per-body STL exports (binary). |
| `renders/<variant>_{front,top,right,iso}.png` | Orthographic screenshots. |

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

Screws, display module, screen plate posts/standoffs, USB tunnel, and
button switch positions are **unchanged** between variants (Jake: "keep
... at their reference positions") — only the shoulder/envelope and
lip/anchor/lug/bay numbers scale with `outer_radius`.

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

Both variants currently build with **zero real interference** and all M1+M2
checks passing.

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
   hidden) in the generated document. **Remaining rough edge:** the L76K
   assembly's own bounding box (likely including its antenna cable/lead)
   extends well past the small frame built for the bare board (observed y
   span ~49mm vs. the ~18mm frame) -- worth a visual check in Fusion; the
   frame itself is sized correctly for the board outline given in SPEC.md.
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

## Print orientation & settings

## Print orientation & settings

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
  print. Print all 4 flat, face-down, no supports needed. If the cap binds
  or rattles in the wall hole, adjust `PARAMS['cap_clearance']`
  (0.25mm default) and re-export.

## Screw list

| Screw | Qty | Joins |
|---|---|---|
| M2×12 socket head | 3 | Bottom bosses A/B/C → Top bosses (Ø1.62 pilot, z 10–19.1) |
| M2×10 socket head | 1 | Bottom boss D → Screen Plate post (Ø1.62, z 10–14.1) |
| M2×6 socket head | 4 | Top posts P1–P4 → Screen Plate (Ø1.62 pilot, z 14.1–20.6) |
| M2×4 socket head | 3 | Screen Plate → board SMT standoffs S1–S3 |

Bottom bosses A/B/C get a Ø4.5×2.2mm counterbore from z=0; boss D gets a
deeper Ø4.5×4.0mm counterbore (its screw tip must stay ≤ z=14.1 — the
USB-C shell sits at z=14.35 just above it).

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
   2026-09-05**: XIAO/Wio/L76K are now correctly placed (see the pass 2
   section above) via `occ.transform` + `design.snapshots.add()`. Remaining
   rough edge: the L76K assembly's own bounding box (likely including its
   antenna cable/lead) extends well past the small frame built for the
   bare board footprint -- worth a visual check in Fusion.
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
8. **Plunger guide rib plate slightly overshoots the outer wall** (~0.04mm,
   trim variant) at its `attach_margin` corners — negligible relative to
   print tolerance, but a future pass could clip the rib footprint to the
   actual local shell surface instead of a flat margin around the plunger
   axis, the same way `clip_box_x_to_cavity` does for the comms bay.

## Verify() output reference

Running `run()` prints, in order: variant, document name, timeline feature
count, the printed body names, each body's bounding box, the full M1 outer
and cavity probe tables (z, expected ρ, found ρ, pass/fail), interference
results, M2 dimensional/probe checks, and (with `export=True`) the STL
export paths and screenshot paths. A clean run ends with `OK: M1+M2 probes
passed`.
