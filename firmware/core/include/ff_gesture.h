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
 *    (`x <= cx - r + back_rim_px`). The finger must then travel
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
 *    (`y >= cy + r - home_rim_px`). Symmetric to G1 on the vertical axis
 *    (`dy <= -home_travel_px`, i.e. "up" by that many px, axis-locked
 *    against `|dx|`), PLUS a minimum mean speed (`up / elapsed_ms >=
 *    0.25`) evaluated at the same moment as the ratio check — a slow
 *    deliberate drag upward from the bottom edge must not fire HOME.
 *    (2026-09-07 map-back-gesture-stall amendment: `back_rim_px`/
 *    `home_rim_px` used to be one shared `rim_px` field, pinned at 28 for
 *    both zones — see this header's own "Edge tolerance" section below
 *    for why they were split and widened to 44/64.)
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
 * A touch whose DOWN point is admitted by NEITHER the padded glass
 * circle (past `r + cfg.edge_slop_px`) NOR either rim zone (this
 * header's own "Edge tolerance" and "Edge tolerance, part 3" sections)
 * can never produce ANY of the three gestures (a corner-pixel touch is
 * not "on the glass" at all) — this is the FSM's own guard, checked once at
 * DOWN, not something the glue has to remember to apply.
 *
 * ## Timing — wrap-safe
 * Every deadline comparison goes through `ff_time_reached`
 * (platform/include/ff_clock.h) — the same twos-complement-subtraction
 * convention every other core FSM in this repo uses (`ff_multitap.h`,
 * `ff_idle.h`, `ff_power_fsm.h`, ...), so a `now_ms` that wraps past
 * `UINT32_MAX` mid-touch still recognises correctly (S28_AC10).
 *
 * ## Stall tolerance (2026-09-07 amendment, docs/specs/S28-gestures.md)
 * `window_ms` is measured from the DOWN sample — but DOWN is also the
 * ONLY sample a stalled poll loop is guaranteed to have delivered before
 * it froze (real bench evidence: a press on the Map face stalled the
 * device's LVGL/touch-poll loop for ~520ms — see the spec's dated
 * amendment for the numbers). Without this rule, that stall alone burns
 * the whole 500ms window before the FIRST post-stall sample even
 * arrives, and a real, well-formed 56px swipe recognises as NONE simply
 * because the device was busy, not because the finger did anything
 * wrong — on the Map face specifically, this made BACK/HOME
 * unreachable (there is no other way off that face).
 *
 * The fix, applied ONCE per touch, at the FIRST sample after DOWN (never
 * re-applied to any later sample, so it cannot repeatedly "refresh" a
 * window during an otherwise-ordinary drag): if that first sample
 * arrives `> cfg.stall_gap_ms` after `t0` (DOWN's own timestamp) — i.e.
 * the gap itself proves the device, not the finger, was the slow party
 * — the window's own clock (`t0`) is moved forward to that sample's
 * `now_ms`, and every subsequent `window_ms`/ratio/speed check
 * (G1 and G2 alike, since both read `t0`) is evaluated against THAT
 * start instead of the original DOWN time. The touch's SPATIAL origin
 * (`x0`/`y0`, and therefore every `dx`/`dy` this FSM ever computes) is
 * left untouched — only the clock moves, never where the finger
 * started, so `back_travel_px`/`home_travel_px` still mean genuine
 * physical displacement from the real touch-down point.
 *
 * A gap `<= stall_gap_ms` (the ordinary case — nothing was stalled)
 * changes nothing: `t0` stays put and every rule behaves exactly as it
 * did before this amendment. This is deliberately a NARROW exception,
 * not a general "be more lenient about timing": a genuinely slow but
 * evenly-sampled swipe (no single gap over `stall_gap_ms`, just a lot
 * of them, each fine on its own, adding up past `window_ms` in total)
 * gets no help from this rule and must still read as NONE — see
 * S28_AC12, the negative control that is this amendment's own proxy
 * guard (AGENTS.md item 6): "a stall-tolerant window recognises a late
 * swipe" is satisfied just as well by an FSM that simply doubled
 * `window_ms` outright, which would wrongly let a slow drag through.
 *
 * ## Edge tolerance (2026-09-07 map-back-gesture-stall amendment, part 2)
 * The DOWN-in-circle guard (this header's own top note, S28_AC9) compares
 * the raw touch point against `cx`/`cy`/`r` with ZERO tolerance —
 * `dist(x,y; cx,cy) <= r`, exactly. Real bench data (docs/specs/
 * S28-gestures.md's dated amendment) shows genuine, deliberate rim-zone
 * touches — the exact motion G1/G2 exist to recognise — landing a few
 * pixels PAST that boundary: a fingertip pressed right at the physical
 * glass edge does not measure as a perfect circle at the pixel level,
 * because `r` itself is already a slightly-trimmed value (see the theme
 * layer's own comment on `FF_THEME_GLASS_R`: "203 measured; pulled in 3
 * px so a ring on it clears the bezel lip" — trimmed for VISUAL
 * clearance, not for touch admission) and per-touch measurement noise
 * adds a little more on top. Before this amendment, a DOWN sample that
 * landed even 1px past `r` was `FF_GESTURE_PHASE_ABORTED` outright —
 * before ANY rim-zone, axis-lock, or window logic ever ran — silently
 * eating a perfectly legitimate edge swipe.
 *
 * The fix: `cfg.edge_slop_px` (new field, default 16) pads ONLY the
 * admission check — `dist(x,y; cx,cy) <= r + edge_slop_px` — leaving
 * every other use of `r` (the rim-zone formulas above) exactly as
 * precise as before. A DOWN this far outside the visual ring is still
 * unambiguously "on the glass" (16px is a small fraction of the ~200px
 * radius, and nowhere close to admitting a genuinely off-glass touch —
 * S28_AC9's own corner-pixel example remains ~280px away, comfortably
 * outside even with this slop applied).
 *
 * ## Edge tolerance, part 3 — rim admission independent of the visual
 * circle (2026-09-07 map-back-gesture-stall amendment, round 3)
 * Part 2's `edge_slop_px` pads the admission circle by a flat 16px in
 * EVERY direction — but a real BACK/HOME swipe starts at the extreme
 * edge of the touch panel, and the panel is not clipped to the round
 * bezel: it reports finger positions out to `x=0`/`x=panel_size_px-1`
 * along the WHOLE left edge, at any `y`, because the panel is a square
 * sensor under a round window. Bench evidence (Jake's puck, live
 * theme geometry `cx=208, cy=206, r=200`, 2026-09-07): a clean left-rim
 * BACK swipe on the Map face, `DOWN(5,119)`, was `ABORTED` at DOWN —
 * `dist((5,119),(208,206)) ≈ 220.9px`, past even the padded radius
 * `r+edge_slop_px=216`. The SAME shape of swipe on Settings,
 * `DOWN(5,183)`, was admitted (`dist ≈ 204px ≤ 216`) and recognised
 * BACK normally. The only difference is how far `y` sits from `cy`
 * (119 is 87px off-centre; 183 is only 23px off) — a flat circular pad
 * can never cover every `y` along the rim without also being wide
 * enough to swallow genuinely off-glass corner touches (`S28_AC9`'s own
 * `(0,0)` example), because the circle simply is not the right SHAPE
 * for admitting a touch whose defining feature is "started at the
 * physical edge, however far up or down that edge".
 *
 * The fix: `gesture_down_admitted` (ff_gesture.c) admits a DOWN if
 * EITHER the padded-circle check above passes, OR the point falls in
 * one of the two rim zones this module already computes `back_alive`/
 * `home_alive` from — `x <= cx - r + back_rim_px` for the BACK zone,
 * `y >= cy + r - home_rim_px` for the HOME zone — PROVIDED the point is
 * also a sane panel coordinate (`0 <= x,y < cfg.panel_size_px`, new
 * field, default 412 — this header's own "the panel size should come
 * from config, not a literal" convention, matching `FF_THEME_PUCK_PX`)
 * and, on the axis PERPENDICULAR to that rim, still within the circle's
 * own unpadded span (`cy-r <= y <= cy+r` for the BACK zone, `cx-r <= x
 * <= cx+r` for the HOME zone). That perpendicular bound is what keeps
 * `S28_AC9`'s corner pixel — `x` well inside the BACK rim's threshold,
 * but `y` far above the circle's own top — from being admitted just
 * because it happens to share one coordinate with a real rim touch: a
 * round window has no glass left up in that corner for a finger to
 * physically touch, no matter how far left `x` reads. The two rim
 * zones this admits are exactly the ones `back_alive`/`home_alive`
 * already gate everything else on, so nothing downstream (axis lock,
 * ratio, window, stall tolerance) changes shape — a rim-admitted touch
 * that then moves the wrong axis first is disqualified exactly the way
 * it always was.
 *
 * The `0 <= x,y < panel_size_px` bound also does the job the old
 * zero-tolerance circle check used to do incidentally: rejecting the
 * driver's own invalid-point sentinels (`(-1,-1)`, or a raw `0xFFFF`
 * that arrives here already narrowed to `int16_t -1` by the caller) —
 * those are never "on the glass" under any interpretation, rim or
 * circle, and this bound is what actually says so explicitly now that
 * the rim zones no longer imply it for free.
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
    /* How deep the LEFT (G1) / BOTTOM (G2) rim zones reach in from the
     * circle's own edge. Two independent fields (2026-09-07 map-back-
     * gesture-stall amendment — used to be one shared `rim_px`, split
     * because bench evidence called for widening them by different
     * amounts: see docs/specs/S28-gestures.md's dated amendment). */
    int16_t back_rim_px;
    int16_t home_rim_px;
    int16_t back_travel_px;      /* G1's required rightward dx */
    int16_t home_travel_px;      /* G2's required upward travel (i.e. -dy) */
    int16_t axis_lock_px;        /* the off-axis travel that disqualifies G1/G2 as a scroll */
    uint16_t window_ms;          /* G1/G2 must reach their travel threshold within this long of DOWN (or of the first post-stall sample — see this header's "Stall tolerance" section) */
    uint16_t long_ms;            /* G3's required hold duration */
    int16_t long_slop_px;        /* G3's allowed total movement from the DOWN point */
    bool long_press_enabled;     /* G3 armed at all — the glue flips this per active face (+ per touch, see ff_gesture_set_long_press) */
    /* This header's "Stall tolerance" section has the full rationale
     * and bench evidence. Checked ONCE per touch, at the first sample
     * after DOWN only. */
    uint16_t stall_gap_ms;
    /* This header's "Edge tolerance" section. Pads the DOWN-in-circle
     * admission check only; `r` itself (and the rim-zone formulas above)
     * stay exact. */
    int16_t edge_slop_px;
    /* This header's "Edge tolerance, part 3" section: the touch panel's
     * own size (a square sensor; the round glass sits inside it), used
     * ONLY to sanity-bound a rim-admitted DOWN and to reject the
     * driver's invalid-point sentinels. NOT the glass circle — that is
     * `cx`/`cy`/`r` above. */
    int16_t panel_size_px;
} ff_gesture_cfg_t;

/**
 * ff_gesture_cfg_default — fill `*cfg` with the spec's pinned defaults:
 * `cx`/`cy`/`r` from the caller, `back_rim_px=44`, `home_rim_px=64`
 * (2026-09-07 amendment — widened from a shared `rim_px=28`, this
 * header's "Edge tolerance" section and docs/specs/S28-gestures.md's
 * dated amendment have the bench evidence), `back_travel_px=56`,
 * `home_travel_px=64`, `axis_lock_px=24`, `window_ms=500`,
 * `long_ms=1200`, `long_slop_px=12`, `long_press_enabled=false` (the
 * glue arms it explicitly once a face is known — see
 * `ff_gesture_set_long_press`), `stall_gap_ms=150` (this header's
 * "Stall tolerance" section), `edge_slop_px=16` (this header's "Edge
 * tolerance" section), `panel_size_px=412` (this header's "Edge
 * tolerance, part 3" section — matches `FF_THEME_PUCK_PX`; the glue may
 * override it explicitly rather than relying on this default staying in
 * sync). NULL-safe (no-op on a NULL `cfg`).
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
    bool stall_checked;            /* the first-post-DOWN-sample stall check (this header's "Stall tolerance" section) has already run for this touch */
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
 * The very FIRST sample fed after DOWN also runs the one-shot stall
 * check this header's "Stall tolerance" section documents (moving the
 * window's own clock forward if that sample arrived suspiciously late)
 * — before anything else this sample might otherwise trigger, so a
 * sample that is BOTH the delayed first-post-stall sample AND the one
 * that happens to cross a travel threshold is judged against the
 * adjusted window, not the stale one.
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
