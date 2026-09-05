/**
 * ff_gesture.h — S28: on-glass navigation gestures (edge-swipe BACK/HOME,
 * long-press flare), the pure recognition FSM.
 *
 * Spec: docs/specs/S28-gestures.md. This module knows nothing about
 * LVGL, indevs, screens, faces, or sounds — it is fed raw touch samples
 * (DOWN/MOVE/UP, in the SAME pixel space a screen hit-tests against —
 * see the spec's own note on `ff_theme_glass_cx/cy(flip)`) and answers
 * exactly one question per sample: "did a gesture just complete?" The
 * app-level glue (app/ff_gesture_glue.c, PR2) is what turns a
 * recognition into an `ff_intent_t` dispatch, decides which face is
 * active, and refuses long-press on an interactive widget — none of
 * that belongs here, mirroring the layering `ff_multitap.h` already
 * documents for the quick-flare counter ("it knows nothing about
 * buttons... that composition is the shell's job, not this module's").
 *
 * ## Why one FSM instance is enough
 * The puck has exactly one finger at a time (a 412px round single-touch
 * panel) — one `ff_gesture_t`, fed every sample the active indev
 * delivers, is the whole story. A DOWN sample starts a fresh "touch";
 * everything until the matching UP sample is that SAME touch, and the
 * FSM recognises AT MOST ONE gesture per touch (`ff_gesture_feed`'s and
 * `ff_gesture_tick`'s own doc comments have the exact rule) — once
 * BACK/HOME/LONG_PRESS fires, or the touch is disqualified as a scroll,
 * nothing else happens until the finger lifts.
 *
 * ## Gesture rules (mirrors the spec's own wording exactly)
 *  - G1 BACK: DOWN inside the glass circle AND within the LEFT rim zone
 *    (`x <= cx - r + rim_px`). The finger must then travel
 *    `dx >= back_travel_px` within `window_ms` of DOWN, with
 *    `|dy| <= 0.6*dx` evaluated the FIRST sample where dx reaches that
 *    threshold (not re-checked on a later, larger dx — see
 *    `ff_gesture_feed`'s implementation comment). AXIS LOCK: if `|dy|`
 *    exceeds `axis_lock_px` at any sample BEFORE dx reaches the
 *    threshold, BACK is disqualified for the rest of this touch (it
 *    reads as a vertical scroll, not a swipe) — this is what keeps a
 *    real scroll gesture starting near the rim from ever firing BACK
 *    (docs/specs/S28-gestures.md's own history note on PR #130).
 *  - G2 HOME: DOWN inside the circle AND within the BOTTOM rim zone
 *    (`y >= cy + r - rim_px`). Symmetric to G1 on the vertical axis
 *    (`dy <= -home_travel_px`, i.e. "up" by that many px, axis-locked
 *    against `|dx|`), PLUS a minimum mean speed (`up / elapsed_ms >=
 *    0.25`) evaluated at the same moment as the ratio check — a slow
 *    deliberate drag upward from the bottom edge must not fire HOME.
 *  - G3 LONG_PRESS: DOWN anywhere inside the circle (no rim
 *    restriction), held for `long_ms` with total movement never
 *    exceeding `long_slop_px` from the DOWN point. Recognised from
 *    `ff_gesture_tick` (a periodic poll), NOT from the eventual UP —
 *    the glue is expected to call `ff_gesture_tick` every frame while a
 *    touch is live, same "call every tick" convention `ff_idle_tick`/
 *    `ff_multitap_pending` already use. Gated on `cfg.long_press_enabled`
 *    (`ff_gesture_set_long_press`) — the glue's job to flip per active
 *    face (and, per touch, per whether the press landed on an
 *    interactive widget — see that function's own doc comment).
 *
 * A touch whose DOWN point is outside the glass circle entirely can
 * never produce ANY of the three gestures (a corner-pixel touch is not
 * "on the glass" at all) — this is the FSM's own guard, checked once at
 * DOWN, not something the glue has to remember to apply.
 *
 * ## Timing — wrap-safe
 * Every deadline comparison goes through `ff_time_reached`
 * (platform/include/ff_clock.h) — the same twos-complement-subtraction
 * convention every other core FSM in this repo uses (`ff_multitap.h`,
 * `ff_idle.h`, `ff_power_fsm.h`, ...), so a `now_ms` that wraps past
 * `UINT32_MAX` mid-touch still recognises correctly (S28_AC10).
 *
 * Pure C11, no I/O, no allocation. `ff_gesture_t` is fully-defined (not
 * opaque), same convention as `ff_multitap_t`/`ff_flare_t`: safe on the
 * stack or in a static; zero-initialize or call `ff_gesture_init()`
 * before first use.
 */
#ifndef FF_GESTURE_H
#define FF_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What `ff_gesture_feed`/`ff_gesture_tick` recognised, if anything.
 * `FF_GESTURE_NONE` is the overwhelming common case — every DOWN/MOVE
 * sample of an ordinary tap or scroll returns it. */
typedef enum {
    FF_GESTURE_NONE = 0,
    FF_GESTURE_BACK,
    FF_GESTURE_HOME,
    FF_GESTURE_LONG_PRESS,
} ff_gesture_kind_t;

/**
 * Tunable geometry/timing, in display-space pixels and milliseconds.
 * `ff_gesture_cfg_default` fills every field with the spec's own pinned
 * constants (28, 56, 64, 24, 500, 1200, 12 — see that function's own doc
 * comment for which field gets which literal, and why they are pinned
 * by a literal test rather than compared symbolically — AGENTS.md's
 * proxy-check rule). `cx`/`cy`/`r` are the caller's own glass-circle
 * centre/radius (the spec's `FF_THEME_GLASS_CX/CY/R`, or their
 * flip-aware `ff_theme_glass_cx/cy(flip)` forms — this header does not
 * depend on app/theme, so the caller resolves those and passes plain
 * ints in).
 */
typedef struct {
    int16_t cx, cy, r;          /* the glass circle this touch space hit-tests against */
    int16_t rim_px;              /* how deep the LEFT/BOTTOM rim zones reach in from the circle's own edge */
    int16_t back_travel_px;      /* G1's required rightward dx */
    int16_t home_travel_px;      /* G2's required upward travel (i.e. -dy) */
    int16_t axis_lock_px;        /* the off-axis travel that disqualifies G1/G2 as a scroll */
    uint16_t window_ms;          /* G1/G2 must reach their travel threshold within this long of DOWN */
    uint16_t long_ms;            /* G3's required hold duration */
    int16_t long_slop_px;        /* G3's allowed total movement from the DOWN point */
    bool long_press_enabled;     /* G3 armed at all — the glue flips this per active face (+ per touch, see ff_gesture_set_long_press) */
} ff_gesture_cfg_t;

/**
 * ff_gesture_cfg_default — fill `*cfg` with the spec's pinned defaults:
 * `cx`/`cy`/`r` from the caller, `rim_px=28`, `back_travel_px=56`,
 * `home_travel_px=64`, `axis_lock_px=24`, `window_ms=500`,
 * `long_ms=1200`, `long_slop_px=12`, `long_press_enabled=false` (the
 * glue arms it explicitly once a face is known — see
 * `ff_gesture_set_long_press`). NULL-safe (no-op on a NULL `cfg`).
 */
void ff_gesture_cfg_default(ff_gesture_cfg_t *cfg, int16_t cx, int16_t cy, int16_t r);

/**
 * The whole FSM. Every member below is internal bookkeeping for ONE
 * touch at a time (DOWN..UP) — no caller outside this module should
 * read or write anything but `cfg` (and even that only through
 * `ff_gesture_set_long_press`). Fully-defined so it can live on the
 * stack or in a `static`, same convention as `ff_multitap_t`.
 */
typedef struct {
    ff_gesture_cfg_t cfg;

    bool touch_active; /* true from a DOWN sample to its matching UP sample */
    uint8_t phase;      /* FF_GESTURE_PHASE_* below */

    int16_t  x0, y0;         /* this touch's DOWN point */
    uint32_t t0;              /* this touch's DOWN time */
    int16_t  last_x, last_y;  /* most recent sample's point (diagnostic; not read by the logic) */

    bool back_alive;               /* G1 still a live candidate for this touch */
    bool home_alive;               /* G2 still a live candidate for this touch */
    bool long_alive;               /* G3 still a live candidate for this touch (disqualified by slop) */
    bool back_threshold_evaluated; /* G1's ratio/window check has already run once (pass or fail, no retry) */
    bool home_threshold_evaluated; /* G2's ratio/window/speed check has already run once */
} ff_gesture_t;

/* Internal phase values for `ff_gesture_t.phase` — not an enum typedef
 * so the struct above stays a plain, allocation-free POD the same way
 * `ff_multitap_t` is; callers never read this field directly. */
#define FF_GESTURE_PHASE_IDLE     ((uint8_t)0u) /* no touch in progress */
#define FF_GESTURE_PHASE_TRACKING ((uint8_t)1u) /* a touch is down; at least one of back/home/long_alive may still be live */
#define FF_GESTURE_PHASE_DONE     ((uint8_t)2u) /* this touch already recognised a gesture — ignored until UP */
#define FF_GESTURE_PHASE_ABORTED  ((uint8_t)3u) /* this touch's DOWN landed outside the glass circle — ignored until UP */

/** Zero `*g` and copy `*cfg` in (or leave every cfg field zero if `cfg`
 * is NULL — the caller is expected to have called
 * `ff_gesture_cfg_default` first in that case). NULL-safe on `g`
 * (no-op). */
void ff_gesture_init(ff_gesture_t *g, const ff_gesture_cfg_t *cfg);

/**
 * ff_gesture_set_long_press — arm or disarm G3 for the CURRENT and any
 * FUTURE touch, until called again. Two independent reasons the glue
 * calls this:
 *   1. Per ACTIVE FACE (the spec: "on the RADAR face only") — the glue
 *      flips this on the Radar<->other-face transition, same shape as
 *      any other "which face is showing" gate in this codebase.
 *   2. Per TOUCH, additionally: at DOWN, if the press landed on an
 *      INTERACTIVE widget (`LV_OBJ_FLAG_USER_1`, set by
 *      `ff_scr_button_create`) or any ancestor up to the face root, the
 *      glue calls this with `false` for the duration of that one touch
 *      (restoring the face-level value on the next DOWN) — this module
 *      has no notion of "widget", so the glue is the only place that
 *      CAN make that call; this setter is simply how it's expressed
 *      down here.
 * Takes effect immediately: if a touch is already `TRACKING` with
 * `long_alive` true and this call disarms it, the very next
 * `ff_gesture_tick` will see `cfg.long_press_enabled == false` and
 * refuse to fire (see that function's own doc comment) — there's no
 * need to also clear `long_alive` here, `ff_gesture_tick` checks both.
 * NULL-safe (no-op on a NULL `g`).
 */
void ff_gesture_set_long_press(ff_gesture_t *g, bool enabled);

/**
 * ff_gesture_feed — feed one raw touch sample: `down` is the CURRENT
 * physical press state (true while a finger is on the glass, false once
 * it lifts) — the same "state, not edge" convention LVGL's own indev
 * data carries. The FSM detects the DOWN edge itself (a `down == true`
 * sample arriving while no touch is already active) and the UP edge
 * (`down == false` arriving while one is) — the caller just reports
 * "where is the finger and is it still down" on every PRESSED/PRESSING/
 * RELEASED sample it receives; it does not need to pre-classify which
 * kind of sample this is.
 *
 * `x`/`y` are in the SAME pixel space `cfg.cx/cy/r` describe (display
 * space, 412x412 — see this header's top comment on flip-awareness).
 *
 * Returns the gesture recognised BY THIS SAMPLE, or `FF_GESTURE_NONE`
 * for the overwhelming majority of samples (every DOWN sample itself,
 * every UP sample, every MOVE sample that doesn't complete a gesture
 * this instant). At most ONE non-NONE return per touch — once BACK or
 * HOME fires, every subsequent sample of the SAME touch (until its UP)
 * returns NONE, mutating nothing further (S28_AC8). A touch whose DOWN
 * lands outside the glass circle never returns anything but NONE for
 * its whole lifetime (S28_AC9) — G1/G2/G3 all require an in-circle DOWN.
 *
 * Recognition detail (see this header's top comment for the rule in
 * plain language): G1/G2's ratio-and-window check runs EXACTLY ONCE per
 * touch, at the first sample where the dominant-axis travel reaches its
 * threshold — not re-evaluated on a later, larger-travel sample if it
 * fails there (a diagonal drag that overshoots the ratio at the exact
 * moment it crosses 56/64px reads as "not this gesture", even if the
 * finger straightens out afterward — S28_AC6). The axis-lock
 * disqualification (`axis_lock_px`) is checked on every sample BEFORE
 * that threshold is reached, not after.
 *
 * NULL `g`: returns FF_GESTURE_NONE, touches nothing.
 */
ff_gesture_kind_t ff_gesture_feed(ff_gesture_t *g, bool down, int16_t x, int16_t y, uint32_t now_ms);

/**
 * ff_gesture_tick — G3's own recognition path, driven by TIME rather
 * than a sample: call this every frame/tick while the FSM might have a
 * touch in progress (same "call every tick, it's a cheap no-op most of
 * the time" convention `ff_idle_tick`/`ff_multitap_pending` already
 * use) — it is what actually returns `FF_GESTURE_LONG_PRESS` once
 * `long_ms` has elapsed since DOWN, WITHOUT waiting for a MOVE/UP
 * sample to arrive and trigger the check (a finger held perfectly still
 * generates no further indev samples at all on some platforms, so a
 * feed()-only design would never fire).
 *
 * Returns `FF_GESTURE_LONG_PRESS` at most once per touch (the FSM moves
 * to its DONE phase on the same rule `ff_gesture_feed`'s BACK/HOME
 * recognition uses — S28_AC8 applies here too), and only when ALL of:
 * a touch is currently `TRACKING` (not already DONE/ABORTED, not idle),
 * `long_alive` is still true (no sample so far exceeded `long_slop_px`
 * of total movement from the DOWN point — S28_AC7's "20px movement"
 * case), `cfg.long_press_enabled` is true (re-checked here, not just at
 * DOWN, so a glue-side disarm mid-touch takes effect immediately), and
 * `now_ms` has reached (`ff_time_reached`, inclusive) `t0 + long_ms`.
 *
 * NULL `g`: returns FF_GESTURE_NONE.
 */
ff_gesture_kind_t ff_gesture_tick(ff_gesture_t *g, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_GESTURE_H */
