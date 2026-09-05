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

1. **Button caps assume a flat outer wall.** The real outer surface curves
   into the R10 shoulder fillet above z=15, and the button cap z-range
   (up to ~19.6–20.0mm) reaches into that curve. The generator treats the
   wall in the button region as the flat plane `x = -outer_radius` for the
   whole cap height. This keeps the mechanism's *dimensions* (hole
   clearance, plunger gap, pocket depth, tab gap) exactly matching
   SPEC.md, at the cost of a small (~1-2mm at the z extremes) contour
   mismatch against the actual shoulder curve — true up the cap's outer
   profile against the real shell surface before printing.
2. **USB tunnel liner assumes a flat +y wall** for the same reason (the
   liner's outer end is at the nominal `spine_b.y + outer_radius`, not the
   true domed end-cap surface).
3. **Comms bay footprints don't all fit at face value.** SPEC's/Jake's bay
   numbers (battery/stack/L76K x-extents, "cavity 52mm wide") are sized
   against a flat mid-height cross-section, but the actual cavity is a
   *revolve* of the shoulder profile around each spine endpoint. Near the
   −y end cap (battery and stack both live at y<0, inside the domed end)
   and near the floor (z≈2-3mm, close to the bottom shoulder curve), the
   real usable width is measurably less than the nominal figure. Building
   the requested boxes at face value punched straight through the outer
   shell (caught by an M1 probe regression during development: found
   26.01mm of "wall" at a point where the shell should have been 24.14mm).
   The generator now clips every bay box to a conservative safe envelope
   (`safe_half_width` / `clip_box_x_to_cavity`), or skips a piece
   entirely (falling back to just its corner pads) if it doesn't fit at
   all. This is a real layout tightness in the bay numbers, not just a
   generator bug — a mechanical pass should either raise the affected
   components off the floor, narrow their footprints, or open up the
   envelope near the −y end cap.
4. **Comms board occurrences (XIAO / Wio-SX1262 / L76K) are inserted but
   NOT positioned.** The display module's placement was read from an
   *existing* reference transform in "Firefly V2 v16"; the comms boards
   have no such reference (SPEC.md: "positions you choose"), so the
   generator inserts them fresh and attempts to place them. Every rigid
   transform approach tried proved unreliable specifically for these
   multi-level nested assemblies (`occ.transform` accepted a
   pure-translation matrix but the change didn't reliably show up in the
   occurrence's own body bounding boxes afterward; `moveFeatures` rejected
   an Occurrence as an input entity, and moving its constituent bodies
   failed because they belong to the referenced document's own component).
   Rather than risk silently-wrong placement, the generator leaves them at
   the source document's native (identity) placement and **hides** them
   (`isLightBulbOn = False`) so renders show the actual printable case.
   **A human pass in Fusion (drag in the browser tree, or Move/Align) is
   required to seat these three boards in the bay** before using their
   geometry for a real interference/clearance check. The 'hat' vs 'wired'
   L76K stack height itself (≈18.5mm vs ≈4mm) is reflected in where the
   generator *intends* to place them, once placement is fixed.
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
