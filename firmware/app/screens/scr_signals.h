/**
 * scr_signals.h — app/screens: the Signals face (S24 inbox rework).
 *
 * Pure rendering of the S24 Signals view (`ff_app_signals_t`,
 * app/include/ff_app_state.h): the shell owns the sub-view state and
 * builds the core conversation model (`ff_inbox_t`, core/include/
 * ff_inbox.h) into the view each tick; this file only projects whichever
 * sub-view is named and forwards intents (app/include/ff_intent.h). No
 * domain logic here (CLAUDE.md: "UI code only renders core state and
 * forwards input") — ordering, membership, unread counts, identity joins
 * and presence categories are all core's (`ff_inbox_build`).
 *
 * Sub-views rendered by this slice (b):
 *   - INBOX  — the conversation list (spec screen 1): big full-bleed
 *     rows (CREW + one per paired member), numbered unread badges, honest
 *     presence for quiet members, and the solid-amber `+` FAB bleeding
 *     off the bottom-right rim. A row tap emits
 *     `FF_INTENT_INBOX_OPEN_THREAD` (u.node_id: 0 = CREW); the FAB emits
 *     `FF_INTENT_INBOX_NEW`.
 *   - PICKER — the recipient picker (the FAB's scope step): CREW pinned
 *     + each paired member in the same big-row style; a row emits
 *     `FF_INTENT_INBOX_PICK`; back emits `FF_INTENT_BACK`.
 *   - THREAD — a minimal, honest STUB (scope name + real signal count +
 *     back); the full message-list render is slice (c)'s. POPUP/RALLY
 *     are slice (d) placeholders — nothing routes to them yet and they
 *     fall back to the inbox render (see ff_app_state.h's
 *     ff_sig_subview_t).
 *
 * ## Round-glass layout notes (the repeated lessons — PR #25/#86, S21/S22)
 * Chrome (the non-clickable header / the pinned back button) sits above a
 * bottom-anchored scroll list; every band's x-inset derives from its own
 * worst-case y via `ff_layout_safe_margin_x`, never flat offsets. Two
 * S24-specific reconciliations between the design canvas and the
 * `test_face_hit_targets.c` sweep, both documented at the constants that
 * implement them:
 *   - rows are VISUALLY touching (the canvas's 68px full-bleed pitch) but
 *     each row's actual tap target is inset 4px top/bottom, so adjacent
 *     hit-rects keep the 8px adjacency floor;
 *   - the FAB's decorative amber disc bleeds off the rim (clipped by a
 *     round `clip_corner` container so nothing paints outside the glass),
 *     while its 48px TAP TARGET sits fully on-glass, and every row's
 *     hit-rect stops 8px short of the FAB's column so the two can never
 *     violate the adjacency floor at any scroll offset.
 */
#ifndef FF_SCR_SIGNALS_H
#define FF_SCR_SIGNALS_H

#include "ff_app_state.h" /* ff_app_signals_t — the S24 view this projects */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_signals_build — render `*v` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck, same convention as `ff_scr_radar_build`).
 * `colorblind` selects the crew-color palette (avatars / the CREW
 * cluster), threaded the same way `ff_scr_radar_build` takes it.
 *
 * A NULL `v` renders nothing. Edge states render honestly (S24 AC9):
 * no paired crew -> the CREW row plus a "no crew linked yet" hint; crew
 * with no traffic -> quiet rows with presence and a CREW "no signals
 * yet" preview; everyone stale -> the legible stale treatment (presence
 * text in the stale-amber tier, never the dimmest gray).
 */
void ff_scr_signals_build(lv_obj_t *parent, ff_app_signals_t const *v, bool colorblind);

/**
 * ff_scr_signals_unread_count — the sum of every conversation's unread
 * count in `v`'s inbox model. A presentational tally over the
 * already-computed model (each conversation's `unread` is core's
 * decision, not this function's) — exposed so the nav chrome's Signals
 * page-dot badge (scr_nav.c) and this face's own header badge count the
 * same thing from the one source. 0 when `v` is NULL. Saturates at
 * UINT16_MAX.
 */
uint16_t ff_scr_signals_unread_count(ff_app_signals_t const *v);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_SIGNALS_H */
