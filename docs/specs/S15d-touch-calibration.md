# S15 slice d(touch) · touch calibration — tap the crosshairs

## Purpose

On real hardware the SPD2010 touch maps *roughly* to screen space but is
shifted + slightly scaled: measured on board 2, both axes top out near raw 406
(not 411) and a center tap reads ~(220, 225) instead of (206, 206). No axis
swap, no mirror — just a modest **offset + scale** per axis, which is exactly
why controls "don't quite land." This slice adds a **calibration flow** (tap a
sequence of on-screen crosshairs), computes the correction, applies it to every
touch, and — where the device store allows — persists it.

Freehand "tap the center" can't calibrate: there's nothing to aim at. The user
must tap a *rendered* target at a *known* screen point; the correction is
computed from where they tapped (raw) vs. where the crosshair was (screen).

## Model (keep it simple — matches the measured error)

**Per-axis affine**, four parameters: `sx = ax*rawx + bx`, `sy = ay*rawy + by`.
No rotation/skew term (the measured error has none; a full 2×2 affine would just
fit tap noise). This corrects offset AND scale, which is the whole observed
error.

## Layout (respect the boundary)

- **core `ff_touchcal`** (pure C11, zero deps, unit-tested): 
  - `ff_touchcal_t` — the 4 params + a `valid` flag.
  - `ff_touchcal_solve(const ff_cal_point_t *pts, int n, ff_touchcal_t *out)` —
    least-squares fit of ax,bx from (rawx→screenx) and ay,by from (rawy→screeny)
    over n≥3 points. Returns false (leaves identity) if the points are
    degenerate (e.g. all same rawx → no x-spread), so a bad capture can't
    produce a garbage transform.
  - `ff_touchcal_apply(const ff_touchcal_t *c, int rawx, int rawy, int *sx, int
    *sy)` — apply + clamp to [0, res-1]. Identity when `!valid`.
  - Store the 4 params in `ff_settings` (core) so persistence rides the existing
    settings mechanism; include a `touch_calibrated` bool.
- **device calibration UI** (`targets/esp32s3`, LVGL): render a crosshair at
  each target in turn, capture the raw tap for that target, feed the pairs to
  `ff_touchcal_solve`, store, apply. This is the ONLY new UI; keep it minimal
  (crosshair + "tap the target" text + progress like "3/5").
- **apply point**: the device touch path (where raw SPD2010 coords enter, in
  `ff_display`/the indev feed) runs `ff_touchcal_apply` before the coord reaches
  LVGL/the shell — so ALL touch (gestures, buttons, everything) is corrected.
  The sim is unaffected (its synthetic input is already in screen space; apply
  identity there).

## Targets (inside the round glass)

Five: **center (206,206)** + four insets **(90,90) (322,90) (90,322)
(322,322)** — all comfortably inside radius 206 (corner distance ≈164px), giving
3 distinct x and 3 distinct y values for a well-conditioned per-axis fit. Show
them one at a time; require a deliberate tap each (debounce: one capture per
target, on release or a stable press).

## Flow (one flash, immediate payoff)

A bring-up STAGE (e.g. `CONFIG_FF_BRINGUP_STAGE` = a new CAL value, or a
`--calibrate`-style gate): run the crosshair sequence → `ff_touchcal_solve` →
store to `ff_settings` → **apply live and continue straight into the normal UI**
so the maintainer immediately feels whether buttons land better, no reflash.
Also `ESP_LOGI` the four computed params + the 5 (raw→screen) pairs so the
maintainer can (a) sanity-check the fit and (b) bake the values as a compile-time
default if per-device NVS persistence isn't wired yet.

## Persistence (honest about the current limit)

`app_main` currently boots against a **stub (no-op) `ff_store`**, so a stored
cal will NOT survive reboot yet. Two honest options — do the first, note the
second:
1. **This slice**: compute + store in `ff_settings` + apply LIVE this session,
   and LOG the params so a measured default can be baked in. Fully fixes the
   feel for the session.
2. **Follow-up (or here if cheap)**: wire a real **NVS-backed `ff_store`** on
   device so the cal (and all settings) persist across reboot. If you wire it,
   say so; if not, file/track it — do NOT fake persistence.

Later, calibration should be re-runnable from **Settings** ("Calibrate touch")
rather than only a build stage — note as the productization step, out of scope
for the first working version.

## Acceptance criteria

1. `ff_touchcal_solve`/`apply` are pure core with unit tests: a known
   offset+scale is recovered from synthetic points; degenerate input returns
   invalid/identity (no garbage transform); apply clamps to [0,411].
2. On device: the crosshair flow captures 5 taps, computes a transform, logs the
   4 params + the pairs, and applies it live — after calibrating, a tap lands on
   the crosshair it targeted (verify by re-tapping) and controls hit noticeably
   better.
3. Corrected coords feed the SAME touch path as before (gestures/long-press/
   buttons all corrected) — no second input path.
4. Sim unaffected: `ctest` green (apply is identity in sim); core tests added;
   `git diff origin/main -- firmware/core` is only the new `ff_touchcal` +
   `ff_settings` cal fields.
5. Persistence: either NVS-backed store wired (cal survives reboot) OR the
   session-live behavior + logged params + a tracked follow-up — stated plainly,
   not faked.

## Out of scope
Multi-touch, gesture tuning, the Settings "Calibrate" entry point (productization),
tap-target SIZE (#99 part 2). This slice is the correction transform + capture flow.

## Amendments

**2026-09-02 — SCREEN flip setting composes AFTER this transform (maintainer
ask; full mechanism in `docs/specs/S21-settings-rework.md`'s own amendment).**
The Fusion-designed case can mount the puck 180° from native orientation
(`ff_settings_t.screen_flip`). The 180° rotation (`ff_touchcal_flip180`,
`ff_touchcal.h`/`.c`, a pure helper alongside `ff_touchcal_apply`) is applied
in the device's `process_coordinates` seam AFTER this slice's calibration
fit, never folded into it — the per-unit `(ax,bx,ay,by)` this slice solves
characterizes the touch SENSOR's own raw-tick error (a property of the
silicon and the glass), which does not change with case orientation, so
composing the two as separate steps means a calibration solved in either
orientation stays valid in both, no re-calibration on a flip toggle. One
consequence: `ff_display_run_calibration`'s crosshair-capture flow
(`ff_cal_release_cb`/`s_cal_capturing`, `ff_display.c`) was taught to
record each captured pair against the true, orientation-independent sensor
reading even when a recalibration is run WHILE already flipped — see that
flag's own doc comment for the derivation of why a naive "just also flip
during capture" would have drifted.
