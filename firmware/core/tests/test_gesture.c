/**
 * test_gesture.c — unit tests for `ff_gesture` (S28: on-glass BACK/HOME
 * edge-swipe + long-press flare recognition FSM).
 *
 * Spec: docs/specs/S28-gestures.md. Test names mirror the spec's own
 * AC numbering (AGENTS.md convention: `S28_AC<n>_...`).
 *
 * Glass geometry used throughout: `FF_THEME_GLASS_CX/CY/R` = (208, 206,
 * 200) — this file does NOT include app/theme/ff_theme.h (core stays
 * dependency-free), the literal values are just copied in, same as
 * every other core test that needs a concrete geometry to drive the
 * FSM under test.
 *
 * THE PROXY, stated up front (AGENTS.md item 6): "a fast enough swipe
 * fires BACK" is satisfied even by an FSM with no axis lock, no window
 * bound, and no rim-zone gate at all. Every positive AC below (1, 5) is
 * paired with a negative control that drives NEARLY the same motion but
 * violates exactly one bound (AC2: wrong start zone, AC3: wrong axis
 * first, AC4: too slow, AC6: wrong ratio) and asserts it does NOT fire.
 */
#include <string.h>

#include "unity.h"

#include "ff_gesture.h"

#define GLASS_CX 208
#define GLASS_CY 206
#define GLASS_R  200

void setUp(void) {}
void tearDown(void) {}

static void gesture_new(ff_gesture_t *g, bool long_press_enabled)
{
    ff_gesture_cfg_t cfg;
    ff_gesture_cfg_default(&cfg, GLASS_CX, GLASS_CY, GLASS_R);
    cfg.long_press_enabled = long_press_enabled;
    ff_gesture_init(g, &cfg);
}

/* ------------------------------------------------------------------- */
/* literal-pinned defaults (proxy guard: a test that only ever compares */
/* symbolically against the values below would survive a silent change) */
/* ------------------------------------------------------------------- */

static void gesture_cfg_default_pins_spec_constants(void)
{
    ff_gesture_cfg_t cfg;
    ff_gesture_cfg_default(&cfg, GLASS_CX, GLASS_CY, GLASS_R);

    TEST_ASSERT_EQUAL_INT16(GLASS_CX, cfg.cx);
    TEST_ASSERT_EQUAL_INT16(GLASS_CY, cfg.cy);
    TEST_ASSERT_EQUAL_INT16(GLASS_R, cfg.r);
    /* 2026-09-07 map-back-gesture-stall amendment: rim_px split and
     * widened (28 -> 44/64) — ff_gesture.h's "Edge tolerance" section. */
    TEST_ASSERT_EQUAL_INT16(44, cfg.back_rim_px);
    TEST_ASSERT_EQUAL_INT16(64, cfg.home_rim_px);
    TEST_ASSERT_EQUAL_INT16(56, cfg.back_travel_px);
    TEST_ASSERT_EQUAL_INT16(64, cfg.home_travel_px);
    TEST_ASSERT_EQUAL_INT16(24, cfg.axis_lock_px);
    TEST_ASSERT_EQUAL_UINT16(500, cfg.window_ms);
    TEST_ASSERT_EQUAL_UINT16(1200, cfg.long_ms);
    TEST_ASSERT_EQUAL_INT16(12, cfg.long_slop_px);
    TEST_ASSERT_FALSE(cfg.long_press_enabled);
    TEST_ASSERT_EQUAL_UINT16(150, cfg.stall_gap_ms);
    TEST_ASSERT_EQUAL_INT16(16, cfg.edge_slop_px);
}

/* ------------------------------------------------------------------- */
/* AC1 — left-rim swipe -> BACK exactly once                            */
/* ------------------------------------------------------------------- */

static void S28_AC1_left_rim_swipe_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* DOWN inside the circle, within the left rim zone: cx-r+back_rim_px
     * = 208-200+44 = 52, so x=20 qualifies. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* dx=60 >= 56, |dy|=0 <= 0.6*60, well inside the 500ms window. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 80, 206, 100));
}

/* ------------------------------------------------------------------- */
/* AC2 — the same motion starting 60px inboard of the (widened) rim ->  */
/* NONE. 2026-09-07 amendment: adjusted from "40px inboard" to "60px    */
/* inboard" because back_rim_px widened 28 -> 44 (ff_gesture.h's "Edge  */
/* tolerance" section) — the old 40px-inboard point (x=76) is now       */
/* itself only 24px past the new, wider zone edge (52), not a clean     */
/* negative control; 60px inboard of the new edge keeps the same        */
/* "comfortably outside the zone" margin the original AC2 intended.     */
/* ------------------------------------------------------------------- */

static void S28_AC2_swipe_starting_inboard_of_rim_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* Left rim zone ends at x = cx-r+back_rim_px = 208-200+44 = 52;
     * start 60px further in (x=112). */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 112, 206, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 172, 206, 100));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, false, 172, 206, 140));
}

/* ------------------------------------------------------------------- */
/* AC3 — a vertical scroll starting at the left rim -> NONE              */
/* ------------------------------------------------------------------- */

static void S28_AC3_vertical_scroll_at_left_rim_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 200, 0));
    /* |dy|=40 > axis_lock_px(24) while dx=0 (< 56): a scroll, BACK is
     * disqualified for the rest of this touch. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 240, 50));
    /* THE PROXY (AGENTS.md item 6): a finger that then straightens back
     * out toward y0 would satisfy BACK's ratio/travel/window checks on
     * their own — asserting NONE here only proves the axis lock still
     * held if the final sample's own dy is SMALL enough that the ratio
     * check alone would have passed. Mutation guard: deleting the
     * axis-lock check above makes this fire BACK instead. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 80, 202, 100));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, false, 80, 202, 140));
}

/* ------------------------------------------------------------------- */
/* AC4 — a slow swipe (600ms) -> NONE                                    */
/* ------------------------------------------------------------------- */

static void S28_AC4_slow_swipe_over_window_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* fix/map-back-gesture-stall amendment: a prompt (well under
     * stall_gap_ms=150) first sample here establishes that nothing was
     * stalled — see ff_gesture.h's "Stall tolerance" section — so the
     * window's clock is NOT moved and the ORIGINAL DOWN time still
     * governs. Without this sample, a single DOWN->600ms jump is
     * genuinely indistinguishable from a stalled device (S28_AC11's own
     * shape) and the amended FSM correctly, not wrongly, rescues it —
     * this test is about a SLOW finger with a live, unstalled poll
     * loop, so it must look like one. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 22, 206, 20));
    /* Same ratio as AC1 (dy=0), but 600ms > the 500ms window. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 80, 206, 600));
}

/* ------------------------------------------------------------------- */
/* AC5 — bottom-rim swipe up -> HOME                                     */
/* ------------------------------------------------------------------- */

static void S28_AC5_bottom_rim_swipe_up_recognises_home(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* Bottom rim zone starts at y = cy+r-home_rim_px = 206+200-64 = 342. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 390, 0));
    /* up=70 >= 64, |dx|=0, elapsed=100ms -> mean speed 0.7 px/ms >= 0.25. */
    TEST_ASSERT_EQUAL(FF_GESTURE_HOME, ff_gesture_feed(&g, true, 208, 320, 100));
}

/* ------------------------------------------------------------------- */
/* AC6 — diagonal beyond the ratio -> NONE                               */
/* ------------------------------------------------------------------- */

static void S28_AC6_diagonal_beyond_ratio_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* dx=56 (exactly the threshold), dy=40 -> |dy|=40 > 0.6*56=33.6:
     * fails the ratio at the moment dx reaches 56. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 76, 246, 50));
}

/* ------------------------------------------------------------------- */
/* AC7 — long press family                                              */
/* ------------------------------------------------------------------- */

static void S28_AC7_long_press_1200ms_within_slop_fires_from_tick(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 206, 0));
    /* Small jitter, well inside the 12px slop budget (dist=4). */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 212, 206, 50));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 1199));
    TEST_ASSERT_EQUAL(FF_GESTURE_LONG_PRESS, ff_gesture_tick(&g, 1200));
}

static void S28_AC7_long_press_with_excess_movement_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 206, 0));
    /* 20px straight-line movement — over the 12px slop budget. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 228, 206, 50));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 1200));
}

static void S28_AC7_long_press_disabled_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false); /* long_press_enabled = false */

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 206, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 1200));
}

/* Extra: disarming mid-touch (ff_gesture_set_long_press(false) after the
 * DOWN, before the deadline) must also block the fire — the glue's
 * "interactive widget" refusal path lands exactly here. */
static void gesture_long_press_disarmed_mid_touch_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 206, 0));
    ff_gesture_set_long_press(&g, false);
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 1200));
}

/* ------------------------------------------------------------------- */
/* AC8 — no second event after recognition, until UP                    */
/* ------------------------------------------------------------------- */

static void S28_AC8_no_second_event_after_recognition_until_up(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 80, 206, 100));

    /* Further MOVE samples (even ones that would otherwise satisfy a
     * long press) and a tick call must all stay silent... */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 80, 206, 150));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 2000));
    /* ...right up through the eventual UP. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, false, 80, 206, 2000));

    /* A FRESH touch afterward is free to recognise again — DONE is
     * per-touch, not permanent. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 3000));
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 80, 206, 3100));
}

/* ------------------------------------------------------------------- */
/* AC9 — DOWN outside the glass circle never starts a gesture           */
/* ------------------------------------------------------------------- */

static void S28_AC9_down_outside_circle_never_starts_a_gesture(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    /* (0,0): distance from (208,206) is ~293px, outside r=200. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 0, 0, 0));
    /* A motion that would satisfy BACK's travel/ratio/window if the
     * DOWN had landed inside the circle... */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 60, 0, 50));
    /* ...and a long hold that would satisfy G3 too. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(&g, 1200));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, false, 60, 0, 1200));
}

/* ------------------------------------------------------------------- */
/* AC10 — now_ms wraparound around 0xFFFFFFFF still recognises          */
/* ------------------------------------------------------------------- */

static void S28_AC10_time_wrap_still_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    uint32_t const t0 = (uint32_t)0xFFFFFFFFu - 50u;
    uint32_t const t1 = t0 + 100u; /* wraps past UINT32_MAX; true elapsed is 100ms */

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, t0));
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 80, 206, t1));
}

static void S28_AC10_time_wrap_still_recognises_long_press(void)
{
    ff_gesture_t g;
    gesture_new(&g, true);

    uint32_t const t0 = (uint32_t)0xFFFFFFFFu - 50u;
    uint32_t const t1 = t0 + 1200u; /* wraps; true elapsed is exactly long_ms */

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 208, 206, t0));
    TEST_ASSERT_EQUAL(FF_GESTURE_LONG_PRESS, ff_gesture_tick(&g, t1));
}

/* ------------------------------------------------------------------- */
/* fix/map-back-gesture-stall (2026-09-07 dated amendment to S28's own  */
/* spec) — AC11/AC12: a stalled poll loop must not eat the recognition  */
/* window (bench evidence: a press on the Map face froze the device's   */
/* touch-poll loop for ~520ms, making BACK/HOME unreachable there —     */
/* see ff_gesture.h's "Stall tolerance" section for the full mechanism  */
/* and docs/specs/S28-gestures.md's own dated amendment for the bench   */
/* numbers). NOTE ON NUMBERING: slice b (test_gesture_glue.c) already   */
/* uses S28_AC11..AC18 for its own (later, sim/glue-level) criteria —   */
/* the coordinator's brief for THIS fix named the new core-level tests  */
/* "AC11"/"AC12" too; kept literally as instructed since these live in  */
/* a different file/module than slice b's AC11/AC12 and test different  */
/* things, but flagged here (and in the PR body) as a real numbering    */
/* collision in the spec's own AC space, not a copy-paste mistake.      */
/* ------------------------------------------------------------------- */

static void S28_AC11_stall_after_down_then_normal_swipe_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* DOWN inside the left rim zone, at t=0. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* The device's own poll loop stalls for 520ms (> stall_gap_ms=150)
     * — this is the FIRST sample fed after DOWN, with no travel yet at
     * this exact instant. The one-shot stall check fires here, moving
     * the window's own clock (t0) forward to t=520. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 520));
    /* A normal 56px swipe, well within 500ms of the FIRST sample
     * (920 - 520 = 400ms) even though it lands 920ms after the real
     * DOWN — which the un-amended FSM measured window_ms against and
     * would have failed (S28_AC4's own rule, applied to a stall instead
     * of a slow finger) — exactly the Map-face BACK-unreachable bug
     * this amendment fixes. Mutation guard: deleting the stall check
     * (or comparing against the ORIGINAL t0=0: 920-0=920 > 500) makes
     * this assert fail instead. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 76, 206, 920));
}

static void S28_AC12_genuinely_slow_swipe_over_window_still_fails(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* THE PROXY, stated up front (AGENTS.md item 6): "a stall-tolerant
     * window recognises a late swipe" is satisfied just as well by an
     * FSM that simply doubled or dropped `window_ms` outright, which
     * would wrongly let a slow, ordinary drag through too. The FIRST
     * sample after DOWN arrives promptly here (20ms, well under
     * stall_gap_ms=150) — nothing was stalled, so t0 is NOT moved, and
     * a genuinely slow drag (56px only after 700ms, over the 500ms
     * window, with every individual gap far under stall_gap_ms) must
     * still read as NONE — the tolerance is for GAPS, not for slowness
     * in general. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 21, 206, 20));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 76, 206, 700));
}

/* ------------------------------------------------------------------- */
/* fix/map-back-gesture-stall — real-puck bench replay (AC13 onward).   */
/* Geometry throughout THIS section is the bench's own, stated directly */
/* in the coordinator's bench log: cx=cy=206, r=206 (the "current left  */
/* rim zone x<=28 / bottom rim zone y>=384" figures only fall out of    */
/* the standard formulas with THIS geometry — 206-206+28=28 and         */
/* 206+206-28=384 — not the theme's own FF_THEME_GLASS_CX/CY/R used     */
/* above; kept distinct from GLASS_CX/CY/R deliberately, per-section,   */
/* rather than silently reusing a name for two different geometries).   */
/* ------------------------------------------------------------------- */

#define BENCH_CX 206
#define BENCH_CY 206
#define BENCH_R  206

static void gesture_new_bench(ff_gesture_t *g)
{
    ff_gesture_cfg_t cfg;
    ff_gesture_cfg_default(&cfg, BENCH_CX, BENCH_CY, BENCH_R);
    ff_gesture_init(g, &cfg);
}

/* AC13 — bug (1): Radar, DOWN(5,175), a 9-sample poll trace with dx
 * reaching 58 (dy=3) at t=136ms, reported NOT recognised on the real
 * puck ("REGRESSION: ... find why the new code rejects dx 58 / dy 3 at
 * 136ms — stall-rule bookkeeping, t0 handling, a changed comparison,
 * the 'first sample' flag").
 *
 * INVESTIGATED, NOT REPRODUCED IN THIS FSM: every inter-sample gap in
 * this trace is <=20ms (well under stall_gap_ms=150), so the stall-
 * tolerance rule (ff_gesture.h's "Stall tolerance" section) never even
 * engages — t0 stays at DOWN's own timestamp throughout. Run against
 * BOTH the pre-amendment FSM (commit 6174eaf, before the stall-
 * tolerance rule existed at all) and the current one (3b6bce7), this
 * exact sequence already returns FF_GESTURE_BACK at t=136 in both —
 * confirmed by direct compile-and-run of both source trees against
 * this literal sample sequence, not just by reading the code. There is
 * no reproducible core-FSM bug here for this trace; kept as a passing
 * regression guard. (If the real device genuinely failed to recognise
 * this exact motion, the cause is outside this module — most likely
 * the production glue/indev delivery path or a geometry mismatch
 * between the bench capture and the puck's actual runtime cx/cy/r —
 * not something `ff_gesture_feed`'s own logic can be made to explain
 * away without contradicting this direct evidence.) */
static void S28_AC13_bench_radar_swipe_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new_bench(&g);

    int const t[]  = {0, 16, 36, 56, 76, 96, 116, 136, 156, 176};
    int const x[]  = {5, 5, 5, 5, 5, 15, 15, 63, 63, 142};
    int const y[]  = {175, 175, 175, 175, 175, 173, 173, 178, 178, 207};

    for (size_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, (int16_t)x[i], (int16_t)y[i], (uint32_t)t[i]));
    }
    /* t=136: dx=58, dy=3 — the sample the bug report names explicitly. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, (int16_t)x[7], (int16_t)y[7], (uint32_t)t[7]));
}

/* AC14 — bug (3): Map, DOWN(5,180), dx first reaches 167 (>=56) at the
 * SAME sample dy jumps to 69 (t=176ms), reported NOT recognised.
 * Hypothesis: "the axis-lock abort (|dy|>24 'before dx reaches 56')
 * fires on the same sample [dx first crosses 56]" — i.e. a same-sample
 * ordering bug.
 *
 * INVESTIGATED, NOT REPRODUCED: `ff_gesture_feed`'s G1 block only
 * evaluates the axis-lock check inside the `dx < back_travel_px`
 * branch; the sample where dx first reaches/exceeds the threshold goes
 * straight to the ratio+window `else` branch and never touches the
 * axis-lock check at all (see that function's own top comment: "The
 * axis-lock disqualification is checked on every sample BEFORE that
 * threshold is reached, not after"). Run directly against this exact
 * sequence, the shipped 3b6bce7 FSM already returns FF_GESTURE_BACK at
 * t=176 (ratio 69/167 = 0.41 <= 0.6, matching the bug report's own
 * arithmetic). No ordering fix was needed; kept as a passing regression
 * guard pinning the (already-correct) evaluation order. */
static void S28_AC14_bench_map_ordering_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new_bench(&g);

    int const t[] = {0, 16, 36, 56, 76, 96, 116, 136, 156, 176};
    int const x[] = {5, 5, 5, 5, 5, 5, 5, 52, 52, 172};
    int const y[] = {180, 180, 180, 180, 180, 180, 180, 192, 192, 249};

    for (size_t i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, (int16_t)x[i], (int16_t)y[i], (uint32_t)t[i]));
    }
    /* t=176: dx=167 (first sample >=56), dy=69, ratio 0.41 <= 0.6. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, (int16_t)x[9], (int16_t)y[9], (uint32_t)t[9]));
}

/* AC15 — bug (4): Map, DOWN(5,156): a REAL, reproducible bug. This
 * DOWN point is 5-206=-201, 156-206=-50 from centre: dist^2=42901 vs
 * r^2=42436 (r=206) — just ~1.1px OUTSIDE the admission circle. Before
 * this amendment, `gesture_in_circle` used `r` with zero tolerance, so
 * this entire touch went straight to FF_GESTURE_PHASE_ABORTED at DOWN
 * — before the stall check, the rim zone, the axis lock, or the ratio
 * check ever ran. A genuine edge-of-glass touch like this is exactly
 * the "touch calibration" phenomenon docs/specs/S28-gestures.md's
 * bench amendment documents elsewhere for the HOME rim (item 6) —
 * here it blocks admission entirely, for BACK. Fixed by
 * `cfg.edge_slop_px` (ff_gesture.h's "Edge tolerance" section):
 * `gesture_in_circle` now compares against `r + edge_slop_px`.
 * Mutation-proven: setting `edge_slop_px` back to 0 (or reverting
 * `gesture_in_circle` to compare against plain `r`) makes this test's
 * FIRST assertion fail (ABORTED instead of TRACKING) — verified by a
 * fresh build with that one-line change reverted. This sample also
 * exercises the stall-tolerance rule (first post-DOWN sample arrives
 * 325ms late) — both fixes work together on the same touch. */
static void S28_AC15_bench_map_edge_admits_and_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new_bench(&g);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 156, 0));
    /* First sample after DOWN, 325ms late (> stall_gap_ms=150): admitted
     * only because of the edge-slop fix (dist ~207.1 > r=206, but
     * <= r+edge_slop_px=222); the stall check also fires here, moving
     * t0 to 325. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 156, 325));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 174, 345));
    /* dx=191 (>=56), dy=103, ratio 103/191=0.54 <= 0.6; 385-325=60ms,
     * well inside the (stall-adjusted) 500ms window. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 196, 259, 385));
}

/* AC16 — bug (2): Map, DOWN(5,219), the bench's OWN positive control
 * ("stall rule OK"): a 327ms stall, then a normal swipe. dist from
 * (206,206) is ~201.4, comfortably inside r=206 even without the edge
 * slop — this one was never blocked by admission, only exercises the
 * stall-tolerance path already covered by AC11/AC12. Kept here as a
 * passing regression guard alongside its siblings in this bench-replay
 * section. */
static void S28_AC16_bench_map_stall_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new_bench(&g);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 219, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 219, 327));
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 101, 199, 347));
}

/* AC17 — bug (5): Map, DOWN(5,207): vertical-first motion must still
 * read as NONE (the #130 regression guard, applied to this section's
 * own bench geometry) — a negative control proving the edge-slop and
 * rim-widening fixes above did not loosen the axis lock. */
static void S28_AC17_bench_map_vertical_first_stays_none(void)
{
    ff_gesture_t g;
    gesture_new_bench(&g);

    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 5, 207, 0));
    /* dx=2, dy=-36: |dy|=36 > axis_lock_px(24) while dx(2) < 56 ->
     * disqualified as a scroll before BACK's threshold is ever reached. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 7, 171, 56));
}

/* ------------------------------------------------------------------- */
/* NULL safety                                                          */
/* ------------------------------------------------------------------- */

static void gesture_null_safe(void)
{
    ff_gesture_cfg_default(NULL, GLASS_CX, GLASS_CY, GLASS_R); /* no crash */
    ff_gesture_init(NULL, NULL);                                /* no crash */
    ff_gesture_set_long_press(NULL, true);                      /* no crash */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(NULL, true, 20, 206, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_tick(NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(gesture_cfg_default_pins_spec_constants);

    RUN_TEST(S28_AC1_left_rim_swipe_recognises_back);
    RUN_TEST(S28_AC2_swipe_starting_inboard_of_rim_is_none);
    RUN_TEST(S28_AC3_vertical_scroll_at_left_rim_is_none);
    RUN_TEST(S28_AC4_slow_swipe_over_window_is_none);
    RUN_TEST(S28_AC5_bottom_rim_swipe_up_recognises_home);
    RUN_TEST(S28_AC6_diagonal_beyond_ratio_is_none);
    RUN_TEST(S28_AC7_long_press_1200ms_within_slop_fires_from_tick);
    RUN_TEST(S28_AC7_long_press_with_excess_movement_is_none);
    RUN_TEST(S28_AC7_long_press_disabled_is_none);
    RUN_TEST(gesture_long_press_disarmed_mid_touch_is_none);
    RUN_TEST(S28_AC8_no_second_event_after_recognition_until_up);
    RUN_TEST(S28_AC9_down_outside_circle_never_starts_a_gesture);
    RUN_TEST(S28_AC10_time_wrap_still_recognises_back);
    RUN_TEST(S28_AC10_time_wrap_still_recognises_long_press);

    RUN_TEST(S28_AC11_stall_after_down_then_normal_swipe_recognises_back);
    RUN_TEST(S28_AC12_genuinely_slow_swipe_over_window_still_fails);

    RUN_TEST(S28_AC13_bench_radar_swipe_recognises_back);
    RUN_TEST(S28_AC14_bench_map_ordering_recognises_back);
    RUN_TEST(S28_AC15_bench_map_edge_admits_and_recognises_back);
    RUN_TEST(S28_AC16_bench_map_stall_recognises_back);
    RUN_TEST(S28_AC17_bench_map_vertical_first_stays_none);

    RUN_TEST(gesture_null_safe);

    return UNITY_END();
}
