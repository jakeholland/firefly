/**
 * test_power_latch_seq.c — S25 latch-hold amendment (2026-09-16 field
 * report): HOST-side pin for the pure ON/OFF ordering
 * (`firmware/targets/esp32s3/components/ff_power/include/
 * ff_power_latch_seq.h`), the same "hoist the pure part, test it here"
 * shape `test_batt_pack_mv.c` already established for that component's
 * battery-conversion math — no gpio.h, no idf.py build.
 *
 * A HAL-mocked recording of every call the sequence makes, asserting
 * both WHICH calls happen and their ORDER — the whole point of this
 * fix (see ff_power_latch_seq.h's top comment): a latch-ON that enables
 * the hold before setting the level, or a power-OFF that drives the
 * pin low before releasing the hold, both "work" in the sense that no
 * gpio call fails, and only a battery bench test (or this file) can
 * tell the difference.
 */
#include <string.h>

#include "unity.h"

#include "ff_power_latch_seq.h"

#define REC_CAP 8

typedef struct {
    char ops[REC_CAP][16];
    int levels[REC_CAP];
    int count;
    int fail_on_op_index; /* -1 = never fail; else make that call (0-based, in RECORDED order) return nonzero */
} rec_t;

static rec_t s_rec;

static void rec_reset(int fail_on_op_index)
{
    s_rec.count = 0;
    s_rec.fail_on_op_index = fail_on_op_index;
}

static int rec_push(char const *name, int level)
{
    TEST_ASSERT_LESS_THAN(REC_CAP, s_rec.count);
    strncpy(s_rec.ops[s_rec.count], name, sizeof(s_rec.ops[s_rec.count]) - 1);
    s_rec.ops[s_rec.count][sizeof(s_rec.ops[s_rec.count]) - 1] = '\0';
    s_rec.levels[s_rec.count] = level;
    int const idx = s_rec.count;
    s_rec.count++;
    return (idx == s_rec.fail_on_op_index) ? -1 : 0;
}

static int mock_set_level(void *io, int level)
{
    (void)io;
    return rec_push("set_level", level);
}

static int mock_hold_en(void *io)
{
    (void)io;
    return rec_push("hold_en", -1);
}

static int mock_hold_dis(void *io)
{
    (void)io;
    return rec_push("hold_dis", -1);
}

static ff_power_latch_hal_t mock_hal(void)
{
    ff_power_latch_hal_t hal;
    hal.io = NULL;
    hal.set_level = mock_set_level;
    hal.hold_en = mock_hold_en;
    hal.hold_dis = mock_hold_dis;
    return hal;
}

void setUp(void) {}
void tearDown(void) {}

/* latch-ON: set_level(1) THEN hold_en, in that order, nothing else. */
static void test_latch_on_sets_high_then_holds(void)
{
    rec_reset(-1);
    ff_power_latch_hal_t hal = mock_hal();

    int const err = ff_power_latch_seq_on(&hal);

    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_INT(2, s_rec.count);
    TEST_ASSERT_EQUAL_STRING("set_level", s_rec.ops[0]);
    TEST_ASSERT_EQUAL_INT(1, s_rec.levels[0]);
    TEST_ASSERT_EQUAL_STRING("hold_en", s_rec.ops[1]);
}

/* latch-ON: if set_level fails, hold_en must never be called — there is
 * nothing correct to hold. */
static void test_latch_on_stops_if_set_level_fails(void)
{
    rec_reset(0); /* the 0th recorded call (set_level) fails */
    ff_power_latch_hal_t hal = mock_hal();

    int const err = ff_power_latch_seq_on(&hal);

    TEST_ASSERT_EQUAL_INT(-1, err);
    TEST_ASSERT_EQUAL_INT(1, s_rec.count); /* hold_en never called */
    TEST_ASSERT_EQUAL_STRING("set_level", s_rec.ops[0]);
}

/* power-OFF: hold_dis THEN set_level(0), in that order — the fix's own
 * property (docs/specs/S25-power-latch.md's Amendments): getting this
 * backwards would drive the pin low while it is still held, which
 * ESP-IDF's own gpio_hold_dis() doc note says is a silent no-op ("the
 * gpio will output the default level if this function is called" —
 * nothing changes the pad until hold is released), so the bug is
 * exactly "power-off returns ESP_OK and the rail never drops." */
static void test_power_off_releases_hold_then_sets_low(void)
{
    rec_reset(-1);
    ff_power_latch_hal_t hal = mock_hal();

    int const err = ff_power_latch_seq_off(&hal);

    TEST_ASSERT_EQUAL_INT(0, err);
    TEST_ASSERT_EQUAL_INT(2, s_rec.count);
    TEST_ASSERT_EQUAL_STRING("hold_dis", s_rec.ops[0]);
    TEST_ASSERT_EQUAL_STRING("set_level", s_rec.ops[1]);
    TEST_ASSERT_EQUAL_INT(0, s_rec.levels[1]);
}

/* power-OFF: if hold_dis fails, set_level must never be called — this
 * is the proxy-check discipline AGENTS.md's standing brief calls out
 * (measure the property, don't just assert a success path): a mutant
 * that swaps ff_power_latch_seq_off's two calls would still pass
 * test_power_off_releases_hold_then_sets_low's *order* assertions only
 * if hold_dis always succeeds — this test additionally proves set_level
 * is conditioned on hold_dis's result, not merely sequenced after it in
 * source order. */
static void test_power_off_stops_if_hold_dis_fails(void)
{
    rec_reset(0); /* the 0th recorded call (hold_dis) fails */
    ff_power_latch_hal_t hal = mock_hal();

    int const err = ff_power_latch_seq_off(&hal);

    TEST_ASSERT_EQUAL_INT(-1, err);
    TEST_ASSERT_EQUAL_INT(1, s_rec.count); /* set_level never called */
    TEST_ASSERT_EQUAL_STRING("hold_dis", s_rec.ops[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_latch_on_sets_high_then_holds);
    RUN_TEST(test_latch_on_stops_if_set_level_fails);
    RUN_TEST(test_power_off_releases_hold_then_sets_low);
    RUN_TEST(test_power_off_stops_if_hold_dis_fails);
    return UNITY_END();
}
