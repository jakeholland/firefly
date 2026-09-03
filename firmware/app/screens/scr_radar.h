/**
 * scr_radar.h — app/screens: the Radar face (S06 slices c/d).
 *
 * Pure rendering: reads an `ff_radar_view_t` and draws it. No domain
 * logic lives here (CLAUDE.md: "UI code only renders core state and
 * forwards input" — every `if` here is about *how to draw* a value core
 * already computed, never about *what the value should be*).
 */
#ifndef FF_SCR_RADAR_H
#define FF_SCR_RADAR_H

#include <stdbool.h>

#include "ff_radar.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_radar_build — renders `*radar` into `parent` (expected to be
 * `FF_THEME_PUCK_PX` square — the shell, scr_nav.c, hands it a tileview
 * tile sized to the puck). Builds fresh children every call; does not
 * clear `parent` itself (caller's responsibility if re-rendering onto a
 * reused object — the sim harness only ever builds once per process, so
 * this has never needed to matter yet).
 *
 * Renders every `radar_mode_t` per docs/specs/S06-radar-face.md's
 * rendering section, plus the never-fixed special case
 * (`ff_radar.h`'s "RENDERER CONTRACT": `mode == RADAR_LOST &&
 * age_str[0] == '\0'` gets "NO FIX YET", never "LAST SEEN"). Also draws
 * the cross-mode chrome: status bar (clock/mesh/battery) and crew ring
 * dots (dashed when a dot's own freshness isn't LIVE).
 *
 * The CLOSE-mode FLARE button emits `FF_INTENT_FLARE_START` through the
 * intent seam (`ff_intent_emit`, app/include/ff_intent.h) — S16 slice c2's
 * `[api]` change dropped the `ff_flare_t *flare_rt` parameter this
 * function used to take (S10 slice b) precisely so this file could stop
 * touching core's flare engine directly. The shell decides what the press
 * means (`ff_shell_intent`'s `FF_INTENT_FLARE_START` case); this file
 * only reports the gesture. Unbound (golden/headless rendering, which
 * never fires a click), the emit is a safe no-op — same contract every
 * intent emit site in this codebase follows (see ff_intent.h's top
 * comment).
 *
 * `colorblind` (S17 slice a, [api]): selects the crew palette every dot/
 * wedge on this face is drawn with — `ff_settings_t.colorblind` projected
 * through as `ff_app_settings_t.colorblind` by the caller (scr_nav.c has
 * the full `ff_app_state_t` in scope; see ff_theme.h's own doc comment
 * for why this is an explicit parameter rather than a hidden global).
 *
 * `screen_flip` (format v8 amendment, [api]): selects which visible-glass
 * centre (`ff_theme_glass_cx`/`_cy`, ff_theme.h) the STALE rim tint
 * (`radar_build_rim_tint`, the one element on this face that hugs the
 * physical bezel) centres on — `ff_settings_t.screen_flip` projected
 * through as `ff_app_settings_t.screen_flip`, same explicit-parameter
 * convention as `colorblind` above. Every other element on this face
 * keeps the plain puck centre (unaffected by flip — see ff_theme.h): the
 * HARDWARE panel mirror (`ff_display_set_flip`, device-only) is what
 * actually re-orients the pixels on glass, so this parameter's only job
 * is keeping the one edge-hugging element concentric with the bezel in
 * either orientation.
 *
 * `locked` (fix/radar-lock-chip-clears-status-bar follow-up, [api]):
 * true iff the Radar-face lock chip (`scr_flare.c`'s
 * `ff_scr_flare_build_lock_chip`, called by `scr_nav.c` right after this
 * function whenever the active face is Radar) is about to be drawn over
 * this same content. `scr_nav.c` passes `state->flare.locked` directly —
 * the SAME fact that gates the chip itself, so the two can never
 * disagree about whether it's showing — same explicit-parameter
 * convention as `colorblind`/`screen_flip` above. When true, the
 * compass arrow's maximum reach is capped at
 * `RADAR_LAYOUT_ARROW_REACH_LOCKED_PX` (`radar_layout.h`) instead of the
 * normal `RADAR_LAYOUT_ARROW_LEN_PX`, so the arrow's head can never reach
 * the lock chip's band regardless of bearing — see that constant's own
 * derivation comment for why (a due-north locked fixture originally
 * shipped with the arrowhead painted squarely under the chip; the
 * arrow's direction is this product's whole point, so unlike painting
 * over the status bar, painting over the HEAD was ruled unacceptable
 * even for a narrow bearing cone — docs/specs/S10-flare.md's Amendments
 * has the full account). Ignored by CLOSE (no compass arrow — a
 * proximity ring instead) and NOFIX/NOSEL (no arrow drawn at all).
 */
void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar, bool colorblind, bool screen_flip,
                        bool locked);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_RADAR_H */
