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
 * FF_FLARE_FMT_LOCK_NAME_MAX — how many BYTES of the locked name
 * ff_flare_fmt_lock_cost will print before truncating, and
 * FF_FLARE_FMT_ELLIPSIS — the marker appended when it does.
 *
 * BYTES, not characters (PR #41 code review, minor finding): the
 * underlying `snprintf("%.*s", ...)` precision is a byte count, crew
 * names arrive as untrusted UTF-8 off the radio (Meshtastic
 * `User.long_name`), and this codebase already truncates by bytes at
 * ingest (`mc_copy_name`). ff_flare_fmt_lock_cost backs the cut off to
 * the last COMPLETE codepoint rather than emitting a severed one, so a
 * name like "ANDRÉ" simply spends 2 of its budget on that one glyph
 * instead of rendering a stray byte.
 *
 * Why truncate at all: `ff_app_flare_t`'s name fields are
 * `char[FF_APP_NAME_LEN]` (16, i.e. up to 15 printable characters), and
 * the puck is a CIRCLE, so a chip that merely fits the 440px bounding
 * box can still have its ends hanging off the visible display (the exact
 * class of bug ff_layout.h exists because of: PR #25 shipped a back
 * button 42px outside the round area).
 *
 * Why the ellipsis is not optional (PR #41 UX review, BLOCKING 2): the
 * previous two-name form truncated with a bare `%.*s`, so ALEXANDRIA and
 * ALEXANDRINA both rendered `ALEXANDRI` — the disclosure chip printed a
 * trade of a person for themselves, which reads as "this costs nothing,
 * press GO", the precise outcome the chip exists to prevent. The
 * single-name wording removes the false-equality failure entirely (there
 * is no second name to collide with), and the ellipsis additionally
 * guarantees the weaker property the reviewer asked for as a floor: a
 * cut name always LOOKS cut, so a truncated name is never mistaken for a
 * whole one.
 *
 * The cap is chosen by MEASUREMENT, not arithmetic. Instrumenting
 * test_scr_flare.c's
 * S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass to print the
 * chip's real lv_area_t and its worst corner's distance from the puck
 * centre, with a 15-character locked name (the longest FF_APP_NAME_LEN
 * permits) at FF_THEME_FONT_HEADLINE:
 *
 *   cap 10 -> chip 386x34 px, worst corner 194.9, slack to r=220  +25.1
 *   cap 11 -> chip 407x34 px, worst corner 205.8, slack to r=220  +14.2  <- shipped
 *   cap 12 -> passes, but within ~3px of the edge
 *   cap 13 -> chip 453x34 px, worst corner 228.6, slack to r=220   -8.6  (fails)
 *   cap 15 -> chip 485x34 px, worst corner 244.5, slack to r=220  -24.5  (fails)
 *
 * So 13 is the failure threshold and 11 keeps genuine margin rather than
 * sitting on the boundary — the same standard PR #41's code reviewer
 * applied to the previous cap. The single-name wording is what bought
 * the increase from 9: dropping the repeated sender name freed the width
 * that now covers ALEXANDRIA and ALEXANDRINA whole, which is exactly the
 * pair that collided under the two-name form.
 */
#define FF_FLARE_FMT_LOCK_NAME_MAX 11
#define FF_FLARE_FMT_ELLIPSIS "..."

/**
 * ff_flare_fmt_lock_cost — the takeover screen's lock-disclosure chip
 * text: `"GO DROPS LOCK - <locked>"` (e.g. `"GO DROPS LOCK - DANA"`).
 *
 * ## What this must disclose, and where that requirement comes from
 * The requirement is PR #20's UX review, BLOCKING finding #3 — "GO must
 * disclose what it costs" — the same finding
 * ff_flare_fmt_go_switches_lock's doc comment already cites. It is NOT
 * stated by S10's Amendment Ruling 2, which is entirely state-machine
 * semantics (that a newer flare never touches an existing lock, that
 * DISMISS returns to the intact lock, that GO is the explicit decision
 * that switches it). Ruling 2 is why an established lock is a decision
 * the user owns; finding #3 is why pressing GO has to say so on screen.
 * PR #41's code review caught this file attributing the second to the
 * first — corrected here, because the whole point of writing the
 * requirement down is that a future re-wording can be checked against
 * it, and a citation pointing at a paragraph that doesn't contain it is
 * worse than no citation.
 *
 * ## Why one name and a verb (PR #41 UX review, BLOCKING 1)
 * The form this replaces was `"GO: DANA > KEV"`. Shortening the original
 * sentence had deleted its verb, and the verb was the disclosure: `A > B`
 * means *via* or *then* in every other context a person meets it
 * (breadcrumbs, file paths, map directions), so the chip read as an
 * itinerary — "go to Dana, then Kev" — i.e. as KEEPING the lock. It also
 * dropped the word LOCK, which is the only vocabulary the user has for
 * this: the Radar face's own chip reads `LOCKED - DANA`
 * (ff_scr_flare_build_lock_chip), and the takeover screen had stopped
 * connecting to it.
 *
 * So: a verb of loss (`DROPS`), the noun the user already knows
 * (`LOCK`), and the same ` - <name>` tail the Radar chip uses, so the two
 * chips visibly belong to each other. No glyph whose convention has to be
 * learned, and no direction to misread — there is no direction in it.
 *
 * The incoming sender's name is deliberately NOT here. It is the
 * takeover's headline, in 22px type, directly above this chip
 * (ff_flare_fmt_headline, built unconditionally by the same function
 * that builds this chip — pinned by
 * S10_ACn_lock_disclosure_is_always_accompanied_by_the_headline). Naming
 * KEV twice on one screen spends the chip's whole width budget on the
 * one fact that is already the largest thing in view.
 *
 * `locked_from_name` being NULL/empty renders as `"?"` — the same
 * explicit unknown marker ff_scr_flare_build_lock_chip uses, never a
 * fabricated identity (CLAUDE.md's honesty rule). Unreachable in
 * practice: the caller gates on ff_flare_fmt_go_switches_lock, which is
 * already false for an empty locked name, so no chip is built at all
 * (verified by PR #41's UX reviewer, who rendered it). Kept as a guard,
 * not claimed as covered.
 *
 * Names longer than FF_FLARE_FMT_LOCK_NAME_MAX bytes are truncated to
 * the last complete codepoint at or before that budget and suffixed with
 * FF_FLARE_FMT_ELLIPSIS. Truncates rather than overflowing if `out_sz` is
 * too small; a NULL `out` or zero `out_sz` is a no-op.
 */
void ff_flare_fmt_lock_cost(char *out, size_t out_sz, char const *locked_from_name);

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_FMT_H */
