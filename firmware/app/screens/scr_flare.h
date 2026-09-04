/**
 * scr_flare.h — app/screens: the S10 slice b flare UI (receive takeover,
 * sender overlay, Radar-face lock chip).
 *
 * Pure rendering (CLAUDE.md: "UI code only renders core state and
 * forwards input") of the flattened `ff_app_flare_t` (ff_app_state.h) —
 * the same "state in, pixels out" projection every other face uses.
 *
 * As of S16 slice c2's `[api]` change, the GO/DISMISS/CANCEL button
 * callbacks no longer take a live `ff_flare_t *rt` to mutate directly —
 * this header no longer includes `ff_flare.h` at all. Pressing a button
 * now emits a semantic intent through the seam (`ff_intent_emit`,
 * app/include/ff_intent.h): GO -> `FF_INTENT_TAKEOVER_GO`, DISMISS ->
 * `FF_INTENT_TAKEOVER_DISMISS`, CANCEL -> `FF_INTENT_FLARE_END`. The
 * shell decides what each means (`ff_shell_intent`'s cases, which still
 * call the correctly-named split core function each — `ff_flare_go` /
 * `ff_flare_dismiss_takeover` / `ff_flare_send_cancel`, never a shared
 * "dismiss" that has to guess intent, per docs/specs/S10-flare.md's
 * Amendments, third entry). Unbound (golden/headless rendering, which
 * never fires a click), every emit here is a safe no-op — same contract
 * every intent emit site in this codebase follows.
 */
#ifndef FF_SCR_FLARE_H
#define FF_SCR_FLARE_H

#include "ff_app_state.h"
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
 * distance line, and two buttons:
 *   - GO: emits `FF_INTENT_TAKEOVER_GO` — an explicit user decision, so
 *     (per the MEDIUM ruling) the shell is allowed to replace any
 *     PREVIOUS lock with it, unlike a passively-arriving takeover.
 *   - DISMISS: emits `FF_INTENT_TAKEOVER_DISMISS` — clears only this
 *     pending takeover, leaves any existing lock untouched.
 * Both buttons are visually distinct filled shapes (not text-only — a
 * previous UX review flagged a grey text-only DISMISS as not looking
 * pressable) and >= FF_THEME_MIN_HIT_PX tall.
 * No-op (draws nothing) if `!flare->takeover_active` — same "renders
 * exactly the state it's handed, no invented fallback" contract as every
 * other screen builder in this codebase.
 */
void ff_scr_flare_build_takeover(ff_app_flare_t const *flare);

/**
 * ff_scr_flare_build_sender_overlay — draws on top of `parent` (expected
 * to be the shell's puck object, scr_nav.c — created LAST there so it
 * paints over the active tile, and lives on the puck rather than inside
 * one tile so it survives a face swipe, matching spec: "own screen
 * pulses amber" is not Radar-specific). Draws: a pulsing amber rim tint
 * around the puck edge, the status line "you are flaring — crew arrows
 * locked on you", a live countdown (flare_fmt.h), and a CANCEL button
 * (emits `FF_INTENT_FLARE_END`) >= FF_THEME_MIN_HIT_PX tall.
 * No-op if `!flare->sending`.
 *
 * `screen_flip` (fix/flare-rim-glass-geometry, `[api]`): the rim is an
 * edge-hugging ring, so — same as `ff_scr_radar_build`'s own
 * `screen_flip` parameter — it must stay concentric with the VISIBLE
 * glass (`FF_THEME_GLASS_CX/CY`, `docs/hardware/glass-offset.md`), which
 * mirrors under the case's flipped mount. Pass
 * `state->settings.screen_flip` straight through, exactly as every
 * `ff_scr_radar_build` call site already does.
 */
void ff_scr_flare_build_sender_overlay(lv_obj_t *parent, ff_app_flare_t const *flare, bool screen_flip);

/**
 * ff_scr_flare_build_lock_chip — draws a small "LOCKED · <NAME>" chip
 * onto `parent` (expected to be the Radar tile — spec: "when LOCKED to a
 * node, the Radar face shows a lock indicator"). Positioned at
 * `radar_layout.h`'s `RADAR_LAYOUT_LOCK_CHIP_DY` — DERIVED from (not a
 * literal alongside) `RADAR_LAYOUT_STATUS_BAR_DY`, specifically so the
 * two can never drift apart the way they did between PR #98 (which moved
 * the status bar for the 412 panel) and this fix (which found the chip
 * still at its pre-#98 literal, now overlapping the status bar instead
 * of sitting 30px clear of it — see radar_layout.h's comment on that
 * constant for the full history and the arithmetic).
 *
 * Unlike the arrow/dots, this chip is NOT registered in radar_layout's
 * collision registry — a judgment call (flagged per AGENTS.md, see this
 * PR's body, and radar_layout.h's own COLLISION ANALYSIS on the constant
 * above): the chip's own position is fixed regardless of mode, so
 * registering it would only ever protect the status bar (which its
 * derived DY already guarantees structurally) at the cost of a 9th
 * chrome rectangle. The one real consequence of staying unregistered is
 * that the compass arrow, for a narrow bearing cone near due north, may
 * pass BEHIND this chip rather than shortening to avoid it — accepted,
 * not overlooked (radar_layout.h's collision analysis works the
 * arithmetic); `scr_nav.c` already builds this chip AFTER
 * `ff_scr_radar_build`, so it painted over the arrow in z-order before
 * this fix too — only painting over the status bar was the bug.
 * No-op if `!flare->locked`.
 *
 * This function does not re-derive the lock fact itself — `flare->locked`
 * is expected to already be true only when the shell's own consult of
 * `ff_flare_locked_node()` (core/include/ff_flare.h) said so (see
 * ff_scr_flare_selection_locked below, which reads the same flattened
 * fact rather than the live core struct as of S16 slice c2). This is the
 * same "screens render already-decided state, never re-derive domain
 * facts" split every other builder in this file follows.
 */
void ff_scr_flare_build_lock_chip(lv_obj_t *parent, ff_app_flare_t const *flare);

/**
 * ff_scr_flare_selection_locked — true iff the navigation lock should
 * suppress crew-selection cycling right now.
 *
 * As of S16 slice c2's `[api]` change this reads the flattened
 * `flare->locked` projection (the shell computes it from
 * `ff_flare_locked_node(f) != 0` in `shell_project_flare`, ff_shell.c) —
 * NOT the live `ff_flare_t` this function used to take a pointer to
 * directly. Screens forward input and render already-decided state; they
 * do not consult core structs, and this was the one place in this file
 * that still did (flagged and closed in this slice's PR body). No crew-
 * selection CYCLING code exists anywhere in this repo yet (grepped: no
 * `ff_crew_select_next` or touch/swipe selection handler exists in
 * app/screens as of this PR — S06's shell has no interaction wiring for
 * it), so there is still no call site to wire this INTO; this accessor
 * exists so that whichever future S06/crew-selection code adds that
 * handler has exactly one correct place to ask.
 * Returns false for `flare == NULL` (no state to read — e.g. a builder
 * called before the first projection — never counts as locked).
 */
bool ff_scr_flare_selection_locked(ff_app_flare_t const *flare);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_FLARE_H */
