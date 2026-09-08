/**
 * scr_nav.h — app/screens: the base-face shell (S06 slice b; carousel
 * retired S26 slice e).
 *
 * Renders whichever ONE of the five base faces (Radar / Now / Signals /
 * Map / Settings) `state->active_face` names into a plain container on
 * the puck. Through S26 slice d these were reached by a horizontal
 * swipe carousel (a page-dot row, an `lv_tileview`, a gesture handler);
 * S26 slice e retired all of that — the BOOT-button launcher
 * (`scr_launcher.h`) is the one way in now, and this file is a pure
 * "build the named face" renderer with no navigation machinery of its
 * own (see scr_nav.c's header comment for the tileview -> plain
 * container swap this took).
 */
#ifndef FF_SCR_NAV_H
#define FF_SCR_NAV_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_button_create — the ONE way any screen file makes an LVGL button.
 *
 * fix/tap-lost-midpress-rebuild (2026-09-07) amendment — read this before
 * touching the PRESS_LOCK flag on this button; the history below is not
 * incidental, it is what the CURRENT mechanism has to preserve.
 *
 * ## The two conflicting requirements
 * (1) A press that starts on a control and SLIDES well away before
 *     lifting must never commit that control's tap (the #145/#148/banner
 *     history two paragraphs down) — a stumble, or a drag gesture that
 *     merely passes over FLARE or Power-off, must read as "I changed my
 *     mind", not a tap.
 * (2) A press that never meaningfully moves — a stationary finger, on a
 *     412px round glass whose touch controller (especially before this
 *     puck's own touch calibration ritual has run — S21's "ship
 *     uncalibrated by default" honest-data rule) reports a few px of raw
 *     coordinate noise around the true contact point — must still commit
 *     on release. Every ~33ms LVGL indev poll, LVGL re-hit-tests the
 *     CURRENT point from scratch UNLESS the pressed object carries
 *     `LV_OBJ_FLAG_PRESS_LOCK` (`indev_proc_press`, lv_indev.c): with the
 *     flag cleared — which used to be this function's ENTIRE
 *     implementation, see below — even a one-sample, sub-hit-rect
 *     excursion that snaps right back the very next poll permanently
 *     clears LVGL's internal `pointer.pressed` bit for the rest of that
 *     touch (it can only become true again on a fresh RELEASED->PRESSED
 *     transition), so `indev_proc_release` never delivers CLICKED even
 *     though the SAME object is still what the finger is visually
 *     resting on at release. This is the maintainer's on-glass bench
 *     report this fix is named for ("most taps take a few tries" on
 *     Inbox/Compose/Signals — logged with `move=0px` at the app's own,
 *     coarser gesture-engine threshold, i.e. exactly the class of
 *     movement too small to matter anywhere else in this codebase but
 *     large enough to trip LVGL's zero-tolerance re-search) — see
 *     docs/specs/S26-device-lifecycle.md's dated amendment for the full
 *     trace and diagnosis.
 *
 * ## The fix: PRESS_LOCK stays SET (LVGL's default); slide-off-cancels is
 * now enforced explicitly, with a real distance tolerance
 * `ff_scr_button_pressing_cb` (scr_nav.c) tracks the DOWN point and
 * calls `lv_indev_wait_release()` — the exact function
 * `app/ff_gesture_glue.c` already uses for BACK/HOME — the first time
 * total displacement exceeds `FF_SCR_BUTTON_SLIDE_CANCEL_PX` (scr_nav.c;
 * reuses `ff_gesture_cfg_t.long_slop_px`'s own precedent, 12px, for "this
 * is still basically a stationary press"). `lv_indev_wait_release` makes
 * every remaining `indev_proc_press` call for this touch a no-op and
 * fires PRESS_LOST (not CLICKED) on release — same mechanism, same
 * observable "never commits" result the old PRESS_LOCK-clearing achieved
 * for a REAL 150px slide (`test_scr_intent.c`'s `PL_*_drag_off_*` /
 * `S99_compose_drag_off_*` / `S26e_launcher_drag_across_satellites_
 * emits_nothing` family — all still pass, unmodified, because every one
 * of them drags well past 12px) — but with PRESS_LOCK left SET, LVGL's
 * own per-poll re-search never runs at all, so a few px of raw touch
 * noise that never reaches OUR threshold can no longer touch
 * `pointer.pressed` in the first place.
 *
 * ## Origin of the requirement PRESS_LOCK-clearing used to satisfy
 * This was found and fixed per-screen, twice, before being generalized
 * here: the launcher hub/satellites (#145, `scr_launcher.c`) and compose's
 * keypad/chips (#148, `scr_compose.c`'s retired `compose_clear_press_lock`)
 * both hand-rolled the identical two-line fix; the banner strip
 * (`scr_banner.c`) copied it a third time. Call this instead of
 * `lv_button_create` directly, everywhere in `app/screens/` — see
 * `test_scr_intent.c`'s drag-off tests (the `S99_compose_drag_off_*` /
 * `S26e_launcher_drag_across_satellites_emits_nothing` family, and the
 * per-face `PL_*_drag_off_*` ones) for the real-indev proof that a real
 * slide-off still emits nothing while a stationary tap — even a jittery
 * one — still does.
 */
lv_obj_t *ff_scr_button_create(lv_obj_t *parent);

/**
 * ff_scr_nav_build — builds the shell on the current default display's
 * active screen (same calling convention as fixture_view.h's
 * ff_fixture_view_build: a display must already be the LVGL default).
 *
 * Every one of the five base faces renders real content
 * (`ff_scr_radar_build`/`ff_scr_lineup_build`/`ff_scr_inbox_build`/
 * `ff_scr_map_build`/`ff_scr_settings_build`, each driven by its own
 * `ff_app_state_t` section). A face outside that set (COMPOSE/FLARE/
 * POWER_MENU/LAUNCHER/NONE — none of which the dispatcher routes here)
 * falls back to Radar rather than an undefined build.
 *
 * S10 slice b additions, both driven by `state->flare` (ff_app_state.h)
 * and both cross-face per spec (not gated on `state->active_face`):
 *  - the Radar content gets a "LOCKED" chip when `state->flare.locked`
 *    (scr_flare.h's ff_scr_flare_build_lock_chip);
 *  - the puck itself gets the pulsing-amber sender overlay when
 *    `state->flare.sending` (scr_flare.h's
 *    ff_scr_flare_build_sender_overlay), and the base face's content is
 *    dimmed to LV_OPA_30 first (PR #20 UX review finding #4: the
 *    overlay must OWN the headline slot while sending, structurally,
 *    for whichever face happens to be showing — not just a
 *    NOSEL-specific fix).
 * The full-screen RECEIVE takeover (`state->flare.takeover_active`) is
 * NOT built here — per spec it "interrupts any face", so the caller
 * (targets/sim/main.c) builds `ff_scr_flare_build_takeover` INSTEAD of
 * calling this function at all when a takeover is pending.
 *
 * S16 slice c2's `[api]` change dropped the `ff_flare_t *flare_rt`
 * parameter this function used to take and forward to
 * `ff_scr_radar_build`'s FLARE button and the sender overlay's CANCEL
 * button: both now emit semantic intents through the seam
 * (`ff_intent_emit`, app/include/ff_intent.h — `FF_INTENT_FLARE_START`
 * and `FF_INTENT_FLARE_END` respectively) instead of mutating a live
 * core struct handed down through three layers of build functions.
 * Unbound (golden/headless rendering, which never fires a click), every
 * emit is a safe no-op — same contract every intent emit site in this
 * codebase follows.
 */
void ff_scr_nav_build(ff_app_state_t const *state);

/**
 * ff_scr_nav_rect_best_remainder / ff_scr_nav_remainder_clears_floor —
 * [api] pure geometry, exposed purely for direct unit testing
 * (test_scr_banner.c), the same "expose the small pure mechanic so a
 * test can name it precisely" convention `ff_scr_launcher_satellite_deg`
 * already set (scr_launcher.h). Real callers never need these directly
 * — `ff_scr_nav_build`'s own internal banner-masking pass is the only
 * production call site — but a test that wants to prove the REMAINDER
 * rule itself (not just its end-to-end effect on one real control)
 * needs to reach it with hand-typed rects, no LVGL object tree required.
 * See scr_nav.c's own top-of-block comment (just above
 * nav_mask_clickables_under_banner) for the full rationale: an object
 * only loses LV_OBJ_FLAG_CLICKABLE while a banner covers it if the
 * largest rectangular piece STILL left uncovered fails
 * FF_THEME_MIN_HIT_PX in either dimension — the same floor
 * test_face_hit_targets.c's sweep already holds every control to,
 * applied to a remainder rect instead of an object's own full rect.
 */
lv_area_t ff_scr_nav_rect_best_remainder(lv_area_t obj, lv_area_t cover);
bool ff_scr_nav_remainder_clears_floor(lv_area_t obj, lv_area_t cover);

/**
 * ff_scr_nav_mask_clickables_under_banner — [api] walk `root`'s subtree
 * (skipping `banner`'s own subtree) and clear LV_OBJ_FLAG_CLICKABLE on
 * any OTHER clickable object whose rect overlaps `*banner_area` AND
 * whose remainder there fails ff_scr_nav_remainder_clears_floor (see
 * that function's doc comment and scr_nav.c's top-of-block comment for
 * the full rule). Shared between `ff_scr_nav_build` (every base face)
 * and `scr_launcher.c` (the launcher's own Inbox satellite) so both
 * apply the identical rule via one implementation rather than two
 * copies drifting apart.
 */
void ff_scr_nav_mask_clickables_under_banner(lv_obj_t *root, lv_obj_t *banner, lv_area_t const *banner_area);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NAV_H */
