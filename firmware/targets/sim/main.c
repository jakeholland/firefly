/**
 * ffsim — Firefly desktop sim target (S13 slice a, extended in slice b).
 *
 * Modes:
 *   ffsim                          window mode: opens an SDL window,
 *                                  runs LVGL's normal timer loop.
 *   ffsim --headless --screenshot DIR
 *                                  renders exactly one frame into an
 *                                  offscreen LVGL buffer (no SDL, no
 *                                  display server required) and writes
 *                                  it to DIR/boot.png, then exits 0.
 *   ffsim --headless --screenshot DIR --fixture FILE.json
 *                                  same, but loads FILE.json into an
 *                                  ff_app_state_t (fixture.h) and renders
 *                                  it instead of the boot screen, writing
 *                                  DIR/<stem>.png. A fixture whose
 *                                  active_face is "radar" gets the real
 *                                  S06 shell + radar face (scr_nav.h);
 *                                  every other face still gets the S13
 *                                  placeholder debug face (fixture_view.h)
 *                                  — see ff_build_face_screen below.
 *   ffsim --fixture FILE.json      window mode with the fixture loaded
 *                                  (interactive preview; same
 *                                  face-selection and load path as
 *                                  headless).
 *
 * --mock-clock freezes the LVGL tick source (see ff_mock_tick_cb below).
 * Headless rendering is deterministic WITHOUT the flag too, but not for
 * the reason an earlier version of this comment claimed: lv_refr_now()
 * does NOT skip the tick — it unconditionally calls lv_anim_refr_now()
 * internally, which reads the tick and runs one animation step (verified
 * against lvgl/src/core/lv_refr.c; this matters now that S06's CLOSE-mode
 * radar face starts a real lv_anim_t for its pulsing rings, see
 * app/screens/scr_radar.c). Determinism instead comes from
 * ff_run_headless() below UNCONDITIONALLY calling
 * lv_tick_set_cb(ff_mock_tick_cb) regardless of whether --mock-clock was
 * passed — the flag is accepted and honored in headless mode purely so
 * callers (tests/run_goldens.sh) can pass it explicitly rather than
 * depending on undocumented default behavior, not because it changes
 * anything here. Getting this reasoning right matters: an "obviously
 * doesn't touch the tick" argument is exactly the kind of thing a future
 * refactor could use to justify removing the unconditional freeze,
 * which would silently reintroduce animation-phase flakiness in
 * tests/run_goldens.sh.
 *
 * The boot screen and the fixture debug face are both scaffolding: real
 * screens arrive with S06+.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "lvgl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "ff_flare.h" /* S10 slice b — ff_flare_init/ff_flare_t, window mode's live engine */
#include "ff_version.h"

#include "fixture.h"
#include "fixture_view.h"
#include "scr_flare.h" /* S10 slice b — full-screen receive takeover */
#include "scr_nav.h"

#define FF_SIM_WINDOW_W 456
#define FF_SIM_WINDOW_H 456

#define FF_COLOR_BG_DARK 0x0b0b10
#define FF_COLOR_AMBER   0xffc66b

/* Builds the boot placeholder UI (dark puck + centered "FIREFLY" label)
 * on whatever the current default display's active screen is. */
static void ff_build_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_SIM_WINDOW_W - 16, FF_SIM_WINDOW_H - 16);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(puck);
    lv_label_set_text(label, "FIREFLY");
    lv_obj_set_style_text_color(label, lv_color_hex(FF_COLOR_AMBER), 0);
    lv_obj_center(label);
}

/* S06 — replaces the S13 debug placeholder with the real shell+radar
 * face whenever a loaded fixture's active_face is radar; every other
 * face still falls through to fixture_view.h's placeholder (Now/Signals/
 * Settings screens arrive with their own specs — S07/S08/S11).
 *
 * S10 slice b: a pending receive takeover (`state->flare.takeover_active`)
 * is checked FIRST and, if true, is the ONLY thing built — per spec
 * ("full-screen takeover regardless of current face"), it replaces
 * whatever face would otherwise show, exactly like a real full-screen
 * interrupt would; `active_face` is not consulted at all on that path.
 * `flare_rt` (NULL in headless one-shot rendering, a real per-process
 * engine in window mode — see ff_run_window below) is forwarded to
 * whichever screen builder needs it for its button callbacks. */
static void ff_build_face_screen(ff_app_state_t const *state, ff_flare_t *flare_rt)
{
    if (state->flare.takeover_active) {
        ff_scr_flare_build_takeover(&state->flare, flare_rt);
        return;
    }
    if (state->active_face == FF_APP_FACE_RADAR) {
        ff_scr_nav_build(state, flare_rt);
    } else {
        ff_fixture_view_build(state);
    }
}

/* Full-frame render mode: the whole buffer is the flushed frame, so the
 * flush callback only needs to signal completion. */
static void ff_headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* --mock-clock: a frozen tick source (always reports the same instant).
 * S13's control-socket-driven "advance mock clock" scenario (spec slice
 * c: inject touch / advance clock / dump state via --ctl PORT) is out of
 * scope for this slice — this stub exists so the flag has real,
 * documented behavior now rather than being a no-op placeholder, and so
 * window mode (which otherwise ticks off SDL_GetTicks, real wall time)
 * can be frozen for manual deterministic testing. */
static uint32_t ff_mock_tick_cb(void)
{
    return 0;
}

/* Converts an LVGL XRGB8888 framebuffer (byte order B,G,R,X per pixel;
 * see lv_color32_t) to tightly packed RGB24 and writes it as a PNG at
 * the exact path given (caller builds the DIR/name.png path). */
static int ff_convert_and_write_png(const char *path, uint8_t const *xrgb_buf, int32_t w, int32_t h)
{
    uint8_t *rgb_buf = malloc((size_t)w * (size_t)h * 3);
    if (rgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %ld byte PNG buffer\n", (long)w * h * 3);
        return 1;
    }
    for (int32_t i = 0; i < w * h; i++) {
        const uint8_t b = xrgb_buf[i * 4 + 0];
        const uint8_t g = xrgb_buf[i * 4 + 1];
        const uint8_t r = xrgb_buf[i * 4 + 2];
        rgb_buf[i * 3 + 0] = r;
        rgb_buf[i * 3 + 1] = g;
        rgb_buf[i * 3 + 2] = b;
    }

    int ok = stbi_write_png(path, w, h, 3, rgb_buf, w * 3);
    free(rgb_buf);

    if (!ok) {
        fprintf(stderr, "ffsim: failed to write %s\n", path);
        return 1;
    }
    printf("ffsim: wrote %s\n", path);
    return 0;
}

/* Renders exactly one frame — either the fixture debug face (if
 * fixture_path is non-NULL) or the boot placeholder — to
 * DIR/<name>.png. Returns 0 on success, 1 on any failure (fixture load,
 * OOM, or PNG write). */
static int ff_run_headless(const char *screenshot_dir, const char *fixture_path)
{
    lv_init();
    lv_tick_set_cb(ff_mock_tick_cb);

    const int32_t w = FF_SIM_WINDOW_W;
    const int32_t h = FF_SIM_WINDOW_H;
    const uint32_t buf_size = (uint32_t)(w * h * 4);

    uint8_t *xrgb_buf = malloc(buf_size);
    if (xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        lv_deinit();
        return 1;
    }

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, ff_headless_flush_cb);
    lv_display_set_default(disp);

    char path[4096];
    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            free(xrgb_buf);
            lv_deinit();
            return 1;
        }
        /* flare_rt = NULL: a headless run renders exactly one frame and
         * exits, so there is no interactivity for any GO/DISMISS/CANCEL
         * button press to act on — see ff_build_face_screen's doc
         * comment and scr_flare.h's top comment for why NULL is always a
         * safe, fully-defined value here (buttons still render, just
         * inertly). */
        ff_build_face_screen(&state, NULL);

        char stem[256];
        ff_fixture_stem(fixture_path, stem, sizeof(stem));
        snprintf(path, sizeof(path), "%s/%s.png", screenshot_dir, stem);
    } else {
        ff_build_boot_screen();
        snprintf(path, sizeof(path), "%s/boot.png", screenshot_dir);
    }

    lv_refr_now(disp);
    int rc = ff_convert_and_write_png(path, xrgb_buf, w, h);

    free(xrgb_buf);
    lv_deinit();
    return rc;
}

static int ff_run_window(const char *fixture_path, bool mock_clock)
{
    lv_init();
    lv_tick_set_cb(mock_clock ? ff_mock_tick_cb : (lv_tick_get_cb_t)SDL_GetTicks);

    lv_display_t *disp = lv_sdl_window_create(FF_SIM_WINDOW_W, FF_SIM_WINDOW_H);
    lv_sdl_window_set_title(disp, "Firefly (ffsim)");
    lv_sdl_mouse_create();

    /* S10 slice b: window mode owns exactly ONE real `ff_flare_t` for the
     * whole process — the live engine GO/DISMISS/CANCEL/FLARE button
     * presses actually mutate (see ff_build_face_screen's doc comment).
     * A local (not static) variable is fine here: ff_run_window never
     * returns before the process exits (its event loop is `while(true)`
     * below), so this outlives every screen built against its address. */
    ff_flare_t flare_rt;
    ff_flare_init(&flare_rt);

    if (fixture_path != NULL) {
        ff_app_state_t state;
        ff_fixture_result_t fr = ff_fixture_load_file(fixture_path, &state);
        if (fr != FF_FIXTURE_OK) {
            fprintf(stderr, "ffsim: failed to load fixture %s (error %d)\n", fixture_path, (int)fr);
            return 1;
        }

        /* PR #20 independent code review, MEDIUM finding: this window is
         * built ONCE from a static `ff_app_state_t` snapshot (`state`)
         * that never changes after load, while `&flare_rt` above is a
         * SEPARATE, real engine the GO/DISMISS/CANCEL/FLARE callbacks
         * genuinely mutate. Nothing re-renders `state` from `flare_rt`
         * (that would need a live crew/name/bearing lookup this
         * fixture-driven sim target has no wiring for at all — out of
         * scope here, and the underlying "nothing re-renders on state
         * change" gap is already tracked as issue #17). The reviewer's
         * concrete repro: load a takeover fixture in window mode and
         * GO/DISMISS silently do nothing ON SCREEN — no visible
         * feedback, no way to tell the click even registered, and (for a
         * full-screen takeover specifically) no way to leave the screen
         * at all short of closing the window. Per the reviewer's own
         * fallback guidance ("if a full fix is out of scope, ensure the
         * window-mode path... logs/labels clearly that it's a static
         * preview" — do not ship a silent trap), this prints an
         * unmissable, impossible-to-miss note up front for exactly the
         * fixtures where that matters (a pending takeover has no other
         * way to dismiss itself), rather than leaving a user to discover
         * the limitation by confused clicking. */
        if (state.flare.takeover_active) {
            fprintf(stderr,
                    "ffsim: NOTE — this is a STATIC single-frame preview. GO/DISMISS "
                    "genuinely mutate the live ff_flare_t (check stdout/a debugger), "
                    "but this window will NOT redraw to reflect it (tracked: issue "
                    "#17 — no live crew/name/bearing wiring exists in this sim target "
                    "to re-derive the display snapshot from). This takeover screen has "
                    "NO on-screen way to leave until that lands — close the window or "
                    "Ctrl+C to exit.\n");
        }

        ff_build_face_screen(&state, &flare_rt);
    } else {
        ff_build_boot_screen();
    }

    while (true) {
        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 ? next_ms : 1);
    }
}

int main(int argc, char **argv)
{
    bool headless = false;
    bool mock_clock = false;
    const char *screenshot_dir = NULL;
    const char *fixture_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_dir = argv[++i];
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            fixture_path = argv[++i];
        } else if (strcmp(argv[i], "--mock-clock") == 0) {
            mock_clock = true;
        }
    }

    printf("ffsim: %s\n", ff_version_string());

    if (headless) {
        if (screenshot_dir == NULL) {
            fprintf(stderr, "ffsim: --headless requires --screenshot DIR\n");
            return 1;
        }
        /* mock_clock is unconditionally honored in headless mode (see
         * ff_run_headless's tick setup) — accepted here without a "not
         * meaningful" warning since passing it explicitly is the
         * documented, supported way callers (tests/run_goldens.sh) opt
         * into that guarantee rather than depending on an undocumented
         * default. */
        (void)mock_clock;
        return ff_run_headless(screenshot_dir, fixture_path);
    }

    return ff_run_window(fixture_path, mock_clock);
}
