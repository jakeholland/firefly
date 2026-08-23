#include <string.h>

#include "unity.h"

#include "ff_version.h"

void setUp(void) {}
void tearDown(void) {}

static void S13_AC1_version_string_is_non_null_and_nonempty(void)
{
    const char *v = ff_version_string();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(strlen(v) > 0);
}

static void S13_AC1_version_string_is_stable_across_calls(void)
{
    TEST_ASSERT_EQUAL_STRING(ff_version_string(), ff_version_string());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S13_AC1_version_string_is_non_null_and_nonempty);
    RUN_TEST(S13_AC1_version_string_is_stable_across_calls);
    return UNITY_END();
}
