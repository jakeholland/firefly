# Glass/pixel-array offset — measuring it with the glass ruler

## The symptom

On the Waveshare ESP32-S3-Touch-LCD-1.46 (SPD2010, 412×412, round bezel),
a software-centered ring — Radar's rim tint, an `lv_obj_center`'d 408px
circle, proven centered with 2/2/2/2px margins by the sim's own golden
screenshot — renders visibly **off-center on real glass**: the left arc is
fully hidden under the bezel while the right arc shows a clear gap. The
render itself is symmetric (the sim golden proves that), so either the
bezel's visible cutout isn't centered on the pixel array, or the SPD2010's
GRAM→glass mapping carries an offset `ff_display.c` doesn't currently
correct for (`esp_lcd_panel_set_gap` is called with `(0, 0)` — see that
call site's comment in `ff_display_panel_init`).

A second observation sharpened the diagnosis: on the current build (no
tileview drawn) a **thin arc curving the opposite way** is also visible at
the far right edge of the glass, alongside the ring being shifted left.
Taken together, the working model is: **the image is shifted LEFT by `dx`
pixels, and the leftmost `dx` framebuffer columns WRAP AROUND to the right
edge of the panel** (a GRAM column-wrap, not a simple crop) — the ring's
hidden left arc reappears, mirrored, at the right. That wrap is itself
evidence the SPD2010's addressable GRAM extends further than the 412px
window `ff_display.c` currently writes: a plain crop (writes past the
visible window silently discarded) could not produce a reappearing arc at
all.

## What the pattern draws (`CONFIG_FF_GLASS_RULER`)

Flashed with `CONFIG_FF_GLASS_RULER=y` (`idf.py menuconfig` → "Firefly
bring-up" → "Draw the glass-ruler calibration pattern at boot and hold
it"), the device draws this directly to the panel — no LVGL, same raw
`esp_lcd_panel_draw_bitmap` path the STAGE-1 test pattern and the boot
splash already use (`ff_display_draw_glass_ruler`,
`firmware/targets/esp32s3/components/ff_display/ff_display.c`) — right
after the panel comes up, then **holds it on glass forever** (a 1s
heartbeat log, never continuing to the splash/LVGL/normal boot):

- A 1px crosshair through pixel center (206,206), full width and height.
- A 1px circle at `r = 205` centered on (206,206) — the framebuffer's
  outermost circle at that center (it comes within 1px of the x=0/y=0
  edges and touches x=411/y=411 exactly). Muted.
- Four **rulers**, each a 1px baseline plus tick marks every 2px inward
  from the panel's own edge, alternating short (4px) / long (8px), with a
  distinct LONG tick (12px long, 2px wide) every 10px — so ticks are
  countable by eye (0, 2, 4, 6, 8, **10**, 12, … 40) without needing
  numerals:
  - **LEFT** (row 206, x = 0..40) — entirely **crew pink** (baseline and
    every tick), a color deliberately distinct from the other three, since
    this is the side under active investigation.
  - **RIGHT** (row 206, x = 371..411), **TOP** (col 206, y = 0..40),
    **BOTTOM** (col 206, y = 371..411) — ink baseline/minor ticks, amber
    major (10px) ticks, unchanged from the original convention.
- Four small filled squares (6px) at the r=205 circle's own 45-degree
  points — `(206 ± 145, 206 ± 145)`, `145 = round(205 · cos 45°)` — amber,
  so a diagonal misalignment (rotation/scale, not a pure X/Y shift) would
  be visible even if the axis rulers alone wouldn't catch it.
- **The wrap-test stripe blocks** (the primary read, added after the
  mirrored-arc observation above):
  - **LEFT**: framebuffer columns x = 0..11, full height, alternating 1px
    **ink/pink** vertical stripes.
  - **TOP**: rows y = 0..11 (x ≥ 12, so it doesn't clobber the corner of
    the left block), alternating 1px **ink/teal** horizontal stripes — the
    same wrap check on the Y axis, a different color pair so the two
    blocks (and which axis wrapped) are never ambiguous.
  - RIGHT and BOTTOM get no stripe block on purpose: a clean, plain ruler
    on those edges — no stray ink/pink or ink/teal pixels bleeding into
    their ink/amber ticks — is itself the "nothing wraps here" signal.

## How to read it

**Primary — count the wrap directly.** Look at the panel's visible RIGHT
edge. However many of the LEFT block's alternating ink/pink columns
reappear there (not hidden under the bezel) **is `dx`**, counted directly
— no arithmetic. If instead the LEFT block is simply hidden under the
bezel with nothing reappearing on the right, the wrap model is wrong for
this unit; fall back to the ruler-tick method below. Read `dy` the same
way from the TOP block's ink/teal stripes, watching the BOTTOM edge.

**Secondary — cross-check with the ruler ticks.** Count how many ticks
(each = 2px) on each ruler are hidden under the bezel:

```
dx = (ticks hidden on LEFT − ticks hidden on RIGHT) / 2
dy = (ticks hidden on TOP  − ticks hidden on BOTTOM) / 2
```

Positive `dx` means the glass's visible center is to the **right** of the
pixel-array center (more of the LEFT ruler is hidden than the RIGHT).
Positive `dy` likewise means the glass center is **below** the pixel
center. This should agree with the direct stripe count above; if it
doesn't, note the discrepancy — it means the offset isn't a pure
translation (check the corner squares next).

The four corner squares confirm (or rule out) a pure X/Y translation: if
all four are equally in/out from the bezel edge along their own diagonal,
the offset is a simple shift and `dx`/`dy` alone describe it fully.

## The correction, next slice

### Primary: `esp_lcd_panel_set_gap(panel, dx, dy)`

`ff_display_panel_init` already calls this (`FF_LCD_X_GAP` /
`FF_LCD_Y_GAP` in `ff_display.c`, currently `(0, 0)` — see that
constant's own comment block for the existing right/bottom-wrap tuning
note it was written against). Investigated in the vendored
`esp_lcd_spd2010` managed component (fetched via `idf_component.yml`,
`espressif/esp_lcd_spd2010 ^2.0.0`; cached locally under
`~/Library/Caches/Espressif/ComponentManager/.../espressif__esp_lcd_spd2010_2.0.0~1_2b62e656/esp_lcd_spd2010.c`
on this machine — the component manager's cache path includes a content
hash, so re-confirm the line numbers below match on another machine
rather than trusting them blindly):

- `panel_spd2010_draw_bitmap` (~line 672) adds the stored gap directly to
  the write window **before** issuing the column/row-address-set commands:

  ```c
  x_start += spd2010->x_gap;
  x_end   += spd2010->x_gap;
  y_start += spd2010->y_gap;
  y_end   += spd2010->y_gap;
  // ...
  tx_param(spd2010, io, LCD_CMD_CASET, ...);  // 0x2A
  tx_param(spd2010, io, LCD_CMD_RASET, ...);  // 0x2B
  ```

  i.e. the gap is exactly a "GRAM column/row-start offset" — precisely the
  primitive this correction needs, and it explains the wrap symptom
  cleanly: with `x_gap=0` our column-0 write lands at GRAM column 0, but
  if the panel's *visible* window starts at some column `dx > 0`, columns
  `0..dx-1` never make it onto glass on their own — they show whatever the
  RAMWR auto-increment carried into that address range from the *same*
  write (the previous window's wraparound), which is exactly a mirrored
  sliver of the far side of the image. A gap of `dx` shifts our writes so
  framebuffer column 0 lands at the window's true visible start.
- `panel_spd2010_set_gap` (~line 743) simply stores `x_gap`/`y_gap` into
  the panel struct — no other side effect, confirming it's safe to call
  once at init as `ff_display_panel_init` already does.
- The vendored **default init sequence's own** CASET/RASET writes (e.g.
  `{0x2A, {0x00,0x00,0x01,0x9B}, 4, 0}` / `{0x2B, {0x00,0x00,0x01,0x9B},
  4, 0}`) program a `0..0x19B` (0..411) window — exactly 412px, matching
  the panel resolution with **no offset baked into the vendor init
  table**. So there's no datasheet/driver-documented constant to read off
  — the wrap symptom itself is the only evidence of spare GRAM in this
  direction, and this ruler pattern is how to measure its size directly
  rather than guess it.

Once `dx`/`dy` are read off the glass, set `FF_LCD_X_GAP`/`FF_LCD_Y_GAP` in
`ff_display.c` to those values (X is the SPD2010's 4-px-aligned axis per
the existing comment there, so round `dx` to a multiple of 4 if it isn't
already one) and re-flash with `CONFIG_FF_GLASS_RULER` back off to confirm
the normal Radar ring now centers on the bezel.

### Fallback: theme-level `FF_THEME_GLASS_DX`/`FF_THEME_GLASS_DY`

If `set_gap` doesn't clean up the render on real hardware (unexpected
panel behavior, or the offset turns out not to be a simple GRAM
column/row shift), the fallback is a small constant pair
(`FF_THEME_GLASS_DX`/`FF_THEME_GLASS_DY`, `firmware/app/theme/ff_theme.h`)
applied as a layout offset to every screen's root container. This is more
invasive — it's an `app/` change, not a `ff_display` HAL change — and,
critically, it does **not** fix the two RAW draws that run before LVGL
exists (`ff_display_draw_test_pattern`, `ff_display_draw_boot_splash`),
which would still assume `(0, 0)` is correct. `set_gap` is the right first
attempt because it corrects the offset at the one HAL seam every draw path
(raw and LVGL alike) shares.

### Touch-calibration caveat

**A gap change moves every displayed pixel relative to the physical touch
sensor** — the SPD2010 touch controller and the display panel are two
independent silicon paths that only share the glass, not GRAM addressing.
The currently-solved `ff_touchcal_t` transform (S15 slice d, `Settings →
Calibrate Touch`) was fit against the OLD (uncorrected) pixel-to-glass
mapping. After landing either correction above, **re-run touch
calibration** — the old fit will be off by roughly the same `dx`/`dy` that
was just corrected on the display side.


## Measurement result — board 2 (2026-09-02)

Read off the ruler by the maintainer over three flashes:

| gap (x, y) | observation |
|---|---|
| (0, 0) | whole image visible but sitting left: pink block ~5/12 columns out from the left bezel, lit background right of column 411, r=205 ring's left arc under the bezel; **no wrap** (nothing striped at the right edge) — the panel's addressable area is wider than 412 and the glass is centred on it |
| (5, 0) | all four corner squares visible and symmetric, ring complete, pink block fully visible; bottom gap slightly larger than top |
| (6, 3) | bottom-right corner square hidden, left ruler more visible than right — overshoot |
| (5, 1) | all four corner squares visible, rulers balanced — but stray white lines at the top and right edges: the SPD2010 needs 4-px-aligned flush windows in panel coordinates, and a non-multiple-of-4 gap misaligns every flush |
| **(4, 0)** | **nearest multiple-of-4 gap; adopted** (within the eye's resolution of (5,1)) |

`FF_LCD_X_GAP 4`, `FF_LCD_Y_GAP 0` in `ff_display.c` (statically asserted to multiples of 4). The outer r=205 ring sits on the
last pixel and is only visible at some viewing angles — the bezel lip covers the outermost
pixels; that is expected and symmetric. Touch calibration was re-run after this change
(pixels moved under the sensor). Other boards of the same SKU should be re-measured with the
ruler rather than assumed identical.
