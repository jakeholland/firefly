UI-brain firmware (LVGL + Meshtastic client API). Starts when boards arrive — develop against the LVGL SDL simulator + meshtasticd.

## Layout

```
firmware/
  core/         firefly-core: pure C11 domain logic (no I/O, no LVGL)
  meshclient/   Meshtastic client library (skeleton — S03)
  festpack/     festpack.json parser (skeleton — S05)
  app/          ff_app_state.h (S13b) + LVGL screens (S06+)
                ff_route.h (S16a routing), ff_shell.h (S16b1 — the running app:
                lifecycle, mc_events_t callbacks, core->view projection)
  targets/sim/  ffsim — desktop sim target (S13); fixture.h/fixture_view.h (S13b);
                ctl_server.h/.c (S13c control socket), live.h/.c (S13c --connect/--pack)
  tools/        compare_png — golden-screenshot pixel diff (S14 slice b)
                dev/         compose.yml + crew_sim.py + CTL.md — the live dev loop (S13d)
  tests/        tests/fixtures/*.json, tests/golden/*.png, run_goldens.sh (S13/S14 slice b)
                e2e/         pytest e2e scenarios against dockerized meshtasticd (S14d)
  third_party/  vendored single-header libs (stb_image_write.h, stb_image.h)
  lv_conf.h     LVGL configuration for the sim build
```

Third-party dependencies (LVGL, Unity) are pulled via CMake FetchContent and
pinned to commit hashes in `firmware/CMakeLists.txt` — never floating tags.
SDL2 is a system dependency, found via `find_package(SDL2)`.

## Build — macOS

```sh
brew install sdl2 cmake
cmake -B build -DFF_TARGET=sim
cmake --build build
ctest --test-dir build --output-on-failure
```

## Build — Ubuntu / Debian

```sh
sudo apt-get update
sudo apt-get install -y libsdl2-dev cmake build-essential
cmake -B build -DFF_TARGET=sim
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run the sim

```sh
./build/ffsim                                 # SDL window, 456x456
./build/ffsim --headless --screenshot out/     # renders one frame -> out/boot.png, exits 0

# S13b: load an ff_app_state_t fixture (schema: tests/fixtures/README.md)
# and render the placeholder debug face instead of the boot screen.
./build/ffsim --headless --screenshot out/ --fixture tests/fixtures/radar_live.json --mock-clock
./build/ffsim --fixture tests/fixtures/radar_live.json   # same, in an interactive SDL window
```

Headless mode never touches SDL or a display server — it renders straight
into an LVGL software framebuffer and writes it out with stb_image_write.
This is the path CI uses to produce the boot screenshot artifact.
`--mock-clock` freezes the LVGL tick source (always meaningful in window
mode; a no-op-but-honored belt-and-suspenders in headless mode, which is
already deterministic on its own — see `targets/sim/main.c`'s header
comment).

### Live mode + the control socket (S13 slice c/d)

```sh
./build/ffsim --headless --ctl 9000 \
    --connect 127.0.0.1:4403 \
    --pack festpack/tests/fixtures/lost-lands-2026.festpack.json
```

Opens a newline-JSON control socket on `127.0.0.1:9000` (protocol:
`tools/dev/CTL.md`) instead of rendering once and exiting — `tap`/
`swipe`/`clock`/`state`/`screenshot`/`quit` commands, one JSON object per
line in, one JSON line back. `--connect HOST:PORT` wires up a live
Meshtastic connection (`targets/sim/live.h`) so `{"cmd":"state"}` dumps
reflect real mesh traffic instead of a static fixture; `--pack FILE`
anchors "my position" at a festpack's venue origin (the sim has no real
GPS); `--ctl-out DIR` sets where the `screenshot` command may write
(`path` in that command is a relative name confined under this root —
never an arbitrary filesystem path — see CTL.md). See `tools/dev/
README.md` for the whole dev loop (dockerized meshtasticd + `crew_sim.py`
scenario driver) and its documented, verified-against-the-real-container
limitations.

## Golden screenshots

```sh
tests/run_goldens.sh                 # render every tests/fixtures/*.json,
                                      # diff against tests/golden/*.png (<=0.5%)
tests/run_goldens.sh --update-golden # regenerate the committed goldens
```

Also proves determinism: every fixture is rendered twice per run and the
two PNGs must be byte-identical before the golden comparison even runs.
On a golden mismatch, a side-by-side `[golden | rendered | diff]` PNG is
written to `tests/golden/_diff_out/` (gitignored scratch output; CI
uploads it as an artifact on failure — see `.github/workflows/ci.yml`).
Schema and the currently-committed fixtures are documented in
`tests/fixtures/README.md`.

## Tests

Unit tests use Unity and run via ctest, one test executable per module
(`test_core`, `test_settings`, `test_store_file`, ...). Test names are
criteria-numbered per spec (`SNN_ACX_description`), per `AGENTS.md`.

- `test_settings` (S11 slice a) — `core/ff_settings.h`/`ff_store.h`:
  load-with-defaults, save/load round trip, `ff_quiet_now` wraparound,
  water-nudge tick, write-amplification, all against an in-memory mock
  store.
- `test_store_file` — `targets/sim/store_file.h`, the sim's trivial
  file-backed `ff_store_t` (single key-value file), plus an end-to-end
  `ff_settings_t` round trip through the real file store.
- `test_fixture` (S13b/c) — `targets/sim/fixture.h`'s JSON loader/dumper:
  the three committed radar fixtures parse to their exact documented
  values, IO/JSON error paths, missing-section defaults,
  `ff_fixture_stem`, and (S13c) `ff_fixture_dump_json`'s dump-then-reload
  round trip, including JSON-escaped strings and a maximally-populated
  state.
- `test_ctl_server` (S13c) — `targets/sim/ctl_server.h`'s line framing
  (exact `FF_CTL_MAX_LINE` boundary, oversized-line resync) and command
  dispatch (every command's success path and guard paths), plus a real
  loopback-socket end-to-end pass.
- `test_live` (S13c) — `targets/sim/live.h`'s festpack-venue-origin
  loading and a full synthetic mc_client -> crew -> radar/signals
  pipeline test (mock transport, hand-encoded protobuf frames, same
  technique as `meshclient/tests/test_meshclient.c`).
- `test_shell` (S16 slice b1) — `app/ff_shell.h`, the running
  application: the roster trust policy across all seven `mc_events_t`
  callbacks (unknown senders reach `ff_heard`, never the paired roster),
  position ages that come from `rx_time`/`last_heard` and never from the
  local clock (the reconnect-replay defect), haptic-vs-quiet-hours
  composition, and the dirty bit computed over the *rendered* projection.
  No transport or handshake anywhere: events are injected through
  `ff_shell_events()`, the same mock-injector seam `ff_wiring.h`
  documents. Two of its criteria are easy to pass for the wrong reason,
  so both carry an explicit measurement — see the file's header comment.
- `test_png_diff` (S14 slice b) — `tools/png_diff.h`'s pixel-diff core,
  including the exact 0.4%/0.5%/0.6% threshold boundary.
- `test_wall` (S16 slice b0) — `core/ff_wall.h`'s wall-clock derivation:
  the `FF_WALL_UNKNOWN` honesty discipline, the plausibility window's
  four boundaries, offset-latch/re-latch, UTC-offset resolution order,
  and the `unix -> local -> (day_doy, now_min)` festival-day mapping. The
  date math additionally has an out-of-band cross-check against Python's
  `datetime` — `tools/dev/wall_crosscheck.py`, not a ctest (it needs a
  compiler and Python at once); run it after touching the civil-date
  code.

## Release checklist

Things that must be done at a release and cannot be enforced by a test,
because enforcing them in code would make a build's behaviour depend on
the day it was built.

- **Bump `FF_WALL_EPOCH_FLOOR`** (`core/include/ff_wall.h`) to the
  release date. It is the wall clock's lower plausibility bound, and it
  is a *decaying* guard: an uncorrected RTC's reported time drifts
  forward with the calendar while a hardcoded constant does not, so every
  year it is not bumped it catches less — silently, with no test failure
  and no warning. `FF_WALL_EPOCH_CEILING` is derived from it and moves
  along automatically. Forward only; never move either bound backwards.
  (Recorded here per PR #37 review, D3.)

## e2e tests (S14 slice d)

`tests/e2e/` (pytest): docker compose up meshtasticd -> `ffsim --headless
--ctl PORT --connect ...` -> drive scenarios with `tools/dev/crew_sim.py`
-> assert via the ctl socket's state dump. Nightly + `workflow_dispatch`
only (`.github/workflows/e2e.yml`) — not a PR gate, docker's too slow.
See `tests/e2e/test_scenarios.py`'s module docstring for exactly what's
verified working end to end and what's a documented, evidence-based
limitation of testing against a single stock (no-hardware) meshtasticd
instance.

```sh
pip install -r tests/e2e/requirements.txt
cd tests/e2e && pytest -v   # conftest.py brings meshtasticd up/down itself
```
