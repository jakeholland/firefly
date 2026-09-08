/**
 * scr_music.h — app/screens: S31 Music/Swarm, the fifth launcher app
 * (docs/specs/S31-music-swarm.md). "Sixty fireflies drift on the round
 * glass. Each beat they flare and lean toward the centre, then wander
 * off again. Loudness sets how bright the swarm glows; silence lets it
 * fade back toward the ordinary idle look." — the concept sheet.
 *
 * ## S31 polish (owner feedback on PR #245, 2026-09-08)
 * Jake, on the field puck: "swarm works, clap flares it and quiet calms
 * it. Would love for it to be more dynamic like the design originally.
 * Not sure we need the text QUIET and LOUD or the MIC chip." Two
 * changes from the original PR: (1) the particle motion model now
 * replicates the coordinator's original concept-mockup feel — see
 * `firmware/core/ff_swarm.h`'s own "S31 polish" comment for the full
 * motion writeup; (2) the QUIET/LOUD word is gone entirely, and the
 * source chip is now shown ONLY when the source is NOT the mic (IMU or
 * NO SOURCE) — with the mic running (the expected, normal case), the
 * glass shows nothing but the clock and the swarm. This is a deliberate
 * narrowing of S31's original "always show a chip" honesty rule, not a
 * loosening of it: an UNUSUAL source is still always labelled (IMU
 * amber, NO SOURCE muted, per `music_source_color`); it is only the
 * NORMAL, expected case (the mic) that now goes unlabelled — the same
 * "only flag what's unusual" posture this codebase already applies
 * elsewhere (e.g. a face with a live peer draws no special banner; a
 * STALE or LOST one does). See docs/specs/S31-music-swarm.md's own
 * "Chrome" section for this call, recorded as the owner's.
 *
 * Pure rendering (CLAUDE.md: "UI code only renders core state and
 * forwards input"): no button, no `FF_INTENT_*` this face emits itself
 * — the only interaction is the BACK/HOME rim gestures every face gets
 * for free from `ff_gesture_glue.c` (S28), independent of this file
 * entirely.
 *
 * Unlike `scr_map.c`, whose draw-op pool exists to avoid CREATING
 * hundreds of `lv_obj_t` on every dirty-key REBUILD, this face's
 * "must never churn every frame" problem is different: the swarm's 60
 * particles move continuously (drift, beat pulls, twinkle) at up to
 * 30fps, far faster than any real render-key-driven rebuild should
 * ever happen (S16's own churn budget). So the pool here IS a plain,
 * PERSISTENT set of `lv_obj_t` circles — a core dot plus one halo ring
 * per firefly (120 objects total — see `scr_music.c`'s top comment,
 * "Object count is bounded by the LVGL heap", for why not 180/why not a
 * canvas) — created ONCE per face-BUILD (this
 * file owns the pointers as a file-static array) and only ever MUTATED
 * in-place — position/size/opacity, never create/delete — by this
 * file's own `lv_timer_create`d per-frame ticker, entirely OUTSIDE the
 * shell's dirty/rebuild path (see `scr_music.c`'s top comment for the
 * full mechanism and the frame-cost reasoning). See `firmware/core/
 * ff_swarm.h`'s own top comment for why the particle simulation itself
 * is core state this file owns privately rather than a member of
 * `ff_app_state_t`.
 */
#ifndef FF_SCR_MUSIC_H
#define FF_SCR_MUSIC_H

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_music_build — builds the Music/Swarm face on the current
 * default display's active screen (own puck/top-level screen, same
 * calling convention as `ff_scr_launcher_build`/`ff_scr_power_menu_
 * build`). Draws: the 60-firefly swarm (seeded from `state->music.seed`,
 * deterministic — see `ff_swarm_init`), a faint static ring, the wall
 * clock (`state->radar.clock_str`, the same honest "--:--" convention
 * every other face's clock uses, centred), and — ONLY when
 * `state->music.source != FF_APP_MUSIC_SRC_MIC` — a small muted source
 * chip at the bottom (IMU / NO SOURCE; S31's honesty rule: an unusual
 * source is always labelled; the normal one, the mic, is not — see this
 * header's own "S31 polish" comment).
 *
 * Also creates this face's own `lv_timer_t` (deleted automatically when
 * this screen is torn down, via an `LV_EVENT_DELETE` hook on the
 * screen — see scr_music.c) that steps the swarm and redraws the dot
 * pool every frame, at 30fps normally / 15fps once `state->radar.
 * batt_pct` is a known reading <= 20% (docs/specs/S31-music-swarm.md's
 * "Frame budget").
 *
 * NULL-safe (no-op, matching every other builder in this directory).
 */
void ff_scr_music_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_MUSIC_H */
