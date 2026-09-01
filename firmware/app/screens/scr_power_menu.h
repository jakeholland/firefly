/**
 * scr_power_menu.h — app/screens: the S26 slice b PWR-button power menu
 * (docs/specs/S26-device-lifecycle.md "(b) Power button -> power menu ->
 * soft power-off").
 *
 * Pure rendering (CLAUDE.md: "UI code only renders core state and
 * forwards input") of a screen with NO dynamic content at all — three
 * fixed labels (Power off / Reboot / Cancel), following the same
 * round-glass filled/outlined pill button shape `scr_flare.c`'s GO/
 * DISMISS establish. That is why `ff_scr_power_menu_build` takes no
 * argument, unlike every other builder in this directory: there is no
 * `ff_app_power_menu_t` to render from — `ff_app_state_t.active_face ==
 * FF_APP_FACE_POWER_MENU` (checked by the caller, `face_dispatch.c` /
 * `ff_face.c`, before this is ever called) is the entire fact this face
 * needs.
 *
 * Each button emits exactly one intent (`ff_intent_emit`, no screen-side
 * branching): Power off -> FF_INTENT_POWER_OFF, Reboot ->
 * FF_INTENT_POWER_REBOOT, Cancel -> FF_INTENT_POWER_CANCEL. The shell
 * decides what each means (`ff_shell_intent`'s cases) — this file never
 * touches GPIO7/GPIO0, `ff_power_fsm_t`, or `esp_restart`.
 */
#ifndef FF_SCR_POWER_MENU_H
#define FF_SCR_POWER_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_power_menu_build — builds the power menu on the current default
 * display's active screen (own puck/top-level screen, same calling
 * convention as `ff_scr_flare_build_takeover`/`ff_scr_compose_build` —
 * the power menu is a full-screen modal, not nested inside `scr_nav.c`'s
 * tileview). Draws: a "POWER" headline and three big round-glass buttons
 * (>= 56 px tall, `LV_STATE_PRESSED` press feedback, `ff_layout`-style
 * round-glass margins) — Power off (solid, the destructive action),
 * Reboot (outlined), Cancel (outlined, muted).
 *
 * No fixture/state parameter (see this header's top comment): always
 * draws the same content. Deterministic and golden-safe like every other
 * screen builder in this codebase.
 */
void ff_scr_power_menu_build(void);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_POWER_MENU_H */
