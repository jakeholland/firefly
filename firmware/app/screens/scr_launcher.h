/**
 * scr_launcher.h — app/screens: the S26 slice e BOOT-button launcher
 * (docs/specs/S26-device-lifecycle.md "(e) Home button + launcher").
 *
 * A ring of four app circles — Now / Signals / Map / Settings, in that
 * fixed order (Radar is not listed here: it IS home, reached by BOOT
 * from any app, never a launcher circle — `ff_route.h`'s
 * `ff_route_launcher_select` doc comment). Pure rendering (CLAUDE.md:
 * "UI code only renders core state and forwards input"): each circle
 * emits exactly one `FF_INTENT_LAUNCHER_SELECT` (payload: `u.launcher_idx`,
 * this file's own fixed circle order — 0=Now, 1=Signals, 2=Map,
 * 3=Settings), and the shell decides whether the tap lands (only while
 * the launcher modal is actually open — `ff_route_launcher_select`'s own
 * guard).
 *
 * Takes the full `ff_app_state_t const *`, unlike `scr_power_menu.c`'s
 * argument-free build: unlike that fully-static face, this one has ONE
 * dynamic fact to render — the Signals circle's unread badge
 * (`ff_scr_signals_unread_count(&state->signals)`, moved here off the
 * old page-dot row the carousel used to carry it on).
 */
#ifndef FF_SCR_LAUNCHER_H
#define FF_SCR_LAUNCHER_H

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_launcher_build — builds the launcher on the current default
 * display's active screen (own puck/top-level screen, same calling
 * convention as `ff_scr_power_menu_build`/`ff_scr_compose_build` — the
 * launcher is a full-screen modal, not nested inside `scr_nav.c`).
 * Draws: four circles (>= 56 px targets, per spec — this file uses
 * 96 px, PR #142 review Design 2: "easier-to-tap targets"), each a
 * real LVGL `LV_SYMBOL_*` kind glyph (compiled into the Montserrat
 * bitmap fonts this codebase already ships — the same symbols
 * `scr_banner.c`/`scr_signals.c` already render; PR #142 review
 * Design 1) plus a short caption label below it, `LV_STATE_PRESSED`
 * press feedback, and the Signals circle's unread badge (scaled with
 * the circle) when `state->signals` has unread items.
 *
 * NULL-safe (no-op, matching every other builder in this directory).
 */
void ff_scr_launcher_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_LAUNCHER_H */
