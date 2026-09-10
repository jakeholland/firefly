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
 * ever happen (S16's own churn budget). See `firmware/core/ff_swarm.h`'s
 * own top comment for why the particle simulation itself is core state
 * this file owns privately rather than a member of `ff_app_state_t`.
 *
 * ## 2026-09-09 canvas renderer (supersedes the PR #247 dot-pool design)
 * The swarm used to be drawn as a PERSISTENT pool of `lv_obj_t` circles
 * — a core dot plus one halo ring per firefly, 120 objects, mutated
 * in-place every frame. Measured on the field puck (`perf` console,
 * Music face open): `lvgl_refresh avg_us=144705` — 145ms/frame, ~7fps —
 * LVGL's own per-object overhead does not stay flat as object count
 * grows the way that design assumed. The swarm is now ONE `lv_canvas`,
 * redrawn by direct pixel writes into its own raw RGB565 buffer every
 * frame — see `scr_music.c`'s own top comment, "Renderer", for the full
 * writeup (sprite pre-rendering, PSRAM/malloc buffer placement, the
 * additive-blend math) and docs/specs/S31-music-swarm.md's dated
 * amendment for the measured numbers. This file's own `lv_timer_create`d
 * per-frame ticker still runs entirely OUTSIDE the shell's dirty/rebuild
 * path exactly as before — only WHAT it redraws each tick changed.
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
 * screen — see scr_music.c) that steps the swarm and redraws the canvas
 * every frame, at 30fps normally / 15fps once `state->radar.
 * batt_pct` is a known reading <= 20% (docs/specs/S31-music-swarm.md's
 * "Frame budget").
 *
 * NULL-safe (no-op, matching every other builder in this directory).
 */
void ff_scr_music_build(ff_app_state_t const *state);

/**
 * ff_scr_music_debug_render_ticks — [test-only] fix/s31-music-idle-drain
 * (2026-09-09): a cumulative count of how many times this face's own
 * per-frame timer actually stepped the swarm and redrew the dot pool —
 * i.e. every call EXCEPT the ones this file's `music_timer_cb` skips
 * because `state->music.screen_awake` reads false (DIM/OFF/SLEEP). Reset
 * to 0 by every `ff_scr_music_build` (a fresh Music session, mirrors
 * `s_last_beat_count`'s own reset there), so a test can build the face,
 * pump a DIM/OFF window, and assert this counter did not move —
 * `targets/sim/tests/test_ctl_music_idle_drain.c`'s whole reason to read
 * it. Not read by any product code (mirrors `ff_shell_music_debug`'s own
 * "one-shot debug getter" convention, app/include/ff_shell.h) — it exists
 * purely so "the swarm timer pauses on DIM/OFF" is a measured property,
 * not an assumed one (AGENTS.md item 6).
 */
uint32_t ff_scr_music_debug_render_ticks(void);

/**
 * ff_scr_music_frame_stats_t / ff_scr_music_debug_frame_stats —
 * [debug-console-only] 2026-09-09 S31 canvas renderer: the last-CLOSED
 * one-second window's rolling average frame period (`frame_period_avg_
 * ms`, the wall time between two consecutive REAL per-frame timer
 * ticks — never the settle step, never a DIM/OFF-paused tick) and
 * average canvas composite draw time (`canvas_draw_avg_us`, everything
 * `music_redraw_canvas` does except the final O(1) `lv_obj_invalidate`)
 * — see `scr_music.c`'s own top comment, "Instrumentation", for exactly
 * what is measured, with what clock, and why. `valid` is false (both
 * numeric fields 0) until the first window has ever closed — an honest
 * "no data yet", never a fabricated 0.00/0, matching `ff_display_perf_t`
 * (esp32s3) own "n/a is not the same fact as an instant zero" rule.
 *
 * This is what `app_main.c`'s `dbgconsole_music_frame` (wired through
 * `ff_dbgconsole_music_frame_fn`, ff_debug_console.h) is a thin
 * passthrough onto — see that typedef's own doc comment for why
 * `ff_debug_console.c` cannot call this getter directly (it deliberately
 * excludes LVGL/this component). Not read by any product code — mirrors
 * `ff_scr_music_debug_render_ticks`'s own "console/test-only getter"
 * role just above.
 */
typedef struct {
    bool valid;
    float frame_period_avg_ms;
    uint32_t canvas_draw_avg_us;
} ff_scr_music_frame_stats_t;

ff_scr_music_frame_stats_t ff_scr_music_debug_frame_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_MUSIC_H */
