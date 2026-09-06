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

- **2026-09-06 — Step 3 (compass calibration) lands, as a standalone
  Settings entry point rather than a first-run flow slice.** The
  magnetometer/IMU driver this file's own 2026-09-03 amendment flagged
  as missing shipped the same day (`firmware/targets/esp32s3/
  components/ff_compass`), so Step 3's live sample source now exists.
  This PR closes the ritual itself: a Settings "CALIBRATE COMPASS" row
  (DEVICE section, next to CALIBRATE TOUCH) opens a full-screen page —
  instruction text, a live progress ring/percentage from
  `ff_geo_cal_progress_pct`, CANCEL always, DONE once coverage clears
  `FF_GEO_CAL_MIN_PROGRESS_PCT` — backed by a shell-owned session
  (`FF_INTENT_COMPASS_CAL_START/_CANCEL/_FINISH/_CLEAR`,
  `ff_shell_compass_cal_sample`/`ff_shell_compass_cal_status`,
  app/include/ff_shell.h) and a matching bench-console `cal` command
  family (`cal`/`cal start`/`cal finish`/`cal cancel`/`cal clear`,
  docs/hardware/comms-brain.md) so the ritual can be driven and
  verified over USB before the touchscreen path is even tried. On
  success the fit writes `ff_settings_t.compass_cal`/`cal_valid`
  (already-existing S11 fields — no format bump needed) and applies
  live to the running compass driver via a new
  `ff_shell_cfg_t.compass_cal_changed` hook, mirroring how
  `ff_compass_set_cal` was already wired at boot.

  **Interpretation calls:**
  - **This is Step 3 alone, not slices (a)/(b) of this spec.** There is
    still no fresh-boot first-run FLOW (name -> pack -> calibrate) —
    `my_name` unset at boot does not yet enter anything, and the pack
    step remains unbuilt. The calibration ritual is reachable only from
    Settings today. A future first-run-flow PR can reuse this same
    session/shell seam for its own Step 3 screen without changes here.
  - **Declination is left as-is** (`ff_geo_cal_finish` always writes
    `declination_deg = 0`, per that function's own doc comment) — not
    addressed by this PR, per this file's Behavior section's silence on
    where a non-zero declination would even come from (no pack/settings
    field for it exists yet).
  - **The Settings row's status pill reads "SET"/"UNSET"**, not the
    literal words "calibrated"/"uncalibrated" this file's Behavior
    section names — the row's fixed-width status pill (96px, shared
    with QUIET HOURS/UNITS) could not fit either word without
    overflowing into the row beside it (caught by
    `test_face_hit_targets.c`'s hit-target sweep against the golden
    fixture — see `scr_settings.c`'s own doc comment on
    `settings_build_compass_cal_row` for the measured reasoning). The
    full-screen ritual page itself is titled "CALIBRATE COMPASS" in
    full.
