/**
 * flare_fmt.h — pure text-formatting helpers for the S10 flare UI
 * (takeover screen, sender overlay).
 *
 * Same rationale as radar_layout.h's split from scr_radar.c (that
 * header's top comment, and PR #16 UX review round 3 / code review round
 * 2's "goldens are pixel-diffs against themselves ... the test must be
 * assertion-level"): every one of these functions is plain C11 with no
 * LVGL dependency, specifically so its output can be asserted on directly
 * in screens/tests/test_flare_fmt.c, not only inferred from noticing a
 * few different pixels in a golden PNG diff.
 *
 * No domain logic lives here (CLAUDE.md) — these are presentation-only
 * transforms of already-decided facts (a name, a bearing in degrees, a
 * countdown in ms), never a decision about what those facts *should* be.
 */
#ifndef FF_FLARE_FMT_H
#define FF_FLARE_FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_flare_fmt_headline — "<NAME> IS FLARING" for the receive takeover's
 * headline. `name` is used verbatim (fixtures/crew names are already
 * display-cased, e.g. "DANA" — this does not re-case anything). If `name`
 * is NULL or empty, falls back to "SOMEONE IS FLARING" rather than
 * fabricating a name (CLAUDE.md: "unknown = explicitly unknown... never
 * fake" — an empty sender name is itself an honest fact worth showing,
 * not something to paper over with a placeholder identity). Truncates
 * (via snprintf) rather than overflowing if `out_sz` is too small for the
 * result; a NULL `out` or zero `out_sz` is a no-op.
 */
void ff_flare_fmt_headline(char *out, size_t out_sz, char const *name);

/**
 * ff_flare_fmt_compass8 — nearest 1-of-8 compass point ("N"/"NE"/"E"/
 * "SE"/"S"/"SW"/"W"/"NW") for `bearing_deg`. Each point spans 45 degrees
 * centered on its own heading (e.g. "N" covers [337.5, 360) union
 * [0, 22.5)); a bearing exactly ON a boundary (22.5, 67.5, ...) rolls
 * FORWARD into the next point, matching this codebase's existing
 * boundary convention (ff_crew_freshness's "age exactly 45000ms is
 * already STALE, not LIVE" — S02; ff_flare_tick's inclusive expiry —
 * ff_flare.h judgment call 1). `bearing_deg` is normalized (fmod +
 * wraparound) before classifying, so negative values and values >= 360
 * both resolve to the same point their normalized angle would. Never
 * returns NULL.
 */
char const *ff_flare_fmt_compass8(float bearing_deg);

/**
 * ff_flare_fmt_countdown — "M:SS" for `expires_in_ms` milliseconds
 * remaining (seconds truncated toward zero, e.g. 59999 -> "0:59", exactly
 * 60000 -> "1:00"), or the literal "--:--" for any negative value (the
 * codebase-wide "n/a" sentinel — ff_app_state.h's `*_expires_in_ms`
 * fields) so a takeover/sender screen never fabricates a countdown for
 * data it doesn't actually have (CLAUDE.md's honesty rule). `expires_in_ms
 * == 0` renders "0:00", not "--:--" — zero is a real (if final) known
 * value, not an unknown one. A NULL `out` or zero `out_sz` is a no-op.
 */
void ff_flare_fmt_countdown(char *out, size_t out_sz, int32_t expires_in_ms);

/**
 * ff_flare_fmt_go_switches_lock — true iff pressing GO on a takeover from
 * `takeover_from_name` would actually change an existing lock, given the
 * currently-locked name `locked_from_name` (empty/NULL: not locked).
 * Pulled out as a pure, unit-testable predicate (PR #20 UX review,
 * BLOCKING finding #3 — "GO must disclose what it costs") rather than an
 * inline `if` in scr_flare.c, per CLAUDE.md ("if you're writing an `if`
 * about domain behavior inside a screen file, it belongs in core" — this
 * is presentation-adjacent, not core-owned domain state, but the same
 * "make the decision a named, testable thing" discipline applies).
 * False whenever there is nothing to disclose: `locked_from_name` is
 * NULL/empty (not locked at all), OR the locked name and the takeover's
 * sender name are the SAME string (re-confirming an existing lock on the
 * same person costs nothing — nothing to disclose). Name comparison is a
 * plain `strcmp` (both fields are always NUL-terminated fixed-budget
 * strings — `ff_app_state.h`'s `char[FF_APP_NAME_LEN]` arrays) — this
 * module has no `ff_app_state.h` dependency itself, so the caller passes
 * plain `char const *` rather than the struct.
 */
bool ff_flare_fmt_go_switches_lock(char const *locked_from_name, char const *takeover_from_name);

/**
 * FF_FLARE_FMT_TRADE_NAME_MAX — how many characters of each name
 * ff_flare_fmt_lock_trade will print before truncating.
 *
 * `ff_app_flare_t`'s name fields are `char[FF_APP_NAME_LEN]` (16, i.e. up
 * to 15 printable characters). Two names at full length, plus the "GO: "
 * prefix and the arrow, is a ~37-character string that at the disclosure
 * chip's type size is WIDER THAN THE ROUND GLASS — and the puck is a
 * circle, so a chip that merely fits the 440px bounding box can still
 * have its ends hanging off the visible display (the exact class of bug
 * ff_layout.h exists because of: PR #25 shipped a back button 42px
 * outside the round area). Truncating is the honest failure mode here:
 * a clipped-but-on-glass name still identifies who you'd be dropping,
 * where a name rendered past the bezel identifies nobody. 9 characters
 * keeps the worst case ("GO: " + 9 + " > " + 9 = 25 chars) comfortably
 * inside the chord available at the chip's y-offset, and crew short
 * names in practice are 3-6 characters ("DANA", "KEV") — this truncation
 * is a guard rail, not the normal path.
 */
#define FF_FLARE_FMT_TRADE_NAME_MAX 9

/**
 * ff_flare_fmt_lock_trade — the takeover screen's lock-disclosure chip
 * text: `"GO: <locked> <arrow> <takeover>"` (e.g. `"GO: DANA > KEV"`).
 *
 * This is the glance-sized form of the disclosure S10's Amendment
 * Ruling 2 requires ("an established LOCK is never silently replaced" —
 * an informed choice must show its cost). It is a pure re-*wording*, not
 * a reduction of what is disclosed: all three facts the ruling needs
 * survive — that a lock exists (`<locked>` is named), that GO is what
 * spends it (the `GO:` prefix names the button, so the chip is a caption
 * for the control 60px below it rather than a free-floating status line),
 * and what is traded for what (the arrow's direction). Issue #27: the
 * sentence form it replaces ("LOCKED ON DANA - GO SWITCHES TO KEV") was
 * legible and correct but was a *read* on the one screen that interrupts
 * the user mid-panic at 2 AM.
 *
 * `arrow` is the separator glyph, supplied BY THE CALLER rather than
 * baked in, because the only good arrow available on this device lives
 * in LVGL's symbol range (`LV_SYMBOL_RIGHT`) and this module deliberately
 * has no LVGL dependency (see this header's top comment — that is what
 * makes it directly unit-testable). NULL or empty falls back to a plain
 * ASCII `">"`, which every font in this repo covers; that fallback is
 * what the unit tests assert against, so the tests never depend on which
 * glyphs a particular compiled font subset happens to include.
 *
 * Either name being NULL/empty renders as `"?"` — the same explicit
 * unknown marker `ff_scr_flare_build_lock_chip` already uses, never a
 * fabricated identity (CLAUDE.md's honesty rule). In practice the caller
 * gates on `ff_flare_fmt_go_switches_lock` first, which is already false
 * for either name being empty, so `"?"` is a defensive path rather than
 * a reachable one.
 *
 * Names longer than FF_FLARE_FMT_TRADE_NAME_MAX are truncated (see that
 * macro for why). Truncates rather than overflowing if `out_sz` is too
 * small; a NULL `out` or zero `out_sz` is a no-op.
 */
void ff_flare_fmt_lock_trade(char *out, size_t out_sz, char const *locked_from_name,
                              char const *takeover_from_name, char const *arrow);

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_FMT_H */
