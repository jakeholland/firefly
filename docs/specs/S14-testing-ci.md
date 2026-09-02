# S14 · testing & CI (BUILD FIRST, with S13)

## Purpose
The merge referee. Agent PRs merge on green — so green must mean something.

## Deliverables
1. **Unit**: Unity via ctest. Convention: one test exe per module (`test_geo`, `test_crew`, …); test names `SNN_ACX_description`. `ctest --output-on-failure`.
2. **Golden screenshots**: `tests/golden/*.png` + `tests/run_goldens.sh` — renders every fixture headlessly, compares with `compare_png` tool (pixel diff, ≤0.5% threshold), emits side-by-side diff PNGs as CI artifacts on failure. `--update-golden` regenerates (deliberate commits only; PR diff shows PNGs).
3. **Sanitizers**: sim/unit builds compile with ASan+UBSan in CI; fuzz smokes (S03/S05) run 10k iters in CI.
4. **E2E** (`tests/e2e/`, pytest): compose up meshtasticd → crew_sim scenarios → ctl-socket assertions + screenshots. Scenarios v1: `test_dana_walk` (position flows to radar view state), `test_flare_reaches_feed` (renamed 2026-09-02 from `test_pulse_feed`/`test_pulse_reaches_feed` — PULSE is retired, see docs/specs/S04-firefly-protocol.md's Amendments), `test_text_roundtrip`. Runs nightly + on `e2e` label (docker too slow for every PR).
5. **GitHub Actions** `ci.yml`:
   - `build-test` (PR gate): Ubuntu, cache CMake deps, build sim, ctest, goldens, upload screenshot artifacts.
   - `esp32-build` (PR gate once S15 lands): espressif/idf docker, build-only.
   - `e2e` (nightly + label).
   - Branch protection expectation: PRs merge via `gh pr merge --squash --auto` after `build-test` green.
6. **`tools/new_module.sh`**: scaffolds module dir + include + test exe + CMake wiring so agents don't hand-copy boilerplate.

## Acceptance criteria
1. Fresh-clone CI run is green with the S13 skeleton (even before feature modules land — placeholder test passes).
2. A deliberately broken golden fails CI and uploads a visual diff artifact.
3. ASan catches a planted heap-overflow in a scratch test (verified once, then removed) — proving sanitizers are actually on.
4. e2e smoke passes against pinned meshtasticd image tag.

## Slices
a) ctest+Unity+ci.yml gate · b) goldens tooling · c) sanitizers+fuzz hooks · d) e2e harness+nightly.
