/**
 * ff_t9.h — core/t9: the multi-tap T9 text-entry engine.
 *
 * Spec: docs/specs/S08-signals-t9.md, slice (a) — "T9 engine + tests" only.
 * No UI, no feed wiring here (those wait on the app shell, S06 PR B).
 *
 * Pure C11, no I/O, zero heap allocation — `ff_t9_t` is a plain struct
 * (fixed-size buffers only) safe to put on the stack or in a static.
 * Zero-initialize or call `ff_t9_reset` before use.
 *
 * ## v1 semantics — multi-tap only
 * Pressing a key appends/cycles a *pending* character shown live by
 * `ff_t9_text()`. Pressing the SAME key again within the commit window
 * cycles that key's letter table (wrapping); pressing a DIFFERENT key, or
 * the commit window elapsing with no further press, commits the pending
 * character into the permanent text and starts a new pending character (or,
 * for the timeout case, simply leaves the text committed with nothing
 * pending).
 *
 * Key layout:
 *   1 -> . , ? !     (punctuation, cycles in that order)
 *   2 -> a b c        3 -> d e f        4 -> g h i
 *   5 -> j k l        6 -> m n o        7 -> p q r s
 *   8 -> t u v        9 -> w x y z
 *   0 -> space (see `ff_t9_key` / `ff_t9_space` below)
 *
 * ## Deviations from the spec's interface sketch (see PR for detail)
 *
 *  - **`ff_t9_tick()` is new — not in the spec's sketch.** The spec says a
 *    pending character commits when "900 ms elapse[s]", but every other
 *    entry point (`ff_t9_key`, `ff_t9_backspace`, `ff_t9_space`) only fires
 *    on a *keypress*. If the user presses a key once and then simply stops
 *    (no further input), nothing ever calls back into this module to
 *    notice the clock ran out — the timeout is unimplementable without an
 *    explicit poll. `ff_t9_tick(t, now_ms)` fills that gap: the UI calls it
 *    once per frame (or at least whenever a pending char is showing) so the
 *    commit-on-timeout behavior actually happens without requiring another
 *    keypress. It is a pure "check the clock and maybe commit" call — safe
 *    to call as often as you like, including when nothing is pending.
 *
 *  - **Commit-window boundary is `< 900ms` cycles, `>= 900ms` commits**
 *    (half-open, matching `ff_crew`'s documented boundary convention).
 *    A same-key repeat landing at *exactly* 900ms since the last press does
 *    NOT cycle — the pending char is treated as already timed out, gets
 *    committed, and the repeat press starts a *fresh* pending character
 *    (cycle index 0) for that key rather than continuing the old cycle.
 *
 *  - **`ff_t9_key(t, 0, now_ms)` and `ff_t9_space(t)` are defined to be
 *    exactly equivalent** (commit any pending char, then append a space).
 *    `now_ms` is accepted by the key-0 path purely for call-site uniformity
 *    with the other 9 keys — space never starts or continues a pending
 *    cycle, so the timestamp has no effect on its behavior. Both entry
 *    points exist because the spec's sketch lists both; callers may use
 *    whichever is convenient (a literal key-0 press vs. a dedicated space
 *    button) and get identical results.
 *
 *  - **`ff_t9_suggest()` is a documented v1 no-op stub.** The spec reserves
 *    this name for the v1.5 predictive/dictionary addendum but v1 is
 *    multi-tap only — there is no dictionary to suggest from. It is defined
 *    (not merely declared) so the symbol exists and links, always returns 0
 *    (no suggestions), and touches none of its arguments. Ignore it in v1;
 *    v1.5 replaces the body.
 *
 *  - **160-char cap applies to *committed* text only, and blocks new
 *    pending chars once full.** Once `ff_t9_text()`'s committed portion
 *    reaches `FF_T9_MAX_LEN` (160) characters, `ff_t9_key()` for any
 *    letter/punctuation key (1-9) is a no-op — it does not start a new
 *    pending character — so the text never grows past 160 and there is
 *    never a "phantom" 161st pending char shown at the cap. Key 0 / space
 *    still commits whatever (if anything) was already pending before the
 *    cap was hit, but likewise will not append the space itself once the
 *    committed text is already at the cap. Backspace is always allowed (it
 *    only ever shrinks the buffer).
 *
 *  - **`ff_t9_insert_text()` is new, added in S08 slice c/d (Signals/
 *    Compose faces) — not in the spec's original interface sketch.** The
 *    Compose keypad's SYM page (docs/specs/S08-signals-t9.md's Amendments:
 *    2026-08-23 owner ruling on the digits/symbols question) inserts
 *    multi-character ASCII shortcuts — emoticons like ":)" — as a single
 *    keypress, which the per-key multi-tap model (one pending char, cycled
 *    by repeated presses of the SAME key) has no way to express: there is
 *    no key whose letter table is `{":)"}` as one "letter". This function
 *    commits any pending char (same first step as `ff_t9_space`), then
 *    appends a plain ASCII string atomically, subject to the same 160-char
 *    cap. It is deliberately named `_text`, not `_utf8` — every string it
 *    accepts is one byte per character (ASCII emoticons/punctuation, S08
 *    Amendments' "acceptable fallback" tier); it does not attempt to
 *    special-case multi-byte codepoint boundaries, so it must not be used
 *    to insert real multi-byte UTF-8 (a future real-emoji tier would need
 *    its own codepoint-aware entry point, not this one, precisely so this
 *    one's backspace-is-always-one-`char` behavior stays correct for
 *    everything it actually accepts).
 */
#ifndef FF_T9_H
#define FF_T9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max committed characters (S08 AC2). Does not include a pending char. */
#define FF_T9_MAX_LEN 160

/** Multi-tap commit window, milliseconds. Half-open: see header note above. */
#define FF_T9_COMMIT_MS ((uint32_t)900u)

typedef struct {
    /* Committed text, NUL-terminated, up to FF_T9_MAX_LEN chars. */
    char    buf[FF_T9_MAX_LEN + 1];
    uint8_t len; /* strlen(buf), cached */

    /* Pending (mid-cycle) character state. */
    bool     has_pending;
    char     pending_char;
    uint8_t  pending_key;   /* 1-9; meaningless when !has_pending */
    uint8_t  cycle_idx;     /* index into pending_key's letter table */
    uint32_t last_key_ms;   /* timestamp of the press that produced pending_char */

    /* Scratch buffer for ff_t9_text(): committed + pending char + NUL,
     * refreshed on every mutating call so the accessor can stay a plain
     * `char const *` into the struct without needing a non-const `t`. */
    char display[FF_T9_MAX_LEN + 2];
} ff_t9_t;

/** ff_t9_reset — clear a T9 buffer to empty, no pending char. */
void ff_t9_reset(ff_t9_t *t);

/**
 * ff_t9_key — handle a keypad key press (0-9) at time `now_ms` (caller's
 * clock, e.g. read off the injected ff_clock_t at the moment of the press).
 *
 * key 0 is space: see the header's "Deviations" note — identical to
 * calling `ff_t9_space(t)`.
 *
 * keys 1-9: multi-tap per the header doc. A key value outside 0-9 is
 * ignored (no-op) — defensive against a caller forwarding a raw, unvalidated
 * hardware code.
 */
void ff_t9_key(ff_t9_t *t, uint8_t key, uint32_t now_ms);

/**
 * ff_t9_tick — poll the commit timer without a keypress. If a pending
 * character has been sitting for >= FF_T9_COMMIT_MS since its last cycle,
 * commits it. Safe to call every frame, including when nothing is pending
 * (cheap no-op). See the header's "Deviations" note for why this exists.
 */
void ff_t9_tick(ff_t9_t *t, uint32_t now_ms);

/**
 * ff_t9_backspace — remove one character: the pending char if one is
 * showing (discarded outright, not decremented back a cycle step — the
 * next press of that key starts a fresh cycle), otherwise the last
 * committed character. No-op if both are empty.
 */
void ff_t9_backspace(ff_t9_t *t);

/**
 * ff_t9_space — commit any pending character, then append a space to the
 * committed text (subject to the 160-char cap). Equivalent to
 * `ff_t9_key(t, 0, now_ms)` for any `now_ms` — see the header's
 * "Deviations" note.
 */
void ff_t9_space(ff_t9_t *t);

/**
 * ff_t9_insert_text — commit any pending character (per `ff_t9_space`'s
 * first step), then atomically append the plain ASCII string `s` to the
 * committed text, subject to the 160-char cap. All-or-nothing: if `s`
 * would not fully fit in the remaining budget, `t` is left completely
 * unmodified (pending char included) and this returns false — a
 * half-inserted symbol reading as garbage would be worse than a silently
 * ignored keypress. Returns true if `s` was fully appended (including the
 * trivial case of an empty `s`, which still commits any pending char).
 * `s` must be NUL-terminated, single-byte-per-character ASCII — see the
 * header's "Deviations" note for why this is not a UTF-8 insertion API.
 * Enforced at runtime, not just documented: any byte in `s` with its high
 * bit set (>= 0x80, i.e. any byte that could only appear as part of a
 * multi-byte UTF-8 sequence) is rejected outright — `t` untouched, same
 * as a would-not-fit `s` — rather than silently truncating, mis-copying,
 * or accepting text that would corrupt on a later byte-at-a-time
 * backspace (S08 PR #25 code review, LOW finding).
 * Returns false without modifying `t` if `t` or `s` is NULL.
 */
bool ff_t9_insert_text(ff_t9_t *t, char const *s);

/**
 * ff_t9_text — committed text plus the live pending character (if any),
 * NUL-terminated. Points into `t`'s own storage; valid until the next
 * mutating call on `t`. Never exceeds FF_T9_MAX_LEN + 1 characters.
 */
char const *ff_t9_text(ff_t9_t const *t);

/**
 * ff_t9_suggest — v1.5 predictive-dictionary hook, reserved by the spec.
 * v1 is multi-tap only (no dictionary): always returns 0 and leaves `out`
 * untouched. See the header's "Deviations" note.
 *
 * `out`/`max_out` describe a caller-provided array of `char const *` the
 * function would fill with up to `max_out` suggestion strings; the return
 * value is the number written (always 0 in v1).
 */
size_t ff_t9_suggest(ff_t9_t const *t, char const **out, size_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* FF_T9_H */
