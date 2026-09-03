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
  new seam and its `batt_pct` stays honestly -1, same as before.

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
    5-sample moving median (rejects a single load-sag outlier — radio TX,
    backlight PWM, an LVGL redraw spike) plus 2-percentage-point display
    hysteresis, so the shown percent does not visibly tick on ordinary ADC
    noise. The first-ever real reading shows immediately (no warm-up wait —
    "unknown-until-read, then real", this spec's own slice (c) framing). A
    gap of >= 30 s since the previous push resets the median window instead
    of blending a stale pre-gap reading with a fresh post-wake one
    (`FF_BATT_FILTER_STALE_GAP_MS`).
  - `app/include/ff_shell.h` / `ff_shell.c`: `[api] void
    ff_shell_set_batt_mv(ff_shell_t *sh, uint16_t pack_mv)` — a push API
    (`ff_shell_set_heading`'s precedent), running the reading through the
    core filter IMMEDIATELY at push time (not deferred to the next tick,
    unlike `heading_deg`/`my_pos`: the filter needs to see every actual
    reading as its own timed event, not resampled at the shell's tick rate —
    see `ff_shell.c`'s `batt_filter` field comment). The filtered result
    reaches `ff_shell_view()`'s `radar.batt_pct` on the next `ff_shell_tick`,
    replacing the `-1` `ff_shell.c` hardcoded at ~line 1630 ("no battery ADC
    on either target yet"). Before the first push, `batt_pct` stays -1,
    honestly rendering "--%" on both Radar's status bar and the launcher's
    status row.
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
    a render key over noise alone. Tests: `one_percent_wobble_does_not_move_display`
    (`core/tests/test_batt.c`); the wobble assertion inside
    `S25c_batt_mv_reflects_filtered_value_and_dirties_launcher_key`
    (`app/tests/test_shell.c`).
  - **AC4** `FF_BATT_LOW_PCT` (`ff_radar.h`) is never masked by the display
    hysteresis: a move that crosses the low-battery boundary shows promptly
    even when it is smaller than the ordinary 2-point hysteresis threshold.
    Tests: `crossing_to_low_shows_promptly_despite_small_delta`,
    `crossing_out_of_low_shows_promptly_despite_small_delta`
    (`core/tests/test_batt.c`).

- **2026-09-03, S25 slice c, device + sim half (`ff_power` battery-sense
  ADC, ctl `batt_mv`).** The follow-up this spec's slice (c) bullet (and
  PR #180's own amendment above) named: reads the Waveshare board's
  battery-sense ADC on `targets/esp32s3` and drives `ff_shell_set_batt_mv`
  from `app_main`'s render loop; gives the sim a `batt_mv` ctl command so
  the gauge can be exercised with no physical battery. No `core/`/`app/`
  change (that landed in PR #180) — this PR only samples and forwards.

  - **Hardware, cited** (Waveshare's own reference driver for this exact
    board, `BAT_Driver.c`:
    https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/BAT_Driver/BAT_Driver.c):
    ADC1 channel 7 (**GPIO8**), `ADC_ATTEN_DB_12`, `ADC_BITWIDTH_DEFAULT`,
    a 1:3 resistive divider, plus a small measured correction Waveshare's
    own code applies on top of the naive ×3 (divide by 0.990476 — their
    silicon's actual divider ratio is not exactly 3:1). Single-cell LiPo,
    no fuel-gauge IC — the divider math is the entire sensor.
  - `ff_power.h`/`.c` (`targets/esp32s3/components/ff_power`, +`esp_adc` in
    its `CMakeLists.txt` `REQUIRES`): `esp_err_t ff_power_batt_init(void)`
    brings up an ADC1 oneshot unit + channel 7, then tries calibration
    tiered curve-fit → line-fit → none (mirroring ESP-IDF's own `esp_adc`
    oneshot example; ESP32-S3 only ever compiles the curve-fit scheme in —
    the line-fit branch is inert on this chip, kept `#if`-guarded for the
    same portability reason the SDK's own example keeps it). Non-fatal on
    failure or on no calibration scheme (logged either way; a puck with no
    battery ADC still boots). `uint16_t ff_power_batt_mv(void)` averages
    `FF_BATT_ADC_SAMPLES` (8) raw `adc_oneshot_read`s, calibrates the
    average, then converts pin mV → PACK mV via ONE named integer constant
    (`FF_BATT_PACK_MV_PER_CAL_MV_X1E6` = round(3.0 / 0.990476 × 1e6) =
    3028847 at full double precision, composing the ×3 divider and the
    ÷0.990476 correction above; a `uint64_t` intermediate avoids
    overflow — review fix: an earlier revision truncated this to
    3028835, one part in ~33000 low). Returns 0 (unknown) on any
    failure or before calibration succeeds — never a fabricated figure.
    Pure HAL: no percent math here, matching `ff_power.h`'s existing
    house rule. Hoisted into its own ESP-free header,
    `ff_power_batt_conv.h`, specifically so a HOST test
    (`targets/sim/tests/test_batt_pack_mv.c`) can pin the conversion by
    literal — cal 1300 → 3938, cal 0 → 0, the `uint16_t` clamp, and the
    round-half-up rule at an exact tie (mutation-checked: the constant
    off by ±1% fails this test). The first successful reading logs
    (`ESP_LOGI`, once) the raw average, calibrated pin mV, and pack mV,
    labelled with which calibration scheme is actually in effect
    (curve / line / NONE), so bring-up can sanity-check against a
    multimeter.
  - `app_main.c`: `ff_power_batt_init()` is called right after the power
    latch (S25 slice a) and logged, non-fatal on failure. One reading is
    pushed via `ff_shell_set_batt_mv` immediately after `ff_shell_init`
    (so the very first face flushed to glass already has it), and again
    every `FF_BATT_SAMPLE_PERIOD_MS` (a named constant, 2000 ms) in the
    main render loop via `ff_time_reached` — the same wraparound-safe
    periodic-deadline convention every other FSM in this file uses.
    app_main only samples and forwards; the core filter (PR #180's
    `ff_batt_filter_t`) decides what is actually shown.
  - Sim: `targets/sim/ctl_server.h`/`.c` gain a `batt_mv` ctl command
    (`{"cmd":"batt_mv","mv":<0..65535>}`, documented in
    `firmware/tools/dev/CTL.md`) that forwards straight to
    `ff_shell_set_batt_mv` (`ctl_loop.c`'s `ctl_loop_batt_mv`) — the sim's
    own affordance for driving the gauge with no ADC hardware, same class
    as `wall`'s bench time-travel and `flare`'s synthetic inbound. New
    test `targets/sim/tests/test_ctl_batt.c` drives it end to end through
    a real ctl/LVGL session: `batt_mv 3700` renders Radar's "30%" (muted);
    `batt_mv 0` renders "--%"; `batt_mv 3400` renders "3%" amber (at/under
    `FF_BATT_LOW_PCT`). Mutation-checked: dropping the
    `ff_shell_set_batt_mv` forward inside `ctl_loop_batt_mv` fails the two
    positive-reading tests (`Expected 30 Was -1` / `Expected 3 Was -1`),
    confirming the test actually exercises the forward and not a proxy.

  **On-glass verification** (no sim unit test can reach the real ADC —
  same posture as slice (a)'s own Verification section):
  - **(a)** Boot log shows the S25c battery-ADC init line
    (`ok`/error name) and, once the render loop's first periodic sample
    fires, the "S25c first battery reading" line — raw average, which
    calibration scheme (curve/line/NONE), calibrated pin mV, and pack mV.
  - **(b)** With a charged battery connected, Radar's/the launcher's
    status bar shows a plausible, non-flickering percent. If a multimeter
    on the pack is available, its reading (mV) should be within ~50 mV of
    the boot log's `pack_mv` — a larger gap means the divider/correction
    constant needs re-deriving for this specific board rather than
    trusted from Waveshare's own measurement.
  - **(c)** Unplug USB (battery only) and watch the percent hold steady
    (no flicker) for at least a minute — the moving-median + hysteresis
    filter (PR #180) doing its job against real load noise (radio TX,
    backlight PWM, LVGL redraw).
  - **(d)** A reading at or under `FF_BATT_LOW_PCT` (15%) shows the amber
    low-battery tint on both Radar's status bar and the launcher's status
    row. May be verified either by draining a real pack to that range, or
    — faster, and what this PR's own CI coverage already exercises — by
    simulating it in the sim via `{"cmd":"batt_mv","mv":3400}` (interpolates
    to 3%) and confirming the same amber tint renders there; the on-glass
    step is only to confirm the real ADC path reaches the same low reading
    honestly when the pack is actually low, not to re-prove the color
    logic itself.
