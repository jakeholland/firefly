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
    TEST_ASSERT_EQUAL_INT16(28, cfg.rim_px);
    TEST_ASSERT_EQUAL_INT16(56, cfg.back_travel_px);
    TEST_ASSERT_EQUAL_INT16(64, cfg.home_travel_px);
    TEST_ASSERT_EQUAL_INT16(24, cfg.axis_lock_px);
    TEST_ASSERT_EQUAL_UINT16(500, cfg.window_ms);
    TEST_ASSERT_EQUAL_UINT16(1200, cfg.long_ms);
    TEST_ASSERT_EQUAL_INT16(12, cfg.long_slop_px);
    TEST_ASSERT_FALSE(cfg.long_press_enabled);
}

/* ------------------------------------------------------------------- */
/* AC1 — left-rim swipe -> BACK exactly once                            */
/* ------------------------------------------------------------------- */

static void S28_AC1_left_rim_swipe_recognises_back(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* DOWN inside the circle, within the left rim zone: cx-r+28 = 36,
     * so x=20 qualifies. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 20, 206, 0));
    /* dx=60 >= 56, |dy|=0 <= 0.6*60, well inside the 500ms window. */
    TEST_ASSERT_EQUAL(FF_GESTURE_BACK, ff_gesture_feed(&g, true, 80, 206, 100));
}

/* ------------------------------------------------------------------- */
/* AC2 — the same motion starting 40px inboard of the rim -> NONE       */
/* ------------------------------------------------------------------- */

static void S28_AC2_swipe_starting_inboard_of_rim_is_none(void)
{
    ff_gesture_t g;
    gesture_new(&g, false);

    /* Left rim zone ends at x=36; start 40px further in. */
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 76, 206, 0));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, true, 136, 206, 100));
    TEST_ASSERT_EQUAL(FF_GESTURE_NONE, ff_gesture_feed(&g, false, 136, 206, 140));
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

    /* Bottom rim zone starts at y = cy+r-28 = 378. */
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

    RUN_TEST(gesture_null_safe);

    return UNITY_END();
}
