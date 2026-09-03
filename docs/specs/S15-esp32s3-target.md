# S15 · esp32s3 target — the real device (when boards arrive)

## Purpose
Same app, real hardware: ESP-IDF component build for the Waveshare ESP32-S3-Touch-LCD-1.46 UI brain + UART transport to the comms brain.

## Deliverables
1. **ESP-IDF project** under `targets/esp32s3/` (IDF 5.x): components wrap core/meshclient/festpack/app unchanged; `idf.py build flash monitor` works.
2. **Display+touch driver**: SPD2010 QSPI panel + touch via Waveshare's reference code, adapted into an LVGL v9 display/indev driver at 412×412; target ≥25 fps radar.
3. **HAL implementations**: `ff_clock` (esp_timer), `ff_store` (NVS), transport UART (GPIO43/44 ⇒ comms brain D6/D7, 115200, matching Meshtastic Serial Module PROTO mode), QMC5883L driver (I2C) + QMI8658 accel (onboard) feeding `ff_geo_heading_deg`, PWM haptics stub (pin per case design), backlight control.
4. **Power scaffolding v1**: screen-off on 20 s idle, tilt/tap wake via QMI8658 interrupt; light-sleep tuning is post-Lost-Lands (issue).
5. **Bundled assets**: Lost Lands festpack embedded via `EMBED_FILES`.
6. **Comms-brain setup doc** (`docs/comms-brain.md`): flash stock Meshtastic "Seeed XIAO S3", enable Serial Module PROTO on D6/D7, region US, channel setup — with exact CLI commands.
7. CI: `esp32-build` job (build-only) becomes a PR gate.

## Acceptance criteria (bench, manual+scripted)
1. Boots to Radar; goldens visually match sim (photo in PR).
2. mc READY over UART against real comms brain; position from T1000-E rabbit appears ≤10 s.
3. Compass: calibrated heading within ~5° of phone compass at 4 cardinal points.
4. Touch: all S06/S08 interactions work; T9 typing usable.
5. 25+ fps radar; no watchdog resets over 30 min soak.
6. CI esp32 build green.

## Slices
a) IDF skeleton + core compiling · b) display/touch driver · c) UART transport + bench handshake · d) sensors + calibration · e) idle/wake + assets.

## Risks (known, accepted)
SPD2010 LVGL driver is the long pole (Waveshare reference exists but quality varies); UART PROTO mode config has community-reported quirks — fixture-record early on bench. Both were chosen consciously over forking Meshtastic.

## Amendments
- **2026-09-02, PR #172 (`debt/ci-gates`) — deliverable 7 / AC6 finally implemented, plus two `sdkconfig.defaults` hygiene items.**
  - **`esp32-build` job.** Added `.github/workflows/esp32.yml` (build-only, `espressif/esp-idf-ci-action` pinned to a commit SHA, `esp_idf_version: v5.3.5`). Builds a 2-leg matrix: `sdkconfig.defaults` alone (the fresh-checkout STAGE-1 bring-up build this target has always defaulted to) and `sdkconfig.defaults;sdkconfig.ci` — a new committed overlay applied via `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" build` that layers `sdkconfig.ci`'s keys on top, selecting `CONFIG_FF_BRINGUP_STAGE=3` + `CONFIG_FF_DEMO_MODE`/`CONFIG_FF_DEMO_LIVE` to match the maintainer's real board sdkconfig (that file is untracked and was never committed; its FF_* values were read directly off it and copied into `sdkconfig.ci` as an explicit, reviewable overlay — see that file's own header). Rationale for the second leg: STAGE 1 alone never links the LVGL/display/touch/demo code the device actually ships with, so a break there had zero CI coverage. Both legs verified locally against the maintainer's own ESP-IDF v5.3.5 checkout before this PR: STAGE-1-defaults built `firefly_esp32s3.bin` at `0x62e20` bytes (74% of the app partition free); the `ci-shipping` leg built at `0xeab20` bytes (37% free) — sizes recorded in the PR body. **Not** added to branch protection as a required check yet — see this PR's body; the maintainer enables that once it's been watched running green.
  - **`sdkconfig.defaults`: `CONFIG_LV_BUILD_EXAMPLES=n` / `CONFIG_LV_BUILD_DEMOS=n` added.** Espressif's registry `lvgl` component defaults both to `y`; confirmed against the maintainer's real board sdkconfig that both were still `y` there (258 of that build's 1910 linked objects are LVGL's own `examples/`/`demos/` trees, with zero callers anywhere in this app). Off by default now, matching what the sim's `firmware/CMakeLists.txt` already forces for FetchContent's lvgl.
  - **Drift-rule documentation added to `sdkconfig.defaults`'s header.** A generated `sdkconfig` (untracked, board-specific — never commit it) does NOT pick up a new `sdkconfig.defaults` entry added after that file first exists: Kconfig only fills in a default for a key ABSENT from `sdkconfig`; a key already present — even as `# CONFIG_FOO is not set` — is left alone, and `idf.py reconfigure` does not change that. The key must be deleted from the board's `sdkconfig` (or the file regenerated via `idf.py fullclean` + reconfigure) for a new default to actually take effect. Live example cited in the comment: `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` (added in PR #98) never reached the maintainer's already-generated board sdkconfig, which is still running without it — silently, because the key was already present there (unset) from an earlier configure. This PR does not touch the maintainer's `sdkconfig` (untracked, never edited by CI or an agent branch) — the fix here is documentation only; whether/when to actually delete the stale keys on the real board is the maintainer's call.
