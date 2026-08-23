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
