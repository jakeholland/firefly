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

## Amendments

**2026-09-02 — clock-format setting + centered header (maintainer ask).**
Two small additions on top of this slice's scrolling-list Settings:

1. **`ff_settings_t.clock_24h`** (`[api]`, format version 6 -> 7,
   `ff_settings.h`): the puck's default clock format is now 12-hour with a
   lowercase am/pm suffix (`"9:46 pm"`), not bare 24-hour — the design
   vocabulary's own mockup form. `clock_24h = true` switches to 24-hour
   (`"21:46"`). A new Settings row, **CLOCK** (`12H | 24H`, the same
   toggle-pair pill shape as UNITS/SHARE), sits directly under UNITS and
   drives it through `FF_SETTING_CLOCK_24H` — the same bare
   `FF_INTENT_SETTING_SET` seam every other row uses; range validation is
   "nonzero is true", same as HAPTICS/GLOW/COLORBLIND.
   **From v7 on, a format bump FORWARD-MIGRATES** (review finding on the
   first version of this PR, corrected here): S21 §4's real NVS-backed
   store means fielded devices exist now — the maintainer's own puck holds
   a genuine v6 blob (brightness, touch calibration, unit preference) —
   so the pre-v1 "no fielded devices to migrate" premise every v2-v6 bump
   relied on is no longer true, and discarding it would violate this
   project's own honest-data ruling that a settings change must never
   silently wipe a unit's stored calibration. A v6 blob's payload is a
   byte-for-byte prefix of v7's (`clock_24h` is a single bool appended at
   the very end, so every earlier field's offset is unchanged): v7's
   loader reads it via a frozen `ff_settings_v6_t` shadow, carries every
   v6 value across field-by-field, and only `clock_24h` — which v6 never
   had — lands at its honest default (false / 12-hour). **The pre-v1
   reject-not-migrate rule still applies to anything OLDER than v6** (<=v5):
   those blobs pre-date the NVS store shipping at all, so no fielded
   device holds one and there is nothing genuine a migration could
   preserve — see `ff_settings.c`'s v7 comment for the full reasoning and
   `test_settings.c`'s `S21_v6_blob_forward_migrates_preserving_every_value`
   / `S11_AC1_load_with_v5_blob_yields_defaults_not_a_migration` for the two
   sides of that boundary.
   The formatter itself (`ff_fmt_clock`) is core logic in `ff_wall.h`/
   `ff_wall.c`, not this file — see docs/specs/S18-wall-clock-trust.md's
   own Amendments entry for the exact format strings and the buffer-size
   bump this required (`FF_RADAR_CLOCK_LEN` 6 -> 9).
2. **Header alignment fix.** The pinned SETTINGS/name header still
   reserved a back-button-shaped gutter and left-anchored both lines
   against it, though the horizontal-carousel rework (S26) had already
   removed the back button itself — so the two lines merely happened to
   sit near center rather than actually being centered, and the
   (unconstrained-width) title visibly drifted right of the (fixed-width,
   left-anchored) name below it
   (`firmware/tests/golden/settings_default.png` before this fix).
   Rebuilt as one flex-COLUMN container, cross-axis centered, so title and
   name share the glass's true vertical axis regardless of their own
   (different) natural text widths — sizes/colors unchanged, no other
   layout change. This inserted the new CLOCK row shifts every row below
   UNITS down by one `FF_SETTINGS_ROW_STEP` (spacing itself unchanged;
   S21's scroll list has no page for it to overflow — it simply scrolls
   into view like every other row).

Goldens regenerated: all six `settings_*` fixtures (header fix + new row)
and a new `settings_clock_24h` fixture/golden (the CLOCK toggle set to 24H,
rendered on the launcher's time·battery row as `"21:46"`). `test_face_hit_
targets` (this slice's scroll-aware sweep) covers the new row generically —
no sweep-file change needed.

**2026-09-02 — SCREEN flip setting (maintainer ask): the Fusion case
mounts the puck upside-down.** The Fusion-designed case holds the puck
180 degrees from the panel's native orientation, so this adds a
**SCREEN** row (`NORMAL | FLIPPED`, the same toggle-pair pill shape as
UNITS/CLOCK/SHARE) implemented as a HARDWARE mirror, not a software
rotation:

1. **`ff_settings_t.screen_flip`** (`[api]`, format version 7 -> 8,
   `ff_settings.h`): bool, default `false` (NORMAL) — a fresh puck's case
   orientation is unknown until the owner sets it, and NORMAL is what
   every pre-v8 puck was already rendering as. Drives the new Settings
   **SCREEN** row (sits directly under CLOCK; SHARE/HAPTICS/GLOW/WATER/
   QUIET/COLORBLIND/CALIBRATE each shift down by one more
   `FF_SETTINGS_ROW_STEP`, same "the scroll list absorbs it" mechanics
   CLOCK's own insertion above already established) through
   `FF_SETTING_SCREEN_FLIP` — the same bare `FF_INTENT_SETTING_SET` seam,
   "nonzero is true" range validation, same as every other two-state row.
   **v7 -> v8 chains the SAME forward-migration policy v6 -> v7
   established above**, one hop further: a real v7 blob (clock_24h
   already shipped with the NVS store, so fielded pucks hold one) is
   migrated via a newly-frozen `ff_settings_v7_t` shadow, and a v6 blob
   chains THROUGH that same v7 shadow (`ff_settings_migrate_v6` now
   targets the v7 shadow; `ff_settings_migrate_v7` — shared by both a
   real v7 blob and a migrated-from-v6 one — carries every value across
   and lands the one field that shape never had, `screen_flip`, at its
   honest default). The <=v5 reject-not-migrate boundary is unchanged.
   See `ff_settings.c`'s v8 comment for the full reasoning and
   `test_settings.c`'s `S21_v7_blob_forward_migrates_preserving_every_value`
   (mirroring the v6 test above) for the new hop's proof.
2. **The mirror is a HARDWARE panel mirror, not a software rotation.**
   `ff_display_set_flip` (`targets/esp32s3/components/ff_display`) calls
   `esp_lcd_panel_mirror(panel, flip, flip)` — the vendored
   `esp_lcd_spd2010` driver DOES implement `mirror` via a real MADCTL
   write (unlike `swap_xy`, a logged no-op on this panel); see that
   function's own doc comment for the exact driver citation and the 4-px
   window-alignment reasoning (the driver's CASET/RASET window math never
   depends on the mirror bits, so the existing alignment holds unchanged
   — verified by reading the driver source, not by a sim render, which
   cannot exercise MADCTL at all). Applied once at boot (right after the
   panel initializes, before the boot splash — so the splash itself
   renders upright) and again immediately on any live Settings change (no
   reboot) — see `app_main.c`.
3. **Touch flips in OUR layer, composed AFTER calibration**, inside the
   existing `process_coordinates` seam: a new pure core helper,
   `ff_touchcal_flip180` (`ff_touchcal.h`/`.c`, unit-tested standalone),
   computes `x' = w-1-x, y' = h-1-y` on the ALREADY-calibrated point. This
   ordering (apply, then flip — never the other way, and never folded
   into the calibration fit itself) is what lets a calibration solved in
   either orientation stay valid in both, with no re-calibration on a
   flip toggle — the crosshair capture flow (`ff_display_run_calibration`)
   was ALSO adjusted so a recalibration run while already flipped fits
   against the true, orientation-independent sensor error rather than a
   flip-contaminated one (see `s_cal_capturing`'s doc comment in
   `ff_display.c` for the full derivation of why that adjustment is
   necessary, not cosmetic).
4. **Glass geometry is a function of the flip.** The bezel hides the
   PHYSICAL left ~5px of the panel (`docs/hardware/glass-offset.md`); under
   a 180° mirror those pixels are the user's RIGHT, so the visible centre
   becomes `FF_THEME_PUCK_PX - FF_THEME_GLASS_CX` (208 -> 204).
   `ff_theme_glass_cx`/`_cy` (`ff_theme.h`, pure `static inline`, same
   convention as `ff_theme_crew_color`) compute this; `radar_build_rim_tint`
   (`scr_radar.c`, the one on-glass element that hugs the physical bezel)
   is the sole consumer (grepped for every other `FF_THEME_GLASS_*` use).
   `ff_scr_radar_build` gained a `screen_flip` parameter (same explicit-
   parameter convention `colorblind` already uses) threaded from
   `scr_nav.c`. See `docs/hardware/glass-offset.md`'s own amendment for
   the flipped-case rule.

Goldens regenerated: the five `settings_*` fixtures affected by the new
row (`settings_brightness`, `settings_colorblind_on`, `settings_default`,
`settings_ghost`, `settings_scrolled_mid` — `settings_scrolled_bottom` and
`settings_clock_24h` render byte-identically, unaffected), plus two new
fixtures: `settings_screen_flipped` (SCREEN toggled to FLIPPED) and
`radar_stale_flipped` (same radar_stale scene, `settings.screen_flip:
true` — proves the rim tint recenters; the sim never mirrors its own
framebuffer, so this fixture exists specifically to prove the CENTRE
moves, not to simulate the physical mirror itself). `test_face_hit_
targets` again covers the new row generically. On-glass verification
(flip on: UI upright in the flipped case, touch lands correctly, rim
centred, boot splash upright) is the maintainer's — see
docs/hardware/glass-offset.md's hardware-in-the-loop convention.
