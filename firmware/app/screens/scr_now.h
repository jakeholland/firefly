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
 * Renders exactly one of three honestly-distinct states, per
 * docs/specs/S07-now-face.md:
 *   - `!now->pack_loaded`: no festpack loaded at all — an honest empty
 *     state, distinct from the TBD banner below (see ff_app_state.h's
 *     `pack_loaded` doc comment for why conflating the two would be
 *     dishonest).
 *   - `now->tbd`: every set's start/end time is null (today's real Lost
 *     Lands state) — the day's lineup list plus a "SET TIMES TBD"
 *     banner, never an invented time.
 *   - otherwise: up to three now-playing rows (stage label, artist,
 *     progress bar in the stage's own color) and a starred-next card with
 *     an "IN N MIN" countdown, or an honest "nothing live right now" note
 *     if there's neither a live row nor an upcoming starred set.
 */
void ff_scr_now_build(lv_obj_t *parent, ff_app_now_t const *now);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_NOW_H */
