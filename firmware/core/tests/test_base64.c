/**
 * test_base64.c — Unity coverage for ff_base64.h/.c (2026-09-09
 * amendment, fix/s31-beat-real-audio).
 */
#include "ff_base64.h"

#include <stdbool.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* RFC 4648's own test vectors ("" / "f" / "fo" / "foo" / "foob" /
 * "fooba" / "foobar") — the standard proxy-resistant check for a base64
 * encoder: every padding case (0/1/2 remainder bytes) appears exactly
 * once across this set. */
static void rfc4648_test_vectors(void)
{
    struct {
        char const *plain;
        char const *want;
    } const cases[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t const n = strlen(cases[i].plain);
        char out[32];
        TEST_ASSERT_TRUE_MESSAGE(
            ff_base64_encode((uint8_t const *)cases[i].plain, n, out, sizeof(out)), cases[i].plain);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].want, out, cases[i].plain);
        TEST_ASSERT_EQUAL_UINT32(strlen(cases[i].want), ff_base64_encoded_len(n));
    }
}

static void binary_data_round_trips_length_correctly(void)
{
    uint8_t bin[320];
    for (size_t i = 0; i < sizeof(bin); i++) bin[i] = (uint8_t)(i * 7u + 3u); /* arbitrary, non-ASCII-safe bytes */

    size_t const want_len = ff_base64_encoded_len(sizeof(bin));
    TEST_ASSERT_EQUAL_UINT32(428u, (uint32_t)want_len); /* 4*ceil(320/3) = 4*107 = 428 */

    char out[512];
    TEST_ASSERT_TRUE(ff_base64_encode(bin, sizeof(bin), out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(want_len, (uint32_t)strlen(out));
    /* Every character is in the standard alphabet or padding. */
    for (size_t i = 0; i < strlen(out); i++) {
        char const c = out[i];
        bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                         c == '/' || c == '=';
        TEST_ASSERT_TRUE_MESSAGE(ok, "non-base64 character in output");
    }
}

static void too_small_buffer_is_rejected_not_truncated(void)
{
    uint8_t const data[3] = {1, 2, 3};
    char out[4] = {'X', 'X', 'X', 'X'}; /* needs 5 (4 chars + NUL); this is deliberately 1 short */
    TEST_ASSERT_FALSE(ff_base64_encode(data, sizeof(data), out, sizeof(out)));
    /* Must not partially fill on failure — still exactly the poison
     * bytes this test seeded, never a truncated-but-plausible encoding. */
    TEST_ASSERT_EQUAL_CHAR('X', out[0]);
}

static void null_and_empty_are_safe_and_honest(void)
{
    char out[8];
    TEST_ASSERT_FALSE(ff_base64_encode(NULL, 3u, out, sizeof(out))); /* in==NULL, n>0 */
    TEST_ASSERT_FALSE(ff_base64_encode((uint8_t const *)"x", 1u, NULL, 8u)); /* out==NULL */

    TEST_ASSERT_TRUE(ff_base64_encode(NULL, 0u, out, sizeof(out))); /* n==0, in may be NULL */
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(rfc4648_test_vectors);
    RUN_TEST(binary_data_round_trips_length_correctly);
    RUN_TEST(too_small_buffer_is_rejected_not_truncated);
    RUN_TEST(null_and_empty_are_safe_and_honest);
    return UNITY_END();
}
