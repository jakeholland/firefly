/**
 * scr_settings.h — app/screens: the Settings face (S11 slice b).
 *
 * Reached by long-press-anywhere (scr_nav.c's `nav_long_press_cb`), which
 * has emitted `FF_INTENT_OPEN_SETTINGS` since S16 slice c1 — the shell
 * rejected it until this renderer existed
 * (`ff_shell.c`'s `k_settings_renderer_exists`, flipped true by this
 * slice). Pure rendering: reads an `ff_app_settings_t` and draws every
 * user-facing preference (units, share mode, haptics, night glow,
 * water-nudge interval, quiet hours, UTC offset, your name) plus BACK.
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
 * ff_scr_settings_build — builds the Settings screen on the current
 * default display's active screen (own puck/top-level screen, same
 * calling convention as `ff_scr_compose_build` — Settings is a modal
 * reached by long-press, not a tileview swipe tile, so it isn't nested
 * inside scr_nav.c's shell).
 *
 * The paint is a pure function of `*settings` (matching every other
 * screen in this codebase): a tap reports the gesture through the intent
 * seam and waits for the next projection to repaint, exactly like every
 * other control since S16 slice c1/c3 — this file never mutates its own
 * argument or holds live editable state beyond the one build-time
 * snapshot every callback needs to compute "current -> next".
 */
void ff_scr_settings_build(ff_app_settings_t const *settings);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_SETTINGS_H */
