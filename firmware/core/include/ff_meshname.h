/**
 * ff_meshname.h — core/meshname: puck-name charset + Meshtastic short-name
 * derivation.
 *
 * Motivation: the "NAME" Settings row (feature: NAME in Settings) needs
 * two small, pure pieces of domain logic that do not belong in the T9
 * editor (a generic reusable engine, ff_t9.h, that knows nothing about
 * puck names specifically) or in `ff_shell.c` (an `if` about what
 * characters a name may contain, or how a long name becomes a four-
 * character Meshtastic short name, is domain behavior — CLAUDE.md's
 * placement rule puts it here, not in `firmware/app/`):
 *
 *  - **`ff_meshname_sanitize`** — the puck name's charset rule (docs/
 *    specs/S12-first-run.md, "Name rules: length clamp, charset A-Z0-9
 *    space", restated for this feature's NAME row): drop everything
 *    that is not a letter, digit, or space, then trim leading/trailing
 *    spaces, then bound to the caller's buffer. Applied once, at COMMIT
 *    time (`ff_shell.c`'s NAME-editor DONE handler) — the T9 editor
 *    itself is allowed to type ABC-mode's key-1 punctuation
 *    (`. , ? !`) or reach the multi-tap engine's own 160-char ceiling;
 *    this function is what turns whatever the editor produced into an
 *    honest puck name rather than rejecting the keystroke mid-edit.
 *
 *  - **`ff_meshname_derive_short`** — the Meshtastic `short_name`
 *    (`User.short_name`, ideally two-ish characters, meshtastic/
 *    protobufs' own `User` doc comment) derived from the puck's long
 *    name: take up to the first 4 alphanumeric characters, uppercased,
 *    in order. Non-alphanumeric characters (spaces, punctuation that
 *    slipped past `ff_meshname_sanitize` some other way) are DROPPED
 *    BEFORE truncating, not after — "Jake H" derives "JAKE" (H would
 *    have survived a naive "take 4 raw chars, then strip" because the
 *    space lands inside the first 4 raw characters and gets dropped
 *    too early), not "JAKE" (same answer here, but "A B C D" makes the
 *    ordering matter: strip-then-truncate gives "ABCD" — padded from
 *    the name's next letters — where truncate-then-strip would give
 *    only "AB", having thrown "C D" away before ever looking at them).
 *    A name with fewer than 4 alphanumeric characters (e.g. "Jo") is
 *    NOT padded with anything fabricated — the short name is simply
 *    however many characters the long name actually has ("JO", not
 *    "JO\0\0" treated as 4 meaningful characters or a made-up filler).
 *    Examples (task brief): "Taylor" -> "TAYL", "Jake" -> "JAKE",
 *    "Jo" -> "JO".
 *
 * Pure C11, no I/O, no heap allocation — plain functions over caller-
 * supplied buffers, unit-tested directly (`core/tests/test_meshname.c`).
 */
#ifndef FF_MESHNAME_H
#define FF_MESHNAME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Output capacity for `ff_meshname_derive_short`: up to 4 characters
 *  plus a NUL terminator. Deliberately independent of Meshtastic's own
 *  wire-level `User.short_name` budget (40 bytes on this repo's nanopb
 *  build, `MC_NAME_MAX`, meshclient/include/mc_client.h) — this is the
 *  PRODUCT rule ("ideally two characters", this feature's own "first 4
 *  characters" ruling), not the wire's ceiling. */
#define FF_MESHNAME_SHORT_LEN 5

/**
 * ff_meshname_sanitize — copy the allowed-charset (letters, digits,
 * space) subsequence of `in`, in order, into `out`, then trim leading
 * and trailing spaces, then NUL-terminate. Case is preserved verbatim
 * (this function does not uppercase — that is `ff_meshname_derive_
 * short`'s job, applied only to the SHORT name).
 *
 * `out` receives at most `out_cap - 1` characters plus a NUL terminator
 * (truncated, not rejected — the established convention this codebase's
 * settings write-through already documents for an overlong `my_name`,
 * `ff_shell.c`'s `shell_setting_set` doc comment on `FF_SETTING_MY_NAME`).
 * A byte that is dropped for being outside the charset does not count
 * against this truncation budget — only KEPT bytes do.
 *
 * No-op (writes an empty string, if `out_cap > 0`) when `in` is NULL.
 * Does nothing at all if `out` is NULL or `out_cap == 0`.
 */
void ff_meshname_sanitize(char const *in, char *out, size_t out_cap);

/**
 * ff_meshname_derive_short — derive a Meshtastic short name from a puck
 * long name. See this header's top comment for the full rule and the
 * three worked examples. `out` is always NUL-terminated on return
 * (`out[0] = '\0'` when `long_name` is NULL, empty, or contains no
 * alphanumeric character at all — an honest empty short name, never a
 * fabricated one). `out` must have room for at least
 * `FF_MESHNAME_SHORT_LEN` bytes.
 *
 * No-op if `out` is NULL.
 */
void ff_meshname_derive_short(char const *long_name, char out[FF_MESHNAME_SHORT_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* FF_MESHNAME_H */
