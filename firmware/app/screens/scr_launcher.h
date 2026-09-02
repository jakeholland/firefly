/**
 * scr_launcher.h — app/screens: the S26 slice e BOOT-button launcher
 * (docs/specs/S26-device-lifecycle.md "(e) Home button + launcher"),
 * AMENDED 2026-09-01 (the maintainer's on-glass decision — see
 * `ff_route.h`'s header note for the full model change: the launcher
 * IS home now, not a modal reached from Radar).
 *
 * FIVE app circles — Radar / Now / Signals / Map / Settings — pure
 * rendering (CLAUDE.md: "UI code only renders core state and forwards
 * input"): each circle emits exactly one `FF_INTENT_LAUNCHER_SELECT`
 * (payload: `u.launcher_idx`, this file's own fixed SEMANTIC order —
 * 0=Radar, 1=Now, 2=Signals, 3=Map, 4=Settings — unrelated to where a
 * circle is drawn, see below), and the shell decides whether the tap
 * lands (only while the launcher is actually showing with nothing over
 * it — `ff_route_launcher_select`'s own guard).
 *
 * Takes the full `ff_app_state_t const *`, unlike `scr_power_menu.c`'s
 * argument-free build: unlike that fully-static face, this one has ONE
 * dynamic fact to render — the Signals/Inbox circle's unread badge
 * (`ff_scr_inbox_unread_count(&state->inbox)`) — plus one more read
 * off `state->radar`, the bottom status row's time/battery (see below).
 *
 * ## Layout, VISUAL REFRESH 2026-09-01 — a compass ring, not a grid
 * The five-circle GRID this header used to describe (2-over-3, all five
 * circles the same 96px size) is retired: Radar is now a 120px HUB disc
 * at the puck's own center, and the other four sit as 88px SATELLITE
 * discs on a 128px orbit around it, N-agnostic (position computed from
 * the count of real, routable satellite apps — today 4, the cardinal
 * points) so a real fifth app later is a small, formula-driven addition
 * rather than a redesign. Every number, the full press-state contract,
 * and the icon-drawing pipeline (LVGL primitives, not image assets — no
 * SVG rasterizer was available when this was built) are documented in
 * `scr_launcher.c`'s own top comment, the single source of truth for
 * this face's geometry (the same "detail lives in the .c, this header
 * summarizes" split the retired grid's own comment used).
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
 * convention as `ff_scr_power_menu_build`/`ff_scr_compose_build`).
 * Draws: the Radar hub (120px, >= the spec's 56px floor with a lot to
 * spare) plus four satellite discs (88px each) on a compass-ring orbit,
 * every disc's icon drawn with LVGL primitives (not an `LV_SYMBOL_*`
 * glyph any more — see scr_launcher.c's top comment), `LV_STATE_PRESSED`
 * press feedback (a disc fills amber and its icon/caption invert to
 * `FF_THEME_COLOR_BG`), the Inbox circle's unread-count badge
 * (`state->inbox`) when nonzero, and a bottom time/battery status row
 * (`state->radar`).
 *
 * NULL-safe (no-op, matching every other builder in this directory).
 */
void ff_scr_launcher_build(ff_app_state_t const *state);

/**
 * ff_scr_launcher_satellite_deg — [api] the N-agnostic satellite ANGLE
 * formula, exposed for direct host-side unit testing (no LVGL, no
 * `ff_app_state_t`): `compass_pos * (360 / n)`, in degrees, 0 = top of
 * the puck, increasing clockwise — the same convention
 * `scr_launcher.c`'s `launcher_deg_to_offset` turns into an actual
 * (dx, dy) offset. `compass_pos` is which of the `n` evenly-spaced
 * compass slots a satellite occupies (0-indexed); `n <= 0` returns 0.0
 * rather than dividing by zero. See `scr_launcher.c`'s top comment,
 * "N-agnostic satellite layout", for why this is a pure function rather
 * than a hand-typed angle table.
 */
float ff_scr_launcher_satellite_deg(int compass_pos, int n);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_LAUNCHER_H */
