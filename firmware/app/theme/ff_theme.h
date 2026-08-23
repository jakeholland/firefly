/**
 * ff_theme.h — app/theme: colors, type scale, and shared layout constants
 * for the radar face (and, going forward, every other LVGL screen).
 *
 * Spec: docs/specs/S06-radar-face.md ("Colors from app/theme/ff_theme.h
 * (amber #FFC66B, stale #FFB454, live-green #9BE07B, crew palette
 * pink/teal/violet/green)").
 *
 * Header-only (named constants + one inline lookup helper) — no .c file,
 * matching the rest of this repo's small theme-style headers. Values are
 * transcribed from the task brief that commissioned this slice (the
 * mockup artboards themselves aren't in-tree — see CLAUDE.md: "screen
 * mockups and plan live as Claude artifacts (ask Jake)" — this agent has
 * no access to them). Flagged per AGENTS.md's "note the interpretation"
 * rule, same as tests/fixtures/README.md's provenance note for the
 * radar_live/stale/close fixture numbers.
 *
 * ## Font substitutions (no mockup fonts vendored)
 * CLAUDE.md's type list is "Unbounded/Outfit/Spline Sans Mono -> device
 * fonts per S06"; none of those are vendored in this repo (only LVGL's
 * built-in Montserrat bitmap fonts are available — see firmware/lv_conf.h).
 * This header rounds every mockup size UP to the nearest enabled built-in
 * Montserrat size, never down — docs/review/ux-raver.md's legibility
 * checklist treats "under ~12px equivalent" as a flag and "under 10px" as
 * an automatic finding, so rounding down would risk shipping exactly the
 * kind of illegible-at-a-glance text that review exists to catch:
 *   name      21px -> montserrat_22 (nearest enabled size at/above 21)
 *   distance  36px -> montserrat_36 (exact)
 *   chip      ~11-12px equivalent -> montserrat_14 (clears both the
 *             "flag" and "automatic finding" thresholds with margin)
 *   label     ~10-11px equivalent -> montserrat_14 (same reasoning —
 *             this app does not ship any display text smaller than 14px)
 *   headline  (NOFIX/NOSEL primary message, not in the spec's own size
 *             table) -> montserrat_20, between name and distance
 * Real device fonts are a tracked follow-up (same "custom fonts tracked
 * as issue" footnote the spec itself carries), not a v1 gate.
 */
#ifndef FF_THEME_H
#define FF_THEME_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Colors (0xRRGGBB — lv_color_hex-ready).
 * ------------------------------------------------------------------- */

#define FF_THEME_COLOR_BG          0x0B0B10 /* puck background */
#define FF_THEME_COLOR_SURFACE     0x14141C /* chips / cards / raised panels */
#define FF_THEME_COLOR_AMBER       0xFFC66B /* primary accent — solid/LIVE arrow, CTA */
#define FF_THEME_COLOR_STALE_AMBER 0xFFB454 /* STALE rim tint / chip */
#define FF_THEME_COLOR_LIVE_GREEN  0x9BE07B /* LIVE chip, CLOSE pulse rings */
#define FF_THEME_COLOR_MUTED       0x8B8A97 /* secondary text, LOST rim tint */
#define FF_THEME_COLOR_DIM         0x55545F /* tertiary text, inactive page dot */
#define FF_THEME_COLOR_INK         0xF2EFE6 /* primary text on dark surfaces */

/* Crew palette — indexed by ff_crew_member_t::color_idx / ff_radar_dot_t
 * ::color_idx ("index into the theme crew palette", ff_crew.h). */
#define FF_THEME_CREW_PINK   0xFF5CA8
#define FF_THEME_CREW_TEAL   0x4FD8C4
#define FF_THEME_CREW_VIOLET 0xB08CFF
#define FF_THEME_CREW_GREEN  0x9BE07B

/**
 * ff_theme_crew_color — 0xRRGGBB for a dot/member's color_idx, wrapping
 * (modulo) so an out-of-range index degrades to a valid color instead of
 * reading out of bounds — honest-but-safe, never a crash over a display
 * nicety.
 */
static inline uint32_t ff_theme_crew_color(uint8_t color_idx)
{
    static const uint32_t palette[] = {
        FF_THEME_CREW_PINK,
        FF_THEME_CREW_TEAL,
        FF_THEME_CREW_VIOLET,
        FF_THEME_CREW_GREEN,
    };
    return palette[color_idx % (sizeof(palette) / sizeof(palette[0]))];
}

/* -------------------------------------------------------------------
 * Type scale — see this header's top comment for the rounding rationale.
 * ------------------------------------------------------------------- */

#define FF_THEME_FONT_NAME     (&lv_font_montserrat_22) /* mockup: 21px */
#define FF_THEME_FONT_DISTANCE (&lv_font_montserrat_36) /* mockup: 36px, mono in spec (see header comment: no mono vendored) */
#define FF_THEME_FONT_CHIP     (&lv_font_montserrat_14) /* mockup: ~11-12px equivalent */
#define FF_THEME_FONT_LABEL    (&lv_font_montserrat_14) /* mockup: ~10-11px equivalent */
#define FF_THEME_FONT_HEADLINE (&lv_font_montserrat_20) /* NOFIX/NOSEL primary message */

/* -------------------------------------------------------------------
 * Layout — matches targets/sim/main.c's window/puck sizing exactly (the
 * puck is the round physical display's stand-in: 456px sim window, 440px
 * puck circle centered in it, same convention main.c's boot screen and
 * fixture_view.c's debug face already use).
 * ------------------------------------------------------------------- */

#define FF_THEME_WINDOW_PX 456
#define FF_THEME_PUCK_PX   440 /* window - 16 */
#define FF_THEME_PUCK_RADIUS_PX (FF_THEME_PUCK_PX / 2)

#define FF_THEME_ARROW_LEN_PX    140 /* S06 spec: "arrow 140 px glyph" */
#define FF_THEME_ARROW_HEAD_PX   26  /* filled circular arrowhead diameter */
#define FF_THEME_RING_RADIUS_PX  185 /* crew-ring dot placement radius from puck center */
#define FF_THEME_DOT_PX          34  /* crew-ring dot diameter */

#define FF_THEME_FLARE_BTN_H_PX 48 /* S06 spec: "FLARE button (48 px high, full hit area)" */

/* docs/review/ux-raver.md checklist item 2: "every tappable thing >= 44px
 * equivalent". Named here so every tap target in app/screens can be
 * checked against one constant instead of a repeated magic number. */
#define FF_THEME_MIN_HIT_PX 44

#ifdef __cplusplus
}
#endif

#endif /* FF_THEME_H */
