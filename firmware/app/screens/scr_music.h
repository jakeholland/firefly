/**
 * scr_music.h — app/screens: S31 Music/Swarm, the fifth launcher app
 * (docs/specs/S31-music-swarm.md). "Sixty fireflies drift on the round
 * glass. Each beat they flare and lean toward the centre, then wander
 * off again. Loudness sets how bright the swarm glows; silence lets it
 * fade back toward the ordinary idle look." — the concept sheet.
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
 * particles move continuously (drift, beat pulls, flare decay) at up
 * to 30fps, far faster than any real render-key-driven rebuild should
 * ever happen (S16's own churn budget). So the pool here IS 60 plain,
 * PERSISTENT `lv_obj_t` circles, created ONCE per face-BUILD (this file
 * owns the pointers as a file-static array) and only ever MUTATED
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
 * deterministic — see `ff_swarm_init`), the wall clock
 * (`state->radar.clock_str`, the same honest "--:--" convention every
 * other face's clock uses), the QUIET/LOUD word (bucketed at
 * `FF_BEAT_LOUD_THRESHOLD`, ff_beat.h), and the source chip (MIC / IMU /
 * NO SOURCE — S31's honesty rule: the source shown is always the source
 * actually used, never invented).
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
