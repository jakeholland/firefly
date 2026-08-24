/**
 * test_screenshot.c — ff_screenshot_write's output-directory creation
 * (issue #3: `ffsim --headless --screenshot DIR` must create DIR itself
 * rather than failing when it doesn't exist yet).
 *
 * Builds a real temp directory per test (mkdtemp, same pattern as
 * test_ctl_out_path.c) and drives the real writer with a tiny 2x2
 * XRGB8888 buffer. Mutation check: delete the mkdir-parents call inside
 * ff_screenshot_write and `creates_missing_directories` fails (the PNG
 * write has nowhere to land, rc becomes 1) — the fix can't silently
 * regress to the old "works after mkdir" behavior.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* mkdtemp (same pattern as test_ctl_out_path.c) */

#include "unity.h"

#include "screenshot.h"

static char g_root[512];

/* 2x2 XRGB8888 (B,G,R,X per pixel) — content is irrelevant, only that
 * stbi gets a valid buffer to encode. */
static uint8_t const g_px[2 * 2 * 4] = {
    0x10, 0x20, 0x30, 0x00, 0x40, 0x50, 0x60, 0x00,
    0x70, 0x80, 0x90, 0x00, 0xa0, 0xb0, 0xc0, 0x00,
};

void setUp(void)
{
    char tmpl[] = "/tmp/ff_shot_test_XXXXXX";
    char *made = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(made);
    (void)snprintf(g_root, sizeof(g_root), "%s", made);
}

void tearDown(void)
{
    char cmd[600];
    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    (void)system(cmd);
}

static bool file_exists(char const *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

static void writes_into_an_existing_directory(void)
{
    char path[600];
    (void)snprintf(path, sizeof(path), "%s/shot.png", g_root);
    TEST_ASSERT_EQUAL_INT(0, ff_screenshot_write(path, g_px, 2, 2));
    TEST_ASSERT_TRUE(file_exists(path));
}

static void creates_missing_directories(void)
{
    /* The issue #3 repro: the output directory does not exist — and to
     * pin the "-p", neither does its parent. */
    char path[600];
    (void)snprintf(path, sizeof(path), "%s/missing/nested/shot.png", g_root);
    TEST_ASSERT_EQUAL_INT(0, ff_screenshot_write(path, g_px, 2, 2));
    TEST_ASSERT_TRUE(file_exists(path));
}

static void uncreatable_directory_fails_loud(void)
{
    /* A regular FILE squatting on a directory component: mkdir says
     * EEXIST (tolerated), then the PNG write itself fails — rc must be
     * 1, never a silent success with the PNG lost. */
    char blocker[600];
    (void)snprintf(blocker, sizeof(blocker), "%s/blocker", g_root);
    FILE *f = fopen(blocker, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    char path[600];
    (void)snprintf(path, sizeof(path), "%s/blocker/shot.png", g_root);
    TEST_ASSERT_EQUAL_INT(1, ff_screenshot_write(path, g_px, 2, 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(writes_into_an_existing_directory);
    RUN_TEST(creates_missing_directories);
    RUN_TEST(uncreatable_directory_fails_loud);
    return UNITY_END();
}
