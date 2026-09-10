/**
 * test_scr_music_sprite_alloc_fail.c — fix/s31-sprites-psram
 * (2026-09-09): the sim-side regression test for the internal-RAM boot-
 * parking bug this PR fixes.
 *
 * ## What actually happened (bench evidence, puck flashed with main
 * a853bb5, PR #252's S31 canvas renderer)
 * `scr_music.c`'s glow-sprite table (`s_sprites`, 8 pre-rendered 45x45
 * RGB565+alpha sprites, ~47.5KB — see that file's own top comment,
 * "Renderer") used to be a plain `static` array: ordinary internal
 * `.bss`. On the esp32s3 target, internal RAM is ALSO where `esp_lvgl_
 * port`'s own (DMA-capable) display buffers must be allocated from —
 * that 47.5KB pushed the internal-RAM budget below what the LVGL port
 * needed, and the device parked forever on the boot splash: `E LVGL:
 * lvgl_port_add_disp_priv(389): Not enough memory for LVGL buffer (buf2)
 * allocation!` -> `ff_display: lvgl_port_add_disp failed` -> `firefly:
 * parked: LVGL display bring-up failed`. Previous main (3ecaae7) booted
 * fine — this file's table is exactly what changed.
 *
 * ## Why the sim can't reproduce the ORIGINAL bug
 * The sim has no internal-RAM/PSRAM distinction to exhaust (`docs/
 * specs/S31-music-swarm.md`'s dated amendment: the map-file `.dram0.bss`
 * budget check, `tools/check_dram_budget.py`, wired into the esp32 CI
 * job, is the real regression test for THAT). What the sim CAN and DOES
 * prove is the other half of this fix: `s_sprites` is now allocated
 * lazily (`music_ensure_sprites`, scr_music.c) and MUST be NULL-safe
 * exactly like `s_canvas_buf`/`music_ensure_canvas_buf` already is — a
 * failed allocation must never crash the draw path, just draw fewer
 * fireflies. `ff_scr_music_debug_force_sprite_alloc_fail` (sim-only,
 * scr_music.h) injects that failure deterministically, without a real
 * OOM.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "ff_app_state.h"
#include "ff_theme.h"
#include "scr_music.h"

#include "lvgl.h"

void setUp(void) {}

/* P0 harness-hang fix — same tearDown-safety-net shape every other test
 * file in this directory carries (test_ctl_music_idle_drain.c's own
 * tearDown comment has the full repro/verification writeup): a failed
 * TEST_ASSERT before lv_deinit()/the alloc-fail reset below would
 * otherwise longjmp past both and leak state into the next test. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
    ff_scr_music_debug_force_sprite_alloc_fail(false); /* MUST reset — this suite's "tests own their state" rule */
}

static uint32_t music_sprite_test_tick_cb(void)
{
    return 0; /* frozen clock — same determinism story as run_goldens.sh --mock-clock */
}

static void music_sprite_test_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* Builds the REAL Music screen (no fixture needed — `ff_scr_music_build`
 * is NULL-safe on `state` but otherwise unconditional, unlike `ff_
 * build_face_screen`'s active-face dispatch) against a zeroed `ff_app_
 * state_t` (an honest "nothing is listening yet, --:-- clock" state —
 * exactly the least-claiming defaults `ff_app_state.h`'s own field
 * comments document) and returns the built puck object for inspection. */
static lv_obj_t *build_music_screen(lv_display_t *disp, ff_app_state_t *state)
{
    memset(state, 0, sizeof(*state));
    ff_scr_music_build(state);
    lv_refr_now(disp); /* drives the settle-step draw synchronously, same as test_scr_inbox_hint.c */
    return lv_screen_active();
}

static void S31_sprite_alloc_failure_draws_without_crashing(void)
{
    lv_init();
    lv_tick_set_cb(music_sprite_test_tick_cb);

    int32_t const w = FF_THEME_WINDOW_PX;
    int32_t const h = FF_THEME_WINDOW_PX;
    uint32_t const buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL(buf);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, music_sprite_test_flush_cb);
    lv_display_set_default(disp);

    /* ---- Inject the failure BEFORE the first-ever build (this
     * process's very first `music_build_sprites` call) so it actually
     * takes the allocation-failed branch instead of hitting the
     * `s_sprites_ready` early-return from some earlier test in the same
     * binary. ---- */
    ff_scr_music_debug_force_sprite_alloc_fail(true);

    ff_app_state_t state;
    lv_obj_t *screen = build_music_screen(disp, &state);
    TEST_ASSERT_NOT_NULL_MESSAGE(screen, "screen must still build with no sprites");

    TEST_ASSERT_FALSE_MESSAGE(ff_scr_music_debug_sprites_ready(),
                               "sprites must NOT read ready when the injected allocation failure fired");

    /* ---- Positive control: the rest of the face still built normally
     * (clock chrome), so "no crash" isn't just "nothing ran at all". ---- */
    lv_obj_t *clock_lbl = NULL;
    uint32_t const n = lv_obj_get_child_count(screen);
    for (uint32_t i = 0; i < n && clock_lbl == NULL; i++) {
        lv_obj_t *puck = lv_obj_get_child(screen, i);
        uint32_t const pn = lv_obj_get_child_count(puck);
        for (uint32_t j = 0; j < pn; j++) {
            lv_obj_t *child = lv_obj_get_child(puck, j);
            if (lv_obj_check_type(child, &lv_label_class)) {
                clock_lbl = child;
                break;
            }
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(clock_lbl, "the clock label must still build even with no sprite table");

    /* ---- Drive a few more real frames (the per-frame ticker, which is
     * what actually calls music_composite_particle for all 60 fireflies
     * every tick) — this is the line that would crash before this PR's
     * NULL check if a sprite-alloc failure survived past the first
     * frame. ---- */
    for (int i = 0; i < 5; i++) {
        lv_tick_inc(33);
        lv_timer_handler();
    }
    lv_refr_now(disp);
    TEST_ASSERT_FALSE_MESSAGE(ff_scr_music_debug_sprites_ready(), "still no sprites after several more frames");

    free(buf);
    lv_deinit();
    ff_scr_music_debug_force_sprite_alloc_fail(false);

    /* ---- Recovery: a FRESH Music session (process-lifetime `s_sprites`
     * is per-process, not per-screen, but the allocation is retried on
     * every `music_build_sprites` call while `s_sprites == NULL` — see
     * that function's own doc comment) with the injected failure lifted
     * must now succeed. Proves the failure path is a transient "this
     * allocation attempt failed", never a permanent "this build is
     * poisoned forever". ---- */
    lv_init();
    lv_tick_set_cb(music_sprite_test_tick_cb);
    buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL(buf);
    disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, music_sprite_test_flush_cb);
    lv_display_set_default(disp);

    screen = build_music_screen(disp, &state);
    TEST_ASSERT_NOT_NULL(screen);
    TEST_ASSERT_TRUE_MESSAGE(ff_scr_music_debug_sprites_ready(),
                              "sprites must recover on the next build once the injected failure is lifted");

    free(buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S31_sprite_alloc_failure_draws_without_crashing);
    return UNITY_END();
}
