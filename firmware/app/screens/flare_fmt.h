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

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_FMT_H */
