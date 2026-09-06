/**
 * test_meshname.c — NAME in Settings: puck-name charset sanitize +
 * Meshtastic short-name derivation (ff_meshname.h).
 */
#include <string.h>

#include "unity.h"

#include "ff_meshname.h"

void setUp(void) {}
void tearDown(void) {}

/* --------------------------------------------------------------------- */
/* ff_meshname_derive_short — the three task-brief examples.             */
/* --------------------------------------------------------------------- */

static void derive_taylor_yields_tayl(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("Taylor", out);
    TEST_ASSERT_EQUAL_STRING("TAYL", out);
}

static void derive_jake_yields_jake(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("Jake", out);
    TEST_ASSERT_EQUAL_STRING("JAKE", out);
}

static void derive_jo_yields_jo_no_fabricated_padding(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("Jo", out);
    TEST_ASSERT_EQUAL_STRING("JO", out);
}

/* Strip-then-truncate, not truncate-then-strip: "A B C D" must yield
 * "ABCD" (padded from the name's next letters), not "AB" (which a naive
 * "take the first 4 raw characters, then strip" would produce, since the
 * two spaces inside the first 4 raw characters would already have eaten
 * two of the four slots). */
static void derive_drops_non_alnum_before_truncating_not_after(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("A B C D", out);
    TEST_ASSERT_EQUAL_STRING("ABCD", out);
}

static void derive_lowercase_input_is_uppercased(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("jake", out);
    TEST_ASSERT_EQUAL_STRING("JAKE", out);
}

static void derive_longer_than_four_truncates_to_four(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("MaximumOverdrive", out);
    TEST_ASSERT_EQUAL_STRING("MAXI", out);
}

static void derive_empty_name_yields_empty_short(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("", out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void derive_null_name_yields_empty_short(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short(NULL, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void derive_no_alnum_at_all_yields_empty_short(void)
{
    char out[FF_MESHNAME_SHORT_LEN];
    ff_meshname_derive_short("!!! ---", out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void derive_null_out_is_a_safe_no_op(void)
{
    ff_meshname_derive_short("Jake", NULL); /* must not crash */
}

/* --------------------------------------------------------------------- */
/* ff_meshname_sanitize — charset A-Z0-9 space, trimmed, bounded.        */
/* --------------------------------------------------------------------- */

static void sanitize_passes_plain_alnum_through(void)
{
    char out[16];
    ff_meshname_sanitize("Jake123", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Jake123", out);
}

static void sanitize_preserves_case(void)
{
    char out[16];
    ff_meshname_sanitize("jAkE", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("jAkE", out);
}

static void sanitize_keeps_interior_spaces(void)
{
    char out[16];
    ff_meshname_sanitize("Jake H", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Jake H", out);
}

static void sanitize_drops_punctuation(void)
{
    char out[16];
    ff_meshname_sanitize("J.a-k!e?", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Jake", out);
}

static void sanitize_trims_leading_and_trailing_spaces(void)
{
    char out[16];
    ff_meshname_sanitize("  Jake  ", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Jake", out);
}

static void sanitize_all_disallowed_yields_empty(void)
{
    char out[16];
    ff_meshname_sanitize("!!!???", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void sanitize_all_whitespace_yields_empty(void)
{
    char out[16];
    ff_meshname_sanitize("     ", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* Truncation counts only KEPT bytes — a disallowed byte dropped along the
 * way must not consume a byte of the output budget. */
static void sanitize_truncation_counts_only_kept_bytes(void)
{
    char out[5]; /* room for 4 chars + NUL */
    ff_meshname_sanitize("J!a!k!e!1!2!3!4!5", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Jake", out);
}

static void sanitize_bounded_to_cap_minus_one(void)
{
    char out[4]; /* room for 3 chars + NUL */
    ff_meshname_sanitize("Taylor", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Tay", out);
    TEST_ASSERT_EQUAL_size_t(3u, strlen(out));
}

static void sanitize_null_in_yields_empty(void)
{
    char out[16];
    ff_meshname_sanitize(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void sanitize_null_out_is_a_safe_no_op(void)
{
    ff_meshname_sanitize("Jake", NULL, 16); /* must not crash */
}

static void sanitize_zero_cap_is_a_safe_no_op(void)
{
    char out[4] = {'X', 'X', 'X', 'X'};
    ff_meshname_sanitize("Jake", out, 0u);
    TEST_ASSERT_EQUAL_CHAR('X', out[0]); /* untouched */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(derive_taylor_yields_tayl);
    RUN_TEST(derive_jake_yields_jake);
    RUN_TEST(derive_jo_yields_jo_no_fabricated_padding);
    RUN_TEST(derive_drops_non_alnum_before_truncating_not_after);
    RUN_TEST(derive_lowercase_input_is_uppercased);
    RUN_TEST(derive_longer_than_four_truncates_to_four);
    RUN_TEST(derive_empty_name_yields_empty_short);
    RUN_TEST(derive_null_name_yields_empty_short);
    RUN_TEST(derive_no_alnum_at_all_yields_empty_short);
    RUN_TEST(derive_null_out_is_a_safe_no_op);

    RUN_TEST(sanitize_passes_plain_alnum_through);
    RUN_TEST(sanitize_preserves_case);
    RUN_TEST(sanitize_keeps_interior_spaces);
    RUN_TEST(sanitize_drops_punctuation);
    RUN_TEST(sanitize_trims_leading_and_trailing_spaces);
    RUN_TEST(sanitize_all_disallowed_yields_empty);
    RUN_TEST(sanitize_all_whitespace_yields_empty);
    RUN_TEST(sanitize_truncation_counts_only_kept_bytes);
    RUN_TEST(sanitize_bounded_to_cap_minus_one);
    RUN_TEST(sanitize_null_in_yields_empty);
    RUN_TEST(sanitize_null_out_is_a_safe_no_op);
    RUN_TEST(sanitize_zero_cap_is_a_safe_no_op);
    return UNITY_END();
}
