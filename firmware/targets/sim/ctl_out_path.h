/**
 * ctl_out_path.h — S13c review fixup (PR #19 finding #2): confines the
 * ctl socket's `{"cmd":"screenshot","path":...}` writes to a single
 * configured output root instead of accepting an arbitrary filesystem
 * path.
 *
 * ## Background
 * The original implementation passed the ctl socket's `path` field
 * straight to `stbi_write_png()` with only a length check — reachable
 * empirically as an arbitrary-file-write primitive:
 * `{"cmd":"screenshot","path":"/tmp/anything.png"}` wrote exactly there.
 * Loopback-only binding limits this to other local processes/users, but
 * that's still a real primitive for a tool that runs unattended in CI.
 *
 * ## Ruling (PR #19 review)
 * Confine writes to a single configured output root (`ffsim --ctl-out
 * DIR`, default a fresh temp directory, or the `--screenshot` dir if one
 * was also given — see main.c). The ctl socket's `path` is treated as a
 * RELATIVE name under that root:
 *   - an absolute `path` is rejected,
 *   - any `..` path component is rejected (wherever it appears — this
 *     is a structural check on the request string, independent of
 *     whether the escape would actually resolve outside the root),
 *   - the path's containing directory is resolved (`realpath()`) and
 *     verified to be the root itself or a real subdirectory of it —
 *     this is what actually defeats a symlink planted inside the root
 *     that points outside it (a purely lexical `..`-component check
 *     can't catch that: `root/link/x.png` has no `..` in it at all, but
 *     `link` -> `/etc` makes it write outside the root anyway),
 *   - a leaf name that already exists as a symlink is rejected too
 *     (closes the narrower gap the directory-level check alone doesn't
 *     cover: the directory itself is fine, but the specific file name
 *     the request wants to (over)write is itself a symlink pointing
 *     elsewhere within — or even outside, if it manages to point past a
 *     directory the first check couldn't see — the confined tree).
 *
 * This module is the pure, unit-testable core of that policy (no
 * sockets, no LVGL) — see targets/sim/tests/test_ctl_out_path.c.
 */
#ifndef FF_CTL_OUT_PATH_H
#define FF_CTL_OUT_PATH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_ctl_out_resolve_root — canonicalizes `root` (which must already
 * exist — main.c is responsible for creating it, e.g. via mkdtemp() for
 * the default temp-dir case) into `out` (capacity `out_sz`) via
 * realpath(). Call this once at ctl-loop startup; the result is what
 * every later ff_ctl_out_resolve_path() call confines requests to.
 * Returns false (root missing, not a directory the process can resolve,
 * or `out_sz` too small) without writing anything useful to `out`.
 */
bool ff_ctl_out_resolve_root(char const *root, char *out, size_t out_sz);

/**
 * ff_ctl_out_resolve_path — validates and confines `requested` (a ctl-
 * socket-supplied, untrusted relative path) under `root_real` (the
 * ALREADY-canonicalized root from ff_ctl_out_resolve_root — this
 * function does not re-resolve it), writing the confirmed-safe absolute
 * path to actually write to into `out` (capacity `out_sz`).
 *
 * On any rejection, returns false and sets `*err` to a short static
 * reason string; `out` is left unspecified (not necessarily
 * NUL-terminated) in that case — callers must check the return value
 * before using `out`. Rejects:
 *   - `requested` that is NULL, empty, or ends in '/' (no leaf name),
 *   - an absolute `requested` (starts with '/'),
 *   - any ".." path component anywhere in `requested`,
 *   - a `requested` whose containing directory doesn't already exist
 *     under `root_real` (this function does not create directories —
 *     only `root_real` itself is guaranteed to exist, by
 *     ff_ctl_out_resolve_root's contract),
 *   - a containing directory that resolves (realpath()) to somewhere
 *     other than `root_real` itself or a real subdirectory of it (the
 *     symlink-escape defense — see this header's top comment),
 *   - a leaf name that already exists as a symlink.
 */
bool ff_ctl_out_resolve_path(char const *requested, char const *root_real, char *out, size_t out_sz,
                              char const **err);

#ifdef __cplusplus
}
#endif

#endif /* FF_CTL_OUT_PATH_H */
