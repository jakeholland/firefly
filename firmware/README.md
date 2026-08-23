UI-brain firmware (LVGL + Meshtastic client API). Starts when boards arrive — develop against the LVGL SDL simulator + meshtasticd.

## Layout

```
firmware/
  core/         firefly-core: pure C11 domain logic (no I/O, no LVGL)
  meshclient/   Meshtastic client library (skeleton — S03)
  festpack/     festpack.json parser (skeleton — S05)
  app/          ff_app_state.h (S13b) + LVGL screens (skeleton — S06+)
  targets/sim/  ffsim — desktop sim target (S13); fixture.h/fixture_view.h (S13b)
  tools/        compare_png — golden-screenshot pixel diff (S14 slice b)
  tests/        tests/fixtures/*.json, tests/golden/*.png, run_goldens.sh (S13/S14 slice b)
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
- `test_fixture` (S13b) — `targets/sim/fixture.h`'s JSON loader: the
  three committed radar fixtures parse to their exact documented values,
  IO/JSON error paths, missing-section defaults, `ff_fixture_stem`.
- `test_png_diff` (S14 slice b) — `tools/png_diff.h`'s pixel-diff core,
  including the exact 0.4%/0.5%/0.6% threshold boundary.
