/**
 * scr_flare.h — app/screens: the S10 slice b flare UI (receive takeover,
 * sender overlay, Radar-face lock chip).
 *
 * Pure rendering (CLAUDE.md: "UI code only renders core state and
 * forwards input") of the flattened `ff_app_flare_t` (ff_app_state.h) —
 * the same "state in, pixels out" projection every other face uses. The
 * one exception, exactly like scr_radar.c's close-range FLARE button, is
 * the GO/DISMISS/CANCEL button callbacks: pressing them forwards straight
 * into the named core entry point (`ff_flare_go` / `ff_flare_dismiss_takeover`
 * / `ff_flare_send_cancel`) with zero branching of its own — the state
 * machine, not this file, owns what each button means (docs/specs/
 * S10-flare.md's Amendments, third entry: GO and DISMISS/CANCEL must call
 * the correctly-named split function each, never a shared "dismiss"
 * that has to guess intent).
 *
 * `ff_flare_t *rt` (runtime) is the live flare engine these callbacks
 * mutate. It is deliberately SEPARATE from the `ff_app_flare_t const
 * *flare` display snapshot every build function also takes: `flare` is
 * what gets drawn (works from a standalone fixture with no live engine
 * at all — golden-screenshot rendering, `rt == NULL`); `rt` is what a
 * button PRESS acts on (only meaningful in interactive/window mode,
 * where targets/sim/main.c owns one real `ff_flare_t` for the process).
 * Passing NULL for `rt` is always safe — every callback below no-ops on
 * NULL rather than crashing, so headless single-frame golden rendering
 * (which never fires a click at all, but still builds the same button
 * objects) needs no special-casing.
 */
#ifndef FF_SCR_FLARE_H
#define FF_SCR_FLARE_H

#include "ff_app_state.h"
#include "ff_flare.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_flare_build_takeover — builds the full-screen receive takeover
 * on the CURRENT default display's active screen (same calling
 * convention as ff_scr_nav_build/ff_fixture_view_build: replaces
 * whatever face would otherwise be shown, per spec "full-screen takeover
 * regardless of current face"). Draws: a pulsing amber burst mark, the
 * "<NAME> IS FLARING" headline (flare_fmt.h), a compass-bearing +
 * distance line, the explanatory line ("they lit their puck so you can
 * spot them — arrow's locked on"), and two buttons:
 *   - GO: switches the lock to this sender (`ff_flare_go(rt)`) — an
 *     explicit user decision, so (per the MEDIUM ruling) it's allowed to
 *     replace any PREVIOUS lock, unlike a passively-arriving takeover.
 *   - DISMISS: clears only this pending takeover (`ff_flare_dismiss_takeover(rt)`)
 *     — leaves any existing lock, and any OTHER field on `*rt`, untouched.
 * Both buttons are visually distinct filled shapes (not text-only — a
 * previous UX review flagged a grey text-only DISMISS as not looking
 * pressable) and >= FF_THEME_MIN_HIT_PX tall.
 * No-op (draws nothing) if `!flare->takeover_active` — same "renders
 * exactly the state it's handed, no invented fallback" contract as every
 * other screen builder in this codebase.
 */
void ff_scr_flare_build_takeover(ff_app_flare_t const *flare, ff_flare_t *rt);

/**
 * ff_scr_flare_build_sender_overlay — draws on top of `parent` (expected
 * to be the shell's puck object, scr_nav.c — created LAST there so it
 * paints over the active tile, and lives on the puck rather than inside
 * one tile so it survives a face swipe, matching spec: "own screen
 * pulses amber" is not Radar-specific). Draws: a pulsing amber rim tint
 * around the puck edge, the status line "you are flaring — crew arrows
 * locked on you", a live countdown (flare_fmt.h), and a CANCEL button
 * (`ff_flare_send_cancel(rt)`) >= FF_THEME_MIN_HIT_PX tall.
 * No-op if `!flare->sending`.
 */
void ff_scr_flare_build_sender_overlay(lv_obj_t *parent, ff_app_flare_t const *flare, ff_flare_t *rt);

/**
 * ff_scr_flare_build_lock_chip — draws a small "LOCKED · <NAME>" chip
 * onto `parent` (expected to be the Radar tile — spec: "when LOCKED to a
 * node, the Radar face shows a lock indicator"). Positioned clear of
 * every mode's status-bar reservation (RADAR_LAYOUT_STATUS_BAR_DY) but,
 * unlike the arrow/dots, is NOT registered in radar_layout's collision
 * registry — a judgment call (flagged per AGENTS.md, see this PR's body):
 * the chip's own position is fixed regardless of mode, so it never
 * competes for space with the mode-specific content the registry exists
 * to protect (arrow head / ring dots), and adding a 9th chrome type to
 * that registry for a chip that never moves would be complexity spent
 * where there's no actual collision risk to resolve.
 * No-op if `!flare->locked`.
 *
 * This function does not call `ff_flare_locked_node()` itself —
 * `flare->locked` is expected to already be true only when that accessor
 * (core/include/ff_flare.h) says so (see ff_scr_flare_selection_locked
 * below for the one call site that DOES consult it directly). This is
 * the same "screens render already-decided state, never re-derive
 * domain facts" split every other builder in this file follows.
 */
void ff_scr_flare_build_lock_chip(lv_obj_t *parent, ff_app_flare_t const *flare);

/**
 * ff_scr_flare_selection_locked — true iff `rt`'s navigation lock should
 * suppress crew-selection cycling right now. A one-line wrapper around
 * `ff_flare_locked_node(rt) != 0` (core/include/ff_flare.h) — spec S10
 * AC3 / this PR's task brief: "consult ff_flare_locked_node(); do NOT
 * re-implement the rule — the state machine owns it." No crew-selection
 * CYCLING code exists anywhere in this repo yet (grepped: no
 * `ff_crew_select_next` or touch/swipe selection handler exists in
 * app/screens as of this PR — S06's shell has no interaction wiring for
 * it), so there is no call site to wire this INTO yet either; this
 * accessor exists so that whichever future S06/crew-selection code adds
 * that handler has exactly one correct place to ask, rather than
 * re-deriving "is the lock active" from `ff_flare_t` fields itself.
 * Returns false for `rt == NULL` (no live engine — e.g. golden/headless
 * rendering — never counts as locked).
 */
bool ff_scr_flare_selection_locked(ff_flare_t const *rt);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_FLARE_H */
