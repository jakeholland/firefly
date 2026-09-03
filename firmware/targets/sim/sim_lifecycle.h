/**
 * sim_lifecycle.h — debt/sim-window-lifecycle: the ONE per-tick device
 * lifecycle pump shared by every sim entry point that drives a LIVE
 * `ff_shell_t` frame after frame: the `--ctl PORT` session
 * (`ctl_loop.c`), window mode's live loop (`main.c`'s `ff_run_window`),
 * and `--demo`'s live window loop (`ff_demo_run.c`'s
 * `ff_run_demo_window`).
 *
 * ## What this closes
 *
 * Before this file, `ctl_loop.c` was the ONLY sim entry point that ran
 * the S26 device lifecycle (`ff_idle_init`/`ff_idle_tick`/
 * `ff_idle_touch_gate`/`ff_shell_keep_awake`/`ff_shell_take_wake`) —
 * window mode's live loop and `--demo`'s live loop both rebuilt the
 * LVGL tree unconditionally on any dirty tick, with no gating at all.
 * Consequences: a live tick landing between an SDL mouse-down and
 * mouse-up tore down the button under the cursor — the exact on-glass
 * bug S26's rebuild-mid-tap latch fixed for `app_main.c` and
 * `ctl_loop.c`, never fixed here — and DIM/OFF/SLEEP/notify-wake were
 * completely unobservable in the manual dev loop (window mode is the
 * ONLY place a human ever watches the puck breathe in real time; the
 * ctl loop is headless).
 *
 * ## Design
 *
 * `ff_ctl_loop_ctx_t` (ctl_loop.h) already owns `idle`/`touch_gate`/
 * `rebuild_pending`/`rebuild_count` as directly-named fields —
 * `targets/sim/tests/test_idle_render_skip.c` and
 * `test_wakeonly_touch.c` (both off-limits to this change per the PR
 * brief) read `ctx.idle`/`ctx.rebuild_count` directly, so those fields
 * keep their exact names and types. Rather than wrap them in a new
 * struct (which would break those tests), this file extracts the PUMP
 * LOGIC ITSELF — the sequence `ctl_loop.c`'s `ff_ctl_loop_pump` already
 * had — into a free function taking that state by pointer/value.
 * `ctl_loop.c` now calls this function instead of inlining the same
 * five lines a second time; `main.c` and `ff_demo_run.c` each own a
 * private `ff_sim_lifecycle_t` (this file's own bundle of the same
 * fields) and call the exact same function.
 *
 * ## Interpretation calls (see the PR body for the full writeup)
 *
 * - **DIM renders identically to ACTIVE.** On the real device, DIM only
 *   changes the backlight PWM duty — the framebuffer content is
 *   untouched. The sim has no backlight to dim, and matching that
 *   truth (rather than inventing a dim overlay nothing else in this
 *   codebase has) is what "render DIM/OFF as the sim already does for
 *   ctl" means in practice: `ctl_loop.c` never rendered a dim overlay
 *   either, because there was never anything to enact — see
 *   `ff_idle_brightness_pct`'s own doc comment (core/include/ff_idle.h)
 *   for the value a real backlight driver would use.
 * - **OFF/SLEEP render as a solid black overlay in WINDOW mode only**
 *   (drawn on `lv_layer_top()`, independent of the active screen so the
 *   rebuild-skip gate below is untouched by it). This is new behaviour
 *   `ctl_loop.c` does not need (nobody watches its offscreen buffer with
 *   human eyes while OFF; its own AC3 test only asserts
 *   `rebuild_count`, never pixel content) but window mode's whole point
 *   is a human watching the glass — a screen that silently keeps
 *   showing its last frame forever while "OFF" would be actively
 *   misleading (CLAUDE.md: honest data over pretty data extends to a
 *   dev tool's own display). Any mouse click wakes it (the gated
 *   pointer read callback below fires `ff_idle_input` on press-begin
 *   regardless of idle state, exactly like a real touch) — see
 *   `ff_sim_lifecycle_apply_blank_overlay`.
 * - **Sim "power off" = the window stays open, black, and any click
 *   wakes it** — not a process exit. Closing the SDL window (or
 *   Ctrl+C) is still how the process actually quits, same as every
 *   other window-mode path in this file; nothing here adds a new exit
 *   path.
 */
#ifndef FF_SIM_LIFECYCLE_H
#define FF_SIM_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "ff_app_state.h"
#include "ff_idle.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-caller lifecycle bundle: one instance per live window-mode
 *  session (`main.c`'s live `ff_run_window`, `ff_demo_run.c`'s
 *  `ff_run_demo_window`). `ctl_loop.c` keeps its own pre-existing,
 *  identically-shaped fields on `ff_ctl_loop_ctx_t` instead of this
 *  struct — see this header's top comment for why — and forwards them
 *  into `ff_sim_lifecycle_pump` the same as everyone else. Zero-init or
 *  call `ff_sim_lifecycle_init()` before first use. */
typedef struct {
    ff_idle_t            idle;
    ff_idle_touch_gate_t touch_gate;
    bool                 rebuild_pending;
    uint32_t             rebuild_count;
} ff_sim_lifecycle_t;

/** Zeroes `*lc` and initialises `idle`/`touch_gate` (NULL-safe no-op). */
void ff_sim_lifecycle_init(ff_sim_lifecycle_t *lc);

/**
 * ff_sim_lifecycle_pump — the ONE per-tick idle-FSM-plus-rebuild-gate
 * pump, extracted out of `ctl_loop.c`'s pre-existing `ff_ctl_loop_pump`
 * (S16 slice d / S26 slice c) so it, `main.c`'s live window loop, and
 * `ff_demo_run.c`'s live window loop all run the exact same logic.
 * Mirrors `app_main.c`'s per-frame order:
 *
 *   1. `shell_wake` (this frame's `ff_shell_take_wake` return — S26(d):
 *      a pushed banner wakes a dim/off screen) -> `ff_idle_input`.
 *   2. `ff_idle_tick(idle, now_ms, keep_awake, sleep_inhibit)`.
 *   3. `dirty` (this frame's `ff_shell_tick` return) -> latch
 *      `*rebuild_pending = true` — never cleared except by an actual
 *      rebuild below, so a dirty tick that lands mid-deferral is never
 *      lost, only delayed (app_main.c's "accumulate dirty, drain on the
 *      first eligible tick" contract).
 *   4. Rebuild (`lv_obj_clean(lv_screen_active())` +
 *      `ff_build_face_screen(state)`, counting `*rebuild_count`) IFF
 *      `*rebuild_pending && !finger_down && !screen_blank`, where
 *      `screen_blank` is OFF or SLEEP. This is the exact parity fix
 *      this PR also makes to `ctl_loop.c`'s own gate, which was
 *      missing the `finger_down` term `app_main.c`'s
 *      `!ff_display_touch_is_down()` has had since the rebuild-mid-tap
 *      fix (docs/specs/S26-device-lifecycle.md's amendment) — without
 *      it, a dirty tick landing between a press and a release tears
 *      down the button under the finger before its CLICKED can fire.
 *
 * `finger_down` must be the RAW pointer-down truth (not gated, not
 * debounced) — the same "physical level, not a UI decision" contract
 * `ff_idle_touch_gate` itself takes for its own `pressed` parameter.
 * `keep_awake` is the already-combined `ff_shell_keep_awake(view,
 * false)` predicate (every sim caller passes `false` for the
 * touch-calibration source: none of them have a blocking calibration
 * flow). `sleep_inhibit` is always `false` on every current sim caller
 * (no light sleep on host, nothing to inhibit) — taken as a real
 * parameter rather than hardcoded so the signature stays honest about
 * which `false` is a deliberate omission, matching `ff_idle_tick`'s own
 * shape.
 *
 * Returns the resulting `ff_idle_state_t` — window mode uses it to
 * show/hide the OFF/SLEEP black overlay (see
 * `ff_sim_lifecycle_apply_blank_overlay`); the ctl loop's own call site
 * only needs the earlier by-reference outputs, same as before.
 */
ff_idle_state_t ff_sim_lifecycle_pump(ff_idle_t *idle, bool *rebuild_pending, uint32_t *rebuild_count,
                                       uint32_t now_ms, bool dirty, bool shell_wake, bool finger_down,
                                       bool keep_awake, bool sleep_inhibit, ff_app_state_t const *state);

/**
 * ff_sim_lifecycle_apply_blank_overlay — window-mode-only OFF/SLEEP
 * visualisation (see this header's top comment's interpretation-call
 * note). Shows or hides a full-screen opaque black `lv_obj_t` on
 * `lv_layer_top()` (created lazily on first call, reused after) based on
 * `state`: shown for `FF_IDLE_STATE_OFF`/`FF_IDLE_STATE_SLEEP`, hidden
 * otherwise. Independent of `lv_screen_active()` — toggling it never
 * touches, and is never touched by, the rebuild-skip gate above. Not
 * called from `ctl_loop.c` (headless — see this header's top comment).
 */
void ff_sim_lifecycle_apply_blank_overlay(ff_idle_state_t state);

/**
 * ff_sim_lifecycle_pointer_ctx_t / ff_sim_lifecycle_pointer_read_cb —
 * the gated indev read callback every LIVE window-mode pointer indev
 * installs (`main.c`'s live `ff_run_window`, `ff_demo_run.c`'s
 * `ff_run_demo_window`), replacing a bare `lv_sdl_mouse_create()` (which
 * delivers every press straight to LVGL with no wake-only gating at
 * all — S26's amendment never applied to window mode before this PR).
 *
 * Reads the live SDL mouse state directly (`SDL_GetMouseState`) instead
 * of depending on `lv_sdl_mouse_create()`'s own internal (file-static,
 * unreachable from here) tracking: `lv_sdl_window_create` already
 * installs a 5ms timer that drains the whole SDL event queue
 * unconditionally (`sdl_event_handler`, `lv_sdl_window.c`) for its own
 * window/keyboard handling, regardless of which indevs exist — so
 * `SDL_GetMouseState` here always reads the same up-to-date position
 * `lv_sdl_mouse`'s own callback would, with no separate event pump
 * needed.
 *
 * Same amendment `ctl_loop.c`'s own `ctl_loop_pointer_read_cb`
 * implements for its synthetic pointer (S26 wake-only-touch,
 * docs/specs/S26-device-lifecycle.md "(c) Inactivity -> dim -> screen
 * off"): a press that BEGINS while idle is not ACTIVE wakes the screen
 * (`ff_idle_input`) but is swallowed for its ENTIRE gesture (LVGL is
 * told RELEASED throughout, with the real last point — no PRESSED
 * style, no CLICKED); a press that began ACTIVE is delivered normally.
 * `ctl_loop.c` keeps its own near-identical copy rather than switching
 * to this one — its point source is the ctl socket's tap/swipe/hold
 * commands, not SDL, so its read_cb shape genuinely differs even though
 * the GATING logic (this function's whole reason to exist) is
 * copied from it verbatim.
 */
typedef struct {
    ff_sim_lifecycle_t *lc;          /* not owned; must outlive the indev */
    uint32_t (*now_ms_cb)(void);     /* this session's own tick source (SDL_GetTicks / a frozen demo clock) */
} ff_sim_lifecycle_pointer_ctx_t;

/** `lv_indev_get_user_data(indev)` must be a non-NULL
 *  `ff_sim_lifecycle_pointer_ctx_t *` (set via `lv_indev_set_user_data`
 *  before this is ever read — same contract `ctl_loop.c`'s pointer
 *  indev already has). */
void ff_sim_lifecycle_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* FF_SIM_LIFECYCLE_H */
