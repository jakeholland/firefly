/**
 * test_ctl_out_path.c — S13c review fixup (PR #19 finding #2):
 * ctl_out_path.h's screenshot-path confinement.
 *
 * Builds a real temp directory tree per test (mkdtemp), including a
 * symlink escape case — this is exactly the kind of policy where "looks
 * right" and "is right" diverge, so it's tested against a real
 * filesystem, not mocked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"

#include "ctl_out_path.h"

static char g_root_template[] = "/tmp/ff_ctlout_test_XXXXXX";
static char g_root[512];
static char g_root_real[512];

void setUp(void)
{
    char tmpl[sizeof(g_root_template)];
    memcpy(tmpl, g_root_template, sizeof(g_root_template));
    char *made = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(made);
    (void)snprintf(g_root, sizeof(g_root), "%s", made);
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_root(g_root, g_root_real, sizeof(g_root_real)));
}

void tearDown(void)
{
    /* Best-effort recursive-ish cleanup: tests only ever create a
     * shallow, known set of entries directly under g_root. */
    char cmd[1024];
    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    (void)system(cmd); /* NOLINT: test-only, fixed local temp path */
}

/* ---------------------------------------------------------------------
 * ff_ctl_out_resolve_root
 * ------------------------------------------------------------------- */

static void resolve_root_missing_dir_fails(void)
{
    char out[512];
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_root("/no/such/dir/at/all", out, sizeof(out)));
}

static void resolve_root_output_buffer_too_small_fails(void)
{
    char out[2];
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_root(g_root, out, sizeof(out)));
}

/* ---------------------------------------------------------------------
 * ff_ctl_out_resolve_path — the four cases the review explicitly asked
 * for, plus the guard paths around them.
 * ------------------------------------------------------------------- */

static void plain_name_accepted(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_path("shot.png", g_root_real, out, sizeof(out), &err));

    char expected[1100];
    (void)snprintf(expected, sizeof(expected), "%s/shot.png", g_root_real);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void nested_subdir_that_exists_accepted(void)
{
    char sub[600];
    (void)snprintf(sub, sizeof(sub), "%s/sub", g_root_real);
    TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0700));

    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_path("sub/shot.png", g_root_real, out, sizeof(out), &err));

    char expected[1100];
    (void)snprintf(expected, sizeof(expected), "%s/sub/shot.png", g_root_real);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void absolute_path_rejected(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("/etc/passwd.png", g_root_real, out, sizeof(out), &err));
    TEST_ASSERT_NOT_NULL(err);
}

static void absolute_path_within_root_still_rejected(void)
{
    /* Even an absolute path that HAPPENS to already be under the root
     * must be rejected — the contract is "requested is a relative name",
     * not "requested resolves somewhere safe". */
    char out[1024];
    char const *err = NULL;
    char abs_but_in_root[700];
    (void)snprintf(abs_but_in_root, sizeof(abs_but_in_root), "%s/shot.png", g_root_real);
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path(abs_but_in_root, g_root_real, out, sizeof(out), &err));
}

static void dotdot_escape_rejected(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("../escape.png", g_root_real, out, sizeof(out), &err));
    TEST_ASSERT_NOT_NULL(err);
}

static void dotdot_in_middle_of_path_rejected(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("sub/../../escape.png", g_root_real, out, sizeof(out), &err));
}

static void empty_path_rejected(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("", g_root_real, out, sizeof(out), &err));
}

static void trailing_slash_rejected(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("sub/", g_root_real, out, sizeof(out), &err));
}

static void nonexistent_subdir_rejected(void)
{
    /* This module does not create directories — only root_real itself
     * is guaranteed to exist. */
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("never/made/shot.png", g_root_real, out, sizeof(out), &err));
}

/* The headline symlink-escape case: a directory INSIDE the confined root
 * that is actually a symlink pointing somewhere else entirely. No ".."
 * appears anywhere in the request — a lexical-only check would wave this
 * straight through; only resolving and checking the real path catches
 * it. */
static void symlinked_subdir_escape_rejected(void)
{
    char outside[600];
    (void)snprintf(outside, sizeof(outside), "%s_outside", g_root);
    TEST_ASSERT_EQUAL_INT(0, mkdir(outside, 0700));

    char link_path[700];
    (void)snprintf(link_path, sizeof(link_path), "%s/escape_link", g_root_real);
    TEST_ASSERT_EQUAL_INT(0, symlink(outside, link_path));

    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("escape_link/shot.png", g_root_real, out, sizeof(out), &err));
    TEST_ASSERT_NOT_NULL(err);

    (void)unlink(link_path);
    (void)rmdir(outside);
}

/* A symlinked ROOT itself (e.g. --ctl-out points at a symlink) must
 * still work normally for anything genuinely under it — confinement is
 * against root_real (already resolved), not against re-deriving realpath
 * per call, so this just confirms the setUp()-computed g_root_real (which
 * itself went through ff_ctl_out_resolve_root) behaves like any other
 * real directory. */
static void symlinked_root_itself_still_confines_correctly(void)
{
    char link_root[600];
    (void)snprintf(link_root, sizeof(link_root), "%s_linkroot", g_root);
    TEST_ASSERT_EQUAL_INT(0, symlink(g_root, link_root));

    char link_root_real[512];
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_root(link_root, link_root_real, sizeof(link_root_real)));
    TEST_ASSERT_EQUAL_STRING(g_root_real, link_root_real); /* resolves to the same real target */

    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_path("shot.png", link_root_real, out, sizeof(out), &err));

    (void)unlink(link_root);
}

/* The leaf-is-a-symlink gap the directory check alone can't cover: the
 * containing directory resolves fine (it IS the root), but the specific
 * file name being written to is itself a symlink pointing outside. */
static void leaf_that_is_a_symlink_rejected(void)
{
    char outside_file[600];
    (void)snprintf(outside_file, sizeof(outside_file), "%s_outside_file", g_root);
    FILE *f = fopen(outside_file, "w");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    char leaf_link[700];
    (void)snprintf(leaf_link, sizeof(leaf_link), "%s/shot.png", g_root_real);
    TEST_ASSERT_EQUAL_INT(0, symlink(outside_file, leaf_link));

    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_FALSE(ff_ctl_out_resolve_path("shot.png", g_root_real, out, sizeof(out), &err));
    TEST_ASSERT_NOT_NULL(err);

    (void)unlink(leaf_link);
    (void)unlink(outside_file);
}

/* A leaf name that does NOT yet exist (the normal case — screenshot is
 * about to CREATE it) must still be accepted; only an EXISTING symlink
 * at that name is rejected. */
static void nonexistent_leaf_name_is_not_treated_as_a_symlink(void)
{
    char out[1024];
    char const *err = NULL;
    TEST_ASSERT_TRUE(ff_ctl_out_resolve_path("brand_new_name.png", g_root_real, out, sizeof(out), &err));
}

/* ctl_server.c's ctl_err() embeds *err directly into a JSON string value
 * with no escaping of its own (documented, deliberate — see its comment)
 * — every error string this module can return must therefore be
 * quote-free, or it corrupts the ctl socket's own response. Bitten once
 * already elsewhere in this PR's review-fixup round (a swipe-direction
 * error message with embedded quotes); this test collects every
 * rejection reason this module can produce and checks all of them. */
static void error_messages_contain_no_embedded_quotes(void)
{
    char out[1024];
    char const *err = NULL;

    char const *requested[] = {"/etc/passwd.png", "../escape.png", "", "sub/",
                                 "never/made/shot.png"};
    for (size_t i = 0; i < sizeof(requested) / sizeof(requested[0]); i++) {
        err = NULL;
        TEST_ASSERT_FALSE(ff_ctl_out_resolve_path(requested[i], g_root_real, out, sizeof(out), &err));
        TEST_ASSERT_NOT_NULL(err);
        TEST_ASSERT_NULL_MESSAGE(strchr(err, '"'), err);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(resolve_root_missing_dir_fails);
    RUN_TEST(resolve_root_output_buffer_too_small_fails);

    RUN_TEST(plain_name_accepted);
    RUN_TEST(nested_subdir_that_exists_accepted);
    RUN_TEST(absolute_path_rejected);
    RUN_TEST(absolute_path_within_root_still_rejected);
    RUN_TEST(dotdot_escape_rejected);
    RUN_TEST(dotdot_in_middle_of_path_rejected);
    RUN_TEST(empty_path_rejected);
    RUN_TEST(trailing_slash_rejected);
    RUN_TEST(nonexistent_subdir_rejected);
    RUN_TEST(symlinked_subdir_escape_rejected);
    RUN_TEST(symlinked_root_itself_still_confines_correctly);
    RUN_TEST(leaf_that_is_a_symlink_rejected);
    RUN_TEST(nonexistent_leaf_name_is_not_treated_as_a_symlink);
    RUN_TEST(error_messages_contain_no_embedded_quotes);

    return UNITY_END();
}
