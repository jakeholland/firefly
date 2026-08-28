/**
 * scr_signals.h — app/screens: the Signals face (S22 rework).
 *
 * Pure rendering of the core Signals view-model
 * (`ff_sigview_t`, core/include/ff_sigview.h): a unified, scrolling list
 * of RECENT feed rows + a `· CREW ·` divider + quiet crew rows (each with
 * an honest presence label), an always-visible send-target line, and the
 * three kind-colored action buttons RALLY / PULSE / COMPOSE. No domain
 * logic here (CLAUDE.md: "UI code only renders core state and forwards
 * input"): the view-model — ordering, identity joins, presence categories,
 * and the persistent target — is all computed in core and owned by the
 * shell (app/ff_shell.c builds one `ff_sigview_t` per tick); this screen
 * only projects it and forwards intents (app/include/ff_intent.h):
 *
 *   - every selectable row emits `FF_INTENT_SIG_SELECT_MEMBER` (its crew
 *     node id) — the shell validates it against the roster and updates the
 *     view-model's target;
 *   - the target line's clear (✕) emits `FF_INTENT_SIG_CLEAR_TARGET`;
 *   - RALLY / PULSE / COMPOSE emit `FF_INTENT_SIG_RALLY` / `_PULSE` /
 *     `_COMPOSE`. This slice (b) only EMITS/ROUTES them; the real
 *     `ff_proto` send + rally-to-crew confirm are slice (d).
 *
 * ## Round-glass layout (the repeated lesson — PR #25/#86, S21)
 * Chrome is pinned ABOVE a bottom-anchored scroll list, the shape S21
 * settled on for Settings and the ONLY shape the `test_face_hit_targets.c`
 * sweep accepts for a list whose every row is a tap target: a clickable
 * control docked BELOW an overflowing clickable-row list collides, in the
 * sweep's scroll-invariant raw-rect adjacency pass, with the rows that
 * overflow past the viewport at scroll 0. So the send target line + the
 * three action buttons sit between the header and the list (not below it,
 * as the canvas mockup drew them), and the list is the bottom-most
 * clickable band — its overflow extends into the empty space by the bottom
 * pole, colliding with nothing. Every band's x-inset is derived from its
 * worst-case y via `ff_layout_safe_margin_x`, never flat offsets.
 */
#ifndef FF_SCR_SIGNALS_H
#define FF_SCR_SIGNALS_H

#include "ff_app_state.h" /* ff_app_state_t -> ff_sigview_t (the field type) */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_signals_build — render `*v` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck, same convention as `ff_scr_radar_build`).
 * `colorblind` selects the crew-color palette (member dots / target dot),
 * threaded through the same way `ff_scr_radar_build` takes it.
 *
 * A NULL `v` renders nothing. An otherwise-empty view (no recent rows and
 * no quiet crew — only the structural divider) renders an honest empty
 * state in the list band, never a fabricated sample row (CLAUDE.md).
 */
void ff_scr_signals_build(lv_obj_t *parent, ff_sigview_t const *v, bool colorblind);

/**
 * ff_scr_signals_unread_count — how many RECENT rows in `v` are unread.
 * A presentational tally over the already-computed rows (each row's
 * `unread` is core's decision, not this function's) — exposed so the nav
 * chrome's Signals page-dot badge (scr_nav.c) and this face's own header
 * badge count the same thing from the one source. 0 when `v` is NULL.
 */
uint16_t ff_scr_signals_unread_count(ff_sigview_t const *v);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_SIGNALS_H */
