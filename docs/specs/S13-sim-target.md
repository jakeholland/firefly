# S13 · sim target — the dev loop (BUILD FIRST)

## Purpose
Run the whole app on a Mac/Linux host: LVGL in an SDL window or fully headless, TCP transport to meshtasticd, screenshot export, state injection. Everything else's testability depends on this.

## Deliverables
1. **CMake superbuild**: top-level `CMakeLists.txt`, `FF_TARGET=sim`; FetchContent pins: LVGL v9.x, SDL2 (system or fetched), Unity, nanopb, jsmn — all commit-pinned.
2. **`ffsim` binary** flags:
   - `--window` (default): SDL 456×456 with bezel chrome, mouse=touch, arrow keys=swipe, `s` saves screenshot.
   - `--headless --screenshot DIR`: renders each fixture state once → `DIR/<fixture>.png`, exits.
   - `--fixture FILE.json`: loads an `ff_app_state` fixture (JSON schema documented in `tests/fixtures/README.md`) instead of live wiring.
   - `--connect HOST:PORT`: live mode via mc TCP transport; `--pack FILE` preloads festpack.
   - `--script FILE.py`⇢ no — scripting stays external: sim exposes a control socket (`--ctl PORT`, newline JSON: inject touch, advance mock clock, dump state, screenshot) that the Python e2e harness drives.
3. **PNG writer**: LVGL snapshot buffer → PNG (stb_image_write, vendored single header).
4. **Mock clock**: `--mock-clock` makes time advance only via ctl socket (determinism for goldens/alarms).
5. **`tools/dev/`**: `compose.yml` (meshtasticd), `crew_sim.py` (Meshtastic Python API: spawn fake nodes, walk paths at Legend Valley coords, send flares — "Dana walks 300 m NE at 1.2 m/s" one-liners; the `flare` verb replaced `pulse` 2026-09-02, PULSE retired, see docs/specs/S04-firefly-protocol.md's Amendments), `record_fixture.py` (captures S03 handshake bytes).

## Acceptance criteria
1. `cmake -B build -DFF_TARGET=sim && cmake --build build` clean from fresh clone on macOS + Ubuntu (CI proves Ubuntu; macOS documented).
2. Headless renders `radar_live.json` → identical PNG across two runs (determinism: fixed seed, mock clock, no AA nondeterminism).
3. Ctl socket: inject tap → state dump reflects selection change; screenshot-on-demand works.
4. `crew_sim.py` against dockerized meshtasticd: `ffsim --connect` reaches mc READY and receives ≥1 position (smoke, CI-nightly).
5. Window mode runs at ≥30 fps with radar animating (manual criterion, noted in PR).

## Slices
a) CMake superbuild + LVGL SDL window + PNG dump · b) fixtures + headless + mock clock · c) ctl socket · d) tools/dev harness.

## Amendments

- **2026-08-24, PR (S16 slice b2) — `live.{c,h}` is retired; `--connect`
  is re-pointed at the app shell.**

  Slice (c) shipped `targets/sim/live.{c,h}` as interim wiring for
  `--connect`/`--pack`, self-documented as a "deliberate, PR-flagged
  spec-gap deviation": the S06+ orchestration layer did not exist yet,
  and the ctl socket's `state` dump and the S14 e2e scenarios were
  meaningless without *something* live to dump. That gap is now closed
  for real — S16's `ff_shell_t` is the orchestration layer live.c stood
  in for — so the deviation is closed rather than left dangling, per
  S16's "Amendments to prior specs".

  What changed for this spec's deliverables:
  - `--connect HOST:PORT` drives an `ff_shell_t` over the mc TCP
    transport; the ctl `state` dump reads `ff_shell_view()` and gains a
    `"wall"` key from `ff_shell_wall()` (tools/dev/CTL.md).
  - live.c's "every heard node is auto-paired" behavior — its stated
    only-honest-option stopgap — does NOT survive by default: the shell
    enforces S16's roster trust policy. The dev harness opts back into
    trust with `--dev-trust-all` (sim-only, compiled out on device,
    logged at startup; S16 AC6 and its Amendments).
  - live.c's venue-origin-as-my-position and fixed-north-heading dev
    stand-ins survive, but in `main.c`, visibly, through the shell's
    public sensor seam (`ff_shell_set_my_pos`/`ff_shell_set_heading`) —
    the shell itself refuses to fabricate either (S16b1's ruling).
  - Criterion 4's smoke ("reaches mc READY, receives ≥1 position") is
    superseded in practice by firmware/tests/e2e's
    `test_position_reaches_radar`, which now runs with
    `--dev-trust-all`.
