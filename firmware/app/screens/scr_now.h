/**
 * scr_now.h — app/screens: the Now face (S07 slice b).
 *
 * Pure rendering: reads an `ff_app_now_t` and draws it. No domain logic
 * lives here (CLAUDE.md: "UI code only renders core state and forwards
 * input" — every `if` here is about *how to draw* a value the caller
 * already computed, never about *what the value should be*). Same
 * calling convention as scr_radar.h's `ff_scr_radar_build`.
 */
#ifndef FF_SCR_NOW_H
#define FF_SCR_NOW_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_now_build — renders `*now` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck, same as scr_radar.h's contract). Builds fresh
 * children every call; does not clear `parent` itself.
 *
 * Dispatches on `now->state` (`now_state_t`, ff_app_state.h) — six
 * mutually-exclusive-by-construction states, per docs/specs/S07-now-face.md
 * and PR #21's review rounds (plus issue #48's NOW_TIME_UNKNOWN addition):
 *   - `NOW_NO_PACK`: no festpack loaded at all — an honest empty state,
 *     distinct from every TBD-flavored state below (see now_state_t's doc
 *     comment for why conflating them would be dishonest).
 *   - `NOW_TIME_UNKNOWN` (issue #48): a festpack IS loaded, but the puck
 *     does not yet know what time it is (the normal cold-boot path,
 *     before a mesh timestamp latches). Distinct from `NOW_NO_PACK` on
 *     purpose — the missing fact is the CLOCK, not the pack, and the
 *     copy says so instead of telling the user to redo something they
 *     already did.
 *   - `NOW_TBD`: every set on the day lacks a known time (today's real
 *     Lost Lands state) — the full day lineup plus a "SET TIMES TBD"
 *     banner, never an invented time.
 *   - `NOW_MIXED`: SOME of the day's sets have a known time and some
 *     don't (the expected near-term state as Lost Lands' real pack times
 *     land stage-by-stage) — known-time sets render as usual (compact
 *     "known so far" lines) AND the still-unknown sets stay visible in a
 *     "still TBD" list, under the same banner as NOW_TBD. Fixes an
 *     earlier bug where an unknown-time set would silently vanish the
 *     moment ANY set on the day got a real time.
 *   - `NOW_LIVE`: every set on the day has a known time and something is
 *     currently playing or starred-upcoming — up to three now-playing
 *     rows (stage label, artist, progress bar in the stage's own color)
 *     and/or a starred-next card with an "IN N MIN" countdown.
 *   - `NOW_NOTHING_PLAYING`: every set's time is known, but nothing is
 *     live and nothing is starred-upcoming right now — an honest "nothing
 *     live right now" note, distinct from NOW_NO_PACK and every TBD
 *     state.
 */
void ff_scr_now_build(lv_obj_t *parent, ff_app_now_t const *now);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NOW_H */
