# S12 · first-run flow (stretch for Lost Lands)

## Purpose
Out-of-box: name → pack → calibrate. Mockups "First run — name", "First run — pack", "Calibrate" are layout authority. SoftAP portal is v2; v1's pack step shows beam/skip only (portal row hidden behind `FF_FEATURE_PORTAL=0`).

## Behavior
- Trigger: `my_name` unset at boot → flow; skippable except calibration prompt (can defer with "arrow may be wrong" warning chip on radar until calibrated).
- Step 1: T9 name entry (reuses S08 engine), 2–12 chars, saved to settings + pushed to comms brain as Meshtastic owner short/long name (admin message; long = name, short = first 4).
- Step 2: pack: list packs found in storage (sim: `--pack path` flag preloads; device: bundled Lost Lands pack in firmware assets v1) + "beam from a friend" row = **placeholder toast "coming soon" v1** (honest, greyed).
- Step 3: calibration ritual: live progress from `ff_geo_cal_progress_pct`, figure-eight art, completes → save cal → Radar.
- Re-entry: settings hidden row "run setup again".

## Acceptance criteria
1. Flow state machine: fresh boot enters; named boot skips; skip paths land on radar with warning chip; defer-calibration chip clears after later calibration.
2. Name rules: length clamp, charset A–Z0–9 space; pushed admin message captured by mock mc.
3. Cal progress renders live from fed synthetic samples (sim input script), completes at ≥70% coverage.
4. Goldens: `firstrun_name.json`, `firstrun_pack.json`, `calibrate_64.json`.

## Slices
a) flow machine + name step · b) pack step + bundled pack plumbing · c) calibrate step wiring.

## Amendments

- **2026-09-03 — audit finding: compass calibration has no UI and no IMU
  driver yet (note only).** The Settings-row audit
  (`feat/settings-audit-sections`, see
  `docs/specs/S21-settings-rework.md`'s own 2026-09-03 Amendment and
  `docs/specs/S11-settings.md`'s matching one for the row-level findings)
  swept this spec too while tracing every consumer of
  `ff_geo_cal_t`/`compass_cal`: **none of this file's three slices have
  landed.** There is no first-run flow, no Step 3 calibration screen, and
  — the harder gap — no IMU/magnetometer driver anywhere under
  `firmware/targets/esp32s3/`, so `ff_geo_cal_progress_pct` (core, pure
  math, already implemented per `ff_geo.h`/`ff_geo.c`) has no live sample
  source to report progress on even if a screen existed to show it. This
  is unrelated to S21's **Calibrate Touch** row (touch-panel affine
  correction, `ff_display_run_calibration`) — that one is real, shipped,
  and NVS-persisted; this spec is the COMPASS ritual (heading/orientation,
  the figure-eight gesture), a different sensor, a different calibration,
  and a different (nonexistent) driver.

  This is a note, not an implementation: no slice of S12 was worked in
  `feat/settings-audit-sections`, and nothing here changes the "stretch
  for Lost Lands" status this file's own title already carries. Recorded
  so the next agent who reaches for `ff_geo_cal_progress_pct` expecting a
  live compass finds this pointer instead of re-discovering the gap from
  scratch.
