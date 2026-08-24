# Firefly — agent context

Open-source festival friend-compass puck. Dual-brain: a Seeed XIAO+SX1262 comms brain runs STOCK Meshtastic; the UI brain (Waveshare ESP32-S3-Touch-LCD-1.46, 412×412 round) runs this repo's app and speaks Meshtastic's client API over UART (dev: TCP to meshtasticd). Field-test deadline: Lost Lands, Sep 18–20 2026.

## Ground rules

- **Specs are the contract.** Every feature has a spec in `docs/specs/`. Implement to the spec's acceptance criteria; if a spec is ambiguous, note the interpretation in the PR body — do not silently invent behavior.
- **Workflow:** see `AGENTS.md`. Branch → implement + tests → local build green → PR → merge on CI green. Never commit to main directly.
- **All logic goes in `firmware/core/` (pure C11, zero deps, no I/O, no LVGL).** UI code only renders core state and forwards input. If you're writing an `if` about domain behavior inside a screen file, it belongs in core.
- **Interfaces first:** modules talk through the headers in `*/include/`. Changing a public header is a spec change — flag it in the PR title with `[api]`.
- **Tests required:** unit tests (Unity/ctest) for core changes; golden-screenshot test for UI changes. UI PRs must attach rendered PNG screenshots (the sim target dumps them — see docs/specs/S13).
- **Honest data over pretty data:** unknown = explicitly unknown (null, stale flag). Never fake freshness, positions, or times. This is a product value, enforced in review.
- Design references: screen mockups and plan live as Claude artifacts (ask Jake) — the mockup geometry is authoritative for layouts (412×412 circle, colors, type: Unbounded/Outfit/Spline Sans Mono → device fonts per S06).
- Festival data comes from github.com/jakeholland/fest-almanac (`festpack.json`, schema v0.1). Never hardcode festival content outside test fixtures.

## Build

- Desktop sim + tests: `cmake -S firmware -B build -DFF_TARGET=sim && cmake --build build -j8 && ctest --test-dir build --output-on-failure`
- **The local gate above uses the host compiler — clang on macOS — while CI runs GCC, and CI is the authority** (#44, found the hard way in PR #41: GCC's `-Wstringop-truncation` has no clang equivalent, so the local gate was green and could never have been anything else). Read every local "clean under `-Wall -Wextra -Werror`" claim as "clean under clang's interpretation", and say which compiler you built with in the PR body. For changes touching string handling or arithmetic, run a second local build under Homebrew GCC before pushing — it must be **`gcc-14`** explicitly (`brew install gcc@14`), because plain `gcc` on macOS is Apple clang in disguise: `CC=gcc-14 cmake -S firmware -B build-gcc -DFF_TARGET=sim && cmake --build build-gcc -j8 && ctest --test-dir build-gcc --output-on-failure`
- Screenshots: `./build/ffsim --headless --screenshot out/` (the sim creates the output dir itself — #3)
- Device target (`FF_TARGET=esp32s3`) arrives with S15; keep core compiling under `-Wall -Wextra -Werror` both ways.
