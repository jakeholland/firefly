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
