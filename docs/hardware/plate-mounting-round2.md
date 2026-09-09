# Screen Plate mounting — round 2 (post-free candidates)

**Status: analysis + drawings only. No changes to `firefly_case.py`, params, exports, or
README. No PR.** Read-only work in a fresh worktree
(`/private/tmp/claude-501/case-analysis`, `origin/main` reset to `caf949c`). Continues
round 1 (`docs/hardware/plate-mounting-analysis.md`) after Jake's verdict on it:

> "The 4 screws in the corner are unhelpful, we cannot continue with that design. I need
> better candidate drawings too."

Round 1's own `#1` recommendation ("stiffen the plate + keep P1–P4, add one rib") is
**withdrawn** by that verdict — it kept the corner-cluster ceiling posts. This pass only
considers designs that **remove all four ceiling posts (P1–P4)** and re-derives every
number from `PARAMS` independently (`scratch/geom.py`, verbatim ports of
`rho_at_z`/`inner_rho_at_z`/`rho_from_spine`/`bottom_floor_z`/`plate_edge_x` — cross-checked
against round 1's numbers, matching to the mm; see "What I could not verify" at the end for
the one thing still open). No Fusion build of the *generator* was needed to re-derive the
geometry (pure Python, same functions the live gates use) — but a **live Fusion scratch
document was built** for the isometric renders (see §4), reusing the real generator via
`hardware/case/README.md`'s own `runpy` pattern, trim variant.

## 0. What's reused vs. new

Round 1's own geometric proof stands and isn't re-litigated: a literal N/S/E/W ceiling-post
spread is **impossible** here (window bore vs. GPS patch vs. true outer wall leave a hard
265.8°-wide "SW pocket" ceiling posts can exist in, `docs/hardware/plate-mounting-analysis.md`
§1). What's new this pass: four candidate designs that **do not use ceiling posts at all**,
each drawn as a full to-scale set (plan view, two section views, isometric Fusion render),
verified against real obstacles (`scratch/candidates.py`), and ranked with numbers.

---

## 1. Candidates

All four remove P1–P4 entirely. Assembly-order baseline they modify (was, since pass 9g):
display module into Top → Screen Plate onto P1–P4 → button caps → comms-bay hardware into
Bottom → GPS antenna + foam → compass module → halves joined → screws A/B1/B2/C, D,
(P1–P4), S1–S3.

### Candidate 1 — Screen Tray

The plate becomes a shallow tray: a downturned skirt on its north, east and west edges. On
closing, the skirt's bottom edge lands on Bottom's floor (west) or a shallow shelf near the
lip ring (east), so screws A/B1/B2/C and D clamp the tray up against the display from below
via the skirt, not via discrete ceiling posts. One part, prints face-down (flat pan on the
bed, skirt standing up — a plain 90° wall, self-supporting, no overhangs).

- Plan view: `docs/hardware/r2-plan-candidate1-tray.png`
- Sections (x=0 sagittal + transverse @ y=45): `docs/hardware/r2-section-candidate1-tray.png`
- Isometric (Fusion scratch doc, plate hidden to show the skirt runs against the real Bottom
  floor and battery box): `docs/hardware/r2-fusion-candidate1-tray.png`

**North tip is the weak spot.** The plan view shows why: the plate's north tip (the ~14mm
arc closest to the window/dome) can't take a skirt at all without colliding with the
display's own bbox / FPC relief right there — round-1's own §1.3 finding (window bore
almost concentric with the dome axis) applies again. That corner is left to S1/S3 alone
(the display's own M2 standoffs), which hold the plate *up* against the display but do
nothing to stop it flexing *down* under a drop.

**Real numbers** (`scratch/draw_section.py:cand1_transverse_feature`, `bottom_floor_z`):

| Run | x (skirt line) | Bottom floor z here | Skirt bottom target | Skirt drop (16.1→bottom) |
|---|---|---|---|---|
| West (y=45) | −24.10 | 4.82 | 5.32 (0.5mm above floor) | 10.78mm |
| East (y=45) | 21.29 | 2.71 | 3.21 (0.5mm above floor) | 12.89mm |

**Tolerance: 0.5mm short + 1mm adhesive foam strip, not a crush rib.** Reasoning: the skirt
is a *long, curved, thin* wall (not a stub post), so its printed height error compounds
with the plate's own bed-adhesion/warp tendency (PETG, noted as a real risk in round 1 §2)
along its whole run, not just at one point — a crush rib assumes a single predictable
contact patch, which a running wall along a curve doesn't give you. A soft interface
(foam) forgives the height error **and** the fact west/east runs land on floor of two
different heights (4.82 vs 2.71mm) without needing two different rib heights tuned by
hand. Expected print-height error over a ~11–13mm drop: **±0.16–0.19mm** (0.15mm/10mm ×
height, see §3), well inside a 1mm foam strip's compression range.

**Assembly order change:** none needed at the top level (tray still goes in before halves
close), but the tray must be held square against the display by S1–S3 alone *before* the
skirt lands (skirt only engages once Bottom and Top come together) — same "temporary
location, permanent support only on close" contract candidate 2/4 also need.

**What else must change:** `build_screen_plate` needs a new skirt-generation step (an
offset/thicken along the plate's own already-computed edge, `plate_edge_x`); Bottom gets no
new geometry on the west run (skirt lands on the existing floor) but may want a shallow
raised shelf on the east run so the foam strip has a defined seat rather than the bare
sloped fillet; `plate_z` stays a flat sheet, no thickness change forced by this candidate
alone (though §5's pick still recommends it).

### Candidate 2 — Full Sandwich

Bottom grows 4 short compression ribs (N, NE, NW, E) reaching from its own floor up to the
plate's underside; the plate goes to 1.6mm flat sheet (from 1.0mm). Genuinely spread across
the plate's real footprint, in **compression** when the case screws draw the halves shut —
PETG's best load mode, not bending.

- Plan view: `docs/hardware/r2-plan-candidate2-ribs.png`
- Sections (x=0 sagittal, rib N real since it sits at x=0 + transverse @ y=67 through rib
  N): `docs/hardware/r2-section-candidate2-ribs.png`
- Isometric (Fusion, plate hidden — ribs standing on the real Bottom floor, battery box in
  view): `docs/hardware/r2-fusion-candidate2-ribs.png`
- Exploded view (plate lifted 30mm to show it separating cleanly from the ribs/floor):
  `docs/hardware/r2-fusion-candidate2-exploded.png`

**Rib geometry** (`scratch/candidates.py`, floor z via `bottom_floor_z`, r=2.2mm cylinders):

| Rib | xy | Bottom floor z | Height (floor→plate z=16.1) | Nearest existing screw |
|---|---|---|---|---|
| N | (0.0, 67.0) | 2.00 | **14.10mm** | D @ 7.00mm |
| NE | (20.0, 66.0) | 5.38 | 10.72mm | S3 @ 8.42mm |
| NW | (−23.4, 58.0) | 4.95 | 11.15mm | S1 @ 13.38mm |
| E | (21.0, 36.0) | 2.58 | 13.52mm | S2 @ 21.30mm |

All 4 clear the battery, GPS chimney, and header cutout (checked live,
`scratch/candidates.py`: `battery=False gps=False header=False` for every rib).

**Tolerance and strength, with numbers.** A rib loaded in axial compression is a different
animal from the old cantilevered posts: axial stiffness `k = EA/L` (E=1800MPa PETG,
A=π·2.2²=15.2mm² for r=2.2mm) vs. the old worst-case cantilever bending stiffness
`k = 3EI/L³` (round-1's own NE-corner case, L=57.3mm, t=1mm, w=20mm → k≈0.048 N/mm,
δ≈3.14mm under the display's static 0.15N):

| Rib | L (mm) | k_axial (N/mm) | vs. old k_bend (0.048 N/mm) | δ under 14.7N drop load |
|---|---|---|---|---|
| N | 14.10 | 1941 | **40,600× stiffer** | 7.6 µm |
| NE | 10.72 | 2553 | 53,400× stiffer | 5.8 µm |
| NW | 11.15 | 2455 | 51,300× stiffer | 6.0 µm |
| E | 13.52 | 2024 | 42,300× stiffer | 7.3 µm |

Even the worst rib is ~40,000× stiffer under the same load than the old cantilever mode —
the drop-load deflection collapses from millimetres to single-digit micrometres. The
remaining *bending* deflection between ribs (the plate still spans real distance between
support points) also drops hard once the plate goes to 1.6mm: worst span is S2 at 21.30mm
from the nearest rib, `δ = P·L³/(3EI)` with I=w·t³/12: **0.039mm at 1.6mm thickness**, an
**80× improvement** over round-1's 3.14mm baseline.

**Height-tolerance risk is real and must be managed, same as round 1 flagged.** Expected
print height error (0.15mm per 10mm of print height, per the button-mechanism precedent):
rib N ±0.211mm, NE ±0.161mm, NW ±0.167mm, E ±0.203mm. A rib printed even 0.2mm tall could
preload the display against the window hard enough to matter before the case screws seat;
printed short, it does nothing. **Fix: print every rib 0.2mm short of nominal + a 0.5mm
adhesive foam pad on each tip** (same compliant-interface family as candidate 1's skirt —
deliberately unified across candidates so one print-test method covers both).

**What breaks / gap:** no rib on the plain west side (only NW, which is really "west of the
dome," not "west of the flat band") — S1 is the nearest support there at 13.38mm, tolerable
but the true west mid-span (y 30–50) is the one direction this candidate under-serves. This
is exactly what candidate 4 patches.

**Assembly order change:** display+plate must be held in XY by something *other* than the
ribs before the halves close (a few short non-load-bearing locating pegs/ridges on the
plate or the header cutout's own alignment), since the ribs only engage once Bottom and Top
come together — new step, not in the pass-9g baseline.

**What else must change:** new `PARAMS['plate_support_ribs']` (xy dict, same shape as
`top_posts`); a new `add_plate_support_ribs(root, bodies, p)` mirroring `add_root_reinforcement`'s
45°-collar self-supporting-root pattern but growing from Bottom's floor instead of Top's
ceiling; `plate_z` → (15.5, 17.1) (top face unchanged, same reasoning round 1 §3(f) already
worked out); `verify_post_walls`/`verify_plate_post_spread`/`verify_root_fillets` all
currently iterate `p['top_posts']` — either retarget them at the new ribs or add matching
`verify_*_ribs` gates; `check_interference` re-run against battery/GPS/header-cutout.

### Candidate 3 — Wall Ledges (bayonet)

Six 45° wedge ledges grow from Top's inner wall at plate height (z 16.1–17.1), spread W /
NW / N (×2) / NE / E — matching plate tabs. Plate is inserted from below and rotated
~10–15° to lock (bayonet); S1–S3 then stop reverse rotation. Printable face-down (Top's
existing orientation: flat ceiling face on the bed) with the bed-facing side as the 45°
face — the same self-supporting-45° precedent the file already uses for `add_root_reinforcement`'s
collar.

- Plan view: `docs/hardware/r2-plan-candidate3-ledges.png`
- Sections (x=0 sagittal, N1/N2 projected off-axis + transverse @ y=68.7 through N1/N2,
  both walls): `docs/hardware/r2-section-candidate3-ledges.png`
- Isometric (Fusion, plate visible — the 6 ledge tabs poking past the plate's own north
  edge): `docs/hardware/r2-fusion-candidate3-ledges.png`

**Ledge geometry** (`scratch/candidates.py`; reach = 3mm inward from the true wall,
z 16.1–17.1):

| Ledge | xy (anchor, on wall) | wall_gap (room behind it) | Nearest screw |
|---|---|---|---|
| W | (−24.5, 42.0) | 1.50mm | S1 @ 26.18mm |
| NW | (−21.9, 62.0) | 1.84mm | S1 @ 10.34mm |
| N1 | (−8.0, 68.7) | 7.30mm | S1 @ 5.45mm |
| N2 | (8.0, 68.7) | 7.30mm | S3 @ 4.84mm |
| NE | (20.0, 66.0) | 1.47mm | S3 @ 8.42mm |
| E | (24.5, 42.0) | 1.50mm | S2 @ 26.34mm |

**Widest spread of any candidate** — 6 points genuinely around N/NE/E/W/NW, the literal
"top / left / right" answer to Jake's complaint (no candidate gives south the same
treatment, but D + the existing A/B1/B2/C screws already anchor that end). **Tolerance is
an XY/engagement problem, not a height-stack problem** — no vertical member to
mis-print — so the 0.15mm/10mm height-error model doesn't apply here at all; the relevant
number is the **0.2mm engagement gap** between the wedge tip and the plate tab, which is
well inside typical FDM XY accuracy (~±0.1mm) and doesn't accumulate with print height the
way a rib does. **Stiffness** with plate staying at 1.0mm: worst span is S2 at 26.34mm from
the nearest ledge, `δ = P·L³/(3EI)` → **0.305mm**, a 10.3× improvement over round-1's
3.14mm baseline (less than candidate 2's 80× because the plate isn't thickened and the
worst span is longer — S2 sits south of where the ledges cluster).

**What breaks:** assembly gets a real new step (insert-and-rotate) that the pass-9g order
doesn't have; the brief's own flagged risk applies directly — rotating the plate under 6
ledges must clear the header cutout, the battery-plug window, and the FPC relief through
the whole rotation arc, not just at the final resting position. This needs a live
interference sweep across the rotation path (not just at 0° and final angle) before
cutting real code — flagged as open verification, not assumed clear here.

**Assembly order change (numbered):**
1. Display module into Top (unchanged).
2. Screen Plate inserted from below at a rotated offset angle, rotated into the 6 ledges
   (bayonet lock).
3. S1–S3 driven — these now double as **rotation stops**, not just display retention.
4. Button caps, comms-bay hardware, GPS antenna, compass module (unchanged).
5. Halves joined, screws A/B1/B2/C + D driven (no P1–P4 step — removed).

**What else must change:** new `PARAMS['plate_wall_ledges']` (6-entry xy dict);
`add_plate_wall_ledges` reusing `add_lanyard_corner_block`'s wall-anchored-boss pattern
(that function already proves a boss can be grown from Top's wall with a real root, just
currently only at the lanyard end); plate outline gains 6 tab cutouts/notches sized to the
ledges; a new `verify_ledge_rotation_clearance` gate (interference-checks the plate through
the actual insert-and-rotate sweep, not just the final position — this is the one gate that
doesn't have a close analogue elsewhere in the file yet).

### Candidate 4 — Hybrid (N ledges + E/W ribs)

Two of candidate 3's ledges (N1, N2 — the pair actually near the window, where round-1's
own §1.3 proved the ceiling ring is real but thin) plus two of candidate 2's ribs (E, W —
mirrored at (21,36)/(−21,36), both floor z=2.58). No rotation needed: N1/N2 are a simple
**slide-under** hook (plate's north edge slides under 2 tabs, no bayonet twist), so this
keeps candidate 3's XY-only tolerance story for the north while getting candidate 2's
compression stiffness on the true east/west mid-span — the one direction candidate 2 alone
under-serves.

- Plan view: `docs/hardware/r2-plan-candidate4-hybrid.png`
- Sections (x=0 sagittal, all 4 features projected off-axis + transverse @ y=36 through
  rib E/W): `docs/hardware/r2-section-candidate4-hybrid.png`
- Isometric (Fusion, plate visible — N1/N2 tabs visible past the plate's north edge; rib
  E/W are under the plate in this view, same as they are in candidate 2's own iso render):
  `docs/hardware/r2-fusion-candidate4-hybrid.png`

**Real numbers** (same underlying data as candidates 2/3, recombined):

| Point | Nearest support | Distance |
|---|---|---|
| Screw D | N1/N2 | 11.82mm |
| S1 | N1 | 5.45mm |
| S2 | rib E/W | 21.30mm |
| S3 | N2 | 4.84mm |
| Plate NE corner | rib E | 14.92mm |
| Plate NW corner | rib W | 18.65mm |
| Plate SE corner | rib E | 7.44mm |
| Window centre | N1/N2 | 20.34mm |

Worst span (S2, 21.30mm) matches candidate 2's own worst span exactly (same rib E/W
positions) → same **0.039mm @ 1.6mm plate** stiffness number and the same 40,000×-class
axial-vs-bending improvement for the 2 ribs. The 2 ledges carry the north tip that
candidate 1 left to S1/S3 alone and candidate 2 left thin (NW's 1.84mm wall_gap vs. N1/N2's
7.30mm) — **this is the only candidate that gives the north tip a real mechanical support
point that isn't also a display-retention screw.**

**Assembly order (numbered):**
1. Display module into Top.
2. Screen Plate slid in from below, north edge under N1/N2 (no rotation — straight
   insertion, tabs catch on the plate's own north-edge notches).
3. S1–S3 driven (display retention only here, not a rotation stop — simpler than
   candidate 3 alone).
4. Button caps, comms-bay hardware, GPS antenna, compass module.
5. Halves joined — rib E/W only engage now, compressing onto the plate's underside as
   A/B1/B2/C + D draw the halves shut (no P1–P4).

**What else must change:** smallest parts list of the three real candidates —
`PARAMS['plate_wall_ledges']` (2 entries, not 6), `PARAMS['plate_support_ribs']` (2 entries,
not 4), `plate_z`→(15.5,17.1), plate gets 2 north notches (not 6) + no header/skirt
changes; `add_plate_wall_ledges` + `add_plate_support_ribs` (both reused from candidates
2/3's own new functions — no third geometry function needed); gates: `verify_ledge_rotation_clearance`
is **not needed** here (no rotation — a simpler `verify_ledge_slide_clearance`, straight-line
insertion only, suffices), plus the same rib-height/interference gates as candidate 2.

**Printability:** identical to candidates 2 and 3 individually — ribs are vertical
compression columns on Bottom's own bed-side floor (self-supporting, no overhang, same
precedent as the existing corner blocks); ledges are 45°-faced wall bosses on Top's
existing face-down print orientation (same precedent). Nothing new to verify on the print
side beyond what candidates 2/3 already established.

---

## 2. Ranked shortlist

| # | Candidate | Worst-span δ | New Bottom ribs | New Top ledges | Assembly complexity | Verdict |
|---|---|---|---|---|---|---|
| **1** | **4 — Hybrid** | 0.039mm (@1.6mm plate) | 2 | 2 (slide, no rotation) | Lowest of the 3 real fixes | **Recommended** |
| 2 | 2 — Full Sandwich | 0.039mm (@1.6mm plate) | 4 | 0 | Low (temp XY location before close) | Best raw stiffness, but west mid-span thin (NW only) and 4 height-critical ribs vs. hybrid's 2 |
| 3 | 3 — Wall Ledges | 0.305mm (@1.0mm plate) | 0 | 6 | Highest (bayonet rotation + rotation-path interference sweep still unverified) | Best if height-tolerance risk is the #1 fear (zero height-critical members) — but widest untested assembly risk |
| 4 | 1 — Screen Tray | n/a (continuous skirt, not point-spans) | n/a | n/a | Low (1 part) | North tip still effectively unsupported except by S1/S3; foam-strip fix is a compliance band-aid, least uniform of the four |

**#1 — Candidate 4 (Hybrid).** It answers "spread it across top/left/right/bottom" more
literally than any single-mechanism candidate: north via 2 simple slide-under ledges
(cheapest possible fix for the thinnest part of the ceiling ring, no rotation risk), east
and west via 2 compression ribs (PETG's best load mode, ~40,000× stiffer than the old
cantilever mode under drop load). It carries **half** the height-tolerance-critical members
of candidate 2 (2 ribs, not 4) and **none** of candidate 3's unresolved rotation-path
interference risk, while matching candidate 2's own best-case stiffness number exactly
(same rib positions/spans). Candidate 2 is the strong #2 if Jake would rather have one
mechanism family (all ribs, no ledges) even at the cost of a thinner west side and twice
the ribs to tolerance-manage. Candidate 3 is worth keeping in reserve specifically **if**
the rib height-tolerance risk turns out to bite in practice (it has zero vertical members
to mis-print) — but its rotation-path clearance needs a live Fusion interference sweep
before it's trusted, which this pass didn't do (see below). Candidate 1 is the easiest
single-part change but leaves the actual complaint (north tip, "doesn't give proper
support") the least resolved of the four.

### Concrete `firefly_case.py` / `PARAMS` changes for #1 (Hybrid)

- Remove `PARAMS['top_posts']` (P1–P4) and the `add_top_posts` call in the build order
  (`firefly_case.py` ~line 5074); remove/retarget the 3 gates that iterate
  `p['top_posts']`: `verify_post_walls`, `verify_plate_post_spread`, `verify_root_fillets`.
- New `PARAMS['plate_wall_ledges'] = {'N1': (-8.0, 68.7), 'N2': (8.0, 68.7)}` — Top-side,
  z=`plate_z` band, 45° wedge, 3mm reach, 0.2mm engagement gap to the plate's own north-edge
  notches.
- New `PARAMS['plate_support_ribs'] = {'E': (21.0, 36.0), 'W': (-21.0, 36.0)}` — Bottom-side,
  r=2.2mm cylinders, floor→`plate_z[0]` minus 0.2mm (compliant allowance), r=2.2mm.
- `PARAMS['plate_z']`: `(16.1, 17.1)` → `(15.5, 17.1)` (trim; top face fixed since S1–S3/D
  key off it, same derivation `plate_post_D_z = (split_z, plate_z[0])` already
  auto-recomputes).
- New `add_plate_wall_ledges(root, bodies, p, clip_tool=None)` — reuse
  `add_lanyard_corner_block`'s wall-boss-with-real-root pattern (currently lanyard-end
  only).
- New `add_plate_support_ribs(root, bodies, p)` — reuse `add_root_reinforcement`'s
  45°-self-supporting-collar pattern, rooted on Bottom's floor instead of Top's ceiling.
- Plate outline (`build_screen_plate`): 2 small notches at the north edge for the N1/N2
  tabs; no other outline change.
- New gates: `verify_ledge_slide_clearance` (straight-line insertion sweep, not full
  rotation — simpler than candidate 3's version), `verify_rib_contact` (rib-top-face vs.
  plate-underside real point-containment, not a nominal-dimension assert), extend
  `check_interference` to the 2 ribs + 2 ledges against battery/GPS/header-cutout/S-screws.
- New assembly-order documentation (README "Print orientation / notes" + build-sheet):
  the 5-step order in Candidate 4's write-up above, replacing the pass-9g baseline's
  P1–P4 step.

### Coupon to print first

A single small coupon combining **both** new mechanisms, matching the file's own
"print the mechanism in isolation" discipline (`export/coupons/`):

- A short arc of Top's north wall carrying **one** ledge (N2, the better-documented of the
  pair) plus its 45° root, oriented print-face-down exactly as it prints in the full case.
- A short square of Bottom's floor carrying **one** rib (E, the tallest of the two hybrid
  ribs at 13.52mm — the more height-tolerance-exposed of the pair) with its compliant tip
  allowance (printed 0.2mm short, foam pad added by hand).
- A matching plate corner fragment (north-edge notch for the ledge + the header-side edge
  near the rib) to test both engagements together.

Print this coupon first, check (a) the ledge's 0.2mm slide-engagement by hand-fitting the
plate fragment, and (b) the rib's actual printed height against the 13.52mm nominal (with
calipers) before committing to a full 6+ hour case print with both features baked in — the
same order of operations the button-mechanism coupons already established for
`plunger_travel`/`plunger_pretravel`.

---

## 3. Printability summary (all candidates)

| Feature | Body | Orientation | Overhang risk |
|---|---|---|---|
| Skirt (candidate 1) | Screen Plate | Face-down (existing), skirt grows up | None — plain 90° wall from a flat base |
| Ribs (candidates 2, 4) | Bottom | Face-down on flat back (existing) | None — vertical columns growing away from the bed, same precedent as the existing corner blocks (`add_root_reinforcement`'s "plain vertical extrusion... needs no more support than the bosses it replaces") |
| Wall ledges (candidates 3, 4) | Top | Face-down on flat ceiling (existing) | None if the 45° face is oriented toward the bed side — same self-supporting-45° precedent `add_root_reinforcement`'s collar already uses |

No candidate changes either half's print orientation. No candidate needs supports if the
45°/90° orientations above are respected.

---

## 4. Fusion scratch document

Built via `hardware/case/README.md`'s own pattern (`runpy.run_path(...); g['run'](_context)`),
trim variant, in a **new, unsaved** `Firefly Case Gen 15b-trim ...` document — never opened,
modified, or saved over any of Jake's real documents ("Firefly V2 v16", the board
reference models, etc. were all confirmed still `isModified: false` throughout). All 4
candidates' mock solids (17 total: 5 ribs as simple cylinders — N/NE/NW/E/W, 6 ledges as
simple boxes, 6 tray-skirt segments as simple boxes) were added as a single new component
(`R2_Mocks`) via `TemporaryBRepManager` primitives (no sketches/gates/exports), then shown
one candidate's set at a time via body visibility for each render. The exploded view
(candidate 2) used a real `MoveFeatures` translation (+30mm Z on the Screen Plate body),
then undone (`fusion_mcp_update` undo) to restore the document. Renders were written
directly to disk with `viewport.saveAsImageFile(...)`, the same call the generator's own
`take_orthographic_screenshots` uses.

---

## What I could not verify

- **Candidate 3's rotation-path interference** (plate through the actual insert-and-rotate
  sweep against header cutout / battery-plug window / FPC relief) was not checked live —
  flagged explicitly in §1's candidate-3 write-up and in the ranked shortlist as the reason
  it isn't the top pick despite its wider spread.
- **The Fusion mock solids are illustrative primitives** (cylinders/boxes), not the real
  45°-wedge/compliant-tip geometry — the plan and section drawings carry the precise
  geometry; the Fusion renders are for spatial context (relative to the real Bottom, Top,
  Screen Plate, battery box, GPS chimney, and the actual inserted display module) only.
- **PETG modulus (1800MPa) and the 0.15mm/10mm print-height-error figure are carried over
  from round 1** (literature/precedent values, not measured on Jake's actual filament/
  printer this pass).
- **No new print-test was run this pass** — the coupon plan in §2 is the recommended next
  step, not yet executed.

## Diagram files

- `docs/hardware/r2-plan-candidate1-tray.png`, `r2-plan-candidate2-ribs.png`,
  `r2-plan-candidate3-ledges.png`, `r2-plan-candidate4-hybrid.png`
- `docs/hardware/r2-section-candidate1-tray.png`, `r2-section-candidate2-ribs.png`,
  `r2-section-candidate3-ledges.png`, `r2-section-candidate4-hybrid.png`
- `docs/hardware/r2-fusion-candidate1-tray.png`, `r2-fusion-candidate2-ribs.png`,
  `r2-fusion-candidate2-exploded.png`, `r2-fusion-candidate3-ledges.png`,
  `r2-fusion-candidate4-hybrid.png`
- Round-1 diagrams, still valid background: `docs/hardware/plate-mounting-geometry.png`,
  `plate-mounting-section.png`, `plate-mounting-candidates.png`
