/**
 * ctl_out_path.c — see ctl_out_path.h.
 */
#include "ctl_out_path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096 /* POSIX guarantees this exists, but fall back just in case */
#endif

/* True iff `requested` is non-empty, doesn't start with '/', doesn't end
 * in '/', and none of its '/'-separated components is exactly "..".
 * Purely lexical — does NOT confirm anything about where it resolves on
 * disk (that's ff_ctl_out_resolve_path's realpath()-based check, the
 * only thing that can actually catch a symlink escape). */
static bool ctl_out_path_shape_is_safe(char const *requested)
{
    if (requested == NULL || requested[0] == '\0') return false;
    if (requested[0] == '/') return false;
    size_t len = strlen(requested);
    if (requested[len - 1] == '/') return false;

    char buf[PATH_MAX];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, requested, len + 1);

    char *saveptr = NULL;
    char *tok = strtok_r(buf, "/", &saveptr);
    while (tok != NULL) {
        if (strcmp(tok, "..") == 0) return false;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return true;
}

bool ff_ctl_out_resolve_root(char const *root, char *out, size_t out_sz)
{
    if (root == NULL || out == NULL || out_sz == 0) return false;

    char resolved[PATH_MAX];
    if (realpath(root, resolved) == NULL) return false;

    size_t n = strlen(resolved);
    if (n >= out_sz) return false;
    memcpy(out, resolved, n + 1);
    return true;
}

bool ff_ctl_out_resolve_path(char const *requested, char const *root_real, char *out, size_t out_sz,
                              char const **err)
{
    /* NOTE: no embedded '"' in any of these — ctl_server.c's ctl_err()
     * inserts error messages into a JSON string value with no escaping
     * of its own (documented assumption: every message it's given is a
     * static, quote-free literal). An earlier version of this string had
     * literal quotes around ".." and corrupted the JSON response. */
    static char const *const err_shape =
        "screenshot path must be a non-empty relative path with no .. components";
    static char const *const err_no_dir = "screenshot directory does not exist under the configured output root";
    static char const *const err_escape = "screenshot path escapes the configured output root (--ctl-out)";
    static char const *const err_symlink = "refusing to write through an existing symlink";
    static char const *const err_too_long = "screenshot path too long";

    if (root_real == NULL || out == NULL || out_sz == 0 || err == NULL) return false;

    if (!ctl_out_path_shape_is_safe(requested)) {
        *err = err_shape;
        return false;
    }

    char candidate[PATH_MAX];
    int cn = snprintf(candidate, sizeof(candidate), "%s/%s", root_real, requested);
    if (cn < 0 || (size_t)cn >= sizeof(candidate)) {
        *err = err_too_long;
        return false;
    }

    /* Split into directory + leaf: the leaf file may not exist yet
     * (that's exactly what the caller is about to create), but
     * realpath() requires every component up to the last to exist —
     * resolve the directory part, verify THAT is confined, then
     * re-attach the leaf name verbatim. `candidate` always contains at
     * least the '/' between root_real and requested, so strrchr never
     * returns NULL here. */
    char *slash = strrchr(candidate, '/');
    *slash = '\0';
    char const *leaf = slash + 1;

    char resolved_dir[PATH_MAX];
    if (realpath(candidate, resolved_dir) == NULL) {
        *err = err_no_dir;
        return false;
    }

    size_t root_len = strlen(root_real);
    size_t dir_len = strlen(resolved_dir);
    bool confined = (dir_len == root_len && memcmp(resolved_dir, root_real, root_len) == 0) ||
                     (dir_len > root_len && memcmp(resolved_dir, root_real, root_len) == 0 &&
                      resolved_dir[root_len] == '/');
    if (!confined) {
        *err = err_escape;
        return false;
    }

    int on = snprintf(out, out_sz, "%s/%s", resolved_dir, leaf);
    if (on < 0 || (size_t)on >= out_sz) {
        *err = err_too_long;
        return false;
    }

    /* Directory-level confinement (above) resolves symlinks anywhere in
     * the directory path, but can't protect against the leaf name
     * ITSELF already being a symlink pointing outside the confined
     * directory — check that separately. lstat (not stat) so this
     * inspects the link, not its target. */
    struct stat st;
    if (lstat(out, &st) == 0 && S_ISLNK(st.st_mode)) {
        *err = err_symlink;
        return false;
    }

    return true;
}
