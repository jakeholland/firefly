/**
 * scr_settings.h — app/screens: the Settings face (S11 slice b).
 *
 * Settings is one of the five base faces `ff_route`'s swipe axis still
 * ranges over (RADAR/NOW/SIGNALS/MAP/SETTINGS — unchanged since the
 * horizontal-carousel rework), but S26 slice e retired the swipe
 * GESTURE as a live way to reach any of them: it is reached from the
 * BOOT-button launcher's Settings circle now (`scr_launcher.h`), and
 * left the same way every other launcher-reached face is — BOOT again,
 * straight back to Radar (`ff_route_home`). Pure rendering: reads an
 * `ff_app_settings_t` and draws every
 * user-facing preference (units, share mode, haptics, night glow,
 * water-nudge interval, quiet hours, UTC offset, your name).
 * No domain logic here (CLAUDE.md) — every control is a bare
 * `FF_INTENT_SETTING_SET` emitter; range validation and persistence are
 * the shell's (`ff_shell.c`'s `shell_setting_set`), same "screens stay
 * pure renderers" split every other face in this codebase already uses.
 *
 * `my_name` is DISPLAY-ONLY in this slice — see scr_settings.c's header
 * comment for why editing it was not force-fit onto the Compose T9 path.
 */
#ifndef FF_SCR_SETTINGS_H
#define FF_SCR_SETTINGS_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_settings_build — builds the Settings face into `parent` [api].
 *
 * HORIZONTAL-CAROUSEL REWORK: Settings is a SWIPE TILE now, so it builds
 * into the caller's container (`scr_nav.c` passes the Settings tile) —
 * the same tile-parented convention every other swipe face uses — rather
 * than onto the active screen. `parent == NULL` or `settings == NULL`
 * draws nothing.
 *
 * The paint is a pure function of `*settings` (matching every other
 * screen in this codebase): a tap reports the gesture through the intent
 * seam and waits for the next projection to repaint, exactly like every
 * other control since S16 slice c1/c3 — this file never mutates its own
 * argument or holds live editable state beyond the one build-time
 * snapshot every callback needs to compute "current -> next".
 */
void ff_scr_settings_build(lv_obj_t *parent, ff_app_settings_t const *settings);

/**
 * ff_scr_settings_reset_scroll — [api] discard the remembered scroll
 * offset so the NEXT ff_scr_settings_build() renders from the top.
 *
 * The Settings list preserves its scroll position across the in-place
 * rebuild a settings-change intent triggers (toggling a row must not jump
 * the list back to the top). That preservation is WRONG on a FRESH entry
 * into Settings from another face — arriving from Radar must always land
 * at the top, not wherever a previous visit left the list. The face
 * dispatcher (targets/sim/face_dispatch.c, targets/esp32s3/main/ff_face.c)
 * calls this on the not-Settings -> Settings transition, so a fresh entry
 * resets while a same-face rebuild preserves. A no-op cost when called
 * redundantly (it only clears a file-static int).
 */
void ff_scr_settings_reset_scroll(void);

/**
 * ff_scr_settings_apply_scroll_hint — [api] sim golden-harness hook: scroll
 * the live Settings list to `y` (device points, clamped by LVGL to the
 * scrollable range) so a golden can capture a NON-zero scroll offset. A
 * no-op when `y <= 0` or no Settings list is currently built — so the live
 * shell path (which always passes 0) is completely unaffected and only a
 * fixture that explicitly asks for a scrolled render moves the list. See
 * ff_app_state_t.ui_settings_scroll_y (a sim-only render hint, parsed by
 * targets/sim/fixture.c) and the scrolled goldens in
 * firmware/tests/run_goldens.sh.
 */
void ff_scr_settings_apply_scroll_hint(int32_t y);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_SETTINGS_H */
