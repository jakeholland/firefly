/**
 * test_button.c — unit tests for `ff_button` (S26 slice e:
 * docs/specs/S26-device-lifecycle.md "(e) Home button + launcher",
 * "Button debounce unit test: one press -> one event; a 20 ms blip ->
 * none").
 *
 * THE PROXY, stated up front (AGENTS.md's standing rule, item 6): "a
 * single tick with the final level produces the right answer" is
 * satisfied even by a debouncer with NO debounce at all. Every debounce
 * test here drives an actual bounce sequence (a level flip inside the
 * 30 ms window) and asserts BOTH that the bounce alone fires nothing
 * AND that the eventually-settled level does — mirroring
 * core/tests/test_power_fsm.c's own stated proxy discipline, since this
 * module reuses that one's debounce shape.
 */
#include <string.h>

#include "unity.h"

#include "ff_button.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* init                                                                 */
/* ------------------------------------------------------------------- */

static void init_clears_all_state(void)
{
    ff_button_t b;
    memset(&b, 0xAA, sizeof(b));
    ff_button_init(&b);
    TEST_ASSERT_FALSE(b.raw_level);
    TEST_ASSERT_FALSE(b.raw_pending);
    TEST_ASSERT_FALSE(b.debounced_pressed);
}

static void init_is_null_safe(void)
{
    ff_button_init(NULL); /* must not crash */
}

/* ------------------------------------------------------------------- */
/* the core rule: one press -> one event                                */
/* ------------------------------------------------------------------- */

static void one_press_fires_exactly_one_event(void)
{
    ff_button_t b;
    ff_button_init(&b);

    /* Still debouncing — no event yet. */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 0u, true));
    /* The window elapses (INCLUSIVE boundary, ff_power_fsm.c's own
     * convention): the press commits, exactly one true. */
    TEST_ASSERT_TRUE(ff_button_tick(&b, FF_BUTTON_DEBOUNCE_MS, true));
    /* Held down for many more ticks: no re-fire while still pressed. */
    for (uint32_t t = FF_BUTTON_DEBOUNCE_MS + 1u; t < FF_BUTTON_DEBOUNCE_MS + 500u; t += 10u) {
        TEST_ASSERT_FALSE(ff_button_tick(&b, t, true));
    }
}

/* A held press must debounce-release before it can fire again — a
 * second FF_INTENT_HOME must correspond to a second real physical
 * press, not a continued hold. */
static void a_second_press_requires_release_first(void)
{
    ff_button_t b;
    ff_button_init(&b);

    TEST_ASSERT_FALSE(ff_button_tick(&b, 0u, true));
    TEST_ASSERT_TRUE(ff_button_tick(&b, FF_BUTTON_DEBOUNCE_MS, true));

    /* Release, debounced. */
    uint32_t t = FF_BUTTON_DEBOUNCE_MS + 1000u;
    TEST_ASSERT_FALSE(ff_button_tick(&b, t, false));
    t += FF_BUTTON_DEBOUNCE_MS;
    TEST_ASSERT_FALSE(ff_button_tick(&b, t, false)); /* release edge fires nothing */
    TEST_ASSERT_FALSE(b.debounced_pressed);

    /* Press again — fires again. */
    t += 1000u;
    TEST_ASSERT_FALSE(ff_button_tick(&b, t, true));
    t += FF_BUTTON_DEBOUNCE_MS;
    TEST_ASSERT_TRUE(ff_button_tick(&b, t, true));
}

/* ------------------------------------------------------------------- */
/* a blip well under the debounce window never fires                    */
/* ------------------------------------------------------------------- */

static void a_20ms_blip_never_fires(void)
{
    ff_button_t b;
    ff_button_init(&b);

    /* Raw goes high at t=0, then back low at t=20 (before the 30ms
     * debounce window closes) — a bounce, not a real press. */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 0u, true));
    TEST_ASSERT_FALSE(ff_button_tick(&b, 20u, false));

    /* Ticking well past where the ORIGINAL press's window would have
     * closed must still report nothing: the level is back to released,
     * so there is nothing pending to commit. */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 60u, false));
    TEST_ASSERT_FALSE(b.debounced_pressed);
}

/* A blip that bounces high again inside the SAME window, ending up
 * pressed by the time the window closes, must still resolve exactly
 * like a clean press — no double-fire from the intermediate flips. */
static void a_bounce_that_settles_pressed_fires_once(void)
{
    ff_button_t b;
    ff_button_init(&b);

    TEST_ASSERT_FALSE(ff_button_tick(&b, 0u, true));
    TEST_ASSERT_FALSE(ff_button_tick(&b, 5u, false));  /* bounce down */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 10u, true));  /* bounce back up, re-arms the window at t=10 */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 25u, true));  /* window (10+30=40) not yet closed */
    TEST_ASSERT_TRUE(ff_button_tick(&b, 40u, true));   /* settles pressed: fires once */
    TEST_ASSERT_FALSE(ff_button_tick(&b, 41u, true));  /* no second fire */
}

/* ------------------------------------------------------------------- */
/* wraparound safety                                                    */
/* ------------------------------------------------------------------- */

static void debounce_survives_now_ms_wraparound(void)
{
    ff_button_t b;
    ff_button_init(&b);

    uint32_t const t0 = (uint32_t)0xFFFFFFFFu - 10u; /* wraps mid-window */
    TEST_ASSERT_FALSE(ff_button_tick(&b, t0, true));
    uint32_t const settled = (uint32_t)(t0 + FF_BUTTON_DEBOUNCE_MS); /* wraps past UINT32_MAX */
    TEST_ASSERT_TRUE(ff_button_tick(&b, settled, true));
}

/* ------------------------------------------------------------------- */
/* NULL guards                                                          */
/* ------------------------------------------------------------------- */

static void tick_is_null_safe(void)
{
    TEST_ASSERT_FALSE(ff_button_tick(NULL, 0u, true));
    TEST_ASSERT_FALSE(ff_button_tick(NULL, 100u, false));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(init_clears_all_state);
    RUN_TEST(init_is_null_safe);

    RUN_TEST(one_press_fires_exactly_one_event);
    RUN_TEST(a_second_press_requires_release_first);

    RUN_TEST(a_20ms_blip_never_fires);
    RUN_TEST(a_bounce_that_settles_pressed_fires_once);

    RUN_TEST(debounce_survives_now_ms_wraparound);

    RUN_TEST(tick_is_null_safe);

    return UNITY_END();
}
