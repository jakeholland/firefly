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
5. **`tools/dev/`**: `compose.yml` (meshtasticd), `crew_sim.py` (Meshtastic Python API: spawn fake nodes, walk paths at Legend Valley coords, send pulses — "Dana walks 300 m NE at 1.2 m/s" one-liners), `record_fixture.py` (captures S03 handshake bytes).

## Acceptance criteria
1. `cmake -B build -DFF_TARGET=sim && cmake --build build` clean from fresh clone on macOS + Ubuntu (CI proves Ubuntu; macOS documented).
2. Headless renders `radar_live.json` → identical PNG across two runs (determinism: fixed seed, mock clock, no AA nondeterminism).
3. Ctl socket: inject tap → state dump reflects selection change; screenshot-on-demand works.
4. `crew_sim.py` against dockerized meshtasticd: `ffsim --connect` reaches mc READY and receives ≥1 position (smoke, CI-nightly).
5. Window mode runs at ≥30 fps with radar animating (manual criterion, noted in PR).

## Slices
a) CMake superbuild + LVGL SDL window + PNG dump · b) fixtures + headless + mock clock · c) ctl socket · d) tools/dev harness.
