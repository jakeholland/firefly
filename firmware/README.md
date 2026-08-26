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
                ctl_server.h/.c (S13c control socket); --connect/--pack drive the
                app shell (S16b2 — the interim live.h/.c wiring is retired)
  targets/esp32s3/  ESP-IDF project, real ESP32-S3 hardware (S15). Slice a
                (this one): the project skeleton — components/ wrap
                core/festpack/meshclient/app UNCHANGED, main/app_main.c
                boots the shell against stub HALs, no peripherals touched.
                See "Build — ESP32-S3 (device)" below.
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

## Build — ESP32-S3 (device, S15 slice a)

```sh
source ~/esp/esp-idf/export.sh   # or: . ~/esp/esp-idf/export.sh
# first time only, if the xtensa toolchain isn't installed yet:
#   ~/esp/esp-idf/install.sh esp32s3
cd firmware/targets/esp32s3
idf.py set-target esp32s3   # only needed once, or after deleting build/
idf.py build
idf.py -p <PORT> flash monitor   # once the board is on the bench
```

This is a SEPARATE build entry point from the `cmake -S firmware ...`
sim build above — `idf.py`, not `cmake --build`. It does not
`add_subdirectory()` the sim tree; every component under
`targets/esp32s3/components/` points its `SRCS`/`INCLUDE_DIRS` straight
at the existing `firmware/{core,festpack,meshclient,app}` sources, so
those compile for xtensa UNCHANGED — literally the same `.c` files the
sim target builds. `main/app_main.c` boots `ff_shell_t` against stub
HALs (`esp_timer`-backed clock, a no-op store, no transport) and never
touches a peripheral — the display/touch/UART/sensor drivers are slices
b/c/d. See `docs/specs/S15-esp32s3-target.md` and the S15a PR body for
the full slice breakdown and every host-portability finding this port
surfaced.

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
Meshtastic connection driving the app shell (`app/include/ff_shell.h`,
S16b2) so `{"cmd":"state"}` dumps reflect `ff_shell_view()` — real mesh
traffic filtered by the roster trust policy — instead of a static
fixture; add `--dev-trust-all` (sim-only, compiled out on device) to
auto-pair the dev daemon's NodeInfo senders, see `tools/dev/CTL.md`;
`--pack FILE` anchors "my position" at a festpack's venue origin (the
sim has no real GPS); `--ctl-out DIR` sets where the `screenshot` command may write
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
  S16 slice b2 added the AC6 cutover tests: one drives the shell's own
  `mc_client_t` through a scripted in-memory transport with hand-encoded
  protobuf frames (the retired `test_live`'s technique, now proving the
  exact pipeline `ffsim --connect` ships), plus the `--dev-trust-all`
  dev-affordance semantics and the on_my_info heard-purge.
- `test_png_diff` (S14 slice b) — `tools/png_diff.h`'s pixel-diff core,
  including the exact 0.4%/0.5%/0.6% threshold boundary.
- `test_wall` (S16 slice b0; trust gate S18 slice a) — `core/ff_wall.h`'s
  wall-clock derivation: the `FF_WALL_UNKNOWN` honesty discipline, the
  plausibility window's four boundaries, offset-latch/re-latch, UTC-offset
  resolution order, and the `unix -> local -> (day_doy, now_min)`
  festival-day mapping. Also `ff_wall_observe`'s trust tier (issue #49):
  a BOOTSTRAP-tier disagreement can never move an established latch (the
  second-stranger scenario), only TRUSTED can, and the pre-existing
  expired-latch/backwards-monotonic branch stays deliberately trust-blind.
  S18 slice c (#40) adds the pack-derived plausibility window: the effective
  gate window defaults to the fixed bootstrap bounds and tightens to a
  loaded pack's dates via `ff_wall_set_window`; `ff_wall_unix_from_doy` is
  the exposed civil-date primitive the derivation shares; and
  `ff_wall_ceiling_deadline_near` is the build-date proximity guard
  (synthetic-date tested, never the real clock). The festpack->window
  derivation itself (`ff_wall_window_from_pack`, honest fallback for a
  null-dated pack) is `test_wall_window` (app), and the shell wiring
  (load_pack tightens the window; a Sep 2029 stamp inside the fixed window
  is rejected once Lost Lands loads) is in `test_shell`.
  The date math additionally has an out-of-band cross-check against
  Python's `datetime` — `tools/dev/wall_crosscheck.py`, not a ctest (it
  needs a compiler and Python at once); run it after touching the
  civil-date code.

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
  - S18 slice c (#40) narrowed *when* this matters and added a backstop.
    Once a pack loads, the plausibility window tightens to the festival's
    own dates (`ff_wall_window_from_pack` -> `ff_wall_set_window`), so the
    fixed window's decay only affects the **no-pack bootstrap** path (the
    want_config handshake). And that decay is no longer fully silent:
    `ffsim` prints a loud, dated warning at startup once the **build date**
    is within 12 months of the ceiling (`ff_wall_ceiling_deadline_near`,
    surfaced by CI's headless-screenshot step). It is still a *warning*,
    not a pass/fail — a build's verdict must not depend on the day it ran
    — so this bump remains a human release step, now with a year of
    tracked runway instead of nothing.

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
