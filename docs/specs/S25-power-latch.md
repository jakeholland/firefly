# S25 — Power latch + soft power button (`ff_power`)

Status: draft (2026-09-01). Device-target only (`targets/esp32s3`). Slice (a)
— the battery keep-alive latch — is the whole of this first cut; the button
slices (b+) are scoped here but deferred.

## Why

The Waveshare ESP32-S3-Touch-LCD-1.46 does **not** hard-wire the battery to
the system rail. A soft-latch power circuit gates it: pressing **PWR**
momentarily powers the rail (enough to start booting), and firmware must then
drive the **SYS_EN** hold line high to keep the latch closed. Release SYS_EN
(or hold PWR past the panel's ~6 s hardware force-off) and the rail drops.

The firmware never drove SYS_EN, so on battery the puck died the instant the
user released PWR — it only ran while the button was physically held (the user
was closing the latch by hand) or while USB fed the rail directly (bypassing
the latch entirely). This is fatal for the Lost Lands field test, which is
battery-only. Waveshare's own board driver
([`PWR_Key.c`](https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/PWR_Key/PWR_Key.c))
is the authority for the pins and sequence.

## Hardware contract (from the board's reference driver)

Both signals are **direct ESP32-S3 GPIOs**, not TCA9554 expander pins — so the
latch has no dependency on the I2C bus / expander bring-up and can be asserted
in the first microseconds of `app_main`:

- **GPIO7 — SYS_EN / PWR_Control** (output). Drive **HIGH** to latch battery
  power on; drive **LOW** for a soft power-off (cuts the rail on battery; on
  USB the board stays up but the latch is released).
- **GPIO6 — PWR key** (input). The PWR button state, read for press /
  long-press detection. Reserved for slice (b); not touched in slice (a).

Neither pin is in the display/touch pin map (`ff_display.c`: 4, 5, 10, 11, 18,
21, 40–46) nor an ESP32-S3 strapping/flash pin, so both are free.

## Slice (a) — the latch (this cut)

A device-only `ff_power` component exposing one call:

```c
esp_err_t ff_power_latch_on(void); /* configure GPIO7 output, drive HIGH */
```

`app_main` calls it as its **first statement**, before the shell alloc and all
peripheral bring-up: on battery the window is only until the user's finger
lifts, so the latch must beat the display init's tens-of-ms of delays. Pure
I/O — no `core/` logic (there is no domain decision here, only a pin drive).

### Acceptance criteria

- **AC1** `ff_power_latch_on` configures GPIO7 as a push-pull output and drives
  it high, returning the `gpio_config`/`gpio_set_level` error verbatim on
  failure (logged); success is logged with the pin number.
- **AC2** It is the first thing `app_main` does — before `s_shell_p` alloc and
  before `ff_bringup_panel`. A latch failure is logged but **non-fatal**: the
  puck keeps booting (on USB it runs regardless; on battery it was going to
  die anyway, and a running-but-unlatched puck on the bench is more debuggable
  than a park loop).
- **AC3** Device-only: the component lives under `targets/esp32s3/components`
  and is never compiled into the sim; the sim build + goldens are unaffected.
- **AC4** No behavior change on USB (the rail is USB-fed; the extra latch is
  harmless), and the field/demo split is untouched (this is config-agnostic —
  it runs in every device build, demo or field).

### Verification

No sim unit test is possible (pure GPIO HAL, no host path) — the honest gate,
matching the other `targets/esp32s3` HAL code (`ff_display`), is:
- device `idf.py build` links STAGE 3, both demo and field configs;
- **on glass**: with the battery connected and USB unplugged, the puck stays
  running after the PWR button is released (the symptom that motivated this).

## Deferred slices (scoped, not built)

- **(b) PWR button read + soft power-off.** Poll GPIO6 (debounced) on a
  periodic task; a long press drives GPIO7 low (`ff_power_off`). The
  press-duration state machine (short / long / the reference driver's
  sleep→restart→shutdown tiers) is **domain logic and belongs in `core/`** as a
  pure, unit-tested `ff_power_fsm` fed tick + level; the target only samples
  the pin and enacts the FSM's decision. Ties to the `ff_button` GPIO0 idea so
  PWR (power) and BOOT (UI) share one input-forwarding shape.
- **(c) Battery gauge.** `BAT_Driver` reads pack voltage via ADC; feed an
  honest charge estimate into the status bar (unknown-until-read, never a faked
  full — the honest-data value). Its own slice — see this spec's Amendments
  for the CORE + SHELL half; the device ADC read (Waveshare ADC1 ch7 / GPIO8,
  12 dB atten, 1:3 divider) and the sim hook are a follow-up PR, not built
  here.
- **(d) Deep sleep.** A long-press "off" that `esp_deep_sleep`s with GPIO wake
  instead of cutting the rail, for faster resume. Depends on (b).

## Amendments

- **2026-09-03, PR #180, S25 slice c, core + shell half (`ff_batt`, `ff_shell_set_batt_mv`).**
  Lands the platform-neutral half of the battery gauge — everything the
  device-side ADC-read PR (this slice's own deferred bullet above) will call
  into. No `targets/esp32s3` change in this PR; the sim still never calls the
  new seam and its `batt_pct` stays honestly -1, same as before. **Revised
  during PR review** (two blocking findings, two should-fixes — see below);
  the design described here is the FINAL one that landed, not the PR's first
  draft.

  - `core/include/ff_batt.h` / `core/src/ff_batt.c`: `int8_t
    ff_batt_pct_from_mv(uint16_t pack_mv)` — a single-cell LiPo open-circuit
    voltage table (3300→0%, 3500→5%, 3600→10%, 3700→30%, 3800→50%, 3900→65%,
    4000→80%, 4100→92%, 4200→100%, piecewise-linear between points, clamped
    at the ends), sourced from the same breakpoints this project's own
    fuel-gauge reference guides (Adafruit's MAX1704x/LC709203F "Custom
    Battery Profile" learn guides, SparkFun's near-identical LiPo Fuel Gauge
    table) already point to — not a datasheet for a specific cell, since this
    project has not picked one. Returns -1 (unknown) for `pack_mv == 0` or
    outside a `[2500, 4600]` mV plausibility window — a reading out there is
    a broken/absent sense line, not an extreme charge level, and is reported
    as unknown rather than clamped into a plausible-looking percent
    (CLAUDE.md's "honest data over pretty data").
  - `ff_batt_filter_t` + `ff_batt_filter_init`/`ff_batt_filter_push`: a
    4-sample PRE-FILLED moving AVERAGE in the mV domain (not a moving
    median — the review's first blocking finding, below) plus Schmitt-style
    hysteresis on the resulting percent, so the shown percent does not
    visibly tick on ordinary ADC noise. The first-ever real reading shows
    immediately (no warm-up wait — "unknown-until-read, then real", this
    spec's own slice (c) framing: a freshly pre-filled window's average
    equals that one sample). A gap of >= 30 s since the previous push resets
    the averaging window instead of blending a stale pre-gap reading with a
    fresh post-wake one (`FF_BATT_FILTER_STALE_GAP_MS`). A sense line that
    goes dead (an implausible/zero reading `FF_BATT_FILTER_DEAD_AFTER` (3)
    times in a row) reverts the display to -1 rather than continuing to show
    a number frozen from before it died (the review's should-fix #3).
  - `app/include/ff_shell.h` / `ff_shell.c`: `[api] void
    ff_shell_set_batt_mv(ff_shell_t *sh, uint16_t pack_mv, uint32_t now_ms)`
    — a push API (`ff_shell_set_heading`'s precedent), running the reading
    through the core filter IMMEDIATELY at push time (not deferred to the
    next tick, unlike `heading_deg`/`my_pos`: the filter needs to see every
    actual reading as its own timed event, not resampled at the shell's tick
    rate — see `ff_shell.c`'s `batt_filter` field comment). `now_ms` is the
    CALLER's own clock reading at the moment of the read, not (as the PR's
    first draft had it) `sh`'s own last-tick clock — the review's should-fix
    #4: `sh->now_ms` defaults to 0 before the shell's first tick, so a
    battery read before that first tick, followed by an ordinary tick and a
    second read shortly after, could look like a many-second gap against the
    stale "time 0" the first read was recorded at and spuriously trip the
    stale-gap reset. The filtered result reaches `ff_shell_view()`'s
    `radar.batt_pct` on the next `ff_shell_tick`, replacing the `-1`
    `ff_shell.c` hardcoded at ~line 1630 ("no battery ADC on either target
    yet"). Before the first push, `batt_pct` stays -1, honestly rendering
    "--%" on both Radar's status bar and the launcher's status row.
  - `shell_render_key`'s LAUNCHER branch (the opaque-overlay render-key mask
    that keeps a live radar arrow / feed churn underneath the launcher from
    rebuilding its tree under a finger) gained a THIRD restored scalar,
    `radar.batt_pct`, alongside the pre-existing `active_face` and unread-
    badge/banner exceptions: the launcher's status row (S26 slice e) renders
    `batt_pct` verbatim, so before this fix a battery reading that changed
    while the launcher was showing (the common case — the launcher is home)
    left the render key byte-identical and the row sat stale, the same
    clobber class the banner exception (issue/PR #157) already fixed for a
    different field.

  **Review findings that changed the design** (docs/review/code-review.md
  item 6, "the proxy" — both blocking findings were measured, not reasoned
  about: the reviewer ran the actual filter against an adversarial input and
  counted displayed transitions):
  - **Blocking #1 — moving-median oscillation.** The PR's first draft used a
    5-sample moving MEDIAN. Fed an input alternating between two mV values
    every push (3720 mV / 3650 mV, 2 s apart — the load-sag pattern a radio
    TX duty cycle actually produces), a median's majority flips every single
    sample once the window is full of alternating values, producing 29/29
    displayed changes over a 60 s run — the exact flicker the hysteresis was
    supposed to prevent. Fixed by switching to a small (4-sample) PRE-FILLED
    moving AVERAGE in the mV domain: averaging is far less sensitive to
    perfect alternation (an even-sized window of alternating inputs
    converges toward the true mean instead of flip-flopping), and
    pre-filling every window slot with the first reading (rather than
    growing the window one sample at a time) avoids a SECOND, unrelated
    instability the growing-mean design change introduced on its own first
    pass — a variable per-sample weight (1/2, then 1/3, then 1/4) during
    warm-up that bounced the display through 3 values before settling. The
    window size (4, not the original draft's 5 or the review's own
    example-suggested 8) was chosen by literally simulating this filter
    against a battery of alternating stress pairs spanning the OCV table and
    picking the smallest window whose worst case held to the review's
    <= 2-change budget.
  - **Blocking #2 — symmetric low-threshold exemption strobed the alert.**
    The PR's first draft exempted BOTH directions of a `FF_BATT_LOW_PCT`
    crossing from the ordinary hysteresis (prompt on the way down, equally
    prompt on the way back up). Fed an input alternating right at the
    boundary (3625 mV = 15% / 3630 mV = 16%), that produced 29/29 flips of
    the amber low-battery state. Fixed by making the exemption ASYMMETRIC:
    a downward crossing into low still promotes immediately and
    unconditionally (the alert must never be delayed, even by 1 point), but
    an upward exit from low now requires the filtered value to clear
    `FF_BATT_LOW_PCT + FF_BATT_HYSTERESIS_PCT` (17) — a full hysteresis
    margin past the boundary, not merely past it — before the display is
    allowed to leave the low band. The 3625/3630 alternation now settles at
    15% (or 16%, phase-dependent) and never leaves it once it lands.

  **Acceptance criteria** (mirroring this spec's own "unknown-until-read,
  never a faked full" framing for slice (c)):
  - **AC1** `batt_pct` is `-1` (unknown) until the first
    `ff_shell_set_batt_mv` call; the very first real reading then shows
    immediately, no multi-sample warm-up delay. Tests:
    `before_any_push_display_is_unknown`, `first_sample_shows_immediately`
    (`core/tests/test_batt.c`); `S25c_batt_mv_reflects_filtered_value_and_dirties_launcher_key`
    (`app/tests/test_shell.c`).
  - **AC2** the mV→percent table is honest and exact at its published
    points, interpolates linearly between them, and reports unknown (not a
    clamped guess) outside the plausibility window. Tests:
    `table_point_3700_is_30_pct`, `table_boundaries_by_literal`,
    `interpolation_midpoint`, `clamps_below_table_to_zero`,
    `clamps_above_table_to_hundred`, `unknown_when_zero`,
    `unknown_below_plausibility_window`, `unknown_above_plausibility_window`
    (`core/tests/test_batt.c`).
  - **AC3** ordinary reading noise (a sub-hysteresis wobble) never changes
    the displayed percent, so it never flickers the status bar/row or dirties
    a render key over noise alone — including ADVERSARIAL, perfectly
    alternating noise (the review's own literal simulation acceptance test:
    a 3720/3650 mV alternation every 2 s for 60 s must produce <= 2
    displayed changes after the first reveal; the fixed filter measures 2).
    Tests: `one_percent_wobble_does_not_move_display`,
    `S180_alternating_3720_3650_settles_within_two_changes`
    (`core/tests/test_batt.c`); the wobble assertion inside
    `S25c_batt_mv_reflects_filtered_value_and_dirties_launcher_key`
    (`app/tests/test_shell.c`).
  - **AC4** `FF_BATT_LOW_PCT` (`ff_radar.h`) crossing DOWNWARD into low is
    never masked by the display hysteresis, even for a sub-threshold move —
    but crossing back UP out of low requires clearing a full hysteresis
    margin past the boundary (asymmetric on purpose; see Blocking #2 above),
    so a reading hovering right at the boundary settles into the low band
    and holds rather than strobing the alert (the review's own literal
    simulation acceptance test: a 3625/3630 mV alternation every 2 s for
    60 s must settle at <= 15% and never leave the low band; the fixed
    filter measures 0 changes after the reveal). Tests:
    `crossing_to_low_shows_promptly_despite_small_delta`,
    `crossing_out_of_low_requires_the_full_exit_margin`,
    `S180_alternating_3625_3630_settles_at_or_below_low_and_stays`
    (`core/tests/test_batt.c`).
  - **AC5** (added by the review's should-fix #3) a sense line that stops
    reporting a plausible reading — `FF_BATT_FILTER_DEAD_AFTER` (3)
    consecutive implausible/zero pushes — reverts the display to -1 rather
    than continuing to show a stale percent as current. Tests:
    `dead_sense_line_reverts_to_unknown_after_the_threshold`,
    `fewer_than_threshold_bad_samples_does_not_revert`,
    `mutation_target_dead_revert_is_load_bearing` (`core/tests/test_batt.c`).
  - **AC6** (added by the review's should-fix #4) `ff_shell_set_batt_mv`
    uses its caller-supplied `now_ms` for every filter decision, never a
    cached shell clock reading — so a battery push before the shell's first
    tick, followed by a tick and a second push shortly after, does not
    spuriously trip the stale-gap reset. Test:
    `S25c_set_batt_mv_before_first_tick_does_not_spuriously_reset`
    (`app/tests/test_shell.c`).
