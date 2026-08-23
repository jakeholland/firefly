/**
 * now_layout.h — pure-geometry/format helpers for the Now face (S07 slice
 * b). Plain C11, no LVGL — same "compute is unit-testable, render is a
 * golden PNG" split as app/screens/radar_layout.h/.c
 * (app/screens/tests/test_now_layout.c exercises this file directly).
 *
 * ## Relationship to radar_layout.h — what's reused, what isn't, and why
 *
 * The Now face's content is a vertical stack (now-playing rows, a
 * starred-next card, or a scrollable day-lineup list), never elements
 * placed radially around a center point the way the radar face's arrow
 * and crew-ring dots are. radar_layout.h's actual resolvers
 * (`radar_layout_resolve_arrow`, `radar_layout_resolve_dots`) are a
 * search over ANGLE/LENGTH against a reserved-rectangle registry — there
 * is nothing analogous to search for here, so this file does not
 * duplicate (or depend on) that machinery.
 *
 * What DOES generalize, and IS reused (directly, by #include, from
 * app/screens/scr_now.c — not from this file, to keep this module's own
 * dependency list at zero): radar_layout.h's `RADAR_LAYOUT_PAGE_DOT_DY`,
 * already documented there as "Cross-mode chrome, present on every
 * render" — i.e. not radar-specific at all, just homed in that header
 * because radar was the first face built. scr_now.c computes its
 * scrollable lineup list's bottom edge directly FROM that constant (not a
 * second independently-chosen magic number that could silently drift
 * from it) — real collision risk, not just visual tidiness: scr_nav.c
 * draws the page-dot row directly on the puck, on top of every tile
 * (including this face's), at that exact dy, on every render. This
 * file's own `NOW_LAYOUT_ROW0_DY` etc. constants, by contrast, are
 * independently chosen (not derived from `RADAR_LAYOUT_STATUS_BAR_DY`):
 * the Now face never draws status-bar chrome on its own tile (nothing in
 * `ff_app_now_t` carries clock/mesh/battery data — see ff_app_state.h),
 * so there's no literal collision to prevent there, only a "look
 * reasonably consistent with the Radar tile's top margin" judgment call,
 * which doesn't warrant a hard #include-and-derive dependency the way the
 * page-dot bound does. See scr_now.c's top comment for the actual
 * #include and the derived constant.
 *
 * What's genuinely NEW here (not extracted from radar_layout.h, because
 * no analog exists there): `now_layout_chord_half_width_px`, a round-
 * screen keep-out primitive radar never needed (its content sits near
 * vertical center, where the puck's circular silhouette is at its
 * widest; the Now face's TBD-banner/lineup-list content runs much closer
 * to the top of the puck, where the chord narrows enough to matter). If
 * a third face ever needs the same primitive, this is a reasonable
 * extraction candidate — not done preemptively here (YAGNI; two call
 * sites don't justify a new shared module yet).
 */
#ifndef FF_NOW_LAYOUT_H
#define FF_NOW_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches app/theme/ff_theme.h's FF_THEME_PUCK_RADIUS_PX (220 =
 * FF_THEME_PUCK_PX/2 = 440/2) numerically. Not #include-d from there
 * directly — ff_theme.h pulls in "lvgl.h" (for lv_color_hex/lv_font_t),
 * which would make this supposedly-LVGL-free geometry module transitively
 * depend on LVGL to get one integer. radar_layout.h has the exact same
 * "independently-declared, must-match-by-convention" relationship with
 * FF_THEME_PUCK_PX (see that header's own top comment) — same tradeoff,
 * same precedent. */
#define NOW_LAYOUT_PUCK_RADIUS_PX 220.0f

/* ---------------------------------------------------------------------
 * Vertical layout constants (center-relative: puck center = dy 0), same
 * convention as radar_layout.h's RADAR_LAYOUT_*_DY constants. Interpretation
 * call, no mockup access (CLAUDE.md: "screen mockups... live as Claude
 * artifacts (ask Jake)" — this agent has none) — chosen to keep every row
 * clear of the round bezel (see now_layout_chord_half_width_px) and clear
 * of the page-dot row scr_nav.c draws at RADAR_LAYOUT_PAGE_DOT_DY (186).
 * Flagged per AGENTS.md's "note the interpretation" rule, same category as
 * ff_theme.h's own type-scale/geometry provenance note.
 * ------------------------------------------------------------------- */

/* LIVE state: section header (only shown when at least one row is live —
 * see scr_now.c) plus up to 3 now-playing rows, top-aligned, evenly
 * spaced. The header exists specifically for the UX review brief's
 * "2-second test" (docs/review/ux-raver.md item 1: "what do I think this
 * screen is? If I can't say instantly, that's a finding") — a bare stack
 * of artist/stage/bar rows with no label reads ambiguously (a favorites
 * list? history?) at a strobe-lit glance; "NOW PLAYING" removes the
 * ambiguity for the cost of one label. */
#define NOW_LAYOUT_LIVE_HEADER_DY (-172.0f)
#define NOW_LAYOUT_ROW0_DY (-146.0f)
#define NOW_LAYOUT_ROW_SPACING_DY 54.0f
#define NOW_LAYOUT_ROW_STAGE_OFFSET_DY (-15.0f)  /* stage-label line, relative to the row's own dy */
#define NOW_LAYOUT_ROW_ARTIST_OFFSET_DY 2.0f      /* artist-name line */
#define NOW_LAYOUT_ROW_BAR_OFFSET_DY 20.0f        /* progress-bar line */
#define NOW_LAYOUT_ROW_BAR_TRACK_W_PX 210
#define NOW_LAYOUT_ROW_BAR_H_PX 6

/* LIVE state: starred-next card, below the row stack. NEXT_COUNTDOWN_DY
 * carries extra clearance from NEXT_STAGE_DY (was 122, UX review round 1
 * — see scr_now.c's now_build_next_card) because the countdown now
 * renders at FF_THEME_FONT_DISTANCE (36px, up from 22px) per UX review
 * round 2 (PR #21 finding #4): the taller glyph needs the room. */
#define NOW_LAYOUT_NEXT_LABEL_DY 40.0f
#define NOW_LAYOUT_NEXT_ARTIST_DY 64.0f
#define NOW_LAYOUT_NEXT_STAGE_DY 88.0f
#define NOW_LAYOUT_NEXT_COUNTDOWN_DY 132.0f

/* LIVE state, no rows at all (a quiet moment: pack loaded, known
 * schedule, nothing live right now) — same headline/sub vertical slots as
 * radar's NOSEL treatment for visual consistency across faces. */
#define NOW_LAYOUT_QUIET_HEADLINE_DY (-10.0f)
#define NOW_LAYOUT_QUIET_SUB_DY 30.0f

/* TBD state: banner + scrollable day-lineup list. NOW_LAYOUT_LINEUP_TOP_DY
 * is independently chosen (below the banner); the list's BOTTOM edge is
 * deliberately NOT a constant here — see this header's top comment —
 * scr_now.c derives it directly from radar_layout.h's
 * RADAR_LAYOUT_PAGE_DOT_DY instead of duplicating a second magic number
 * that could drift out of sync with where the page dots actually are. */
#define NOW_LAYOUT_TBD_BANNER_DY (-156.0f)
#define NOW_LAYOUT_LINEUP_TOP_DY (-118.0f)

/* Empty state: no pack loaded at all. */
#define NOW_LAYOUT_EMPTY_HEADLINE_DY (-10.0f)
#define NOW_LAYOUT_EMPTY_SUB_DY 30.0f

/**
 * now_layout_format_countdown — formats `mins_until` as the spec's
 * literal countdown text ("IN N MIN", docs/specs/S07-now-face.md's
 * Behavior section: "'IN N MIN' >= 13 px"). `mins_until` is documented
 * (ff_sched.h's ff_next_t) as always > 0 for real engine output, but this
 * is fixture/render-facing, not engine-facing — a hand-authored fixture
 * could carry 0 or a negative value, so this floors at 0 rather than
 * printing a confusing negative countdown ("IN -3 MIN" reads as a bug, not
 * data). Always NUL-terminates; truncates (via snprintf) if `out_sz` is
 * too small for a pathological `mins_until`.
 */
void now_layout_format_countdown(int16_t mins_until, char *out, size_t out_sz);

/**
 * now_layout_bar_fill_px — pixel fill width for a now-playing row's
 * progress bar: `pct_done` (clamped to [0,100], defensive against a
 * fixture carrying an out-of-range value even though ff_now_row_t docs it
 * as already 0-100) as a fraction of `track_w_px` (clamped to >= 0).
 * Integer math (no float rounding surprises): `track_w_px * pct / 100`,
 * truncating toward zero.
 */
int32_t now_layout_bar_fill_px(uint8_t pct_done, int32_t track_w_px);

/**
 * now_layout_chord_half_width_px — the round puck's half-width (the
 * horizontal distance from center to the circular bezel) at vertical
 * offset `dy` from center. `sqrt(r^2 - dy^2)`, clamped to 0 for any `dy`
 * at or beyond the radius (never a negative width, never NaN from a
 * negative sqrt argument). This is the Now face's actual "respect the
 * round screen" primitive — see this header's top comment for why it's
 * new rather than reused from radar_layout.h (no analog exists there).
 */
float now_layout_chord_half_width_px(float dy);

#ifdef __cplusplus
}
#endif

#endif /* FF_NOW_LAYOUT_H */
