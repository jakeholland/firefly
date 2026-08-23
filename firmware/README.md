UI-brain firmware (LVGL + Meshtastic client API). Starts when boards arrive — develop against the LVGL SDL simulator + meshtasticd.

## Layout

```
firmware/
  core/         firefly-core: pure C11 domain logic (no I/O, no LVGL)
  meshclient/   Meshtastic client library (skeleton — S03)
  festpack/     festpack.json parser (skeleton — S05)
  app/          LVGL screens (skeleton — S06+)
  targets/sim/  ffsim — desktop sim target (S13)
  third_party/  vendored single-header libs (stb_image_write.h)
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
```

Headless mode never touches SDL or a display server — it renders straight
into an LVGL software framebuffer and writes it out with stb_image_write.
This is the path CI uses to produce the boot screenshot artifact.

## Tests

Unit tests use Unity and run via ctest, one test executable per module
(`test_core`, ...). Test names are criteria-numbered per spec
(`SNN_ACX_description`), per `AGENTS.md`.
