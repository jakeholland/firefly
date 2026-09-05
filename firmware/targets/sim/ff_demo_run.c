/**
 * ff_demo_run.c — see ff_demo_run.h. The sim's `--demo` render driver.
 *
 * Boots a real (no-transport) ff_shell_t and seeds it into Firefly Fields
 * through ff_demo_seed — the same core APIs a live mesh would drive — then
 * renders each face. The four Radar shots re-seed a fresh shell with a
 * different default selection so each of the four Radar states (live arrow
 * / close-range / stale / no-fix) can be captured; Now/Map/Signals render
 * from the canonical seed. Nothing here fakes state: the wall clock is
 * latched, freshness is aged, and the faces render whatever core computes.
 */
#include "ff_demo_run.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_demo.h"
#include "ff_demo_festpack.h"
#include "ff_gesture_glue.h" /* S28 slice b — on-glass BACK/HOME/long-press-flare */
#include "ff_intent.h"
#include "ff_shell.h"
#include "screenshot.h"
#include "sim_lifecycle.h" /* debt/sim-window-lifecycle — the idle+rebuild pump shared with ctl_loop.c/main.c */

#define FF_DEMO_WINDOW_W 412
#define FF_DEMO_WINDOW_H 412

/* The monotonic clock the demo shell reads: a single counter ff_demo_seed
 * pins to FF_DEMO_NOW_MS, so every face renders at the seeded instant. */
static uint32_t s_demo_clock_ms;
static uint32_t demo_clock_now_ms(void *user)
{
    (void)user;
    return s_demo_clock_ms;
}

/* Frozen LVGL tick for the headless renders — deterministic frames (same
 * reasoning as main.c's ff_mock_tick_cb: a single lv_refr_now with no
 * timers/anims run never reads it, but freezing makes it certain). */
static uint32_t demo_frozen_tick(void)
{
    return 0;
}

static void demo_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t demo_win_tick(void)
{
    return SDL_GetTicks();
}

/* Seed a fresh shell into the demo world with `primary` as the default
 * Radar selection. Returns 0 on success. */
static int demo_seed_shell(ff_shell_t *shell, fp_pack_t *pack, ff_clock_t *clk, uint32_t primary)
{
    /* Static, not stack: same "fixed arena, sim/host don't care" call as
     * everything else in this file — see fp_pack.h's FP_MAX_TOKENS. */
    static jsmntok_t s_toks[FP_MAX_TOKENS];

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = clk;
    cfg.store = NULL;
    cfg.pack = pack;
    cfg.toks = s_toks;
    cfg.ntoks = FP_MAX_TOKENS;
    /* cfg.transport zeroed => no transport; events injected by ff_demo_seed. */

    if (ff_shell_init(shell, &cfg) != 0) {
        fprintf(stderr, "ffsim --demo: ff_shell_init failed\n");
        return -1;
    }
    if (ff_demo_seed(shell, (char const *)ff_demo_festpack_json, (size_t)ff_demo_festpack_json_len, &s_demo_clock_ms,
                     primary) != 0) {
        fprintf(stderr, "ffsim --demo: ff_demo_seed failed (festpack parse or wall latch)\n");
        return -1;
    }
    return 0;
}

int ff_run_demo_headless(const char *screenshot_dir)
{
    lv_init();
    lv_tick_set_cb(demo_frozen_tick);

    int32_t const w = FF_DEMO_WINDOW_W;
    int32_t const h = FF_DEMO_WINDOW_H;
    uint32_t const buf_size = (uint32_t)(w * h * 4);
    uint8_t *xrgb_buf = malloc(buf_size);
    if (xrgb_buf == NULL) {
        fprintf(stderr, "ffsim --demo: out of memory allocating framebuffer\n");
        lv_deinit();
        return 1;
    }

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, demo_flush_cb);
    lv_display_set_default(disp);

    struct {
        char const *name;
        ff_app_face_t face;
        uint32_t primary;
    } const shots[] = {
        {"radar", FF_APP_FACE_RADAR, FF_DEMO_NODE_DANA},        /* live arrow */
        {"radar_riley", FF_APP_FACE_RADAR, FF_DEMO_NODE_RILEY}, /* close-range */
        {"radar_maya", FF_APP_FACE_RADAR, FF_DEMO_NODE_MAYA},   /* stale — LAST SEEN 25 MIN */
        {"radar_sam", FF_APP_FACE_RADAR, FF_DEMO_NODE_SAM},     /* no fix */
        {"now", FF_APP_FACE_LINEUP, 0},
        {"map", FF_APP_FACE_MAP, 0},
        {"signals", FF_APP_FACE_INBOX, 0},
    };

    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_clock_t clk;
    clk.now_ms = demo_clock_now_ms;
    clk.user = NULL;

    int rc = 0;
    for (size_t i = 0; i < sizeof(shots) / sizeof(shots[0]); i++) {
        if (demo_seed_shell(&shell, &pack, &clk, shots[i].primary) != 0) {
            rc = 1;
            break;
        }
        (void)ff_shell_tick(&shell, s_demo_clock_ms);

        /* Render the requested face. active_face selects the builder;
         * every sub-view (radar/now/map/inbox) is already populated in
         * the projection regardless of which one is "active". */
        ff_app_state_t view = *ff_shell_view(&shell);
        view.active_face = shots[i].face;

        lv_obj_clean(lv_screen_active());
        ff_build_face_screen(&view);
        lv_refr_now(disp);

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s.png", screenshot_dir, shots[i].name);
        if (ff_screenshot_write(path, xrgb_buf, w, h) != 0) {
            rc = 1; /* ff_screenshot_write already logged the detail */
        } else {
            printf("ffsim --demo: wrote %s\n", path);
        }

        ff_shell_close(&shell);
    }

    free(xrgb_buf);
    lv_deinit();
    return rc;
}

int ff_run_demo_window(void)
{
    lv_init();
    lv_tick_set_cb(demo_win_tick);

    lv_display_t *disp = lv_sdl_window_create(FF_DEMO_WINDOW_W, FF_DEMO_WINDOW_H);
    lv_sdl_window_set_title(disp, "Firefly (ffsim --demo: Firefly Fields)");

    /* debt/sim-window-lifecycle: a gated pointer indev, not a bare
     * lv_sdl_mouse_create() — same reasoning as main.c's live window
     * loop (see sim_lifecycle.h's top comment): every live sim pointer
     * now goes through the wake-only-touch gate. `now_ms_cb` reads
     * SDL_GetTicks (real elapsed dev-desk time), deliberately NOT the
     * demo world's own frozen `s_demo_clock_ms` (pinned to Sat 21:30,
     * S20 static) — DIM/OFF/SLEEP must be observable by a human sitting
     * at this window in real time regardless of what fictional in-world
     * instant the seeded festival shows. */
    static ff_sim_lifecycle_t s_demo_lifecycle;
    static ff_sim_lifecycle_pointer_ctx_t s_demo_pointer_ctx;
    ff_sim_lifecycle_init(&s_demo_lifecycle);
    s_demo_pointer_ctx.lc = &s_demo_lifecycle;
    s_demo_pointer_ctx.now_ms_cb = SDL_GetTicks;
    lv_indev_t *pointer_indev = lv_indev_create();
    lv_indev_set_type(pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(pointer_indev, &s_demo_pointer_ctx);
    lv_indev_set_read_cb(pointer_indev, ff_sim_lifecycle_pointer_read_cb);

    static ff_shell_t shell;
    static fp_pack_t pack;
    static ff_clock_t clk;
    clk.now_ms = demo_clock_now_ms;
    clk.user = NULL;

    if (demo_seed_shell(&shell, &pack, &clk, FF_DEMO_NODE_DANA) != 0) {
        return 1;
    }

    /* Bind the seam so swipe/tap navigate the seeded world. */
    ff_intent_emit_bind(ff_shell_intent_sink, &shell);
    ff_gesture_glue_attach(pointer_indev, &shell); /* S28 slice b */

    (void)ff_shell_tick(&shell, s_demo_clock_ms);
    ff_build_face_screen(ff_shell_view(&shell));

    printf("ffsim --demo: Firefly Fields is live (Sat 21:30). Swipe/tap to explore. Ctrl+C to quit.\n");

    while (true) {
        uint32_t const now_ms = SDL_GetTicks(); /* idle/lifecycle clock — see the note above */
        bool const dirty = ff_shell_tick(&shell, s_demo_clock_ms);
        bool const shell_wake = ff_shell_take_wake(&shell); /* S26(c)+(d) banner wake */
        ff_app_state_t const *view = ff_shell_view(&shell);

        int mx, my;
        bool const finger_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0u;
        bool const keep_awake = ff_shell_keep_awake(view, false); /* no blocking touch-cal flow in the sim */

        ff_idle_state_t const idle_state =
            ff_sim_lifecycle_pump(&s_demo_lifecycle.idle, &s_demo_lifecycle.rebuild_pending,
                                   &s_demo_lifecycle.rebuild_count, now_ms, dirty, shell_wake, finger_down,
                                   keep_awake, /* sleep_inhibit */ false, view);
        ff_sim_lifecycle_apply_blank_overlay(idle_state);

        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 ? next_ms : 1);
    }
}
