# S21 · settings rework — scrolling list, Calibrate Touch, NVS persistence

## Purpose

Three maintainer-requested changes, on top of the now-merged calibration (#103)
and brightness/settings (#105):
1. **Scrolling Settings**, replacing #105's pagination (the maintainer prefers a
   scroll list — the natural pattern for a settings screen).
2. A **"Calibrate Touch"** entry in Settings that runs the crosshair capture
   flow from the running app (no reflash, no build stage).
3. **Persistence that sticks**: a real NVS-backed store so touch calibration,
   brightness, name, quiet hours — everything in `ff_settings` — survives a
   reboot. Today `app_main` uses a no-op stub store, so nothing persists.

## 1 — Scrolling Settings (replaces pagination)

Replace the two-page paginated Settings (`scr_settings.c`, #105) with a single
**vertically-scrolling list** holding every row: units, share mode, haptics,
glow, water nudge, quiet hours, brightness slider, UTC offset, colorblind, and
the new **Calibrate Touch** row. The header (back button + SETTINGS + name)
stays PINNED at the top (does not scroll away) so back is always reachable;
the rows below scroll.

**This is a settings *list* scrolling — NOT the face tileview.** The face
tileview's user-scroll is (correctly) disabled; a settings list scrolling is
normal and expected. Use `LV_SCROLLBAR_MODE_AUTO` (or off) and vertical-only
scroll; snap is optional.

## 2 — The tap-target sweep must go scroll-aware (the real blocker)

#105 paginated because `test_face_hit_targets.c` reads each clickable's ABSOLUTE
rect and fails any control scrolled off-glass. That check is too strict for a
scroll container. Fix the sweep so a control inside a scrollable list is
verified **as it appears when scrolled into view** (relative to the scroll
viewport / clamped scroll range), not at a fixed absolute position:
- A control reachable only by scrolling is on-glass *when scrolled to*; check it
  there (min hit size + the round-glass containment + adjacency to its scroll
  neighbours), not against the current static viewport.
- The pinned header controls (back button) keep the existing absolute check.
- Keep the global all-pairs adjacency guarantee within the visible window; two
  rows far apart in scroll are never simultaneously tappable, so they are not an
  adjacency pair — model that correctly rather than exempting by size.
State the model you chose in the PR; the sweep is the objective gate for this
whole slice, so it must stay meaningful, not neutered.

## 3 — Calibrate Touch entry

A Settings row "CALIBRATE TOUCH" that, on tap, invokes the crosshair flow
(`ff_display_run_calibration` — already merged from #103), installs the solved
transform (`ff_display_touch_set_cal`), stores it to `ff_settings`, and returns
to Settings. Device-only behaviour (the sim has no touch panel): in the sim the
row renders but the action is a no-op/stub (honest — say "device only" or just
do nothing), so goldens/tests still pass. Wire it through the existing intent
seam (a new `FF_INTENT_CALIBRATE_TOUCH` shell-owned, mirroring the other setting
intents) — no second path.

## 4 — NVS-backed store (persistence that sticks)

Implement a real `ff_store` on device backed by ESP-IDF **NVS** (the S15 spec's
`ff_store`=NVS deliverable), replacing the no-op stub in `app_main`:
- Save/load the `ff_settings` blob (already a versioned, self-validating blob —
  the store just needs to persist/return the bytes) to an NVS namespace/key.
- On boot: load → if present and the version validates, apply; else defaults.
  Apply brightness + touch cal from the loaded settings on boot.
- On any settings change (brightness slider, calibrate, name, quiet, ...):
  persist. The reject-not-migrate policy already handles a stale/older blob.
- Keep `core` pure — NVS lives in the target HAL; `ff_settings` stays the pure
  (de)serializer it already is.
This makes Calibrate Touch (and brightness, name, quiet hours) survive reboot.

## 5 — Default touch cal is identity (honest-uncalibrated, NVS-refined)

Keep the default `ff_settings` touch cal as **identity** (`ax=1 bx=0 ay=1 by=0`,
`touch_calibrated=false`). A freshly-flashed puck genuinely has not been
calibrated, so "uncalibrated / correct nothing" is the honest default — raw
coordinates are close enough to operate the UI (the observed panel skew is a
~15px offset, well inside a 44px hit target), and the owner runs the in-app
Calibrate Touch flow (#3) if/when they want a refined per-unit fit, which NVS
then persists. Do NOT bake a specific unit's measured affine in as everyone's
default: `touch_calibrated=true` on a fresh unit would be a lie, and applying
one panel's correction to a possibly-different panel is worse than honest raw
passthrough. The reject-not-migrate policy falls back to this identity default
(honest raw), never to a foreign measured transform.

(Decision: Jake, 2026-08-27, reversing the earlier "representative board-2
default" direction on the honest-data value.)

## Acceptance criteria
1. Settings is a single scrolling list (no page chip); the back button is pinned
   and always reachable; every prior row plus Calibrate Touch is present.
2. `test_face_hit_targets` is scroll-aware and green — controls verified as
   scrolled-into-view, the header absolutely, adjacency modelled correctly (the
   sweep still catches a genuine too-small/too-close control; add a test proving
   it still fails on a deliberately shrunk control).
3. Calibrate Touch launches the crosshair flow on device (`idf.py build` exit 0),
   stores + applies the result; sim renders the row and stays green.
4. NVS store persists settings across reboot on device (brightness, cal, name,
   quiet); boot applies them. Core stays pure; NVS in the target HAL.
5. Default touch cal set as chosen (representative-measured or identity) with an
   honest comment; sim + gcc-14 gates green, goldens regenerated for the new
   Settings layout.

## Out of scope
Multi-profile settings, cloud sync, the compose predictive-T9 UX (#104's follow-up).
