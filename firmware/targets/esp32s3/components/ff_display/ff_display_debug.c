/**
 * ff_display_debug.c — bring-up-only diagnostic draw surfaces for the
 * Waveshare ESP32-S3-Touch-LCD-1.46 panel, split out of ff_display.c
 * (tech-debt sprint, device-target slice: a pure mechanical move, zero
 * behaviour change — see that PR for the file map and rationale).
 *
 * Both surfaces here draw RAW via esp_lcd_panel_draw_bitmap, no LVGL:
 *
 *   - ff_display_draw_test_pattern — STAGE 1 "first light": a solid fill
 *     then a two-colour split, proving orientation and RGB565 byte order
 *     with no GUI stack in the way (docs/specs/S15b). Always compiled in
 *     (every bring-up stage can reach it).
 *
 *   - ff_display_draw_glass_ruler — the CONFIG_FF_GLASS_RULER (default
 *     OFF) bezel/pixel-array offset diagnostic (docs/hardware/glass-
 *     offset.md). Compiled out entirely when the Kconfig option is off,
 *     exactly as before this split.
 *
 * Neither surface is reachable from the normal boot path (app_main only
 * calls into these under an explicit bring-up stage or Kconfig gate);
 * the honest-data rule (AGENTS.md: "honesty rules bind debug surfaces
 * too") still applies to every log line here.
 *
 * ffd_panel and ff_rgb565_swap are shared with ff_display.c (the boot
 * splash) via ff_display_internal.h — see that header's own doc comment
 * for the extern-vs-accessor design choice this split made.
 */
#include "ff_display.h"
#include "ff_display_internal.h"

#include <math.h> /* sqrtf — the glass ruler's outer-circle intersection precompute */

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_display";

/* =====================================================================
 * b1 step 3 — first light: solid fill + two-colour split (no LVGL).
 *
 * SPD2010 takes RGB565 big-endian over the wire; ESP32 stores RGB565
 * little-endian, so each pixel is byte-swapped here (the demo does the
 * same with SPI_SWAP_DATA_TX). Whether the swap is CORRECT is exactly
 * what this stage proves: a swapped-wrong panel shows the wrong colours,
 * and the split's left/right halves prove orientation. draw_bitmap x
 * bounds stay full-width (0..412, both divisible by 4 — the SPD2010's
 * documented alignment rule); the colour boundary lives in the pixel
 * data, not the draw rectangle.
 * ===================================================================== */
esp_err_t ff_display_draw_test_pattern(void)
{
    if (ffd_panel == NULL) {
        ESP_LOGE(TAG, "draw_test_pattern called before panel_init");
        return ESP_ERR_INVALID_STATE;
    }

    /* Draw in horizontal bands from a small INTERNAL-DMA buffer. A full
     * 412x412 frame (~331 KB) cannot be pushed in one draw_bitmap: sending
     * that from PSRAM in a single SPI transaction returns ESP_ERR_NO_MEM
     * (the SPI/esp_lcd layer can't get enough internal DMA for a transfer
     * that large), and a full-frame INTERNAL buffer will not fit (~240 KB
     * free). BAND_LINES divides 412 evenly (412 = 4*103) so every band is
     * 4-line aligned on the y axis, matching the SPD2010's documented
     * draw-alignment rule; the band buffer is a few KB of internal DMA RAM. */
    enum { BAND_LINES = 4 };
    const size_t band_px = (size_t)FF_LCD_H_RES * BAND_LINES;
    uint16_t *band = heap_caps_malloc(band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        ESP_LOGE(TAG, "test pattern: OOM allocating %u byte band buffer", (unsigned)(band_px * 2));
        return ESP_ERR_NO_MEM;
    }

    /* Solid fill: amber (#FFC66B -> RGB565 0xFE2D). Every band is identical. */
    const uint16_t amber = ff_rgb565_swap(0xFE2D);
    for (size_t i = 0; i < band_px; i++) band[i] = amber;
    for (int y = 0; y < FF_LCD_V_RES; y += BAND_LINES) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(ffd_panel, 0, y, FF_LCD_H_RES, y + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "solid-fill band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
            heap_caps_free(band);
            return err;
        }
    }
    ESP_LOGI(TAG, "first light: solid amber fill drawn (%d bands)", FF_LCD_V_RES / BAND_LINES);
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Two-colour split: left half green (#9BE07B->0x9F6F), right half red
     * (#FF0000->0xF800). Boundary at x=206 lives in pixel data; every band
     * is identical, so fill the band once and repeat it down the screen. */
    const uint16_t green = ff_rgb565_swap(0x9F6F);
    const uint16_t red = ff_rgb565_swap(0xF800);
    for (int ly = 0; ly < BAND_LINES; ly++) {
        uint16_t *row = band + (size_t)ly * FF_LCD_H_RES;
        for (int x = 0; x < FF_LCD_H_RES; x++) {
            row[x] = (x < FF_LCD_H_RES / 2) ? green : red;
        }
    }
    for (int y = 0; y < FF_LCD_V_RES; y += BAND_LINES) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(ffd_panel, 0, y, FF_LCD_H_RES, y + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "split band y=%d draw_bitmap failed: %s", y, esp_err_to_name(err));
            heap_caps_free(band);
            return err;
        }
    }
    ESP_LOGI(TAG, "first light: two-colour split drawn (left=green right=red)");

    heap_caps_free(band);
    return ESP_OK;
}

#if CONFIG_FF_GLASS_RULER
/* =====================================================================
 * Device-only diagnostic (CONFIG_FF_GLASS_RULER, default OFF) — the
 * "glass ruler" boot pattern. Measures the bezel/pixel-array offset
 * (docs/hardware/glass-offset.md) by eye: on the current build a
 * software-centered ring renders visibly off-center on the glass (the
 * left arc fully hidden under the bezel, a mirrored sliver reappearing at
 * the far right — a GRAM column-wrap, not a simple crop), so this pattern
 * gives the maintainer something to COUNT rather than eyeball a vague
 * "a few px" shift. Drawn RAW via esp_lcd_panel_draw_bitmap, the exact
 * same band-buffer/byte-swap shape ff_display_draw_test_pattern (this
 * file, above) and ff_display_draw_boot_splash (ff_display.c) already
 * establish — no new draw path.
 *
 * Compiled out entirely when CONFIG_FF_GLASS_RULER is off (the default):
 * this whole block, so a field/demo build is byte-identical either way.
 * ===================================================================== */

/* Pre-swap RGB565 for the theme colors this pattern uses (see
 * ff_rgb565_swap, ff_display_internal.h, and ff_display_draw_boot_splash's
 * comment in ff_display.c on why these are transcribed rather than
 * included — ff_theme.h pulls in LVGL, and this draws before LVGL
 * exists). Computed as ((r>>3)<<11)|((g>>2)<<5)|(b>>3) from the RGB888
 * hex named alongside each — verified with a throwaway script, not
 * hand-arithmetic, given a wrong-but-plausible color here would be
 * exactly the kind of "measuring with a broken ruler" mistake this
 * diagnostic exists to avoid. */
#define FF_RULER_INK   0xF77C /* FF_THEME_COLOR_INK   0xF2EFE6 */
#define FF_RULER_AMBER 0xFE2D /* FF_THEME_COLOR_AMBER 0xFFC66B — same value ff_display_draw_test_pattern's "amber" already uses */
#define FF_RULER_MUTED 0x8C52 /* FF_THEME_COLOR_MUTED 0x8B8A97 */
#define FF_RULER_PINK  0xFAF5 /* FF_THEME_CREW_PINK   0xFF5CA8 */
#define FF_RULER_TEAL  0x4ED8 /* FF_THEME_CREW_TEAL   0x4FD8C4 */
#define FF_RULER_BG    0x0842 /* FF_THEME_COLOR_BG    0x0B0B10 — same value ff_display_draw_boot_splash's "bg" already uses */

/* Ruler tick geometry, shared by all four rulers via `d` = distance
 * inward from the panel's own edge (0, 2, 4, ... 40 — 21 positions). A
 * multiple of 10 is a MAJOR tick: 12px long, 2px wide (vs. the minor
 * ticks' 1px width) so every 10-px mark is unmistakable by eye without
 * needing numerals (the brief's "hard to draw raw" constraint). Non-major
 * ticks alternate short(4px)/long(8px) by (d/2)%2 — purely a visual
 * cadence so consecutive 2px ticks don't blur into one blob; only the
 * majors carry measurement meaning. */
static inline int ff_ruler_tick_len(int d) {
    if (d % 10 == 0) return 12;
    return ((d / 2) % 2 == 0) ? 4 : 8;
}
static inline int ff_ruler_tick_w(int d) { return (d % 10 == 0) ? 2 : 1; }

/* Per-pixel color for the glass-ruler pattern at (x,y). `row_x_left` /
 * `row_x_right` and `col_y_top` / `col_y_bot` are the r=205 outer
 * circle's precomputed intersections for THIS row and THIS column (see
 * ff_display_draw_glass_ruler's comment on why — no per-pixel sqrt over
 * ~170k pixels).
 *
 * Priority (first match wins), high to low:
 *  1. the four 45-degree corner squares
 *  2. the LEFT wrap-test stripe (x<12) — the PRIMARY dx read
 *  3. the TOP wrap-test stripe (y<12) — the PRIMARY dy-wrap read
 *  4. the outer r=205 circle
 *  5-8. the four rulers (LEFT=uniform pink; RIGHT/TOP/BOTTOM=ink+amber)
 *  9. the plain crosshair through (cx,cy) everywhere else
 *  10. theme background
 * Priority matters where regions overlap by construction (e.g. the LEFT
 * ruler's baseline runs through the LEFT stripe's x<12 columns too — the
 * stripe wins there, since it's the primary read this update introduced;
 * the ruler's own ticks from x=12 on are still a secondary cross-check). */
static uint16_t ff_ruler_classify(int x, int y, int row_x_left, int row_x_right,
                                   int col_y_top, int col_y_bot)
{
    const int cx = FF_LCD_H_RES / 2; /* 206 */
    const int cy = FF_LCD_V_RES / 2; /* 206 */

    /* 1. Four 6px filled corner squares at the r=205 circle's own
     * 45-degree points: (cx +/- 145, cy +/- 145), where 145 =
     * round(205 * cos 45deg) = round(205 * 0.70710678). A diagonal
     * misalignment (rotation/scale, not a pure translation) shows up here
     * even if the axis rulers alone wouldn't catch it. */
    {
        const int off = 145;
        const int csx[2] = {cx - off, cx + off};
        const int csy[2] = {cy - off, cy + off};
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                int const ax = csx[i] - 3;
                int const ay = csy[j] - 3;
                if (x >= ax && x < ax + 6 && y >= ay && y < ay + 6) {
                    return FF_RULER_AMBER;
                }
            }
        }
    }

    /* 2. LEFT wrap-test stripe: columns x=0..11, full height, alternating
     * 1px ink/pink verticals. Whatever wraps around to the panel's RIGHT
     * edge under the current (uncorrected) GRAM addressing counts BY EYE
     * as "N striped columns visible at the right edge" = dx directly —
     * see docs/hardware/glass-offset.md. Highest priority below the
     * corners so the count is never occluded by anything else drawn. */
    if (x < 12) {
        return (x % 2 == 0) ? FF_RULER_INK : FF_RULER_PINK;
    }

    /* 3. TOP wrap-test stripe: rows y=0..11 (x already >= 12, claimed by
     * #2 above), alternating 1px ink/teal horizontals — the same wrap
     * check on the Y axis. A different color pair than the left stripe so
     * the two blocks, and which axis wrapped, are never ambiguous. */
    if (y < 12) {
        return (y % 2 == 0) ? FF_RULER_INK : FF_RULER_TEAL;
    }

    /* 4. Outer circle, r=205 from (cx,cy) — the framebuffer's outermost
     * circle at this center (206-205=1, i.e. it comes within 1px of the
     * x=0/y=0 edges and touches x=411/y=411 exactly). 1px ring, muted. */
    if (x == row_x_left || x == row_x_right || y == col_y_top || y == col_y_bot) {
        return FF_RULER_MUTED;
    }

    /* 5. LEFT ruler: baseline row cy, x=0..40. Entirely crew-pink — both
     * the baseline and every tick — a single color deliberately distinct
     * from the ink/amber convention the other three rulers keep, so this
     * one reads as "the ruler on the side that's wrapping" at a glance.
     * Ticks are vertical marks (perpendicular to the horizontal baseline)
     * centered on row cy, growing toward the panel center as x increases
     * from the true edge (x=0). Kept as a cross-check against the stripe
     * read above, not the primary measurement anymore. */
    if (x <= 40) {
        if (y == cy) {
            return FF_RULER_PINK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            if (x < d || x >= d + w) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (y >= cy - half && y < cy + half) {
                return FF_RULER_PINK;
            }
        }
    }

    /* 6. RIGHT ruler: baseline row cy, x=371..411, ink + amber major
     * ticks (unchanged convention) — the dx cross-check, and the place a
     * maintainer confirms the mirrored/wrapped arc the bezel photo showed
     * lines up with what the left stripe predicts. */
    if (x >= 371) {
        if (y == cy) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            int const x2 = 411 - d;
            int const x1 = x2 - (w - 1);
            if (x < x1 || x > x2) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (y >= cy - half && y < cy + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 7. TOP ruler: baseline col cx, y=0..40 (y<12 already claimed by the
     * top stripe, #3), ink + amber major ticks — the dy cross-check. */
    if (y <= 40) {
        if (x == cx) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            if (y < d || y >= d + w) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (x >= cx - half && x < cx + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 8. BOTTOM ruler: baseline col cx, y=371..411, ink + amber major
     * ticks — confirms nothing wraps on this edge either (no stripe block
     * here; a clean plain ruler is itself the "nothing wrapped" signal). */
    if (y >= 371) {
        if (x == cx) {
            return FF_RULER_INK;
        }
        for (int d = 0; d <= 40; d += 2) {
            int const w = ff_ruler_tick_w(d);
            int const y2 = 411 - d;
            int const y1 = y2 - (w - 1);
            if (y < y1 || y > y2) continue;
            int const half = ff_ruler_tick_len(d) / 2;
            if (x >= cx - half && x < cx + half) {
                return (d % 10 == 0) ? FF_RULER_AMBER : FF_RULER_INK;
            }
        }
    }

    /* 9. Plain crosshair through pixel center (cx,cy), everywhere the
     * rulers above don't already claim (the long straight middle span). */
    if (x == cx || y == cy) {
        return FF_RULER_MUTED;
    }

    /* 10. Theme background, everywhere else. */
    return FF_RULER_BG;
}

esp_err_t ff_display_draw_glass_ruler(void)
{
    if (ffd_panel == NULL) {
        ESP_LOGE(TAG, "draw_glass_ruler called before panel_init");
        return ESP_ERR_INVALID_STATE;
    }

    const int cx = FF_LCD_H_RES / 2; /* 206 */
    const int cy = FF_LCD_V_RES / 2; /* 206 */
    enum { RULER_R = 205 };

    /* Outer-circle hit test, gap-free WITHOUT a per-pixel sqrt: precompute
     * the r=205 ring's row-wise (x at each y) AND column-wise (y at each
     * x) intersections ONCE — 412+412 sqrtf calls total, not one per each
     * of the ~170k pixels below. The union of a row-complete and a
     * column-complete sampling of a circle has no gaps at any angle (each
     * axis's own sampling is sparsest exactly where the OTHER axis's is
     * densest) — same "no float cost in the per-pixel hot loop"
     * discipline PR #146 established for the boot splash's ray
     * rasterizer (ff_display_draw_boot_splash, ff_display.c). Heap, not
     * stack: ~1.6KB apiece, freed before return, matching this file's
     * "transient, never a static/.bss allocation" convention for these
     * raw-draw helpers. */
    int *col_y_top = heap_caps_malloc((size_t)FF_LCD_H_RES * sizeof(int), MALLOC_CAP_INTERNAL);
    int *col_y_bot = heap_caps_malloc((size_t)FF_LCD_H_RES * sizeof(int), MALLOC_CAP_INTERNAL);
    if (col_y_top == NULL || col_y_bot == NULL) {
        ESP_LOGE(TAG, "glass ruler: OOM allocating circle column tables");
        heap_caps_free(col_y_top);
        heap_caps_free(col_y_bot);
        return ESP_ERR_NO_MEM;
    }
    for (int x = 0; x < FF_LCD_H_RES; x++) {
        int const ddx = x - cx;
        if (ddx * ddx > RULER_R * RULER_R) {
            col_y_top[x] = -1;
            col_y_bot[x] = -1;
            continue;
        }
        int const dy = (int)(sqrtf((float)(RULER_R * RULER_R - ddx * ddx)) + 0.5f);
        col_y_top[x] = cy - dy;
        col_y_bot[x] = cy + dy;
    }

    enum { BAND_LINES = 4 };
    const size_t band_px = (size_t)FF_LCD_H_RES * BAND_LINES;
    uint16_t *band = heap_caps_malloc(band_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (band == NULL) {
        ESP_LOGE(TAG, "glass ruler: OOM allocating %u byte band buffer", (unsigned)(band_px * 2));
        heap_caps_free(col_y_top);
        heap_caps_free(col_y_bot);
        return ESP_ERR_NO_MEM;
    }

    for (int y0 = 0; y0 < FF_LCD_V_RES; y0 += BAND_LINES) {
        for (int ly = 0; ly < BAND_LINES; ly++) {
            int const y = y0 + ly;
            int row_x_left = -1, row_x_right = -1;
            int const ddy = y - cy;
            if (ddy * ddy <= RULER_R * RULER_R) {
                int const dx = (int)(sqrtf((float)(RULER_R * RULER_R - ddy * ddy)) + 0.5f);
                row_x_left = cx - dx;
                row_x_right = cx + dx;
            }
            uint16_t *row = band + (size_t)ly * FF_LCD_H_RES;
            for (int x = 0; x < FF_LCD_H_RES; x++) {
                uint16_t const raw = ff_ruler_classify(x, y, row_x_left, row_x_right,
                                                        col_y_top[x], col_y_bot[x]);
                row[x] = ff_rgb565_swap(raw);
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(ffd_panel, 0, y0, FF_LCD_H_RES, y0 + BAND_LINES, band);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "glass ruler: band y=%d draw_bitmap failed: %s", y0, esp_err_to_name(err));
            heap_caps_free(band);
            heap_caps_free(col_y_top);
            heap_caps_free(col_y_bot);
            return err;
        }
    }

    heap_caps_free(band);
    heap_caps_free(col_y_top);
    heap_caps_free(col_y_bot);
    ESP_LOGI(TAG, "glass ruler drawn: crosshair + r=%d ring + 4 rulers (2px ticks to 40px) "
                  "+ 45deg corner squares + left/top dx/dy wrap-probe stripes — see "
                  "docs/hardware/glass-offset.md to read it",
             (int)RULER_R);
    return ESP_OK;
}
#endif /* CONFIG_FF_GLASS_RULER */
