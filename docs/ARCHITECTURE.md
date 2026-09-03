# Architecture

## The one diagram

```
┌─────────────────────────── UI brain (this repo) ───────────────────────────┐
│  app/           LVGL screens: render core state, forward input. No logic.  │
│  core/          firefly-core: PURE C11, zero deps, all domain logic        │
│  meshclient/    Meshtastic client lib: protobuf framing over a transport   │
│  festpack/      festpack.json subset parser (spec: fest-almanac v0.1)      │
│  targets/sim    SDL window or headless PNG · transport = TCP→meshtasticd   │
│  targets/esp32s3  ESP-IDF · transport = UART → comms brain                 │
└────────────────────────────────────────────────────────────────────────────┘
                                   │ Meshtastic client API (protobufs)
┌─────────────── Comms brain ──────▼─────────────────────────────────────────┐
│  STOCK Meshtastic (XIAO ESP32S3 + Wio-SX1262 + L76K GPS). Not our code.    │
│  Dev stand-in: meshtasticd (native/Docker) — same API over TCP.            │
└────────────────────────────────────────────────────────────────────────────┘
```

## Principles

1. **Core is pure.** `firefly-core` has no I/O, no allocation surprises (caller-provided buffers or a fixed arena), no LVGL, no ESP-IDF. Everything interesting — bearing math, staleness, schedule queries, T9, protocol encode/decode — is a pure function or an explicit state machine. Consequences: unit-testable on any host, compiles to WASM for future web tools, C ABI bindable from Swift (the archived iOS app could become a companion) and Python (test harness).
2. **Interfaces are vtables + plain structs.** Cross-module boundaries are C structs of function pointers (`ff_transport_t`, `ff_clock_t`, `ff_store_t`) injected at init. No globals in libraries; each module exposes `ff_x_init(ctx, deps)` / `ff_x_tick(ctx, now_ms)` / events out via callback.
3. **The UI is a projection.** Screens read a single `ff_app_state_t` snapshot and render it. Input handlers translate touches into core commands. This is what makes golden-screenshot testing deterministic: same state in, same pixels out.
4. **One codebase, two targets.** `FF_TARGET=sim` (SDL2 or headless framebuffer→PNG) and `FF_TARGET=esp32s3` (ESP-IDF component build). Platform code lives only under `targets/`; a `ff_hal.h` seam covers clock, storage, transport, sensors.
5. **Stock radio, versioned app protocol.** We never fork Meshtastic. Firefly-specific packets (pulse/flare/rally/status) ride a private portnum with a 1-byte version prefix (spec S04). Old and new pucks must coexist at a festival.
6. **Honest state.** Unknowns are represented, not papered over: every position carries `age_ms`; every schedule time is nullable; the UI is required to render staleness (spec S06).

## Reusable libraries (deliberate extraction seams)

| Lib | What it is | Who else would want it |
|---|---|---|
| `meshclient` | Embedded C Meshtastic client (framing, want_config handshake, nodeDB, messaging) over a transport vtable | Any MCU project driving a Meshtastic node — no clean C lib exists today |
| `core/t9` | Predictive multi-tap T9 engine with pluggable dictionary | Any tiny-screen device |
| `core/geo` | Bearing/distance/tilt-compensated-heading kit | Any compass gadget |
| `festpack` | Parser for the open festpack format | Anything consuming fest-almanac |

They live in-tree now (one repo, fast iteration) with **no cross-includes except through public headers** — extraction later is `git mv` + a CMake file, not surgery.

## Languages

C11 for firmware and libs (LVGL/ESP-IDF ecosystem, C ABI = the multi-language interface). Python for the sim/e2e harness and tooling. TypeScript later for `web/`. Rust was considered for core and declined: esp-rs is viable but doubles toolchain friction against LVGL/ESP-IDF for no v1 gain; the C ABI seam keeps the door open.

## Testing strategy

- **Unit (ctest + Unity):** core logic, table-driven, criteria-numbered per spec.
- **Golden screenshots:** sim renders known `ff_app_state_t` fixtures headlessly → PNG → pixel-diff against `tests/golden/`. Update goldens deliberately via `--update-golden`, reviewed in PR diffs (PNGs render in GitHub).
- **E2E scenarios (Python + meshtasticd):** docker-compose runs meshtasticd; `tests/e2e/` scripts inject nodes/positions/messages via the Meshtastic Python API ("Dana walks 300m NE"), drive the headless sim over its TCP transport, assert on exported state + capture screenshots.
- **CI (GitHub Actions):** sim build + unit + goldens on every PR (merge gate); esp32s3 build job added with S15 to prevent sim-only drift; e2e nightly + on-demand label.

## Repo layout

```
firmware/
  core/         include/  src/  tests/            pure C11 domain logic
  meshclient/   include/  src/  tests/  proto/     Meshtastic client lib
  festpack/     include/  src/  tests/             festpack.json parser
  platform/     include/                           shared types (ff_clock_t, ff_latlon_t)
  app/          screens/  theme/  include/  tests/ LVGL UI: renders core state, forwards input
  targets/      sim/  esp32s3/
    esp32s3/components/  ff_core  ff_meshclient  ff_meshclient_proto  ff_festpack
                          ff_platform  ff_power  ff_display  ff_app  ff_app_ui
                          ff_jsmn  ff_nanopb  esp_lcd_touch_spd2010
  tools/        compare_png/png_diff (goldens)  social/ (render_scene.py)  dev/ (crew_sim, ctl docs)
  tests/        golden/  fixtures/  e2e/ (pytest + meshtasticd)
  assets/       demo/ (S20's embedded demo festpack)
  third_party/  vendored: jsmn.h, stb_image*.h — FetchContent pins: lvgl, unity, nanopb
docs/
  specs/        S01..S26 feature specs (the contracts)
  screens/      committed sim screenshots (PR artifacts)
  hardware/     bring-up notes (glass offset, bench rig)
  review/       code-review.md, ux-raver.md review personas
web/            (stub — README.md only so far) flasher + setup page
case/           (stub — README.md only so far) enclosure CAD/STL
```
