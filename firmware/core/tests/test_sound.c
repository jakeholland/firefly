/**
 * test_sound.c — S27 core: the sound event vocabulary, tone patterns,
 * quiet-hours policy, and priority table (docs/specs/S27-sounds.md).
 *
 * Covers:
 *   - every pattern stays within the step/duration budgets (a RUNTIME
 *     sweep of the public API, on top of ff_sound.c's own compile-time
 *     `_Static_assert`s — the header promises both).
 *   - ff_sound_pattern_for's out-of-range rejection.
 *   - ff_sound_should_play's policy table: sounds off silences
 *     everything; quiet hours exempts ONLY FLARE_SENT/FLARE_INCOMING;
 *     otherwise every event plays.
 *   - ff_sound_priority / ff_sound_preempts: FLARE_INCOMING preempts
 *     anything (including a second FLARE_INCOMING); a MESSAGE never
 *     interrupts a FLARE_*.
 */
#include "unity.h"

#include "ff_sound.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Pattern table budgets (runtime re-verification of the header's
 * compile-time guards — see ff_sound.h's own comment on why both exist).
 * ------------------------------------------------------------------- */

static void S27_every_pattern_stays_within_step_and_duration_budget(void)
{
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        ff_sound_pattern_t const *p = ff_sound_pattern_for((ff_sound_event_t)e);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(FF_SOUND_PATTERN_MAX_STEPS, p->n);
        TEST_ASSERT_GREATER_THAN_UINT8(0, p->n); /* every real event has SOME pattern */

        uint32_t total_ms = 0;
        for (uint8_t i = 0; i < p->n; i++) {
            total_ms += p->steps[i].ms;
        }
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(FF_SOUND_PATTERN_MAX_MS, total_ms);
    }
}

/* FLARE_SENT: documented as C5-E5-G5, a rising 3-note. Pinned by name so
 * a future edit to the table is a deliberate, reviewed change to what
 * the spec says the sender hears, not a silent drift. */
static void S27_flare_sent_is_the_documented_rising_triad(void)
{
    ff_sound_pattern_t const *p = ff_sound_pattern_for(FF_SOUND_FLARE_SENT);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(3, p->n);
    TEST_ASSERT_EQUAL_UINT16(FF_SOUND_NOTE_C5, p->steps[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(FF_SOUND_NOTE_E5, p->steps[1].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(FF_SOUND_NOTE_G5, p->steps[2].freq_hz);
}

/* FLARE_INCOMING: "repeated twice" (spec) — the same non-rest note pair
 * must appear twice in the pattern, separated by a rest (freq_hz == 0). */
static void S27_flare_incoming_repeats_its_phrase_twice_with_a_rest_between(void)
{
    ff_sound_pattern_t const *p = ff_sound_pattern_for(FF_SOUND_FLARE_INCOMING);
    TEST_ASSERT_NOT_NULL(p);

    int rest_idx = -1;
    for (uint8_t i = 0; i < p->n; i++) {
        if (p->steps[i].freq_hz == 0) {
            rest_idx = (int)i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, rest_idx, "FLARE_INCOMING has no rest step separating its repeats");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, rest_idx, "the rest must not be the very first step (nothing to repeat)");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(p->n - 1, rest_idx, "the rest must not be the very last step (nothing after it)");

    /* The phrase before the rest and the phrase after it are identical
     * (freq_hz-for-freq_hz) — the actual "repeated twice" claim. */
    int const before_len = rest_idx;
    int const after_len = p->n - rest_idx - 1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(before_len, after_len, "the repeated phrase is not the same length before/after the rest");
    for (int i = 0; i < before_len; i++) {
        TEST_ASSERT_EQUAL_UINT16(p->steps[i].freq_hz, p->steps[rest_idx + 1 + i].freq_hz);
    }
}

/* MESSAGE: "one soft blip" — a single step. */
static void S27_message_is_one_blip(void)
{
    ff_sound_pattern_t const *p = ff_sound_pattern_for(FF_SOUND_MESSAGE);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(1, p->n);
}

/* RALLY: "two-note". */
static void S27_rally_is_two_notes(void)
{
    ff_sound_pattern_t const *p = ff_sound_pattern_for(FF_SOUND_RALLY);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(2, p->n);
}

/* BATT_LOW: "descending two-note" — the second note's frequency must be
 * LOWER than the first's. */
static void S27_batt_low_is_a_descending_two_note(void)
{
    ff_sound_pattern_t const *p = ff_sound_pattern_for(FF_SOUND_BATT_LOW);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(2, p->n);
    TEST_ASSERT_LESS_THAN_UINT16(p->steps[0].freq_hz, p->steps[1].freq_hz);
}

static void S27_pattern_for_rejects_out_of_range(void)
{
    TEST_ASSERT_NULL(ff_sound_pattern_for((ff_sound_event_t)-1));
    TEST_ASSERT_NULL(ff_sound_pattern_for(FF_SOUND_COUNT));
    TEST_ASSERT_NULL(ff_sound_pattern_for((ff_sound_event_t)(FF_SOUND_COUNT + 5)));
}

/* ---------------------------------------------------------------------
 * ff_sound_should_play — the policy table.
 * ------------------------------------------------------------------- */

static void S27_sounds_off_silences_every_event_even_flare_incoming(void)
{
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        TEST_ASSERT_FALSE(ff_sound_should_play((ff_sound_event_t)e, /*sounds_on*/ false, /*quiet_now*/ false));
        TEST_ASSERT_FALSE(ff_sound_should_play((ff_sound_event_t)e, /*sounds_on*/ false, /*quiet_now*/ true));
    }
}

static void S27_sounds_on_not_quiet_plays_every_event(void)
{
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        TEST_ASSERT_TRUE(ff_sound_should_play((ff_sound_event_t)e, /*sounds_on*/ true, /*quiet_now*/ false));
    }
}

/* The mutation target (task's mutation (a)): quiet hours must exempt
 * ONLY FLARE_SENT/FLARE_INCOMING - every other event must be silent. */
static void S27_quiet_hours_exempts_only_the_two_flare_events(void)
{
    TEST_ASSERT_TRUE(ff_sound_should_play(FF_SOUND_FLARE_SENT, true, true));
    TEST_ASSERT_TRUE(ff_sound_should_play(FF_SOUND_FLARE_INCOMING, true, true));

    TEST_ASSERT_FALSE(ff_sound_should_play(FF_SOUND_MESSAGE, true, true));
    TEST_ASSERT_FALSE(ff_sound_should_play(FF_SOUND_RALLY, true, true));
    TEST_ASSERT_FALSE(ff_sound_should_play(FF_SOUND_BATT_LOW, true, true));
    TEST_ASSERT_FALSE(ff_sound_should_play(FF_SOUND_TAP, true, true));
}

static void S27_should_play_rejects_out_of_range(void)
{
    TEST_ASSERT_FALSE(ff_sound_should_play((ff_sound_event_t)-1, true, false));
    TEST_ASSERT_FALSE(ff_sound_should_play(FF_SOUND_COUNT, true, false));
}

/* ---------------------------------------------------------------------
 * ff_sound_priority / ff_sound_preempts.
 * ------------------------------------------------------------------- */

static void S27_flare_incoming_is_the_highest_priority(void)
{
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        if (e == FF_SOUND_FLARE_INCOMING) continue;
        TEST_ASSERT_GREATER_THAN_INT(ff_sound_priority((ff_sound_event_t)e), ff_sound_priority(FF_SOUND_FLARE_INCOMING));
    }
}

static void S27_flare_incoming_preempts_anything_including_itself(void)
{
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        TEST_ASSERT_TRUE_MESSAGE(ff_sound_preempts(FF_SOUND_FLARE_INCOMING, (ff_sound_event_t)e),
                                 "FLARE_INCOMING must preempt every currently-playing event, spec: "
                                 "'preempts anything'");
    }
}

/* The mutation target's twin (spec's explicit rule, restated as a
 * priority-table property): a MESSAGE never interrupts a FLARE_*. */
static void S27_message_never_preempts_a_flare_event(void)
{
    TEST_ASSERT_FALSE(ff_sound_preempts(FF_SOUND_MESSAGE, FF_SOUND_FLARE_SENT));
    TEST_ASSERT_FALSE(ff_sound_preempts(FF_SOUND_MESSAGE, FF_SOUND_FLARE_INCOMING));
}

static void S27_same_tier_preempts_itself(void)
{
    /* priority(x) >= priority(x) is always true: a fresh instance of the
     * same event is allowed to restart the pattern rather than being
     * silently dropped mid-play. */
    for (int e = 0; e < FF_SOUND_COUNT; e++) {
        TEST_ASSERT_TRUE(ff_sound_preempts((ff_sound_event_t)e, (ff_sound_event_t)e));
    }
}

static void S27_priority_out_of_range_is_the_lowest_tier(void)
{
    TEST_ASSERT_EQUAL_INT(0, ff_sound_priority((ff_sound_event_t)-1));
    TEST_ASSERT_EQUAL_INT(0, ff_sound_priority(FF_SOUND_COUNT));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S27_every_pattern_stays_within_step_and_duration_budget);
    RUN_TEST(S27_flare_sent_is_the_documented_rising_triad);
    RUN_TEST(S27_flare_incoming_repeats_its_phrase_twice_with_a_rest_between);
    RUN_TEST(S27_message_is_one_blip);
    RUN_TEST(S27_rally_is_two_notes);
    RUN_TEST(S27_batt_low_is_a_descending_two_note);
    RUN_TEST(S27_pattern_for_rejects_out_of_range);

    RUN_TEST(S27_sounds_off_silences_every_event_even_flare_incoming);
    RUN_TEST(S27_sounds_on_not_quiet_plays_every_event);
    RUN_TEST(S27_quiet_hours_exempts_only_the_two_flare_events);
    RUN_TEST(S27_should_play_rejects_out_of_range);

    RUN_TEST(S27_flare_incoming_is_the_highest_priority);
    RUN_TEST(S27_flare_incoming_preempts_anything_including_itself);
    RUN_TEST(S27_message_never_preempts_a_flare_event);
    RUN_TEST(S27_same_tier_preempts_itself);
    RUN_TEST(S27_priority_out_of_range_is_the_lowest_tier);

    return UNITY_END();
}
