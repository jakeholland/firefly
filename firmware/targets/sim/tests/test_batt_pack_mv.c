/**
 * test_batt_pack_mv.c — S25 slice c, review fix: HOST-side pin for the
 * pure pin-mV -> pack-mV conversion arithmetic
 * (`firmware/targets/esp32s3/components/ff_power/include/
 * ff_power_batt_conv.h`), hoisted out of `ff_power.c` specifically so
 * this can run as an ordinary `ctest` — no ADC, no `idf.py build`,
 * nothing ESP-specific (the header includes only `<stdint.h>`).
 *
 * A prior revision of `ff_power.c` shipped
 * `FF_BATT_PACK_MV_PER_CAL_MV_X1E6` as `3028835` — a truncation of
 * `3.0 / 0.990476 * 1e6` (the correct value, at full double precision,
 * is `3028847`; `3028835` is one part in ~33000 low, silently under-
 * reporting every pack-voltage reading by a few mV). Nothing in the
 * device build could have caught that: it compiles, links, and produces
 * a plausible-looking (just slightly wrong) number. This file exists so
 * the exact constant is pinned by literal, on the host, in the normal
 * gate.
 *
 * Expected values below are computed independently (Python, exact
 * integer arithmetic) against the CORRECT constant (3028847), not
 * copied from `ff_power_batt_conv.h`'s own implementation — the whole
 * point is that a wrong constant in that header must fail these
 * literals, not agree with them.
 */
#include "unity.h"

#include "ff_power_batt_conv.h"

void setUp(void) {}
void tearDown(void) {}

/* cal_mv=1300 -> exactly 3938 (independently: (1300*3028847+500000)/1e6
 * = 3938.0011 -> 3938 after integer division). An ordinary, realistic
 * pin reading (corresponds to a ~3.9V pack — comfortably mid-range). */
static void cal_1300_gives_exact_pack_mv(void)
{
    TEST_ASSERT_EQUAL_UINT16(3938u, ff_power_batt_pack_mv_from_cal_mv(1300u));
    TEST_ASSERT_EQUAL_UINT64(3938u, ff_power_batt_pack_mv_unclamped(1300u));
}

/* cal_mv=0 -> 0. Not the "no reading" sentinel here (that's
 * ff_power_batt_mv's own early-return on s_batt_adc/s_batt_cali being
 * NULL, one layer up) — this is just the honest arithmetic result of
 * converting a zero reading, which also happens to compose correctly
 * with ff_shell_set_batt_mv's own "pack_mv == 0 means unknown"
 * convention one layer up from THAT. */
static void cal_0_gives_0(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, ff_power_batt_pack_mv_from_cal_mv(0u));
    TEST_ASSERT_EQUAL_UINT64(0u, ff_power_batt_pack_mv_unclamped(0u));
}

/* The uint16_t clamp: ff_power_batt_pack_mv_unclamped(cal) crosses
 * UINT16_MAX between cal=21636 (unclamped 65532, still representable)
 * and cal=21637 (unclamped 65535 — coincidentally lands exactly on the
 * ceiling here, computed independently, not chosen to be exact). Both
 * sides pin the clamp is applied exactly where the arithmetic crosses
 * it, not early or late; a clearly-over-range cal (100000, unclamped
 * 302885) pins the clamp actually holds well past the boundary too. */
static void clamps_to_uint16_max(void)
{
    TEST_ASSERT_EQUAL_UINT64(65532u, ff_power_batt_pack_mv_unclamped(21636u));
    TEST_ASSERT_EQUAL_UINT16(65532u, ff_power_batt_pack_mv_from_cal_mv(21636u));

    TEST_ASSERT_EQUAL_UINT64(65535u, ff_power_batt_pack_mv_unclamped(21637u));
    TEST_ASSERT_EQUAL_UINT16(65535u, ff_power_batt_pack_mv_from_cal_mv(21637u));

    TEST_ASSERT_EQUAL_UINT64(302885u, ff_power_batt_pack_mv_unclamped(100000u));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ff_power_batt_pack_mv_from_cal_mv(100000u));
}

/* Round-half-up AT an exact tie. Solving `cal * 3028847 mod 1e6 ==
 * 500000` (the true fractional result is exactly X.5) for the smallest
 * non-negative cal: the constant and 1e6 are coprime (3028847 is odd,
 * not a multiple of 5), so solutions are spaced exactly 1e6 apart, and
 * the smallest one is cal_mv == 500000 itself — verified independently
 * via the modular inverse, not guessed. That is far outside any
 * physically plausible pin reading (ff_power_batt_pack_mv_from_cal_mv
 * does no range validation of its own — see that function's doc
 * comment) AND past UINT16_MAX, which is exactly WHY this test uses the
 * UNCLAMPED helper: at cal_mv=500000 the true value is exactly
 * 1514423.5, and floor (wrong) vs round-half-up (correct, what this
 * file's implementation does) disagree by exactly 1 — 1514423 vs
 * 1514424 — a difference the clamped function alone could never expose
 * (both floor and round-up clamp to the same 65535). */
static void rounds_half_up_at_an_exact_tie(void)
{
    /* Confirm the tie itself, independent of the function under test:
     * cal_mv * CONST must be exactly N*1e6 + 500000 (a true X.5), not
     * merely close to one. */
    uint64_t const product = (uint64_t)500000u * FF_BATT_PACK_MV_PER_CAL_MV_X1E6;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(500000ULL, product % 1000000ULL,
                                      "cal_mv=500000 must land on an exact .5 — this test's premise, not a fuzzy approximation");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1514423ULL, product / 1000000ULL,
                                      "the true value's integer part (floor) must be 1514423, i.e. true value == 1514423.5");

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1514424u, ff_power_batt_pack_mv_unclamped(500000u),
                                      "exact .5 tie must round UP (1514423.5 -> 1514424), not floor to 1514423");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(cal_1300_gives_exact_pack_mv);
    RUN_TEST(cal_0_gives_0);
    RUN_TEST(clamps_to_uint16_max);
    RUN_TEST(rounds_half_up_at_an_exact_tie);

    return UNITY_END();
}
