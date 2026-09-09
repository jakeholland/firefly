# Firefly V2 case — independent printability review

Reviewer: independent FDM/DFM pass (Bambu/Prusa-class, PETG/PLA), read-only.
Repo: `jakeholland/firefly`, reviewed at `origin/main` = `51c5d16` (worktree
`/private/tmp/claude-501/review-print`, `git worktree add` off a fresh
`git fetch`; the main checkout was never touched).

Scope: `hardware/case/README.md` (pass 9 through 15b), `firefly_case.py`,
`params_trim.py`/`params_current.py`, the exported `export/trim/*.stl`
(the shipping variant — Jake's own decision, 2026-09-04) and `export/current/*.stl`,
`tools/offline_stl_check.py`, and the coordinator's "candidate 5" display-mount
proposal. Nothing here was modified — no code, no params, no exports, no README
edits.

**Method.** I ran the repo's own `tools/offline_stl_check.py` against
`export/trim` and `export/current` (both `OVERALL: PASS`, matching the README).
I then wrote independent, pure-Python/NumPy scans directly against the exported
STL bytes (not the README's narrative) — per-triangle overhang-angle
classification, whole-mesh connected-component analysis (island detection),
and targeted ray-casts through named features — because several of Jake's
complaints post-date the passes that claimed to fix related issues, and the
offline scanner's own whitelist boxes are broad enough to hide a regression
inside them. Every numeric claim below comes from one of those scans; the two
saved evidence plots are `docs/hardware/lip_ring_overhang_scan.png` and
`docs/hardware/top_overhang_histogram.png`, referenced inline in blocked area 1.

---

## Findings

### 1. BLOCKER — the alignment-ring seam chamfer (finding 6, pass 9c) is not present in the shipped STL; the ring's actual overhang is a flat 90° shelf, not a small step

**Feature / code:** `add_lip_anchor_reliefs()`, `firefly_case.py:800-861`.
The lip/anchor ring is two stacked stadium rings (`stadium_ring_solid`,
joined into `Top`): `lip_r=(23.95, 25.75)` at `lip_z=(9.2, 10.0)`, `anchor_r=
(23.95, 26.40)` at `anchor_z=(10.0, 11.0)` (trim; `current` is the same shape
2mm further out — `lip_r=(25.95,27.75)`, `anchor_r=(25.95,28.40)`, same
z-band). `PARAMS['lip_ring_seam_chamfer'] = 0.5` (`params_current.py:222-226`)
is meant to bevel the outer step where the anchor (wider, above) overhangs the
lip (narrower, below) via `chamfer_stadium_edge_at()` (`firefly_case.py:1230`),
called at line 853. The README (pass 9c / finding 6, `README.md:1226-1237`)
and the "Known limitations" table (`README.md:4859`, item J6) both say this is
"Fixed" and "confirmed clean by the offline overhang scan."

**Evidence.** I scanned `export/trim/Top.stl` and `export/current/Top.stl` for
any triangle whose face normal has an intermediate (~45°) Z-component in the
exact radial band the chamfer targets (trim: r 25.6–26.5mm, z 9.4–10.6mm;
current: r 27.6–28.5mm, same z-band), across the full 360° of the stadium
(both the straight sides and both domed ends — 240–253 triangles sampled per
variant in that band):

```
trim:    n=240 triangles in chamfer-target band, diag(45°-ish) candidates: 0
current: n=253 triangles in chamfer-target band, diag(45°-ish) candidates: 0
```

Every triangle in that band is either perfectly flat (`nz = ±1.0`, a hard 90°
shelf) or a plain vertical wall (`nz ≈ 0.0`). There is no facet at any
intermediate angle anywhere on the ring's outer step, on either variant. See
`docs/hardware/lip_ring_overhang_scan.png` — a cross-section scatter of every
mesh point in the ring's r/z window, colored by face-normal Z-component; the
three target radii (`lip_r[0]`/`anchor_r[0]`=23.95, `lip_r[1]`=25.75,
`anchor_r[1]`=26.40) are marked, and the flat step is visible as a hard
red→blue transition with nothing at the ±0.5–0.8 (chamfer) color in between.

**Root cause (most likely, not directly provable without the live Fusion
timeline):** `chamfer_stadium_edge_at` is explicitly best-effort — it matches
edges by midpoint radius and silently returns 0 (no assertion) if the match
fails or if `chamferFeatures.add()` raises `RuntimeError` (lines 1265-1268).
Passes after 9c (the pass-9 per-boss reliefs, the pass-9 stack3 keepout cut,
and pass 15's post/boss relayout) all cut additional notches through this
same ring at boss/keepout locations, fragmenting its once-clean 4-arc outer
edge loop into many disjoint arcs. A multi-edge chamfer selection like that is
a plausible `RuntimeError` case in Fusion's chamfer feature. Since nothing in
`verify()` checks that the chamfer *actually landed* (only that the ring
itself has 0 non-manifold edges and 0 bad overhang **clusters**, and this
region falls inside the very wide `general_ceiling_overhang` whitelist box in
`tools/offline_stl_check.py`, `y ∈ [-16,79]`, `x ∈ [-32,32]` — nearly the
whole underside of Top), a silent no-op here would never have tripped any
gate.

**What this means for Jake's print:** this is very likely the "lip on the
top case for the alignment ring" he is describing. It sits right at the
Top/Bottom parting line (z 9.2–11 is only just above `split_z=10.0`) and is
the shelf the display module's PCB registers against — a rough,
support-scarred surface here risks both a poor Top/Bottom mating line and a
tilted display seat, not just cosmetics.

**Recommended fix.** Two things, not one:
1. **Restore the chamfer, defensively.** Change `chamfer_stadium_edge_at`'s
   caller to assert (or at least log loudly) when `found == 0`, so a future
   regression like this can't happen silently again — the existing
   best-effort/no-assert pattern is appropriate for a purely cosmetic fillet,
   but this one is print-load-bearing.
2. **The 0.5mm step chamfer was never enough anyway.** Even when it worked,
   it only bevels the 0.65mm-wide *outer* step (`anchor_r[1]-lip_r[1]`); the
   ring's own flat *top* face at `anchor_z[1]=11.0` (from r=23.95 out to
   r≈26.40, i.e. the actual seat the display's PCB rests on) is a completely
   flat, 0°-from-horizontal shelf with no chamfer treatment at all — a
   textbook full support-needed overhang, confirmed directly: every triangle
   I sampled at `z=11.0` in that radius band reads `nz=+1.000` (dead flat).
   Reprofile the ring the same way pass 15's window-bore fix already proved
   out (§4 below, `add_window`'s `cone_frustum_solid` cut, slope
   deliberately just under 45° from vertical): instead of two flat-topped
   stacked cylinders, taper the ring's outer wall continuously from
   `lip_r[1]` at `lip_z[0]` up to `anchor_r[1]` at `anchor_z[1]` as a single
   45°-max cone frustum. The available rise (1.8mm total, `lip_z[0]` to
   `anchor_z[1]`) only buys ~1.8mm of radial growth at a true 45° slope,
   short of the needed 2.45mm (`anchor_r[1]-lip_r[0]`) — so either keep a
   small (~0.5-1mm wide) flat registration land near the bore-facing inner
   edge (where the display actually needs a true flat seat) and taper only
   the *outer* portion back to the shell at ≤45°, or accept a slightly
   steeper-than-45°-but-still-better-than-90° taper there (PETG commonly
   tolerates up to ~55-60° from vertical on a short run before visible
   sagging) rather than the current hard corner.

**Severity: blocker** — it is the literal feature Jake asked to have fixed,
the "fix" already merged is not actually present in the geometry being
shipped, and it sits on a fit-critical surface (display seat / parting line).

---

### 2. SHOULD-FIX — button guide-rib/collar tips still present small unsupported ledges, and the rib's ceiling-gusset region sits inside a hard-to-clear channel

**Feature / code:** `button_geometry()` (`firefly_case.py:2209-2400`),
`add_button()`'s rib plate / tab-relief lane (`firefly_case.py:2535-2650`),
the pass-15 ceiling gusset (`firefly_case.py:2713-2737`, `README.md:4129-4168`,
item 5).

**Background (already fixed, verified structurally sound).** Pass 15 item 5
correctly diagnosed that the guide rib + wall connector used to hang
completely unattached to the ceiling ("floating," Jake's word) — a genuinely
unsupportable internal island deep in a cavity that only opens through a
~14×24mm hole. The fix ties the rib to `top_ceiling_underside_z` via a
diagonal gusset anchored at the wall-connector's outboard end (clear of the
display board's own footprint, learned the hard way from a first attempt that
hit a real interference). I independently re-verified this is not just
"interference-clean" but **actually one connected solid**: a whole-mesh
connected-component pass over `export/trim/Top.stl` (12,524 triangles) finds
**exactly 1 component** — no disconnected/floating geometry anywhere in Top,
buttons included. Good: the gusset genuinely closes the "floating rib" defect
and the mechanism can print without a completely separate, hard-to-reach
internal support tower.

**What's still rough, evidenced directly:** scanning triangles in the button
housing footprints (`x ∈ [-19,-10]`, `y ∈ [34,45]` for Power, `[55,66]` for
Home; `z 8–22`, i.e. below the general dome ceiling) turns up local overhangs
the general scan's clustering hides:

```
Home button, z 15.6–17.0 (near the guide rib's own chamfered lead-in / plunger tip):
  x=-17.71 y=64.83 z=16.31  nz=0.967  (~75° "up" -> steep downward-facing overhang once printed flipped)
  x=-17.38 y=65.35 z=16.55  nz=0.903  (~65°)
  x=-17.62 y=65.95 z=16.78  nz=0.808  (~54°)
  ... (area ~0.5mm^2 each, several of them)
Home button, z 23.8-27.0 (the rib/gusset's own attach region under the ceiling):
  x=-18.30 y=64.72 z=23.80  nz=1.000  area=8.43mm^2  (flat, 0deg)
  x=-18.35 y=65.84 z=27.02  nz=0.709  area=3.49mm^2  (~45deg, this one is fine)
```

None of these are individually above `offline_stl_check.py`'s 30mm² cluster
threshold, and they fall inside the same `general_ceiling_overhang` whitelist
box as everything else near the dome end, so the existing tooling reports
"clean" here too. The `8.43mm²` flat patch sits right where the ceiling
gusset attaches, and the ~54-75° facets sit right at the guide rib's own
lead-in — both are exactly the kind of thing that needs a slicer support
touch **inside the button housing's own narrow channel** (the same
"open" the coupon-fit instructions describe as ~9.6×24×18.6mm — barely
finger-width), not out on the open dome ceiling where support is trivial to
place and pull. Support debris or a snapped-off support nub in this channel
is a real risk to the guide rib itself (a thin printed feature that the
switch's own actuation force already loads, per pass 15 item 6's plunger
analysis).

**Recommended fix:** add small 45° lead-in chamfers on (a) the guide rib's
own inboard edge where it meets the plunger-tip pocket, and (b) the ceiling
gusset's own attach face, so that the small residual overhangs above shrink
toward self-supporting rather than needing supports specifically inside the
housing. If any support is still needed there, note it explicitly in the
print-orientation instructions (the current README only says "check the lug
and USB liner overhangs on your slicer" — the button-housing ceiling
attachment deserves the same call-out).

**Severity: should-fix** — small area, not a blocker, but it is inside the
one feature Jake specifically flagged ("alignment rings for the buttons have
printability issues") and it's the hardest place in the whole case to clear
support from.

---

### 3. VERIFIED GOOD — the window bore fix (pass 15 item 7) is real and effective in the shipped STL

**Feature / code:** `add_window()`, `firefly_case.py:946-1030`.
`window_dia=45.30`, `window_center=(0,50)`, `window_chamfer=0.5`. The pass-15
fix replaced an edge-matched chamfer (which silently left ~30° gaps at the
±X extremes because `window_dia/2=22.65mm` sits past trim's `flat_rho=
22.14mm`, into the curved shoulder) with a `cone_frustum_solid` boolean
**cut**, geometry-independent of whether the local surface is flat or curved.
Cone: `r` grows `chamf+0.2=0.7mm` over `z`-span `chamf+0.6=1.1mm` —
`atan(0.7/1.1) = 32.5°` from vertical, comfortably under the 45° ceiling
with margin, exactly as the code comment claims.

**Independent confirmation, done two ways, both clean:**
- A pure ray-cast point-containment check against the actual exported
  `export/trim/Top.stl` at the cone's own mid-slope radius/z, sampled every
  10° around the full 360°: **0/36 angles read solid** (all hollow, as
  required — the old defect specifically showed up at the ±X extremes, both
  of which are included in this sweep).
- The same check on the ring/window rim generally: no residual flat notch or
  gap anywhere in the sampled ring.

This matches the README's own live-build probe and its "independent
pure-Python ray-cast" claim — unlike finding 1, this one really is fixed in
the shipped geometry. No action needed; it's a good template for finding 1's
recommended fix (§1) and for candidate 5 (§6).

---

### 4. VERIFIED GOOD — Bottom's screw-hole "replug" (pass 15 item 8) is also genuinely fixed

**Feature / code:** `add_case_boss()` / `add_root_reinforcement()`
(`firefly_case.py:1397`ff). Pass 13's 45°-collar reinforcement is a solid
revolve that (pre-fix) silently replugged the pilot hole/counterbore over its
own z-band on every *Bottom*-side boss. Ray-casting straight down through
`export/trim/Bottom.stl` at all five boss centers (A(-15.5,-8), B1(-12.5,-15),
B2(12.5,-15), C(15.5,-8), D(0,60)) finds **0 triangle crossings at every one**
— i.e. the ray never touches solid material at any of the five pilot axes,
confirming all five are genuinely open through-holes in the shipped STL, not
just in the historical live-Fusion probe the README quotes. No action needed.

---

### 5. NICE-TO-HAVE — general ceiling overhang is large but expected; mesh integrity is otherwise clean

- Whole-mesh connected-component check: `Top` (12,524 triangles) and
  `Bottom` (9,862 triangles) each resolve to **exactly 1 connected
  component**, 0 non-manifold edges (matches `offline_stl_check.py`). No
  floating islands anywhere in either body.
- An area-weighted overhang-angle histogram of `Top` restricted to
  triangles *outside* the broad `general_ceiling_overhang` whitelist box
  still finds ~165mm² total of >46°-flagged area, but every individual
  cluster is well under 1mm² (see `docs/hardware/top_overhang_histogram.png`)
  — small ribs/pegs near the south (L76K/compass) end, not worth chasing
  individually.
- The large (~488mm²) flat interior-ceiling overhang the whitelist already
  accepts is real and will need ordinary slicer support (tree/organic) —
  that's normal and unavoidable for a hollow dome printed face-down, and the
  cavity is wide open for support removal before Bottom/the display go in.
  No design change recommended here; just make sure the slicer's support
  density/interface settings are tuned for PETG release, since the display
  seats directly under part of this ceiling.

---

## 6. Candidate 5 (display mount: two ears + S2 crossbar, no plate) — printability guidance before it's built

Proposal under review: two ears grown from Top's dome wall at S1 `(-12, 65)`
and S3 `(11.6, 65.46)`, and a crossbar "wall-to-wall" at the S2 line
(`board_standoffs['S2'] = (0.04, 32.22)`, `params_current.py:393-395`, y
band ≈29–34.5), all at z 15.5–18.5, printed as part of Top face-down. Ceiling
reference numbers (trim): `top_ceiling_underside_z = 26.0`, display board
`display_pcb.z = (20.59, 21.81)`, display bbox `z = (23.3, 28.0)`, inner
fillet `R = 8.0` (transitions from the vertical cavity wall into the flat
ceiling somewhere around z≈24–26, by direct analogy to the whitelist's own
"z~21-23 at ~40°" note for `current`, shifted +3 for trim).

**S1/S3 ears — should print clean with a short local gusset.** These grow
directly from the dome wall at y≈65 (near the case's own +y tip, past
`spine_b`), so the unsupported reach is only wall→standoff, not a
cavity-spanning bridge. Recommendation:
- Give each ear's underside (in CAD, its `+Z`-facing top, which becomes the
  downward face once Top prints flipped) a 45°-max chamfer back to the wall,
  exactly the `cone_frustum_solid` idiom already proven at the window bore
  (§3) and the root-reinforcement collars (`ROOT_FILLET_R=1.8`,
  `ROOT_COLLAR_RISE=1.5`, `firefly_case.py:1338-1339`) — don't hand-roll a
  new chamfer helper, reuse `cone_frustum_solid` as a join, same call
  signature.
- Watch the same defect class pass 15 item 8 found: if a reinforcement
  collar is joined in *after* the standoff's own pilot hole is cut, re-cut
  the hole afterward (`add_case_boss`'s fix) — collars are solid revolves
  that will replug a hole passing through their own z-band.
- Expected tolerance: a short (<10mm), wall-anchored, chamfered cantilever
  like this should hold to roughly the same **±0.15–0.3mm** the rest of the
  case's posts/bosses already work to (the file's own repeated
  "flat-ray-vs-true-curvature" slack budget). No bridging is involved, so
  layer-to-layer consistency should be good.

**S2 crossbar — do not plan on this being support-free at 52mm.** This is
the one place candidate 5 needs a real design change, not just a chamfer,
and it's worth flagging before it's modeled:
- S2 sits at `x≈0.04`, i.e. dead-centered under the display board
  (`display_bbox.x = [-22.39, 22.39]`) — the same board pass 15 item 5's
  button-gusset dead-end hit (a real 14.28mm³ interference) when it tried to
  run a gusset straight up toward the ceiling under this same footprint. A
  45° gusset from the S2 crossbar (z 15.5–18.5) up to the R8 fillet/ceiling
  (z≈24–26) would have to pass directly through `display_pcb`'s own z-band
  (20.59–21.81) at essentially the same XY — there is no lateral dodge
  available here the way item 5 found for the button (which could shift
  outboard to `s_wall`, clear of the display). **Any straight-up gusset at
  S2 is a probable board interference, not just a printability nuisance** —
  this needs its own `check_interference` gate against the display
  occurrence before it's trusted, the same way item 5 was caught.
- Height budget for *any* self-supporting slope at S2 is thin regardless:
  crossbar top (18.5) to fillet start (≈24) is only ~5.5mm of rise, giving
  ~5.5mm of horizontal reach at a true 45° — nowhere near enough to span the
  ~52mm wall-to-wall run (inner cavity half-width ≈ `outer_radius - wall` =
  26mm each side in the straight section) with periodic gussets without
  either (a) a large number of them (≈8–10 pairs at ~5-6mm spacing, cluttering
  the cavity and multiplying the interference risk above), or (b) accepting
  unsupported bridge segments well past typical clean-bridging length
  (∼5–10mm is comfortable for PETG with good part cooling; 15-25mm needs
  tuning and will show visible sag; 52mm flat and unsupported will sag
  significantly and likely fail to print a usable flat seat at all).
- **Recommendation:** don't model S2 as one continuous flat wall-to-wall
  beam. Either (a) treat S2 like S1/S3 — a short local boss cantilevered in
  from whichever wall is closer, accepting it won't be a continuous
  stiffening bar, or (b) if a continuous bar is wanted for rigidity, design
  it expecting **minimal, deliberate slicer support**: the cavity is wide
  open at this stage of the print (nothing built yet at z<18.5 in that area
  besides the ears/side walls) so a couple of easily-reached, easily-removed
  drop supports from the bed up to the bar's underside are cheap here —
  unlike the button housings (§2), there is no narrow channel trapping them.
  Keep any support landing spots off the S2 screw seat's own local pad
  (chamfer that pad's edges at 45° so it's self-supporting for a few mm
  around the hole itself, even if the rest of the bar needs help), and off
  the ear-to-crossbar interfaces. If you go this route, add the interference
  gate against the display board mentioned above regardless of whether
  supports or gussets end up doing the work.
- **Tolerance to expect:** if the crossbar is left as an unsupported flat
  bridge, expect sag on the order of several tenths of a millimeter up to
  ~1mm at midspan (52mm is well past comfortable PETG bridging) — not
  acceptable for a screw seat. If printed with minimal slicer support (or
  redesigned as short local bosses per the recommendation), expect it can
  return to the case's usual **±0.15–0.3mm** working tolerance, same as
  S1/S3 and the other posts — but verify this with a coupon print (the same
  idiom the button caps already use, `export/coupons/`) before committing a
  full Top print, since this is new, unvalidated geometry.

---

## Prioritised list

1. **[Blocker]** Restore/replace the lip/anchor alignment-ring chamfer
   (`add_lip_anchor_reliefs`, `firefly_case.py:800-861`) — confirmed absent
   from `export/{trim,current}/Top.stl` despite README pass 9c claiming it
   fixed; the ring's flat `z=anchor_z[1]` top shelf also needs its own
   self-supporting profile, not just the outer step. See §1.
2. **[Should-fix]** Before modeling candidate 5's S2 crossbar as a literal
   wall-to-wall beam, add a `check_interference` gate against the display
   PCB for any gusset/support reaching toward the ceiling under its
   footprint, and replan S2 as short local bosses or a beam that
   deliberately expects minimal slicer support rather than 45°-only
   self-support (the height/interference budget doesn't allow full
   support-free at this span). See §6.
3. **[Should-fix]** Chamfer the button guide-rib's lead-in edge and the
   ceiling-gusset attach face (~54-75° facets and one flat 8.4mm² patch
   found inside the Home button housing) so slicer support isn't needed
   inside the narrow, hard-to-clear button channel. See §2.
4. **[Nice]** Add a loud assertion (not silent no-op) to
   `chamfer_stadium_edge_at` when its edge match count is 0, so a future
   regression like finding 1 can't hide behind the broad
   `general_ceiling_overhang` whitelist again.
5. **[Nice]** S1/S3 ears in candidate 5: reuse `cone_frustum_solid` for a
   45° root chamfer (same idiom as the window bore and the existing
   root-reinforcement collars) and re-cut any standoff pilot hole after
   joining a collar, per the pass-15-item-8 gotcha. See §6.
6. No action needed: window bore fix (§3) and Bottom screw-hole fix (§4)
   are both genuinely present and correct in the shipped STLs; general dome
   ceiling overhang (§5) is normal and already handled by ordinary slicer
   supports.
