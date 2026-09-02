# S15 slice b · display + touch — first light on the glass

## Purpose

The S15a skeleton proved the pure-C core boots and runs `ff_shell` on real
ESP32-S3 silicon against **stub** HALs — the LCD stays dark, no input. Slice b
replaces the stub display + input HALs with real ones, so the app the sim has
been rendering for weeks renders **on the puck's own glass**, and a physical
tap/swipe drives it. This is the payoff of building S16 (the shell) before S15
(the device): the loop already works and is fully tested in the sim; slice b
plugs a panel and a touch controller into it. **`ff_shell`, the screens, and
`core/` do not change.** If a change is needed inside them, it's a bug in this
slice's HAL boundary, not a licence to edit the app.

## The hardware (verified against Waveshare docs 2026-08-26 — the implementer
re-verifies each against Waveshare's own ESP-IDF demo before trusting it)

Board: **Waveshare ESP32-S3-Touch-LCD-1.46** (the two boards on hand are
ESP32-S3 rev v0.2, 16 MB flash, 8 MB octal PSRAM).

- **Display: SPD2010, QSPI**, 412×412, RGB565. Round physical glass, addressed
  as a 412×412 square (corners exist in the framebuffer but sit under the bezel).
  - Data SDA0..3 = GPIO **46/45/42/41**, SCK = GPIO **40**, CS = GPIO **21**,
    TE (tearing-effect) = GPIO **18**, backlight (BL) = GPIO **5**.
  - **LCD_RST is NOT a direct GPIO — it is on an I²C IO expander (EXIO2).**
- **Touch: SPD2010 capacitive, I²C** — SDA = GPIO **11**, SCL = GPIO **10**,
  INT = GPIO **4**. **TP_RST is on the IO expander (EXIO1), not a GPIO.**
- **IO expander** (Waveshare uses a TCA9554-class part on I²C — implementer
  confirms the exact chip + address from the schematic/demo): drives EXIO1
  (TP_RST) and EXIO2 (LCD_RST). **It must be brought up FIRST and both resets
  released through it, or nothing lights up.** This is the single most common way
  this bring-up fails silently — a naïve driver that toggles a GPIO for reset
  gets a dark screen with no error.

Components to prefer over hand-rolling (verify exact registry names/versions;
pin them in `idf_component.yml`):
- `espressif/esp_lcd_spd2010` (QSPI panel) — or Waveshare's vendored init
  sequence if the registry panel driver doesn't match this glass.
- `espressif/esp_lcd_touch_spd2010` + `espressif/esp_lcd_touch`.
- `espressif/esp_io_expander_tca9554` (or the matching part) for EXIO.
- `espressif/esp_lvgl_port` if it cleanly supports **LVGL v9** + this panel;
  otherwise hand-roll the LVGL v9 `lv_display` flush → `esp_lcd_panel_draw_bitmap`
  and the `lv_indev` read → touch. LVGL version MUST match the sim's (v9) so the
  same `core`/screen render code produces the same pixels.

## Layering (respect the existing boundary)

- `core/` unchanged. `app/` (ff_shell + screens) unchanged — they already render
  through LVGL and consume abstract input events; the sim proves it.
- The new code lives ONLY in `targets/esp32s3/` as a HAL/driver component
  (extend `ff_platform`, or add an `ff_display` component). It provides:
  1. a real **display HAL**: an LVGL v9 `lv_display` whose flush pushes to the
     SPD2010 panel — the device analogue of the sim's SDL flush;
  2. a real **input HAL**: SPD2010 touch → the SAME abstract tap/swipe events
     `ff_shell` already handles (the sim injects these via the ctl socket; on
     device they come from the panel). Reuse the shell's existing input entry
     points — do not invent a second input path.
- `app_main` swaps the stub display/input HALs for these. Nothing else in
  `app_main`'s boot order changes.

## Slices within b — sequenced to de-risk (do them in order; each is a
gate, and each has a serial-log signature the implementer captures)

- **b1 — first light (pixels, no LVGL).** IO expander up → release LCD_RST &
  TP_RST → backlight on → QSPI panel init → `esp_lcd_panel_draw_bitmap` a solid
  fill and a two-colour split. This alone proves: expander works, reset released,
  QSPI wired, init sequence correct, colour order (RGB565 byte-swap) right,
  panel gap/offset for the round 412×412 correct. NO LVGL yet — isolate the panel
  from the GUI stack. **Milestone: a known colour fills the round glass.**
- **b2 — the app on glass.** Bring up LVGL v9 against the panel; render one real
  `ff_shell` face (radar no-selection is the honest, simplest — it's mostly the
  ring + empty state). Prove the actual app projection reaches the glass,
  matching the sim's render of the same state.
- **b3 — touch drives it.** SPD2010 touch → shell input; a physical tap and a
  swipe change the shell state (face switch, selection) exactly as the sim ctl
  socket's `tap`/`swipe` do. Feed the INT line; debounce per the controller.

Framebuffer/perf notes (apply as needed, not upfront gold-plating): double
buffer in PSRAM, use TE to avoid tearing, DMA the QSPI transfer. The S15 spec's
≥25 fps radar target is the slice's *perf* AC but b1/b2 correctness come first.

## Honesty (the project value still binds on hardware)

The panel shows `ff_shell`'s real projected state — never a canned splash faked
to look further along than the firmware is. If touch isn't calibrated yet, don't
fake positions; log raw coords and say so. A photo in the PR that shows a state
the firmware can't actually produce is the exact dishonesty the sim rules forbid,
now in physical form.

## Verification — hardware-in-the-loop (this is NOT sim-test-gated)

Slice b cannot be proven by `ctest` — a driver can compile perfectly and paint a
black or garbled screen. Verification is on real hardware:
- **Serial-log gates**: each b-step logs its init (expander ACK + read-back,
  panel ID if the controller exposes one, LVGL start, first flush done, touch
  coords on tap). No panic, no brownout, no watchdog reset. The implementer
  captures these by flashing **board 2** (`/dev/cu.usbmodem3`) and reading serial
  with the pyserial reset-and-read pattern (`idf.py monitor` needs a TTY and
  fails headless).
- **Visual gate (requires the maintainer's eyes)**: b1 colour fill, b2 face on
  glass, b3 tap/swipe response — confirmed by looking at the puck. This is a
  collaborative step, not something an agent asserts.
- **IRAM/DRAM budget**: re-check the S15a IRAM-99.99% concern now that a real
  panel driver + LVGL + PSRAM framebuffer are linked. Report the `idf.py size`
  breakdown; if IRAM overflows, move non-ISR code out of IRAM before proceeding.
- **Sim untouched**: `cmake -S firmware -B build -DFF_TARGET=sim` still 38/38;
  `core/` and `app/` diffs are empty (the whole slice is under `targets/esp32s3/`).

## Acceptance criteria

1. **b1**: a known solid colour (and a two-colour split to prove orientation +
   no byte-swap error) fills the 412×412 panel; backlight controllable. Serial
   log shows expander + reset + panel init succeeding, no panic.
2. **b2**: LVGL v9 renders a real `ff_shell` face on the panel — the same
   projection the sim renders for that state (a photo in the PR, honest per above).
3. **b3**: a physical tap and a physical swipe change shell state via the SAME
   input events the sim ctl socket injects — no second input path bolted on.
4. **Budget + stability**: `idf.py size` reported; no watchdog/brownout over a
   several-minute soak on the bench; IRAM concern resolved or explicitly still-open
   with numbers.
5. **Isolation**: sim gate still 38/38; `core/` and `app/` unchanged; all new
   code under `targets/esp32s3/`; components pinned in `idf_component.yml`.

## Out of scope for b (tracked, not silently dropped)

UART transport to the comms brain (slice c), sensors/compass calibration (slice
d), idle/wake power (slice e), embedded festpack assets (slice e). Touch
*calibration* accuracy beyond "tap/swipe register in the right region" is slice
d's concern; b needs the events wired, not surveyed-accurate coordinates.

## Amendments

**2026-09-02 — SCREEN flip setting (maintainer ask; full mechanism in
`docs/specs/S21-settings-rework.md`'s own amendment).** AC1 above ("a
two-colour split to prove orientation") establishes this panel's ONE
native orientation at bring-up; the Fusion-designed case can mount the
puck 180° from that native orientation, which is now a runtime toggle
(`ff_settings_t.screen_flip`, Settings' SCREEN row) rather than a
fixed board-bring-up fact. `ff_display_set_flip`
(`targets/esp32s3/components/ff_display/ff_display.c`) applies a
HARDWARE `esp_lcd_panel_mirror` — b1's own orientation-proving split-
fill pattern, and STAGE 1's test pattern generally, are drawn assuming
NORMAL orientation and are unaffected by this setting (they run before
`ff_shell_init` ever loads it). b3's touch-event path is amended too:
`ff_touchcal_process_cb` (the SAME `process_coordinates` seam this
slice wired) now composes a `screen_flip`-driven rotation AFTER the
S15d calibration fit — see that spec's own S21 cross-reference — so a
physical tap or swipe still lands on the same LOGICAL element as the
un-flipped case, in either orientation.
