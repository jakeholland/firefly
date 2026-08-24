/**
 * test_shell_settings_persist.c — S16 slice e, AC8.
 *
 * "A FF_INTENT_SETTING_SET is persisted: shell closed, re-inited against
 * the same store, value survives" (docs/specs/S16-app-shell.md,
 * acceptance criteria table). The AC's own wording is "the same store",
 * not "a store" — `app/tests/test_intent.c` already covers validation
 * and the persist-on-change-only discipline against an in-memory spy,
 * but a mock round-trip cannot tell "persisted" from "still resident in
 * the mock's RAM copy of itself" apart. This file uses the REAL sim
 * store seam (`targets/sim/store_file.c`, a flat file on disk) so a
 * shell that merely kept the setting in its own memory — never actually
 * calling `ff_settings_save` — fails this test even though it would pass
 * a purely in-process one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "ff_intent.h"
#include "ff_shell.h"
#include "store_file.h"

static char g_path[512];

typedef struct {
    uint32_t t;
} fake_clock_t;

static uint32_t fake_now(void *user)
{
    return ((fake_clock_t *)user)->t;
}

void setUp(void)
{
    char const *tmp_dir = getenv("TMPDIR");
    if (tmp_dir == NULL || tmp_dir[0] == '\0') {
        tmp_dir = "/tmp";
    }
    snprintf(g_path, sizeof(g_path), "%s/ff_shell_settings_persist_test_%d.kv", tmp_dir, (int)getpid());
    remove(g_path);
}

void tearDown(void)
{
    remove(g_path);
}

static void S16_AC8_setting_set_survives_shell_close_and_reinit_against_the_same_store(void)
{
    ff_store_file_t fsio;
    ff_store_t store = ff_store_file_init(&fsio, g_path);

    fake_clock_t clk = {.t = 100000u};
    ff_clock_t clock = {.now_ms = fake_now, .user = &clk};
    fp_pack_t pack;

    /* --- session 1: SET, then close ------------------------------------ */
    {
        ff_shell_t shell;
        ff_shell_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.clock = &clock;
        cfg.store = &store;
        cfg.pack = &pack;

        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell, &cfg));
        TEST_ASSERT_TRUE(ff_shell_settings(&shell)->imperial); /* default, before the change */

        ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
        in.u.setting.id = FF_SETTING_IMPERIAL;
        in.u.setting.v.i = 0; /* false — an actual change from the default */
        ff_shell_intent(&shell, &in);
        TEST_ASSERT_FALSE(ff_shell_settings(&shell)->imperial);

        char const *name = "RILEY";
        ff_intent_t name_in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
        name_in.u.setting.id = FF_SETTING_MY_NAME;
        name_in.u.setting.v.s = name;
        ff_shell_intent(&shell, &name_in);
        TEST_ASSERT_EQUAL_STRING("RILEY", ff_shell_settings(&shell)->my_name);

        ff_shell_close(&shell);
    }

    /* --- session 2: a FRESH shell object, re-inited against the SAME
     * store (same path on disk) — AC8's exact sequence. -------------- */
    {
        ff_shell_t shell2;
        ff_shell_cfg_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.clock = &clock;
        cfg2.store = &store;
        cfg2.pack = &pack;

        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell2, &cfg2));

        TEST_ASSERT_FALSE(ff_shell_settings(&shell2)->imperial);
        TEST_ASSERT_EQUAL_STRING("RILEY", ff_shell_settings(&shell2)->my_name);

        ff_shell_close(&shell2);
    }
}

/* Negative control for the test above: a value that is NEVER set must
 * NOT survive by accident (e.g. a test harness that leaves a stale file
 * around from a previous run) — proves the file this test reads from is
 * actually this run's, not a leftover. */
static void S16_AC8_a_setting_never_sent_does_not_appear_after_reinit(void)
{
    ff_store_file_t fsio;
    ff_store_t store = ff_store_file_init(&fsio, g_path);

    fake_clock_t clk = {.t = 100000u};
    ff_clock_t clock = {.now_ms = fake_now, .user = &clk};
    fp_pack_t pack;

    {
        ff_shell_t shell;
        ff_shell_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.clock = &clock;
        cfg.store = &store;
        cfg.pack = &pack;
        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell, &cfg));
        /* No SETTING_SET at all this session. */
        ff_shell_close(&shell);
    }

    {
        ff_shell_t shell2;
        ff_shell_cfg_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.clock = &clock;
        cfg2.store = &store;
        cfg2.pack = &pack;
        TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&shell2, &cfg2));

        TEST_ASSERT_TRUE(ff_shell_settings(&shell2)->imperial); /* still the default */
        TEST_ASSERT_EQUAL_STRING("", ff_shell_settings(&shell2)->my_name);

        ff_shell_close(&shell2);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_AC8_setting_set_survives_shell_close_and_reinit_against_the_same_store);
    RUN_TEST(S16_AC8_a_setting_never_sent_does_not_appear_after_reinit);

    return UNITY_END();
}
