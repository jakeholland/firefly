# CAD tooling spike: Fusion-via-MCP vs. headless build123d

**Question:** is Fusion-via-MCP the right tool for the Firefly case generator
(`hardware/case/firefly_case.py`), or should the generator move to a
headless Python CAD kernel?

**Bottom line: move it.** A representative slice of the generator (the Top
shell, its window bore + cone chamfer, its alignment lip/anchor ring, and
two independent instances of the lanyard-corner-block boolean pattern)
built, exported, and gated in **~1.6-2.3 seconds end to end** on this Mac
using [build123d](https://github.com/gumyr/build123d) (OpenCascade/OCP
under the hood) — against Jake's own observed **5-10 minutes per
rebuild+gate cycle** through the Fusion MCP bridge, with none of that
bridge's constraints (60s call cap, ~1000-call collapse, single-agent
lock on Jake's own Fusion session). No geometric defects showed up in the
fidelity check against the real pass-16 export; the differences found are
fully explained by tessellation tolerance and the deliberately-smaller
feature set this spike ported. See **Recommendation** and **Migration
plan** below for how to get there without a rewrite.

This spike did **not** use the Fusion MCP at all (another agent was using
Fusion this session) — everything here is a from-scratch port read
straight from `params_trim.py`.

---

## 1. Setup

```
uv venv hardware/case/spike_headless/.venv --python 3.12
source hardware/case/spike_headless/.venv/bin/activate
uv pip install build123d trimesh matplotlib numpy rtree
```

- `build123d` 0.11.1 installs cleanly on Apple Silicon macOS (26.5.1,
  arm64) via `uv`. First install (via `uv run --with build123d`, no
  persistent venv) took **~62s**, almost all of it downloading
  `cadquery-ocp-novtk` (59.5MB, the OCP/OpenCascade binding) plus
  `scipy`/`scikit-learn`/`sympy` pulled in as transitive deps. A
  **persistent venv** (`uv venv` + `uv pip install`, used for everything
  else in this spike) avoids re-paying that cost: a fresh interpreter
  with `build123d` already installed starts and imports in well under a
  second.
- STL export: `build123d.export_stl(solid, path, tolerance=..., angular_tolerance=...)`.
  Works immediately.
- 3MF export: **not** `build123d.export_3mf` (doesn't exist) — it's
  `build123d.Mesher()` (`.add_shape(solid, ...)` then `.write(path)`,
  backed by `lib3mf`). Confirmed working; both STL and 3MF come from the
  same meshing call, unlike the real repo's `tools/stl_to_3mf.py`, which
  exists specifically because Fusion's STL/3MF exports are two unrelated
  API calls.
- Headless PNG review renders: **plain `trimesh` + `matplotlib`** (Agg
  backend, `Poly3DCollection`) is sufficient — a 3D render of an
  ~15k-triangle mesh took well under a second. `ocp_vscode`/`ocp-tessellate`
  were **not evaluated** (out of the spike's time budget) since the
  matplotlib fallback the brief allowed already worked on the first try;
  worth a follow-up look if hover/inspection (not just static PNGs) turns
  out to matter for review.
- Point-in-mesh containment via `trimesh.Trimesh.contains(...)` needs the
  `rtree` package (not a `build123d`/`trimesh` transitive dep — it errors
  at call time, not at import time, if missing).

## 2. What was ported

New package, `hardware/case/spike_headless/case/` (kept alongside the
spike rather than inside the real generator, since this is throwaway
proof-of-concept code, not a production port):

| file | ports from `firefly_case.py` |
|---|---|
| `params.py` | loads `PARAMS` from the real `params_trim.py` **unmodified** (`from params_trim import PARAMS`, same `sys.path` trick `offline_stl_check.py` itself uses) — every number below is the repo's own, not retyped |
| `shell.py` | `_profile_geometry`/`rho_at_z` (:385), `_inner_profile_geometry` (:509), `build_outer_pill_solid`/`build_inner_pill_solid` (:721/:593), `build_top_shell` (a restriction of `hollow_and_split` to the Top piece, :770), `add_window`'s bore + pass-15 cone-chamfer cut (:946), `add_lip_anchor_reliefs`'s ring + finding-6 seam chamfer (:800, **per-boss/lug reliefs not ported** — out of the representative slice) |
| `features.py` | `add_lanyard_corner_block` (:1611) — capsule (`oriented_stadium_prism`, :322) + outward wedge (`oriented_box_prism`, :350), clipped to the inner cavity (`build_inner_cavity_clip_tool`, :630) and the lip-ring radius, minus the L76K stack keepout, plus two full-height cores and two conical root collars (`add_root_reinforcement`, :1397), minus two M2 pilot holes — instantiated **twice** (see note below) |
| `gates.py` | reuses `tools/offline_stl_check.py` **as-is** (manifold + overhang scan); a new `pilot_wall_probe` (headless port of `verify_post_walls`'s ray/point-containment technique, :6437, run via `build123d`'s `Solid.is_inside`, i.e. OCP's `BRepClass3d_SolidClassifier`, directly against the in-memory solid); a new `interference_volume` (boolean-intersect two solids and read the volume — the headless equivalent of Fusion's live `checkInterferenceInput`) |
| `export.py` | STL + 3MF via `build123d.export_stl` / `Mesher` |
| `build_slice.py` | driver: builds the slice, exports it, runs all three gates, prints timings |

**Note on the "candidate-5 ear."** `firefly_case.py` has no function or
comment named `candidate-5`, and no feature seats at exactly z=21.55 with
a Ø1.62 pilot (`top_post_pilot_z` is `(14.1, 20.6)`; `top_pilot_z` is
`(10.0, 19.1)` — neither matches). The closest real match to the brief's
own description ("capsule + wedge into the dome wall + collar + seat ...
with a Ø1.62 pilot") is the exact `add_lanyard_corner_block` construction
the brief's item 2 already assigns to the lanyard end. Rather than block
the spike on an apparently-nonexistent name, the second "ear" instance
reuses the *identical* corner-block technique at the other screw pair
(C+B2 instead of A+B1) — which is what actually mattered for the spike's
purpose (does OCC handle **two independent instances** of this compound
boolean — stadium capsule + oriented wedge + inner-cavity clip +
ring-radius clip + stack-footprint cut + two full-height cores + two
conical collars + two pilot cuts — as cleanly and fast as Fusion does).
It did: see timings below, and `interference_volume` between the two
blocks' own added material came back exactly `0`.

**Ergonomic difference worth flagging:** Fusion's SPEC.md documents (and
`firefly_case.py` works around) a real gotcha — arbitrary-angle sketches
need a named construction plane, and `oriented_stadium_prism`/
`oriented_box_prism` build canonically on the XZ plane and then rigidly
move the result into place with a hand-built `Matrix3D`
(`move_body_to_frame`, :262) because `setByThreePoints` needs real point
*entities*, not raw coordinates. `build123d`'s `Line`/`ThreePointArc`
take plain 3D points directly, so every oriented prism in this port is a
single `make_face` + `extrude(dir=...)` call, no canonical-build-then-move
step needed at all.

## 3. Timing

All measurements on this Mac (Apple Silicon, macOS 26.5.1), warm
persistent venv (no `uv run` cold-start cost), single process, no
parallelism.

| stage | time |
|---|---|
| Build Top shell (pill outline, R8 ceiling fillet, window bore + cone chamfer, lip/anchor ring) | ~0.42s |
| — shell (extrude + 2 end-cap revolves, outer − inner) | ~0.22s |
| — window bore + pass-15 cone chamfer | ~0.04s |
| — lip/anchor ring + seam chamfer | ~0.12s |
| Build inner-cavity clip tool (`offset`, safety margin) | ~0.07s |
| Add lanyard-end corner block (capsule+wedge+collar+2 pilots) | ~0.31s |
| Add 2nd corner block ("candidate-5" stand-in) | ~0.32s |
| **Full slice build (shell+window+lip/anchor+2 corner blocks)** | **~1.05-1.11s** (3 runs: 1.104s, 1.107s, 1.069s — no drift) |
| Export STL | ~0.019s |
| Export 3MF (`Mesher`/lib3mf) | ~0.3-0.85s |
| Gate: `offline_stl_check` manifold+overhang scan on exported STL | ~0.016s |
| Gate: pilot-wall point-containment probe (4 pilots × 8 angles × 3 z = 96 probes, in-memory OCC solid) | ~0.10s |
| Gate: interference-volume boolean between the two corner blocks | ~0.18-0.21s |
| **Full cycle (1 build + both exports + all 3 gates)** | **~1.6-2.3s** |
| ×3 full rebuild (build only, back to back) | ~3.2s total, ~1.08s/rebuild average, consistent |
| Manifold+overhang gate run directly against the real pass-16 `Top.stl` (15,634 triangles, for comparison) | ~0.06s |
| Load the pass-16 `Top.stl` into `trimesh` (stand-in for "import the display module's exported mesh" — no Fusion this session to export a real one) | ~0.04s |
| `trimesh` containment probe, 2000 random points against that 15,634-triangle mesh (builds the ray-intersection tree, cold) | ~0.50s |
| Same, tree already warm | ~0.41s |
| Single-point containment probe once the tree is warm | ~0.0003s |

**Compare to Fusion:** Jake's own numbers — 5-10 minutes per full
rebuild+gate cycle through the MCP bridge, calls capped at 60s (forcing
the generator to be chunked across many calls), the bridge itself
collapsing after ~1000 script calls, only one agent able to hold it at a
time, and it locks Jake out of his own interactive Fusion session while
in use. Even generously assuming the **full** generator (8,371 lines,
vs. this spike's ~330 ported lines) takes 10-20x longer to build than
this slice once fully ported — call it 15-25 seconds instead of ~1.1s —
that is still on the order of **15-40x faster** than the low end of
Fusion's observed 5-minute cycle, with zero MCP round-trips, zero 60s cap
to chunk around, and it never touches Jake's own Fusion session at all.

## 4. Fidelity check

Compared against the read-only reference export at
`/private/tmp/claude-501/case-pass16/hardware/case/export/trim/Top.stl`
(the *full* generator's Top body for the `trim` variant — not a slice, so
volume is expected to differ; bounding box and the lip/anchor-ring region
are the meaningful direct comparisons since they don't depend on the
unported features).

| metric | this port | pass-16 `Top.stl` | diff |
|---|---|---|---|
| bbox X | -28.00 .. 28.00 | -28.00 .. 28.00 | exact (hard param, `outer_radius=28`) |
| bbox Y | -27.92 .. 79.72 | -28.00 .. 79.78 | 0.06-0.08mm at the tips — within STL tessellation tolerance (this port exported at 0.05mm linear deflection; not necessarily the same tolerance Fusion used) |
| bbox Z | 9.20 .. 28.00 | 9.20 .. 28.00 | exact (hard params `lip_z[0]=9.2`, `top_z=28`) |
| volume | 18,540 mm³ (exact B-rep) / 18,451 mm³ (STL mesh estimate) | 19,319 mm³ | ports ~4.3-4.7% *lower* — fully explained: pass-16's Top also has the top posts, comms-bay/antenna-channel walls, USB tunnel, buttons, mag-module mount, FPC-relief cut, and the full lug ear, none of which this slice ported |
| manifold (`offline_stl_check.check_manifold`) | 0 non-manifold edges, watertight | 0 non-manifold edges, watertight | match |
| overhang scan (`offline_stl_check.scan_stl_overhangs`, real `TOP_WL` whitelist) | 0 bad clusters >30mm²; two ~0.03mm² unlabeled clusters at the corner-block wedge/capsule seam (±14.4, -17.3) | 0 bad clusters >30mm² | both pass the gate; this port's two tiny clusters are 3 orders of magnitude under the 30mm² threshold and are a known, minor artifact of *not* porting `chamfer_stadium_edge_at`'s ring-seam finesse for this representative slice, not a defect in what *was* ported |
| lip/anchor ring (r 23.95-26.40, z 9.2-11, 0.5mm seam chamfer) | present, correct band, no gate failure | present, no gate failure | match |

No unexplained geometric discrepancy: every difference above is either
tessellation-level noise or a direct, listed consequence of this being a
representative slice, not the full generator.

## 5. Effort estimate for a full port

`firefly_case.py` is **8,371 lines / 203 top-level functions** at this
branch's `main` (the brief's "~5000 lines" undercounts today's file by a
good margin — it has grown since). Classified by scanning each
function's body for the idioms below:

| category | lines | % | functions | what's in it |
|---|---|---|---|---|
| plain geometry | 3,837 | 46.3% | 133 | sketches/extrudes/revolves/booleans/chamfers — the shell/window/lip-ring/corner-block work this spike actually ported |
| pure Fusion-quirk workaround | 1,609 | 19.4% | 21 | `dedupe_body`/`_refetch_by_name` (stale body references after a Remove feature), best-effort-fillet-then-conical-collar fallback (`add_root_reinforcement`'s own docstring explains why: a real Fillet feature only matched *some* of a clipped pillar's disconnected root edges), `except RuntimeError: pass` guards, `ObjectCollection`/`Matrix3D` boilerplate |
| verify/check gates | 1,724 | 20.8% | 26 | `verify_*`/`check_*` — most (~16 of 26) are pure-Python analytic checks or point/ray probes against the generator's own solids, which port close to 1:1 (this spike's `pilot_wall_probe`/`interference_volume` demonstrate the pattern); the rest also touch inserted-component occurrences |
| Fusion-assembly-specific | 1,121 | 13.5% | 12 (+9 overlapping the two buckets above) | inserting referenced components (display PCBA, XIAO, Wio, L76K Fusion documents) by occurrence, reading their transforms, live interference-checking against them |

This spike ported ~330 lines of real generator logic (`shell.py` +
`features.py`) — squarely in the "plain geometry" bucket, the easiest and
largest of the four — in a few focused hours including all the
exploratory reading of `firefly_case.py`/`params_current.py`/
`params_trim.py` needed to find the right functions and numbers. Two
findings shape the estimate:

1. **The 19.4% "workaround" bucket should mostly disappear, not get
   translated.** This spike's booleans never silently no-op'd on
   non-touching bodies, never left an orphaned duplicate body, and never
   produced a partially-filleted edge loop — the exact three Fusion
   behaviors `dedupe_body`, `_refetch_by_name`, and
   `add_root_reinforcement`'s collar-not-fillet design exist to work
   around. A real port gets to just trust the boolean instead of
   defensively re-fetching/deduping after every one.
2. **The 13.5% "Fusion-assembly" bucket is the one real blocker**, and it
   has a clean, one-time bypass: `build123d.import_step`/`export_step`
   are both present (confirmed, not assumed, in this session's venv), so
   exporting each referenced Fusion document (display, XIAO, Wio, L76K)
   to STEP **once** gives real B-rep solids to probe/boolean against
   headlessly, exactly the way this spike's `mesh_point_containment`
   stood in for that using the existing pass-16 `Top.stl` as a mesh
   proxy.

**Rough order of magnitude:** 3-6 focused days for the plain-geometry
46% (mechanical and well-understood after this spike), 1-2 days to
delete rather than port most of the 19% workaround code, 2-3 days for
the verify/gate suite (leaning on the `pilot_wall_probe`/
`interference_volume` pattern), and a harder-to-size 2-4 days for the
occurrence/assembly 13.5% once the STEP exports exist, plus one Fusion
session to actually produce those STEP files. Call it **roughly 2-3
calendar weeks** of focused, regression-gated work for a careful
feature-by-feature port — a real project, not an afternoon, but bounded
and well short of a from-scratch rewrite.

## 6. Migration plan (recommended)

1. **One-time, in Fusion:** export STEP for the display module, XIAO,
   Wio, and L76K reference documents — the only things
   `insert_comms_boards`/`add_lug`/the interference checks actually need
   Fusion's document/occurrence model for. Keep Fusion open afterward
   purely as a **viewer** (open the exported STEP/3MF to look at it), not
   as the generator's driver.
2. **Port feature-by-feature**, in roughly the order `firefly_case.py`
   itself builds: shell → window → lip/anchor ring → case screws/bosses
   → top posts → buttons → comms bay/antenna channels → mag module →
   lug/corner blocks. Gate every feature on: (a) `offline_stl_check.py`
   unchanged (it already needs no porting — pure Python); (b) a
   `build123d` point/ray-probe port of that feature's own `verify_*`
   function, following `gates.py`'s `pilot_wall_probe` pattern; (c) a
   diff against the matching pass-16 (or later) STL — volume, bounding
   box, overhang scan — exactly this spike's Section 4 method, used as a
   regression golden.
3. **Delete, don't translate**, `dedupe_body`/`_refetch_by_name`/the
   fillet-then-collar fallback as each feature moves over.
4. **Never re-add Fusion to the build loop.** Jake keeps his own
   interactive Fusion session free the whole time; the generator's own
   iteration loop (edit → rebuild → gate) runs in a persistent `uv`-managed
   venv at ~1-2 second cycle time instead of 5-10 minutes through the MCP
   bridge, with no 60s call cap and no ~1000-call bridge-collapse ceiling
   to manage.
5. New tool stack for the generator: `build123d` (kernel) + a `case/`
   package shaped like this spike's (`shell.py`/`features.py`/
   `gates.py`/`export.py`) + the existing `offline_stl_check.py` unchanged
   + the pass-16-and-later STLs as regression goldens.

## Spike code

`hardware/case/spike_headless/` — `case/{params,shell,features,gates,export}.py`,
`build_slice.py` (driver), `.venv` (git-ignored — recreate with
`uv venv .venv --python 3.12 && source .venv/bin/activate && uv pip install build123d trimesh matplotlib numpy rtree`),
`out/` (exported `Top_slice.stl`/`.3mf` + a render, git-ignored).
