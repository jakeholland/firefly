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
  full — the honest-data value). Its own slice.
- **(d) Deep sleep.** A long-press "off" that `esp_deep_sleep`s with GPIO wake
  instead of cutting the rail, for faster resume. Depends on (b).
