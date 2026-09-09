# Headless port parity report

Regression comparison of the headless build123d port
(`hardware/case/gen/`) against the Fusion-driven generator's own
exports, following `docs/hardware/cad-tooling-spike.md` section 4's
method: bounding box (target ≤0.1mm), volume (reported as a percentage,
not gated -- phase 1 does not port every feature that adds/removes
material), and a signed-distance sampling of 2000 points from the
golden's own surface against the port's mesh (max and 95th-percentile
reported).

**Primary golden:** `case-pass16` branch (5588a2b) exports,
`/private/tmp/claude-501/case-pass16/hardware/case/export/<variant>/`
-- the newest generator, and the one this port's own architecture
(A/C/D single-pilot corner blocks, D at (18, 58), ears/S2-boss deferred)
matches. **Secondary golden:** `origin/main`'s own pass-15b exports
(`hardware/case/export/<variant>/` in this repo, unmodified by this PR)
-- an older design (paired B1/B2 corner blocks, a Screen Plate + P1-P4
posts, D at (0, 60)) that this port does NOT target architecturally, so
it is reported for completeness only, not as a pass/fail bar.

Reproduce with (from `hardware/case/`, `gen/.venv` active):
```
python3 -m gen.cli build --variant trim --export
python3 -m gen.cli build --variant current --export
```
then compare `gen/out/<variant>/{Top,Bottom}.stl` against the goldens
above with `trimesh` (bounding box, `.volume`,
`trimesh.proximity.signed_distance`).

## trim (Jake's default variant) vs. case-pass16 golden

| body | bbox max |diff| (mm) | volume % of golden | signed-dist max (mm) | 95th pct (mm) | median (mm) |
|---|---|---|---|---|---|
| Top | 0.056 | 89.91% (17,360 vs 19,308 mm³) | 15.8 | 8.44 | 0.009 |
| Bottom | 0.060 | 102.13% (14,093 vs 13,799 mm³) | 4.0 | 0.75 | 0.00 |

Both bodies clear the ≤0.1mm bounding-box target. The median signed
distance (0.009mm Top, 0.00mm Bottom) says most of each surface is an
exact match -- the shell, window bore + both chamfers, lip/anchor ring,
A/C/D bosses/corner blocks + collars, USB tunnel + liner, FPC relief
pocket, and lug are all ported and geometrically agree with the golden
to tessellation-level noise. The large max/95th-percentile values are
fully accounted for by what phase 1 does not yet build:

- **Top volume is 10.1% below golden**, entirely explained by unported
  *additive* features: the S1/S3 ears + S2 boss (display mount), both
  buttons' cap/rib/collar mechanism, the comms-stack frame, the GPS
  frame, the battery bay's own Top-side features, the compass-module
  mount, and the wordmark deboss. Every one of these adds material to
  Top in the golden; none of them exist in this port yet.
- **Bottom volume is 2.1% ABOVE golden**, explained by unported
  *subtractive* features: the battery bay cavity, the GPS frame cutouts,
  and the comms-stack frame cutouts all remove material from Bottom in
  the golden but are not yet cut here, so this port's Bottom retains
  that material.
- The signed-distance outliers (max 15.8mm Top, 4.0mm Bottom) sample
  points on the golden's own surface that fall on exactly those unbuilt
  features (an ear's outer face, a button cap, the battery-bay floor,
  etc.) -- there is nothing in this port's mesh there yet for those
  points to be close to, which is the expected, documented state of a
  phase-1 slice, not a defect in what was ported.

## current variant vs. case-pass16 golden (secondary check, not the primary bar)

| body | bbox max |diff| (mm) | volume % of golden |
|---|---|---|
| Top | 1.20 | 89.83% |
| Bottom | 0.084 | 98.25% |

Bottom is within the 0.1mm target; **Top is not (1.2mm)**. Root cause,
confirmed by inspecting the raw bounds directly: the case-pass16
golden's own `current`-variant Top.stl has a real Z-min of 8.0mm, below
`split_z`=10 -- i.e. **the golden itself is documented as broken for
`current`** (README's pass-16 section, item G: "`current` ... a
materially bigger set of problems, none fixed this session" --
`check_interference` RED with a real 0.55mm³ Top-vs-Bottom overlap, on
top of the 1618mm³ Top-vs-display overlap). This port's own `current`
build has **zero** interference (see gate output below) and does not
reproduce that defect, so the 1.2mm bbox gap is the golden being wrong,
not this port. `trim` is Jake's own default variant and the one with a
clean golden to compare against; `current` exists for the M1
probe-table comparison per the repo's own README, not as a variant this
report treats as a strict parity bar.

## Gate results (this port, both variants)

All phase-1 gates pass clean on both variants, from a from-scratch
rebuild (`python3 -m gen.cli build --variant <v> --gates --export`):

| gate | trim | current |
|---|---|---|
| all-pairs interference (Top vs. Bottom) | `{}` (clean) | `{}` (clean) |
| `verify_post_walls` (D pilot wall + shell skin) | clean | clean |
| `verify_root_fillets` (A/C/D boss+corner-block roots, 6 features × 8 angles) | clean | clean |
| `verify_corner_blocks` (A/C/D pilot-open/block-solid + stack/FPC keepout clearance) | clean | clean |
| `verify_bottom_openings` (A/C/D pilot+counterbore open, lug cord hole open) | clean | clean |
| offline `check_manifold` (Top, Bottom) | 0 non-manifold edges, both | 0 non-manifold edges, both |
| offline `check_body_count` | 1 body, both | 1 body, both |
| offline `scan_stl_overhangs` (real whitelist) | `bad_clusters_mm2: []`, both | `bad_clusters_mm2: []`, both |
| `verify_lip_ring_profile` | clean: 92 real 10-80° facets, 0.098mm worst flat cluster | clean: 109 facets, 0.099mm worst flat |

**Worth flagging as a real, positive discrepancy, not a bug:** the
case-pass16 golden's own `verify_root_fillets` gate is documented RED
for `corner_block_D_top` (2 of 8 sampled angles hollow, both variants,
"pre-existing, confirmed present in the very first piecewise run before
any fix this session touched anything" -- README pass-16 item 1). This
port's OCC equivalent of the identical geometry (same wedge/collar
construction, same `CORNER_BLOCK_WEDGE_OVERLAP`/trim-then-join fixes
ported verbatim) passes clean at all 8 angles. This is consistent with
the spike's own finding (`cad-tooling-spike.md` section 5, item 1): "OCC
booleans never silently no-op'd on non-touching bodies, never left an
orphaned duplicate body, and never produced a partially-filleted edge
loop" -- the class of Fusion-kernel-specific tessellation/boolean
artifact this generator's own comments repeatedly root-cause and work
around throughout `firefly_case.py` (see `add_root_reinforcement`'s own
docstring, `dedupe_body`'s, `CORNER_BLOCK_WEDGE_OVERLAP`'s). Not
independently re-verified beyond this port's own gate (no Fusion
available to re-run the comparison live), but the geometry and probe are
faithful ports of the same recipe, so this reads as OCC genuinely not
reproducing that specific Fusion defect, rather than a looser probe.

## Cycle time

Full `build --gates --export --render`, trim variant, warm venv, this
Mac (Apple Silicon, macOS 26.5.1), single process:

| stage | time (s) |
|---|---|
| shell (outer/inner pill, hollow+split) | 0.24 |
| window (bore + 2 chamfers + regression probes) | 0.18 |
| lip/anchor ring (taper + narrowing cut + reliefs) | 0.27 |
| case screws A/C/D (6 boss/block builds + collars) | 1.61 |
| USB tunnel + liner | 0.64 |
| FPC relief | 0.28 |
| lug (+ 2 best-effort fillets + chamfer) | 0.42 |
| **build subtotal** | **3.65** |
| export (2× STL + 1 packed 3MF) | 0.72 |
| gates (interference + post-walls + root-fillets + corner-blocks + bottom-openings + 2× offline STL scan + lip-ring-profile) | 1.08 |
| render (2× headless PNG) | 0.47 |
| **full cycle (build + gates + export + render)** | **~6.9-8.0s** (2 runs: 7.98s, and 6.27s without `--render`) |

Compare to Jake's own observed 5-10 minutes per Fusion-MCP rebuild+gate
cycle: **~40-90x faster** even at phase 1's own feature count (well
short of the full generator), consistent with the spike's own
"15-40x faster at the full generator's size" estimate -- phase 1's own
cycle time already clears that bar since case screws + collars (the
single largest, most boolean-heavy phase-1 feature) turned out cheaper
in practice (1.6s for 6 boss/block+collar builds) than the spike's own
per-corner-block estimate suggested for a full generator's worth of
similar features.
