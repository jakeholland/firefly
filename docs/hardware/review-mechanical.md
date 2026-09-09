# Firefly V2 case — mechanical design review

**Reviewer role**: independent mechanical review (fasteners, load paths, tolerance stacks,
compliant features, serviceability). Read-only — no changes to `firefly_case.py`, params,
exports, or README. Read-only worktree: `/private/tmp/claude-501/review-mech`,
`origin/main` @ `51c5d16` (fetched fresh; no later commits at review time).

**What I read**: `hardware/case/README.md` passes 9–15b + Screw list + Known limitations,
`hardware/case/firefly_case.py` (button mechanism, corner blocks, lip/anchor ring, battery/
comms/GPS/mag bays, window, lug), `hardware/case/params_current.py` / `params_trim.py`,
`docs/hardware/comms-brain.md`, and the coordinator's prior analysis
(`plate-mounting-analysis.md`, `plate-mounting-round2.md`, and the `r2-*.png` drawings,
including `r2-plan-candidate5-ears.png`). No Fusion build was needed — every number below is
either quoted from the README's own live-Fusion-probed history or independently recomputed
in pure Python against the real `PARAMS` (verbatim ports of the same small functions the
round-1/round-2 analyses used), which I ran directly to cross-check the candidate-5 plan
drawing's own annotations (see §3).

Trim is the shipping default and is used for all numbers unless noted; current-variant
numbers are given where they differ materially (current always has 2mm more radial margin
almost everywhere, per its own wider `outer_radius`).

---

## Prioritised list

| Priority | # | Finding |
|---|---|---|
| **Blocker** | F5 | Candidate 5 removes the plate's compliance layer; 3 rigid standoff seats vs. 1 continuous window seat is a real over-constraint with no engineered compliance path today |
| **Blocker** | F6 | Crossbar clashes with the battery-connector clearance zone in **both XY and Z** — a real geometric interference, not a tight-clearance nicety |
| Should-fix | F1 | Lanyard-end closure screw count (4) is far beyond what pull-out strength requires; wall-hugging integration is half-done already (pass 14) |
| Should-fix | F7 | D1/D2 + ear roots need the same proven wedge/collar pattern the corner blocks already use — don't hand-roll a new construction |
| Should-fix | F3 | Lip/anchor ring has no lead-in chamfer on its actual insertion edge (the existing pass-9 chamfer is a different, printability-only edge) |
| Should-fix | F4 | Button mechanism is over-engineered relative to what it needs to guarantee — 8+ independently-tuned clearances per button, several found only by live-Fusion accident across 5 passes |
| Should-fix | F9 | No compliant liner/chamfer between the glass edge and the window's sharp PETG step |
| Should-fix | F10 | XIAO/Wio are retained only by their own board-to-board connectors, cantilevered off the one board (L76K) the case actually holds |
| Nice | F2 | Corner blocks already answer half of Jake's "integrate into the sides" ask — confirm to Jake, don't re-invent |
| Nice | F8 | Battery-plug plate window becomes moot under candidate 5 (no plate) — genuine simplification, contingent on fixing F6 |
| Nice | F11 | LoRa FPC antenna keep-out is a reference marker only, never enforced against the real cable path |
| Nice | F12 | Compass module retention depends on an unmodeled foam pad — already documented, just flagging it stays a real BOM/assembly item |
| Nice | F13 | Battery bay has no positive Y end-stop, only rail + strap friction |
| Watch | F14 | Ear S1's north edge may sit uncomfortably close to the FPC relief pocket's own south edge — can't be confirmed without the real ear geometry, flagging for the modeling pass's own gates |

---

## 1. Screw count and wall integration (Jake's observation 1)

**Feature / generator location**: `add_case_screws` (`firefly_case.py:1903`), which calls
`add_case_boss` (`:1798`) for A/B1/B2/C and D, then `add_lanyard_corner_block` (`:1611`) for
the Top-side halves of A+B1 and C+B2. Screw geometry: `PARAMS['boss_dia']=6.0`,
`top_pilot_dia=1.62` (M2 pilot), `top_pilot_z=(10.0, 19.1)` → **9.1mm of parting-plane-anchored
engagement**, independent of case height. Positions (absolute mm, both variants):
A(−15.5,−8.0), B1(−12.5,−15.0), B2(12.5,−15.0), C(15.5,−8.0), D(0,60).

### F1 — should-fix: 4 lanyard-end screws are well past what pull-out needs

**Pull-out, printed PETG M2 pilot, 9.1mm engagement** (conservative thread-shear model,
`A = π·d_pilot·L`, PETG effective shear strength 15–20 MPa for a printed thread — a standard
conservative de-rate off ~50MPa tensile):

```
A = π × 1.62mm × 9.1mm = 46.3 mm²
F_pullout ≈ 46.3 × 15..20 MPa = 695 .. 926 N   per screw
```

Compare to the loads actually on the table:
- Lanyard tug: README's own pass-15 hand calc puts a firm human yank at 50–150N, and the
  lug root itself (the thing actually taking that load) fails around ~3500N by the same
  method — the lug, not the case screws, is already the load path for this.
- Drop-transmitted load: round-1's own 100g/15g-display estimate is ~15N total, spread
  across whichever fasteners are near the impact — single-digit-to-low-tens of newtons per
  screw in the worst case.

695–926N of pull-out per screw against a ~150N worst-case tug means **each individual screw
already has a >4× margin even if it alone had to react the entire tug** — four of them in a
14mm-wide cluster is not buying meaningfully more strength, it's buying redundancy against a
single screw stripping, which is a real but secondary concern next to the fact that A and B1
(and C and B2) are **only 7.6mm apart, center to center** — `math.hypot(15.5-12.5, 15-8) =
7.62mm`. Pass 14 already merged each pair into one solid corner block for exactly this
reason (it recognised they're really one structural zone, not two independent ones). Two
screws 7.6mm apart inside the same solid block are not "spread across the side of the case"
in any load-path sense — they're two fasteners doing the job of one anchor point.

**What the ring actually needs from the screws** (see §2) is *even clamping pressure to keep
the lip/anchor tongue seated*, not raw pull-out strength — the continuous ~230mm perimeter
tongue is what resists shear/racking between two widely-spaced clamp points, the same way a
gasketed lid only needs a few strong bolts, not one every few mm, once a continuous seal/lap
joint is doing the alignment work.

**Recommendation**: evaluate dropping to **one screw per lanyard-end corner block** (keep
whichever of the pair sits closer to the case's true wall for the best root — B1/B2, since
they're already the ones the pass-14 wedge reaches furthest into real wall material for) plus
**D1/D2 at the dome end** (candidate 5, §3) — **4 total case-closure screws**, down from the
current 5 (A/B1/B2/C/D) or the 6 that a naive D→D1/D2 split alone would give. This frees real
interior volume right where Jake wants it (the corner-block wedge width, `CORNER_BLOCK_PAD` +
`CORNER_BLOCK_REACH`, currently claims the same footprint at the exact XY the comms stack
wants — see `CORNER_BLOCK_STACK_MARGIN`'s own 0.8mm keep-out cut against the L76K PCB, which
had to be added specifically because the two-screw block reaches into that space today).
Confirm with a live `check_interference`/pull-out sanity check before cutting real screws
from the BOM — this is a geometry/statics argument, not a live-probed one.

### F2 — nice: the wall-hugging integration Jake asked for is already half-built

Jake's ask — "more integrated into the sides of the case, maybe similar to the top ears" —
is **already the pass-14 corner-block design**: `add_lanyard_corner_block` builds exactly a
wall-anchored wedge/gusset (capsule between the two screw centres + an oversized wedge
reaching to the true dome wall, clipped by `clip_to_inner_cavity` and the lip/anchor ring),
not two free-standing posts with a web. Worth saying explicitly back to Jake: the "posts with
a web" version he's reacting to may be an out-of-date mental model from before pass 14 landed
— the open question is screw *count* (F1), not the wall-integration pattern, which is done
and proven (0 interference, both variants, per the pass-14 `verify()` output).

---

## 2. The lip/anchor ring (Jake's observation 2)

**Feature / generator location**: `add_lip_anchor_reliefs` (`firefly_case.py:800`).
Trim: `lip_r=(23.95, 25.75)`, `anchor_r=(23.95, 26.40)`, `lip_z=(9.2, 10.0)`,
`anchor_z=(10.0, 11.0)` — a two-step stadium ring, radial width 1.80mm (lip) / 2.45mm
(anchor), spanning the full ~230mm perimeter, joined into **Top only**.

### What it actually does

Reading the code: the ring is built and joined into Top; there is **no matching pocket or
step cut into Bottom** — Bottom's own inner cavity at z 9.2–11 is just the ordinary open
cavity from `hollow_and_split`. So on assembly the ring is a **tongue projecting down from
Top into Bottom's open interior**, its OD sitting close to Bottom's own inner wall (the
"0.25mm nesting clearance" the comments reference) for the ring's entire perimeter.

This means the ring's real job is **self-centering (XY alignment) plus continuous
shear/racking resistance between the two halves** — a full-perimeter lap joint is much
stiffer against the two halves sliding sideways relative to each other (a drop landing
off-axis) than 5–6 discrete screws alone would be, and it removes the burden of XY alignment
from the screws entirely (they only need to pull axially, not also locate the parts). It is
**not** a primary axial-clamping load path — that is the screws' job (A/B1/B2/C/D or
D1/D2). This directly informs F1: because the ring already carries shear/alignment
continuously, the screws can be sized for clamping pressure and pull-out margin (which they
have in huge excess, §1), not for resisting bending/racking on their own — supporting a
lower screw count.

### F3 — should-fix: no lead-in chamfer on the actual insertion edge

Two different chamfers already exist in this area and it's easy to conflate them:

1. **Pass-9 finding 6** (`lip_ring_seam_chamfer = 0.5mm`, applied via
   `chamfer_stadium_edge_at` at `anchor_r[1]`, `z=anchor_z[0]=10.0`) — this bevels the
   **outer step** between the lip and the wider anchor band, purely so that downward-facing
   shelf doesn't need print support when Top prints face-down. It is real and correct, but
   it is **not** on the ring's insertion path.
2. **The ring's own leading edge** — the bottom-most rim of the lip, at `z=lip_z[0]=9.2`,
   `r=lip_r[1]=25.75` — is the edge that actually has to slide into Bottom's cavity and past
   Bottom's own inner wall during assembly. **This edge has no chamfer at all today** — it's
   a sharp 90° corner.

From a mechanical standpoint (not printability, which the other reviewer covers): a sharp
tongue edge sliding into a close-clearance (0.25mm nominal) mating wall, drawn home by
5–6 screws that won't all be started perfectly square, is exactly the geometry that catches,
scrapes a witness mark into the mating wall, or locally stress-risers the tongue tip if the
assembler has to force one corner home before the others. **Recommend a genuine 45°
mechanical lead-in chamfer, ~0.5–0.8mm, on the lip's own bottom OD edge** (z=9.2, r=25.75) —
distinct from and in addition to item 1's existing print chamfer. This is Jake's actual ask
("does it need the 45° chamfer... from a mechanical standpoint") and the honest answer is
**yes, but not the one that already exists** — the existing chamfer solves a different
problem.

---

## 3. Candidate 5 — case-integrated display mount (dedicated section)

Geometry reviewed: two ears grown from Top's dome wall at the display's own standoffs S1
(−12, 65) and S3 (11.6, 65.46); a crossbar wall-to-wall at the S2 line (y 29–34.5), z
15.5–18.5, under the PCB; display screws M2×4 up into its own standoffs from below;
screw D (0,60) retired, replaced by D1/D2 near (±19, 64) into the ear roots, Bottom's
counterbores moved to match. No Screen Plate.

I independently re-derived the profile math this file's own live gates use
(`_profile_geometry`/`rho_at_z`/`_inner_profile_geometry`) and ran it against the real
`params_trim.py`/`params_current.py` to check the plan drawing's own annotations. Key
numbers below are freshly computed, not copied from the drawing.

### 3.1 Where the ears/crossbar actually sit, geometrically

- `spine_b` = (0, 51.8) trim / (0, 50.0) current. S1/S3 (y=65/65.46) and D1/D2 (y=64) are
  **inside the +y dome cap** (past `spine_b`), not the straight band — the profile there is
  the same `_profile_geometry` revolved around `spine_b`, not the straight-band extrusion.
- At z 15.5–18.5 (the ear/crossbar band), that revolved profile is still in its **vertical
  "waist" region** (`top_fillet_center_z` = 18.0 trim / 15.0 current is the top of the
  waist) — the true wall there is a plain 2mm cylindrical shell at `inner_wall_rho` =
  `outer_radius - wall` = **26.0mm trim / 28.0mm current**, not a curving fillet. This is
  good news: unlike the corner blocks (which reach the *ceiling*, deep in the R8 fillet
  curvature — the reason B1/B2 needed the unconditional root collar to cover one otherwise
  hollow angle), the ears/crossbar reach a **flat cylindrical wall**, a simpler target.
- `rho_from_spine_b` for the mount points (trim / current):
  D1/D2 (±19, 64): **22.58mm / 23.60mm** — reach to the true wall (26/28) is **3.42mm /
  4.40mm**.
  S1 (−12, 65): **17.84mm / 19.21mm** — reach to the true wall is **8.16mm / 8.79mm**.
  S3 (11.6, 65.46): **17.92mm / 19.33mm** — reach **8.08mm / 8.67mm**.

All of these reaches are comfortably inside the corner blocks' own proven
`CORNER_BLOCK_REACH = 10.0mm` oversized wedge — i.e. **this is a smaller, easier version of a
problem this file has already solved once.** F7 below is the concrete recommendation that
follows from this.

### F7 — should-fix: reuse the corner-block pattern verbatim for D1/D2 and the ear roots

Don't hand-roll new wedge/gusset math for candidate 5. `add_lanyard_corner_block`
(`firefly_case.py:1611`) already is: a stadium capsule at the boss/root radius, an oversized
outward wedge (found live via a dot product, not assumed) reaching toward the true wall,
clipped by `clip_to_inner_cavity` plus a ring-clearance cylinder, plus two full-height
unclipped cores (`BOSS_CORE_R`) guaranteeing real material top-to-bottom regardless of local
cavity shape, plus the unconditional `add_root_reinforcement` collar (`ROOT_COLLAR_RISE =
1.5mm`) at both ends. Every one of those defensive layers exists because an earlier,
simpler version of the exact same "wall-anchored boss reaching inward" problem broke
live at least once (§ pass 14's own two "keep-out" rounds). Generalizing this same
function (or a thin wrapper around it) to D1/D2 + the two ear roots is the single biggest
risk-reduction move available for this pass — it inherits five passes' worth of already-paid
tuition instead of repeating it.

**Concrete geometry**:
- **D1/D2 pilots**: reuse `top_pilot_dia = 1.62mm`, `top_pilot_z = (10.0, 19.1)` unchanged —
  the same 9.1mm parting-plane-anchored engagement A/B1/B2/C already use, M2×12. Pull-out
  is the same 695–926N calculated in §1 — no new number needed, this is proven-adequate
  geometry, just relocated.
- **Ear root size**: root capsule radius = `boss_dia/2` (3.0mm, matching every other boss in
  the file) at the D1/D2 screw centres; the ear's own reach toward S1/S3 (8.0–8.8mm, see
  above) should get the same unconditional collar reinforcement
  (`add_root_reinforcement`, `ROOT_COLLAR_RISE=1.5mm`) applied at both the wall-root end and
  the standoff end — cheap, and this file's own history (pass 13/14) shows a plain capsule
  alone is not reliably sufficient near any wall/ceiling transition without live-probing
  first, and there's no reason to assume this one is the exception.
- **Bar (crossbar) section**: as specified, 5.5mm (y) × 3.0mm (z), spanning wall-to-wall
  (~44mm trim / ~48mm current). Treated as a fixed-fixed beam (anchored into real wall at
  both ends, like the corner blocks, not a cantilever), `I = w·h³/12 = 5.5×27/12 = 12.4mm⁴`.
  Under a 14.7N worst-case drop load applied at midspan (S2 sits almost exactly at the
  bar's own centre, x=0.04): `δ = PL³/(192EI)` (fixed-fixed, centre load) `= 14.7×44³/
  (192×1800×12.4) ≈ 0.29mm` — an order of magnitude stiffer than the old cantilevered
  Screen-Plate posts (round-1's own 3.1mm estimate at the worst corner), consistent with
  round-2's own "ribs/wall-anchored members are PETG's best load mode" conclusion. **This
  bar is structurally fine as specified** — the problem with it is not stiffness, it's
  routing (F6, next).

### F6 — blocker: crossbar clashes with the battery connector in both XY and Z

I re-ran `battery_connector_window()`'s own formula (verbatim, from `firefly_case.py:2069`)
against both variants' real `PARAMS`:

```
battery_connector_window(trim)   = x (-18.82, -2.36), y (31.30, 42.50)
battery_connector_window(current) = same (x/y are variant-independent inputs)
```

Against the candidate-5 crossbar's stated band (y 29–34.5):

```
Y-range overlap = min(42.50, 34.5) - max(31.30, 29.0) = 3.2mm
```

This is **larger** than the ~1.7mm the candidate-5 drawing flagged for verification — my
number uses the full pass-15 battery-window extents (including `BATTERY_CONNECTOR_TOP_EXTRA
= 3.0mm`, the finger-access allowance added specifically so a person can grip the plug). The
X ranges overlap unconditionally too, since the crossbar runs wall-to-wall and the window's
X-span (−18.82 to −2.36) sits well inside that.

**It gets worse in Z, which the plan drawing (a top-view) can't show**: the battery
connector's own world bbox z-range is **17.2–20.6mm (trim)** / **14.2–17.6mm (current, base
frame)** (`params_current.py`'s `battery_connector_bbox`, offset by `display_z_offset`). The
crossbar's stated z-range is **15.5–18.5mm**. Trim: crossbar top (18.5) laps **1.3mm** into
the connector's bottom (17.2). Current: crossbar (15.5–18.5) fully **overlaps** the
connector's whole z-range (14.2–17.6). This is not a "verify it" flag, it is a **real,
already-quantifiable interference** between the crossbar as specified and the battery
connector's own physical envelope, before any Fusion build is even attempted.

**Recommendation**: don't just shrink the margin — reroute. The existing
`CORNER_BLOCK_STACK_MARGIN` pattern (a keep-out box cut through the **full height** of a
wall-anchored member, sized off a real component footprint + margin, generic over every
screw in a loop) is the exact right tool here: cut the crossbar with a keep-out matching
`battery_connector_window()`'s own XY footprint × the connector's real z-range + a margin,
the same way the corner blocks already fork around the L76K PCB. Concretely: split the
single wall-to-wall bar into **two shorter bar segments** either side of the connector's X
extent (roughly x < −19 and x > −2, using the window's own −18.82/−2.36 edges with margin),
each still reaching from a wall (or from an ear) in to S2, rather than one continuous beam
straight through the connector. This costs some of the F7 stiffness number above (worst-span
math would need re-deriving once the real split is chosen) but a broken beam is strictly
better than a beam that physically occupies the same volume as the plug someone needs to
grip.

**Also found, independently, while checking this**: the crossbar's stated y0=29.0 leaves only
**0.45mm** of clearance to the GPS frame's own outer wall (`gps_frame` opening centred at
y=14.5, half-width 12.75, `+ gps_frame_clear(0.3) + gps_frame_wall(1.0)` → outer edge at
y≈28.55, both variants — the GPS patch's XY footprint doesn't scale with `outer_radius`, so
this number is identical trim/current). **0.45mm is below every other minimum-clearance
convention already established in this file** (`CORNER_BLOCK_RING_CLEARANCE=0.5`,
`wall_clear=0.6` used throughout `add_lip_anchor_reliefs`/`add_lug`, `MIN_RELIEF_CLEARANCE=
1.0`). Recommend nudging the crossbar's y0 north by at least 0.5–1.0mm (to ≥29.5, ideally
≥30.0) to bring this in line with the file's own house standard, independent of the F6 fix.

### F5 — blocker: no engineered compliance for the new 3-standoff + window-seat stack

This is the structurally important question the brief asked me to focus on, and it's real.

**Today** (with the plate): the display glass rests on the window's continuous ceiling-step
seat, and the board is held down by S1–S3 into the Screen Plate, which is itself held by
4 ceiling posts + D. The **plate itself is the compliance element** — a thin, not-perfectly-
rigid sheet held at multiple points can absorb a few tenths of a millimetre of Z mismatch
between "where the window seat says the glass should sit" and "where S1–S3's own standoff
heights say the board should sit," by flexing slightly. Round-1's own deflection estimate
(3.1mm cantilever at the worst corner under just static load) is a *liability* for support,
but it is also, incidentally, the reason the system has never needed to worry about this
particular over-constraint — the plate was floppy enough to not fight the window seat.

**Under candidate 5**: the plate is gone. S1, S2, and S3 now screw directly into **rigid,
case-integral** ears/crossbar. The display board now has **4 independent rigid Z references**
fighting for the same real estate: the window's continuous seat (1 constraint, effectively
infinitely stiff — solid PETG against glass) plus 3 discrete standoff seats (S1/S2/S3, also
rigid PETG). Real print height variance in this file's own documented history runs
**0.15mm per 10mm of print height** (the button-mechanism precedent, reused verbatim in
round-2's own rib/ledge tolerancing) — at an ~18mm ear/crossbar wall-reach, that's up to
**±0.27mm** of realistic height error, camd*before* considering the display module's own
manufacturing tolerance on its 3 standoff heights.

Two outcomes if this isn't addressed, both bad: if the ears/crossbar print even slightly
tall, the screws will draw the board down until the standoffs bottom out *before* the glass
is actually seated against the window — a visible gap/uneven reveal at the window, or light
leak/dust ingress there. If they print short (or the screws are driven fully home), the glass
gets crushed against the window seat by the standoff screws' own preload before the case
even closes — a very plausible crack mechanism for a bezel-mounted glass panel, and worse
than the plate-era failure mode because there is now zero give anywhere in the load path.

**Recommendation** (reusing, not inventing, the convention round-2 already established for
exactly this class of risk — its own ribs/ledges use the same fix):
1. **Print the ear/crossbar top faces 0.2–0.3mm short of nominal**, so the window's own
   continuous seat — not the three standoffs — takes the primary preload by design; the
   screws' job becomes holding the board down against light spring-back, not adding to the
   compressive stack.
2. **Add a thin (0.3–0.5mm) closed-cell foam pad under each of S1/S2/S3**, between the board
   and the ear/crossbar top face — the same material family already used for the compass
   module (F12) and originally considered (and rejected only for a *different*, thinner
   application) at the GPS patch. This takes up the remaining mismatch budget without
   depending on hitting the 0.2–0.3mm under-print target exactly.
3. Independently of 1/2 — see F9 — put a similar thin compliant liner under the glass at the
   window seat itself. Today's design has **zero** compliance there even before candidate 5;
   candidate 5 just removes the one thing (the plate) that was accidentally compensating for
   it.

**New gates this needs** (naming convention matches the file's own existing gates):
- `verify_ear_root_material` — the same 8-angle/`add_root_reinforcement`-style probe
  `verify_root_fillets` already runs, applied at the D1/D2 boss/ear roots.
- `verify_crossbar_battery_clearance` — real point-containment check of the crossbar's final
  geometry against `battery_connector_world_bbox`, gating (not diagnostic) given F6 is a
  confirmed real clash, not a marginal one.
- `verify_seat_coplanarity` (diagnostic, like `verify_plate_post_spread`) — reports the
  built Z-height of the window seat vs. each of S1/S2/S3's own ear-top face, so a future
  print-height regression on this specific relationship is visible before it becomes a
  cracked-glass report.
- `verify_gps_frame_clearance` for the crossbar (≥0.5mm to the GPS frame's outer wall, per
  the 0.45mm finding above).
- Extend `check_interference` to the new ears/crossbar/D1/D2 against the inserted display
  occurrence and the GPS/battery reference boxes, same as every other new feature in this
  file's history.

### F14 — watch: ear S1's reach toward the FPC relief pocket

`fpc_relief`'s own footprint (pass 15, item 1) is `x (-9.0, 9.0)`, `y (71.44, 73.12)` (base
frame). Reading the candidate-5 plan drawing, ear S1's own box appears to reach to roughly
y≈70 at its north edge, x as far as ≈−12 to −22 — close enough to the FPC pocket's own y0
(71.44) that I can't rule out an overlap from the drawing alone (a ~1.4mm gap by eye, which
is inside typical drawing-vs-real-geometry slop). This isn't a confirmed finding — I don't
have live geometry to probe — but flag it as a specific thing the modeling pass's own
`check_interference`/`verify_fpc_relief` run needs to look at explicitly for ear S1, not
just assume clear because the two features look far apart on a whole-case view.

---

## 4. Button mechanism (Jake's observation 3)

**Feature / generator location**: `button_geometry` (`firefly_case.py:2209`) computes the
working axis; `add_button` (`:2384`) builds it. Per button, the built assembly is: cap shaft
(stadium prism, curve-trimmed) → wall hole (an enlarged copy of the shaft) → retaining tab →
tab wall-cut → **tab pass-through lane** (a dedicated relief so the tab can clear the guide
rib during assembly) → nub pocket → guide rib + slot → **wall-reach connector spoke** (so the
rib physically fuses to the wall, since a disjoint Combine-Join silently no-ops in this
Fusion version) → **ceiling gusset** (added pass 15 because the rib was found to be
"floating," anchored at the connector's own outboard end after a first attempt hit real
board interference) → inward stop collar (clipped to the inner cavity separately, because
its own diagonal corners were found to overshoot the true wall on a different code path than
the rib's).

### F4 — should-fix: this is over-engineered relative to what it needs to guarantee

Counting the independently-dimensioned tolerances currently live in `PARAMS` for one button:
`cap_clearance` (0.25mm/side), `rib_slot_clearance` (0.25mm/side), `tab['gap']` (0.6mm),
`tab_relief_margin` (0.3mm/side, a *third* independent clearance around the same tab),
`collar['h']`/`collar['len']` (0.8/1.0mm), `plunger_pretravel` (0.3mm), `plunger_travel`
(0.9mm), `rib_inboard_offset` (6.0mm nominal, but *dynamically shifted at build time* per
button when it would otherwise clash with the real switch actuator), plus two more
geometry-derived offsets (`RIB_CONNECTOR_T_OFFSET`, `CEILING_GUSSET_*`) that exist purely to
make earlier features actually join to something. That's **8+ chained, independently-tuned
values feeding one moving part**, and the README's own history shows the real cost of that:
this single mechanism needed real, live-Fusion-found interference fixes in passes 2, 3, 5, 6,
9b, 12, and 15 — more re-work passes than any other single feature in the file, several of
them ("rib plate floating," "tab can't physically pass the rib," "collar's diagonal corner
overshoots the wall on a different clip tool than the rib uses") root-caused only after a
live `check_interference` run happened to catch them, not from the parametric design
predicting the problem.

That history is itself the evidence for "over-engineered": a design where the *n*th
interaction between independently-derived offsets keeps needing a live probe to catch is one
with too many interacting tolerance chains for its own parametric model to reason about
confidently — exactly the pattern Jake's instinct is picking up on, even without listing the
individual numbers above.

**What it needs to guarantee, restated simply**: (1) the cap doesn't fall out or rattle
sideways, (2) the cap can only travel far enough to actuate the switch, not crash into its
housing, (3) the cap can be assembled once from inside before the halves close. That's it —
everything else above is machinery built to make those three things true through a chain of
separately-clipped boxes.

**A simpler, more robust version** (single guide-sleeve + shoulder plunger, common in
commercial enclosures):
- Replace the rib-plate + slot + tab + tab-relief-lane + wall-connector-spoke + ceiling-gusset
  stack with **one integral guide tube**, printed as part of Top, running from near the wall
  hole inward to just short of the switch — built from the start as a wall-to-ceiling member
  (like the corner blocks), not retrofitted with a separate spoke and gusset after the fact.
  One clearance to tune: sleeve ID vs. shaft OD (a single `rib_slot_clearance`-equivalent).
- Replace the separate **retaining tab** (which needed its own wall cut, its own pass-through
  lane, and its own gap dimension) with a **single shoulder step machined into the plunger
  shaft itself** — a local OD increase that can't pass back out through the sleeve's own ID,
  the same principle as a captive circlip groove, but printed as one continuous part with the
  cap. This removes the tab-lane insertion-path problem entirely (there's no separate feature
  that has to clear a separate lane during assembly — the shoulder is on the same axis as the
  travel, so "insert from inside, it can't come back out" falls out of the geometry instead of
  needing a dedicated relief cut).
- Replace the separate **collar** (a second stop feature, clipped against a *different* tool
  than the rib for reasons the code itself flags as inconsistent) with the **shoulder
  bottoming directly against the sleeve's own inner face** — one stop surface, one
  `plunger_travel` number, instead of collar-vs-rib plus the collar's own separate inner-
  cavity clip.
- This drops the tuned-parameter count from 8+ to roughly **3** (sleeve clearance, shoulder
  engagement depth, pretravel) and removes two entire categories of assembly-order risk (the
  tab pass-through lane, and the collar/rib clip-tool mismatch) that took dedicated passes to
  find and fix.

This is not a blocker — the current mechanism verifies clean today (0 bad of 125 insertion
probes, both buttons, both variants, per pass 15's own gate). It's flagged should-fix because
candidate 5 is already going to touch the Top half's interior geometry substantially, which
is the natural time to simplify a feature that has cost five separate rework passes, rather
than carrying its accumulated complexity forward unchanged into a Top file that's about to
get a second major rework.

---

## 5. Everything else (Jake's observation 4)

### F9 — should-fix: glass edge has no protection at the window seat

`add_window` (`firefly_case.py:946`) cuts a plain bore (`window_dia=45.30`) with a print-
orientation chamfer (pass 15, item 7 — a conical cut, correctly fixed for flush printing) but
**no separate retaining ring or compliant liner** — confirmed directly in the code's own
comment: *"There is no separate retaining ring/lip at the window bore in this design — the
display glass rests on the ordinary ceiling-underside step."* The glass (`display_glass_dia
=44.79`) sits with only **0.51mm radial clearance** to the bore, directly against a sharp
PETG step, with no gasket. This is independent of candidate 5 (it's true of the current
design too) but candidate 5's removal of the plate's compliance (F5) makes it more
consequential — the glass now has fewer places upstream in the load path to absorb a mismatch
before it reaches the glass edge itself.

**Recommendation**: a small (0.3–0.5mm) fillet or chamfer on the ceiling-step's inner corner
(removes the sharp stress-riser the glass edge would otherwise rest against) plus a thin
compliant liner (foam or silicone) between the glass and the step, same material family
already used elsewhere in this design (F12).

### F10 — should-fix: XIAO/Wio retention is entirely secondary

`build_comms_stack_frame`/`add_comms_stack_frame` (`firefly_case.py:3799`/`3849`) retain only
the **L76K** (4 corner pads + a perimeter wall). Per the file's own comment, XIAO and Wio
"float above it, held by their own board-to-board / header connections" — i.e. the case
provides **zero direct mechanical retention** for 2 of the 3 boards in the stack; their
inertia in a drop routes entirely through solder joints/header pins into the one board the
case does hold. This is a reasonable simplification for a soldered, low-mass stack, but
worth a real drop-test check (or a light top-side retention feature — a shallow ceiling boss
or a foam wedge above the Wio module) given it's explicitly called out as a "what happens in
a drop" question.

### F11 — nice: LoRa FPC antenna keep-out is not actually enforced

Already flagged as Known Limitation #5 in the README: `fpc_keepout` is a reference-only box,
"not joined/cut into any body... not a real keep-out enforcement." `verify_antenna_channels`
checks the cut *channel* geometry (real and gated), but nothing checks the actual cable
route/strain relief against this box. Low priority given the channel itself is proven clear,
but worth converting to a live check eventually, per the README's own note.

### F12 — nice: compass module retention is real but physically un-modeled

Per README pass 15 item 4 and Known Limitation #28: retention is (1) two Ø2.7 pegs through
Ø3.0 holes (positive XY), (2) two rest pads (positive Z seating), (3) a ~2.0mm compressible
foam pad on the GPS patch's own top face doing "double duty" as the only real
downward/lateral-slip resistance. This is a sound design (a peg-through-hole pair is a real,
positive location on its own) — just confirming it stays a tracked BOM/assembly-instruction
item, since Fusion has no way to gate on a part it doesn't model.

### F13 — nice: battery bay has no positive Y end-stop

`add_battery_bay` (`firefly_case.py:3727`) builds X-direction rails (`battery_rail_w`,
0.3mm clearance) and a hook-and-loop strap through the rails at 1/3 and 2/3 along the
battery's length — good X constraint and a real Z/vertical hold via strap tension, but no
hard stop at either Y end of the 30mm bay. For an 803040 cell (light, ~15g class part) this
is likely fine in practice — the strap should prevent meaningful travel — but it's worth a
real-cell fit check rather than assuming the strap alone handles a shock load along the bay's
long axis.

### Drop onto concrete, 1.5m — ranked likely failure modes

1. **Glass against the window seat** (F5 + F9) — the most likely failure point *specifically
   under candidate 5*: a rigid 3-standoff + rigid-seat stack with zero engineered compliance
   is a textbook stress-concentration setup for exactly the kind of shock a drop delivers.
   Highest priority to fix before this ships.
2. **Button plunger assembly** (F4) — the most complex, most-reworked geometry in the file;
   a shock impulse landing on a cap is plausible in a hand-carried festival device, and the
   thin retaining tab / collar features are the kind of small, independently-clipped
   cross-sections most likely to have a stress concentration nobody's specifically checked
   for impact (only for static insertion clearance).
3. **Corner-block / D1/D2 screws** — very unlikely to fail in pull-out (§1's 700–900N
   margin), but a sharp corner impact concentrated right at a boss root without the
   collar reinforcement (F7) could still snap a boss off locally; the fix is the same
   collar this file already trusts elsewhere.
4. **Lanyard lug root** — already analyzed (pass 15) at ~3500N estimated failure, wildly
   past any plausible drop or tug load; not a realistic concern.
5. **Ear roots, if built without reinforcement** (F7) — a plausible secondary failure mode
   specifically if the ears are modeled as simple, unreinforced wedges rather than reusing
   the proven corner-block + collar pattern.

---

## Summary of concrete geometry recommendations for candidate 5

| Item | Recommendation |
|---|---|
| Ear/crossbar seat height | Print 0.2–0.3mm short of nominal so the window seat (not the standoffs) takes primary preload |
| Compliance | 0.3–0.5mm closed-cell foam under each of S1/S2/S3; separate thin liner/fillet at the window seat itself (F9) |
| Ear root size | Capsule radius = `boss_dia/2` (3.0mm) at D1/D2; unconditional `add_root_reinforcement` collar (`ROOT_COLLAR_RISE=1.5mm`) at both the wall-root and standoff ends |
| Crossbar section | 5.5mm (y) × 3.0mm (z) is stiffness-fine (δ≈0.29mm @ 14.7N, fixed-fixed) — but **must be split/keep-out-cut around the battery connector's real XY+Z footprint** (F6), not built as one continuous bar |
| Crossbar y0 | Move north to ≥29.5–30.0mm (currently 0.45mm from the GPS frame's outer wall vs. this file's own 0.5–1.0mm conventions) |
| D1/D2 pilots | Reuse `top_pilot_dia=1.62mm`, `top_pilot_z=(10.0,19.1)` unchanged, M2×12 — same proven 9.1mm engagement as A/B1/B2/C |
| D1/D2 & ears construction | Generalize `add_lanyard_corner_block`'s pattern (capsule + oversized clipped wedge + full-height core + collar) rather than new code — required wall reach (3.4–8.8mm) is well inside the proven `CORNER_BLOCK_REACH=10mm` |
| New gates | `verify_ear_root_material`, `verify_crossbar_battery_clearance` (gating, not diagnostic — F6 is a confirmed clash), `verify_seat_coplanarity` (diagnostic), `verify_gps_frame_clearance`, extended `check_interference` |
