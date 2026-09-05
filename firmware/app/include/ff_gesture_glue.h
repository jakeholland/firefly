/**
 * ff_gesture_glue.h — S28 slice b: the app-level LVGL glue for
 * `firmware/core/ff_gesture.h`'s recognition FSM.
 *
 * Spec: docs/specs/S28-gestures.md. This is the ONE place that knows
 * about BOTH the gesture FSM (pure C11, no I/O) AND the LVGL indev/
 * shell/screen world it feeds from and dispatches into — mirroring
 * `ff_shell_home_press`/`ff_shell_multitap_edge`'s own layering (a
 * hardware-facing edge translated into the shell's `ff_intent_t` seam,
 * app/include/ff_shell.h) for a TOUCH-facing source instead of a GPIO
 * one.
 *
 * ## What attaching does
 * `ff_gesture_glue_attach(indev, sh)` is the WHOLE public surface. It:
 *   1. Registers an `LV_EVENT_ALL` callback on `indev` (via
 *      `lv_indev_add_event_cb`) that feeds `ff_gesture_feed` on
 *      `LV_EVENT_PRESSED` (the DOWN sample) and `LV_EVENT_RELEASED` (the
 *      UP sample) — see this file's own `.c` for why `LV_EVENT_PRESSING`
 *      is NOT one of the samples fed this way (verified against the
 *      vendored LVGL 9.5 source: `PRESSING` never reaches an indev-level
 *      listener, only an object-level one — a genuine gap between the
 *      spec's expectation and what this LVGL version actually delivers
 *      here, worked around below).
 *   2. Creates one `lv_timer` (period-driven, independent of any single
 *      indev read) that does two things every tick: while a touch is
 *      active, it POLLS the indev's own current point/state
 *      (`lv_indev_get_point`/`lv_indev_get_state`) and feeds that as the
 *      MOVE sample `ff_gesture_feed` needs mid-drag (this is what stands
 *      in for `PRESSING`) — and it always calls `ff_gesture_tick` for
 *      G3's time-driven long-press recognition. This is "the existing
 *      shell/face tick" the spec calls for, achieved by riding on
 *      LVGL's own `lv_timer_handler()` (which every target already calls
 *      once per frame) rather than asking four separate per-frame loops
 *      (sim window, sim demo window, the sim ctl loop, and app_main.c's
 *      device loop) to each add a new call site.
 *   3. On BACK/HOME: calls `lv_indev_wait_release(indev)` (so the widget
 *      under the finger gets PRESS_LOST — no click, no scroll continues
 *      — its own doc comment has the mechanism), plays the UI tick sound
 *      (`ff_sound_emit(FF_SOUND_TAP)`, gated downstream by `ui_ticks`
 *      exactly like every screen button), and dispatches
 *      `FF_INTENT_BACK`/`FF_INTENT_HOME` via `ff_shell_intent` directly
 *      — the same "this file already holds `ff_shell_t*`, so skip the
 *      process-global `ff_intent_emit` indirection screens need instead"
 *      shape `ff_shell_home_press`/`ff_shell_multitap_edge` already use
 *      for the BOOT-button and 5-tap paths.
 *   4. On LONG_PRESS: dispatches `FF_INTENT_QUICK_FLARE` the same way —
 *      no `lv_indev_wait_release` (a long press has no click waiting to
 *      be swallowed; the takeover that comes up next owns the glass).
 *
 * ## Face + interactive-widget gating (both computed fresh at DOWN)
 * `ff_gesture_set_long_press` is armed for a touch iff, at the moment it
 * began: the active face is `FF_APP_FACE_RADAR`, AND the press did not
 * land on an interactive widget — `lv_indev_get_active_obj()` (the
 * pressed object) or any ancestor up to the screen root carries
 * `LV_OBJ_FLAG_USER_1` (set on every button by `ff_scr_button_create`,
 * `app/screens/scr_nav.c`). Recomputed fresh on every DOWN — a face
 * change or a widget's own flag never has to be "un-armed" mid-touch,
 * there simply isn't a stale value to correct.
 *
 * ## Takeover gating
 * "Gestures are ignored entirely while a takeover is active (FLARE,
 * POWER_MENU)" (the spec's own words) is enforced at the moment of
 * DISPATCH, not by refusing to feed samples: a recognised BACK/HOME/
 * LONG_PRESS is simply dropped, silently, if `ff_shell_view(sh)` reports
 * `flare.takeover_active` or `active_face == FF_APP_FACE_POWER_MENU` at
 * that instant. This is deliberately a LOOSER gate than "never track a
 * touch that started under a takeover" would be — the FSM still tracks
 * such a touch internally — but it can never have a user-visible effect
 * (the FSM's own recognition is a one-shot per touch, and a dropped one
 * is simply never seen again for that touch), and it additionally covers
 * a takeover that appears MID-drag, which a DOWN-time-only check could
 * not. `FF_INTENT_QUICK_FLARE` itself is NOT gated on a takeover inside
 * `ff_shell.c` (mirrors the 5-tap panic path's own "safety beats a menu
 * being open" rule — see `ff_shell_multitap_edge`'s doc comment) —
 * this glue-level gate is what gives G3 the spec's STRICTER rule instead
 * for the on-glass gesture specifically.
 *
 * ## Wake-only touch gate — free, not re-implemented
 * "feed the FSM only from touches the shell would deliver as input"
 * needs no code here at all: every current indev's read callback
 * (`ff_sim_lifecycle_pointer_read_cb`, `ctl_loop_pointer_read_cb`,
 * device's `ff_touch_gate_read_cb`) already reports
 * `LV_INDEV_STATE_RELEASED` to LVGL for the entire duration of a
 * wake-only-swallowed gesture — LVGL therefore never raises
 * `LV_EVENT_PRESSED` for one, so this glue's callback is never even
 * invoked for a touch the shell wouldn't have delivered anyway.
 *
 * ## Geometry — flip-aware, refreshed every touch
 * `cx`/`cy` are recomputed at every DOWN from `ff_theme_glass_cx/cy
 * (view->settings.screen_flip)` (app/theme/ff_theme.h) — NOT the bare
 * `FF_THEME_GLASS_CX/CY` — because the touch coordinates this glue reads
 * (`lv_indev_get_point`) are in the SAME post-touchcal, post-flip
 * "logical"/framebuffer space the screens themselves hit-test in (the
 * device's `ff_touchcal_process_cb` applies the screen-flip 180-degree
 * correction to every raw touch sample before LVGL ever sees it — see
 * that callback's own doc comment, `ff_display.c`) — so the physical
 * rim's LOGICAL position genuinely moves with the flip setting, exactly
 * what the flip-aware helper computes. `r` (`FF_THEME_GLASS_R`) does not
 * change with flip (a mirror doesn't change a radius) and is set once at
 * attach.
 *
 * ## Lifetime
 * `ff_gesture_glue_attach` allocates a small private context on the
 * heap, owned by the indev: it is freed (and the private `lv_timer`
 * deleted) when the indev itself is deleted (`LV_EVENT_DELETE`, which
 * `lv_indev_delete` always sends through the same event list this
 * attaches to) — no separate detach call, matching `lv_indev_add_event_
 * cb`'s own "lives as long as the indev" contract. Attach at most ONCE
 * per indev (the puck has exactly one touch source at a time; a test
 * that wants a second attempt should create a fresh indev, same as any
 * other synthetic-indev test in this repo already does).
 *
 * `sh` must outlive the indev. `indev`/`sh` NULL: `ff_gesture_glue_
 * attach` is a safe no-op.
 */
#ifndef FF_GESTURE_GLUE_H
#define FF_GESTURE_GLUE_H

#include "ff_shell.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_gesture_glue_attach — wire on-glass gesture recognition (S28 G1
 * BACK / G2 HOME / G3 LONG_PRESS-flare) onto `indev`, dispatching
 * through `sh`. See this header's top comment for the full contract.
 * Call once per (indev, sh) pair, after `sh` is a fully-initialized
 * `ff_shell_t*` (`ff_shell_init` already called) — this function never
 * ticks or initializes the shell itself, only reads it.
 */
void ff_gesture_glue_attach(lv_indev_t *indev, ff_shell_t *sh);

#ifdef __cplusplus
}
#endif

#endif /* FF_GESTURE_GLUE_H */
