/**
 * lineup_layout.h — pure-geometry/format helpers for the Now face (S07 slice
 * b). Plain C11, no LVGL — same "compute is unit-testable, render is a
 * golden PNG" split as app/screens/radar_layout.h/.c
 * (app/screens/tests/test_lineup_layout.c exercises this file directly).
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
 * app/screens/scr_lineup.c — not from this file, to keep this module's own
 * dependency list at zero): radar_layout.h's `RADAR_LAYOUT_PAGE_DOT_DY`,
 * already documented there as "Cross-mode chrome, present on every
 * render" — i.e. not radar-specific at all, just homed in that header
 * because radar was the first face built. scr_lineup.c computes its
 * scrollable lineup list's bottom edge directly FROM that constant (not a
 * second independently-chosen magic number that could silently drift
 * from it) — real collision risk, not just visual tidiness: scr_nav.c
 * draws the page-dot row directly on the puck, on top of every tile
 * (including this face's), at that exact dy, on every render. This
 * file's own `LINEUP_LAYOUT_ROW0_DY` etc. constants, by contrast, are
 * independently chosen (not derived from `RADAR_LAYOUT_STATUS_BAR_DY`):
 * the Now face never draws status-bar chrome on its own tile (nothing in
 * `ff_app_now_t` carries clock/mesh/battery data — see ff_app_state.h),
 * so there's no literal collision to prevent there, only a "look
 * reasonably consistent with the Radar tile's top margin" judgment call,
 * which doesn't warrant a hard #include-and-derive dependency the way the
 * page-dot bound does. See scr_lineup.c's top comment for the actual
 * #include and the derived constant.
 *
 * What's genuinely NEW here (not extracted from radar_layout.h, because
 * no analog exists there): `lineup_layout_chord_half_width_px`, a round-
 * screen keep-out primitive radar never needed (its content sits near
 * vertical center, where the puck's circular silhouette is at its
 * widest; the Now face's TBD-banner/lineup-list content runs much closer
 * to the top of the puck, where the chord narrows enough to matter). If
 * a third face ever needs the same primitive, this is a reasonable
 * extraction candidate — not done preemptively here (YAGNI; two call
 * sites don't justify a new shared module yet).
 */
#ifndef FF_LINEUP_LAYOUT_H
#define FF_LINEUP_LAYOUT_H

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
#define LINEUP_LAYOUT_PUCK_RADIUS_PX 220.0f

/* ---------------------------------------------------------------------
 * Vertical layout constants (center-relative: puck center = dy 0), same
 * convention as radar_layout.h's RADAR_LAYOUT_*_DY constants. Interpretation
 * call, no mockup access (CLAUDE.md: "screen mockups... live as Claude
 * artifacts (ask Jake)" — this agent has none) — chosen to keep every row
 * clear of the round bezel (see lineup_layout_chord_half_width_px) and clear
 * of the page-dot row scr_nav.c draws at RADAR_LAYOUT_PAGE_DOT_DY (186).
 * Flagged per AGENTS.md's "note the interpretation" rule, same category as
 * ff_theme.h's own type-scale/geometry provenance note.
 * ------------------------------------------------------------------- */

/* LIVE state: section header (only shown when at least one row is live —
 * see scr_lineup.c) plus up to 3 now-playing rows, top-aligned, evenly
 * spaced. The header exists specifically for the UX review brief's
 * "2-second test" (docs/review/ux-raver.md item 1: "what do I think this
 * screen is? If I can't say instantly, that's a finding") — a bare stack
 * of artist/stage/bar rows with no label reads ambiguously (a favorites
 * list? history?) at a strobe-lit glance; "NOW PLAYING" removes the
 * ambiguity for the cost of one label. */
#define LINEUP_LAYOUT_LIVE_HEADER_DY (-172.0f)
#define LINEUP_LAYOUT_ROW0_DY (-146.0f)
#define LINEUP_LAYOUT_ROW_SPACING_DY 54.0f
#define LINEUP_LAYOUT_ROW_STAGE_OFFSET_DY (-15.0f)  /* stage-label line, relative to the row's own dy */
#define LINEUP_LAYOUT_ROW_ARTIST_OFFSET_DY 2.0f      /* artist-name line */
#define LINEUP_LAYOUT_ROW_BAR_OFFSET_DY 20.0f        /* progress-bar line */
#define LINEUP_LAYOUT_ROW_BAR_TRACK_W_PX 210
#define LINEUP_LAYOUT_ROW_BAR_H_PX 6

/* LIVE state: starred-next card, below the row stack. NEXT_COUNTDOWN_DY
 * carries extra clearance from NEXT_STAGE_DY (was 122, UX review round 1
 * — see scr_lineup.c's lineup_build_next_card) because the countdown now
 * renders at FF_THEME_FONT_DISTANCE (36px, up from 22px) per UX review
 * round 2 (PR #21 finding #4): the taller glyph needs the room. */
#define LINEUP_LAYOUT_NEXT_LABEL_DY 40.0f
#define LINEUP_LAYOUT_NEXT_ARTIST_DY 64.0f
#define LINEUP_LAYOUT_NEXT_STAGE_DY 88.0f
#define LINEUP_LAYOUT_NEXT_COUNTDOWN_DY 132.0f

/* NOW_NOTHING_PLAYING state (pack loaded, known schedule, nothing live
 * right now) — same headline/sub vertical slots as radar's NOSEL
 * treatment for visual consistency across faces. */
#define LINEUP_LAYOUT_NOTHING_PLAYING_HEADLINE_DY (-10.0f)
#define LINEUP_LAYOUT_NOTHING_PLAYING_SUB_DY 30.0f

/* NOW_TBD state: banner + scrollable day-lineup list (every entry, since
 * NOW_TBD means every set on the day lacks a time). LINEUP_LAYOUT_LINEUP_TOP_DY
 * is independently chosen (below the banner); the list's BOTTOM edge is
 * deliberately NOT a constant here — see this header's top comment —
 * scr_lineup.c derives it directly from radar_layout.h's
 * RADAR_LAYOUT_PAGE_DOT_DY instead of duplicating a second magic number
 * that could drift out of sync with where the page dots actually are. */
#define LINEUP_LAYOUT_TBD_BANNER_DY (-156.0f)
#define LINEUP_LAYOUT_LINEUP_TOP_DY (-118.0f)

/* NOW_MIXED state — the SAME banner text/pill NOW_TBD uses ("show the TBD
 * banner whenever ANY set on the day lacks a time" — PR #21 code review
 * finding #1/ruling's literal wording), positioned slightly higher to
 * leave room below for a "known so far" section and the still-unknown
 * list beneath it.
 *
 * UX review round 2 (PR #21, "the state I'll actually live in for weeks
 * before the festival deserves the most care, not the least") RULING:
 * three visually distinct classes inside "known so far", never
 * distinguished by the absence of an element —
 *   (a) PLAYING NOW: the exact same treatment NOW_LIVE gives a row
 *       (stage-colored label + artist + progress bar, via
 *       lineup_build_row() — reused, not re-implemented, because it's the
 *       same fact and must look the same everywhere it appears).
 *   (b) SCHEDULED, NOT STARTED (the starred-next set): countdown-LED —
 *       the "IN N MIN" text is the first, largest, most colorful thing
 *       in its block, exactly like NOW_LIVE's next-card, not a trailing
 *       suffix on an otherwise-plain line — and it has NO progress bar
 *       (round 1's compact "Artist - Stage - IN N MIN" line made this
 *       indistinguishable from (a) except by a suffix nobody was told to
 *       look for).
 *   (c) TIME UNKNOWN: the "STILL TBD" list, unchanged.
 *
 * Because how many (a)-class rows exist varies (0..FF_APP_NOW_MAX_ROWS)
 * and item (b) is optional (next.valid), the vertical stack below
 * MIXED_KNOWN_HEADER_DY is laid out DYNAMICALLY at runtime (scr_lineup.c's
 * lineup_render_mixed advances a running `cursor_dy`) rather than from a
 * fixed set of per-slot constants the way NOW_LIVE's rows are — a
 * pathological 3-rows-and-a-next mixed day is rare (the realistic near-
 * term Lost Lands scenario the review flagged is 1-2 known items), and
 * dynamic placement means the common case doesn't waste the vertical
 * budget the "still unknown" list needs. */
#define LINEUP_LAYOUT_MIXED_BANNER_DY (-174.0f)
#define LINEUP_LAYOUT_MIXED_KNOWN_HEADER_DY (-142.0f) /* "KNOWN SO FAR" — only drawn if at least one row/next exists */
#define LINEUP_LAYOUT_MIXED_ROW0_DY (-110.0f)         /* first (a)-class row's dy, same internal offsets as NOW_LIVE's rows */
#define LINEUP_LAYOUT_MIXED_ROW_SPACING_DY 46.0f       /* between successive (a)-class rows */
#define LINEUP_LAYOUT_MIXED_NEXT_BLOCK_H_DY 46.0f       /* vertical space one (b)-class block reserves (countdown line + artist/stage line) */
#define LINEUP_LAYOUT_MIXED_SECTION_GAP_DY 18.0f        /* gap between the known section's last item and "STILL TBD" */
#define LINEUP_LAYOUT_MIXED_UNKNOWN_HEADER_MIN_DY (-140.0f) /* "STILL TBD" position when there's NO known section to push it down (any_known == false) */
#define LINEUP_LAYOUT_MIXED_UNKNOWN_HEADER_TO_LIST_GAP_DY 20.0f

/* NOW_NO_PACK state: no festpack loaded at all. */
#define LINEUP_LAYOUT_NO_PACK_HEADLINE_DY (-10.0f)
#define LINEUP_LAYOUT_NO_PACK_SUB_DY 30.0f

/* NOW_TIME_UNKNOWN state (issue #48): a pack IS loaded, only the clock is
 * unknown. Same two-line vertical rhythm as NOW_NO_PACK (headline + sub,
 * identical DYs) — this is a sibling "honest empty" state, not a visually
 * distinct one; the words carry the distinction, the layout doesn't need
 * to. */
#define LINEUP_LAYOUT_TIME_UNKNOWN_HEADLINE_DY (-10.0f)
#define LINEUP_LAYOUT_TIME_UNKNOWN_SUB_DY 30.0f

/**
 * lineup_layout_format_countdown — formats `mins_until` as the spec's
 * literal countdown text ("IN N MIN", docs/specs/S07-now-face.md's
 * Behavior section: "'IN N MIN' >= 13 px"). `mins_until` is documented
 * (ff_sched.h's ff_next_t) as always > 0 for real engine output, but this
 * is fixture/render-facing, not engine-facing — a hand-authored fixture
 * could carry 0 or a negative value, so this floors at 0 rather than
 * printing a confusing negative countdown ("IN -3 MIN" reads as a bug, not
 * data). Always NUL-terminates; truncates (via snprintf) if `out_sz` is
 * too small for a pathological `mins_until`.
 */
void lineup_layout_format_countdown(int16_t mins_until, char *out, size_t out_sz);

/**
 * lineup_layout_bar_fill_px — pixel fill width for a now-playing row's
 * progress bar: `pct_done` (clamped to [0,100], defensive against a
 * fixture carrying an out-of-range value even though ff_now_row_t docs it
 * as already 0-100) as a fraction of `track_w_px` (clamped to >= 0).
 * Integer math (no float rounding surprises): `track_w_px * pct / 100`,
 * truncating toward zero.
 */
int32_t lineup_layout_bar_fill_px(uint8_t pct_done, int32_t track_w_px);

/**
 * lineup_layout_chord_half_width_px — the round puck's half-width (the
 * horizontal distance from center to the circular bezel) at vertical
 * offset `dy` from center. `sqrt(r^2 - dy^2)`, clamped to 0 for any `dy`
 * at or beyond the radius (never a negative width, never NaN from a
 * negative sqrt argument). This is the Now face's actual "respect the
 * round screen" primitive — see this header's top comment for why it's
 * new rather than reused from radar_layout.h (no analog exists there).
 */
float lineup_layout_chord_half_width_px(float dy);

#ifdef __cplusplus
}
#endif

#endif /* FF_LINEUP_LAYOUT_H */
