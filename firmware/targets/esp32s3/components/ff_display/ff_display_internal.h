/**
 * ff_display_internal.h — PRIVATE shared state and helpers for the
 * ff_display component's three translation units (ff_display.c,
 * ff_display_debug.c, ff_display_cal.c). Lives in the component's source
 * dir, NOT under include/ — it is never installed as a component include
 * dir and never included from the public include/ff_display.h. That
 * header's contract (every function it declares, FF_BL_MIN_PCT/_MAX_PCT)
 * is UNCHANGED by the split this header exists for; nothing declared here
 * leaks into it.
 *
 * ---- Design choice: extern renamed statics, not accessor functions ----
 * Before the split, ff_display.c was one file with a block of `static`
 * file-scope handles/state at its top, read and written directly
 * (`s_panel`, `s_screen_flip`, `s_active_cal`, ...) by whichever function
 * in that ONE file needed them. Splitting the file across compilation
 * units means the handful of that state a MOVED function still touches
 * can no longer be `static` to ff_display.c alone — it has to be visible
 * from more than one .c file.
 *
 * Two ways to do that: (a) accessor functions (`ffd_get_panel()` /
 * `ffd_set_panel(...)`) or (b) `extern` declarations of the same
 * variables, renamed and de-`static`d, defined in the one file that
 * already owns their lifetime (ff_display.c — every one of these is
 * FIRST written by a function that stays there: panel_init, lvgl_start,
 * touch_start, set_flip, touch_set_cal, ff_touchcal_process_cb). This
 * header takes (b). The task this split serves is explicitly a PURE
 * MECHANICAL MOVE — zero behaviour change — and every read/write site of
 * this state already exists, unchanged, at its original call site;
 * wrapping each in an accessor call would touch every one of those
 * sites, growing the diff and the review surface for no behavioural
 * gain. It would also change the torn-read contract three of these
 * variables lean on (`ffd_active_cal`, `ffd_screen_flip`,
 * `ffd_cal_capturing` — see the "torn read across the write is
 * harmless" reasoning at their definitions in ff_display.c), which
 * assumes a direct memory read, not a function call a future editor
 * could reasonably grow to do more than that. `extern` keeps every call
 * site textually identical (only the declaration/definition moved),
 * which is what "mechanical" means here.
 *
 * The `ffd_` prefix marks a symbol crossing a translation-unit boundary
 * within this component (grep for `ffd_` to find every one) — distinct
 * from the file-local `s_` convention ff_display.c still uses for state
 * no moved function ever touches (the I2C bus, the IO expander handle,
 * the backlight LEDC readiness flag, the touch indev pointer, the idle-
 * gate wiring: all genuinely single-file and left exactly as they were).
 *
 * TAG is deliberately NOT declared here: each of the three .c files
 * carries its own identical `static const char *TAG = "ff_display";` —
 * the ordinary ESP-IDF per-file-TAG convention. It is a logging label,
 * not shared mutable state, so it needs no cross-TU linkage.
 */
#ifndef FF_DISPLAY_INTERNAL_H
#define FF_DISPLAY_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_types.h" /* esp_lcd_panel_handle_t */
#include "esp_lcd_touch.h" /* esp_lcd_touch_handle_t */
#include "lvgl.h"           /* lv_display_t */
#include "ff_touchcal.h"    /* ff_touchcal_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Panel geometry shared by all three files (draw dims, ruler math,
 * calibration target placement). FF_LCD_BITS_PER_PIXEL and the panel
 * gap / 4-px-alignment constants stay local to ff_display.c — no moved
 * function reads them, so they did not move here. */
#define FF_LCD_H_RES 412
#define FF_LCD_V_RES 412

/* ---- RGB565 byte swap: the SPD2010 wants big-endian pixels over the
 * wire, the ESP32 stores RGB565 little-endian (see ff_display.c's
 * top-of-file comment and ff_display_debug.c's "first light" section for
 * the full rationale — the swap is exactly what STAGE 1 proves correct).
 * Three call sites, in all three files: the boot splash (stays in
 * ff_display.c), the STAGE-1 test pattern and the glass ruler (both in
 * ff_display_debug.c). Kept `static inline` exactly as it was as a
 * single-file helper before this split — each TU that uses it now gets
 * its own copy of the same one-line body, the same outcome the compiler
 * already had a free hand to produce with three call sites in one file
 * beforehand (see the PR body's codegen-identity note). */
static inline uint16_t ff_rgb565_swap(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

/* ---- Cross-file shared state (see the design-choice comment above).
 * Each is DEFINED (non-static) at its original point in ff_display.c,
 * with its full original doc comment kept there; this is only the
 * extern declaration that lets ff_display_debug.c / ff_display_cal.c
 * see it. */

/** ffd_panel — the live SPD2010 panel handle (was `s_panel`). Set by
 * ff_display_panel_init (ff_display.c); read by the STAGE-1 test pattern
 * and the glass ruler (ff_display_debug.c) for the same
 * "called before panel_init?" NULL check every raw-draw function here
 * already does. */
extern esp_lcd_panel_handle_t ffd_panel;

/** ffd_touch — the live SPD2010 touch handle (was `s_touch`). Set by
 * ff_display_touch_start (ff_display.c); read by
 * ff_display_run_calibration (ff_display_cal.c) as its own
 * "touch_start ran first?" precondition check. */
extern esp_lcd_touch_handle_t ffd_touch;

/** ffd_lv_disp — the live LVGL display (was `s_lv_disp`). Set by
 * ff_display_lvgl_start (ff_display.c); read by
 * ff_display_run_calibration (ff_display_cal.c), same precondition
 * pattern as ffd_touch above. */
extern lv_display_t *ffd_lv_disp;

/** ffd_active_cal — the active touch-calibration transform (was
 * `s_active_cal`). Identity until ff_display_touch_set_cal installs a
 * fit. Written/read by ff_touchcal_process_cb and
 * ff_display_touch_set_cal (both stay in ff_display.c); ALSO reset to
 * identity by ff_display_run_calibration (ff_display_cal.c) before a
 * capture, per that function's "capture RAW" contract. See the full
 * torn-read reasoning reproduced at this variable's definition in
 * ff_display.c. */
extern ff_touchcal_t ffd_active_cal;

/** ffd_screen_flip — the active screen-flip flag (was `s_screen_flip`).
 * Written by ff_display_set_flip; read by ff_display_lvgl_start's
 * rotation seed and ff_touchcal_process_cb (both ff_display.c), AND by
 * the crosshair-capture flow (ff_display_cal.c) to record each target's
 * PHYSICAL (flip-aware) position — see ff_cal_release_cb's own comment
 * at its definition in ff_display_cal.c for why. */
extern bool ffd_screen_flip;

/** ffd_cal_capturing — true only while ff_display_run_calibration's
 * crosshair capture is live (was `s_cal_capturing`). Set/cleared by
 * ff_display_run_calibration (ff_display_cal.c); read by
 * ff_touchcal_process_cb (ff_display.c) to skip the screen_flip rotation
 * step during capture — see this flag's full doc comment at its
 * definition in ff_display.c. */
extern volatile bool ffd_cal_capturing;

#ifdef __cplusplus
}
#endif

#endif /* FF_DISPLAY_INTERNAL_H */
