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

#include "ff_flare.h" /* S10 slice b — flare_rt param, see ff_scr_radar_build's doc comment */
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
 * `flare_rt` (S10 slice b — [api] new parameter): the live `ff_flare_t`
 * the CLOSE-mode FLARE button's press forwards into
 * (`ff_flare_send_begin`), or NULL to leave that button visible but
 * inert (golden/headless rendering, which never fires a click at all —
 * see scr_flare.h's top comment for the same NULL-is-always-safe
 * contract every S10 button callback in this codebase follows). This
 * screen file makes no decision about WHAT pressing FLARE means beyond
 * that one forwarding call (CLAUDE.md: no domain logic in app/screens).
 */
void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar, ff_flare_t *flare_rt);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_RADAR_H */
