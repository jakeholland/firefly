/**
 * scr_launcher.h — app/screens: the S26 slice e BOOT-button launcher
 * (docs/specs/S26-device-lifecycle.md "(e) Home button + launcher"),
 * AMENDED 2026-09-01 (the maintainer's on-glass decision — see
 * `ff_route.h`'s header note for the full model change: the launcher
 * IS home now, not a modal reached from Radar).
 *
 * A grid of FIVE app circles — Radar / Now / Signals / Map / Settings,
 * in that fixed order. Radar is an ORDINARY circle as of this amendment
 * (no special handling, no privileged position, size, or color — the
 * original cut's "Radar is not listed here, it IS home" rule is
 * retired with the watchface concept it protected). Pure rendering
 * (CLAUDE.md: "UI code only renders core state and forwards input"):
 * each circle emits exactly one `FF_INTENT_LAUNCHER_SELECT` (payload:
 * `u.launcher_idx`, this file's own fixed circle order — 0=Radar,
 * 1=Now, 2=Signals, 3=Map, 4=Settings), and the shell decides whether
 * the tap lands (only while the launcher is actually showing with
 * nothing over it — `ff_route_launcher_select`'s own guard).
 *
 * Takes the full `ff_app_state_t const *`, unlike `scr_power_menu.c`'s
 * argument-free build: unlike that fully-static face, this one has ONE
 * dynamic fact to render — the Signals circle's unread badge
 * (`ff_scr_signals_unread_count(&state->signals)`, moved here off the
 * old page-dot row the carousel used to carry it on).
 *
 * ## Layout — a 2-over-3 grid, not a ring
 * A true rotationally-symmetric 5-point ring (a regular pentagon) and
 * the pre-amendment four-circle diagonal cross both turn out to be
 * geometrically infeasible at this circle's 96px floor once BOTH real
 * constraints are accounted for together: every circle's LVGL hit rect
 * is a full 96x96 SQUARE (not the visual circle inscribed in it — LVGL
 * dispatches touches against the object's rectangular bounds), so (a)
 * it must fit entirely inside the round glass, corner included, and (b)
 * no two circles' squares may come within `FF_HIT_MIN_GAP_PX` (8px) of
 * each other on both axes at once. A regular pentagon cannot keep every
 * vertex clear of the screen's horizontal midline at a radius small
 * enough to also stay on-glass (provably — see scr_launcher.c's layout
 * comment for the numbers); diagonal placements (any circle whose
 * center is off BOTH axes) pay a `sqrt(2)` penalty on their farthest
 * corner that a plain diamond-plus-fifth-circle layout cannot afford
 * either. Two horizontal rows (2 circles top, 3 bottom), each row kept
 * clear of the midline by a real margin and each circle no more than
 * one axis off-center, is the shape that verifiably clears both
 * constraints with margin to spare (see that same comment for the
 * numbers) — chosen for that reason, not for its shape.
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
 * Draws: five circles (>= 56 px targets, per spec — this file uses
 * 96 px, PR #142 review Design 2: "easier-to-tap targets", unchanged by
 * the 2026-09-01 amendment) arranged in a symmetric 2-over-3 grid, each
 * a real LVGL `LV_SYMBOL_*` kind glyph (compiled into the Montserrat
 * bitmap fonts this codebase already ships — the same symbols
 * `scr_banner.c`/`scr_signals.c` already render; PR #142 review
 * Design 1) plus a short caption label near it, `LV_STATE_PRESSED`
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
