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

#include <stdbool.h>
#include <stddef.h>
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
#define FF_THEME_COLOR_DIM         0x55545F /* tertiary text (the retired page-dot row's inactive-dot color, before S26e) */
#define FF_THEME_COLOR_INK         0xF2EFE6 /* primary text on dark surfaces */

/* Crew palette — indexed by ff_crew_member_t::color_idx / ff_radar_dot_t
 * ::color_idx ("index into the theme crew palette", ff_crew.h).
 *
 * S17 slice a (issue #43): FF_CREW_MAX is 8 but this table used to hold
 * only 4 entries, wrapping mod-4 — a full crew guaranteed two adjacent
 * ring/wedge members shared a color. Extended to 8 real entries.
 * Indices 0-3 are BYTE-IDENTICAL to the original 4 (same names, same
 * hexes) — every scene with <=4 crew renders unchanged, no existing
 * golden moves (S17a AC1/AC5). 4-7 are new.
 *
 * The four new hexes (and the colorblind-safe set below) were picked by
 * a small script, not eyeballed: for each candidate, CIE76 ΔE*ab against
 * every OTHER color this app already assigns meaning to (the crew four,
 * the status colors below, and every FF_THEME_MAP_* kind color) was
 * computed, together with WCAG contrast against FF_THEME_COLOR_BG, and
 * the candidate with the largest worst-case margin was kept. This is the
 * same trap issue #43 itself named — "half-solving it produces colours
 * that are nominally distinct and practically identical" — measuring
 * instead of reasoning-harder is this repo's own standing rule
 * (AGENTS.md's "the proxy check"). Every one of the four clears >=25 ΔE
 * from all fourteen other colors this file assigns meaning to (including
 * each other) and >=6:1 contrast against FF_THEME_COLOR_BG, matching the
 * brand four's own visibility. */
#define FF_THEME_CREW_PINK    0xFF5CA8
#define FF_THEME_CREW_TEAL    0x4FD8C4
#define FF_THEME_CREW_VIOLET  0xB08CFF
#define FF_THEME_CREW_GREEN   0x9BE07B
#define FF_THEME_CREW_ORANGE  0xF96306 /* new: vivid orange-red, clear of MAP_MEDICAL(0xFF6B6B)/AMBER family */
#define FF_THEME_CREW_GOLD    0xD3E05C /* new: yellow-green gold, clear of AMBER/STALE_AMBER and CREW_GREEN */
#define FF_THEME_CREW_BLUE    0x7690E5 /* new: periwinkle blue, clear of MAP_POI's slate-blue and CREW_VIOLET */
#define FF_THEME_CREW_MAGENTA 0xF906F9 /* new: hot magenta, clear of CREW_PINK and CREW_VIOLET */

/**
 * Colorblind-safe crew palette (S17 slice a, issue #43 + the toggle in
 * docs/specs/S17-usability-hardening.md) — an Okabe-Ito-derived 8-colour
 * qualitative set: Okabe, M. & Ito, K., "Color Universal Design (CUD) —
 * How to make figures and presentations that are friendly to Colorblind
 * people" (2002), as popularized/tabulated by Wong, B., "Points of view:
 * Color blindness", Nature Methods 8, 441 (2011) — public-domain, the
 * standard reference deutan/protan-safe qualitative 8-colour set (avoids
 * red<->green and blue<->purple confusions; separated primarily by
 * luminance/chroma, not hue alone, per that source's own design goal).
 *
 * "-derived", not verbatim, in exactly one place: the canonical set's
 * 8th entry is black (#000000), chosen for a white page — invisible
 * against this app's near-black FF_THEME_COLOR_BG (a filled black dot on
 * a near-black puck is the one failure this whole slice exists to avoid).
 * Substituted with a pale, low-saturation mauve, which keeps the same
 * "achromatic-leaning, separated mainly by luminance" role the original
 * black played (a near-neutral is trivially hue-safe for any dichromacy)
 * while staying clearly visible on this display. The other seven are the
 * cited hexes UNCHANGED — see the two known-close calls recorded in this
 * slice's PR body (Okabe-Ito's own "orange" sits moderately close to this
 * app's FF_THEME_COLOR_STALE_AMBER chip, and its "sky blue"/"blue" sit
 * moderately close to FF_THEME_MAP_POI) rather than silently retuned,
 * because retuning a cited, externally-verified safe set is exactly the
 * kind of well-meant edit that can quietly undo the verification the
 * citation is for.
 */
#define FF_THEME_CREW_CB_ORANGE  0xE69F00
#define FF_THEME_CREW_CB_SKYBLUE 0x56B4E9
#define FF_THEME_CREW_CB_GREEN   0x009E73 /* "bluish green" */
#define FF_THEME_CREW_CB_YELLOW  0xF0E442
#define FF_THEME_CREW_CB_BLUE    0x0072B2
#define FF_THEME_CREW_CB_VERMILLION 0xD55E00
#define FF_THEME_CREW_CB_PURPLE  0xCC79A7 /* "reddish purple" */
#define FF_THEME_CREW_CB_MAUVE   0xD3B3DB /* substitute for the canonical set's black — see comment above */

/**
 * ff_theme_crew_color — 0xRRGGBB for a dot/member's color_idx, wrapping
 * (modulo) so an out-of-range index degrades to a valid color instead of
 * reading out of bounds — honest-but-safe, never a crash over a display
 * nicety.
 *
 * `colorblind` selects which 8-entry table backs the lookup: the brand
 * palette (false, default) or the colorblind-safe one (true) — S17 slice
 * a's toggle, `ff_settings_t.colorblind`. An EXPLICIT parameter, not a
 * hidden static/global this header flips elsewhere: this header is
 * app-layer (theme/, not core/) but is still included from several
 * independent translation units (scr_radar.c, scr_map.c, scr_settings.c)
 * as a plain header-only inline function with no .c file of its own (see
 * this header's top comment). A file-static "current mode" variable
 * would NOT be shared across those TUs — each would get its own copy,
 * silently desyncing whichever screen last got flipped from whichever
 * screen renders next. A real cross-TU global would need this header to
 * grow a .c file, which is a bigger structural change than a settings
 * toggle warrants. An explicit parameter has neither problem, costs
 * nothing (every call site already has `ff_app_settings_t`/the shell's
 * settings in scope one frame away — see ff_scr_radar_build/
 * ff_scr_map_build's own signatures), and matches this repo's own
 * standing preference for explicit state over hidden globals (ff_radar.h's
 * ff_radar_smooth_t is caller-owned for the identical reason).
 */
static inline uint32_t ff_theme_crew_color(uint8_t color_idx, bool colorblind)
{
    static const uint32_t brand_palette[] = {
        FF_THEME_CREW_PINK,
        FF_THEME_CREW_TEAL,
        FF_THEME_CREW_VIOLET,
        FF_THEME_CREW_GREEN,
        FF_THEME_CREW_ORANGE,
        FF_THEME_CREW_GOLD,
        FF_THEME_CREW_BLUE,
        FF_THEME_CREW_MAGENTA,
    };
    static const uint32_t colorblind_palette[] = {
        FF_THEME_CREW_CB_ORANGE,
        FF_THEME_CREW_CB_SKYBLUE,
        FF_THEME_CREW_CB_GREEN,
        FF_THEME_CREW_CB_YELLOW,
        FF_THEME_CREW_CB_BLUE,
        FF_THEME_CREW_CB_VERMILLION,
        FF_THEME_CREW_CB_PURPLE,
        FF_THEME_CREW_CB_MAUVE,
    };
    uint32_t const *palette = colorblind ? colorblind_palette : brand_palette;
    size_t const n = colorblind ? (sizeof(colorblind_palette) / sizeof(colorblind_palette[0]))
                                 : (sizeof(brand_palette) / sizeof(brand_palette[0]));
    return palette[color_idx % n];
}

/* -------------------------------------------------------------------
 * Map-face (S09) feature-kind colors. Stage features use their OWN
 * `fp_stage_t` color when valid (`ff_app_map_feature_t.color_valid`) —
 * these are the fallback/other-kind palette only, one color per
 * `fp_feature_kind_t` (mirrored app-side as `ff_app_map_kind_t`). No
 * mockup artboards are in-tree for this agent to consult (see this
 * header's top comment) — a good-faith, internally-consistent palette
 * distinct from the crew/status colors above, flagged per AGENTS.md's
 * "note the interpretation" rule. `scr_map.c` owns the kind->color
 * switch itself (not hoisted here as a helper, unlike
 * ff_theme_crew_color) so this header's own dependency footprint stays
 * just lvgl.h/stdint.h rather than pulling in ff_app_state.h for every
 * other screen that includes this file.
 * ------------------------------------------------------------------- */

/* PR #73 review finding #5 (Bailey): the ORIGINAL FF_THEME_MAP_CAMPING
 * (0xB08CFF) was bit-for-bit `FF_THEME_CREW_VIOLET` AND — verified
 * against the real, currently-merged Lost Lands pack — the real
 * festpack's own color for "The Crater" stage (`#b08cff`). Camping,
 * a violet crew dot, and a real stage could all read as the same
 * color on the same small screen the moment camping actually rendered
 * (it didn't before this PR's finding #1 fix — that's how this sat
 * unnoticed). Changed to a warm tan distinct from every other color in
 * this file. The residual, evidenced-but-not-structural collision this
 * does NOT close — a festival's own stage color (pack-authored data,
 * `ff_app_map_feature_t.color_rgb`, not a theme constant) landing on
 * the same hue as a crew member's assigned palette slot — is a
 * genuinely different class of problem (this file has no control over
 * what color a festival picks for its stages) and is exactly the
 * "broader palette rework" Bailey's own finding flags as a follow-up,
 * not a same-PR fix. */
#define FF_THEME_MAP_CAMPING  0xC49A6C /* warm tan/khaki — distinct from crew violet AND the real pack's "Crater" stage color */
#define FF_THEME_MAP_WATER    0x4FD8C4 /* teal — matches CREW_TEAL, intuitively "water" */
#define FF_THEME_MAP_PATH     0x8B8A97 /* == FF_THEME_COLOR_MUTED — paths are neutral chrome, not a destination */
#define FF_THEME_MAP_ENTRANCE 0x9BE07B /* == FF_THEME_COLOR_LIVE_GREEN — "go" */
#define FF_THEME_MAP_VENDOR   0xFFC66B /* == FF_THEME_COLOR_AMBER — warm, food/shop */
#define FF_THEME_MAP_MEDICAL  0xFF6B6B /* new: soft red, the one kind that benefits from standing apart from every other color on this screen */
/* PR #73 review finding #3 (Bailey): the ORIGINAL FF_THEME_MAP_POI was
 * bit-for-bit FF_THEME_COLOR_INK — the same color as every label's own
 * TEXT — so a POI-kind polygon's outline visually merged with the words
 * sitting on top of it (verified on the real pack's "Venue extent" and
 * "Wompy Woods treeline", both kind `poi`). A distinct, muted slate-blue
 * instead — legible as a shape, never confusable with text. */
#define FF_THEME_MAP_POI      0x6B8CAE /* muted slate-blue — generic point of interest, distinct from label text */
#define FF_THEME_MAP_UNKNOWN  0x8B8A97 /* == FF_THEME_COLOR_MUTED — an honest "kind not recognized", never a guessed color */

/* -------------------------------------------------------------------
 * Status-bar alert thresholds (PR #16 code review finding: this cutoff
 * used to live as a bare `<= 15` literal inline in scr_radar.c, an
 * undocumented domain decision hiding in a renderer — CLAUDE.md: "if
 * you're writing an `if` about domain behavior inside a screen file, it
 * belongs in core"). Named and hoisted here so the one existing renderer
 * that reads it, and any future one, share a single definition instead of
 * repeating (and risking drifting) a magic number.
 *
 * `FF_THEME_BATT_LOW_PCT` has no basis in docs/specs/S06-radar-face.md,
 * S02, or S03 — none of those specs define a low-battery threshold at
 * all. 15% is a product-judgment call by this slice's implementer (same
 * "not spec-numeric" category as ff_crew.h's documented
 * FF_CREW_RSSI_TREND_THRESHOLD_DBM), now also written down in
 * docs/specs/S06-radar-face.md's behavior section so it has exactly one
 * source of truth. Both this threshold and mesh-loss share the same
 * amber "alert" treatment (`FF_THEME_COLOR_STALE_AMBER`) — flagged in UX
 * review as an inconsistency when mesh-loss alone rendered as flat grey
 * chrome (losing the mesh radio breaks the entire point of the puck: it
 * should look at least as alarming as a low battery, not less). */
#define FF_THEME_BATT_LOW_PCT 15

/* -------------------------------------------------------------------
 * Type scale — see this header's top comment for the rounding rationale.
 * ------------------------------------------------------------------- */

#define FF_THEME_FONT_NAME     (&lv_font_montserrat_22) /* mockup: 21px */
#define FF_THEME_FONT_DISTANCE (&lv_font_montserrat_36) /* mockup: 36px, mono in spec (see header comment: no mono vendored) */
#define FF_THEME_FONT_CHIP     (&lv_font_montserrat_14) /* mockup: ~11-12px equivalent */
#define FF_THEME_FONT_LABEL    (&lv_font_montserrat_14) /* mockup: ~10-11px equivalent */
#define FF_THEME_FONT_HEADLINE (&lv_font_montserrat_20) /* NOFIX/NOSEL primary message */
/* S24 slice c bugfix (scr_inbox.c: "the thread view looks a bit
 * smashed; the messages could be larger"). The Signals thread's own
 * message-BODY text (bubble text, RALLY place name, the 1:1 flare
 * callout sentence) — design canvas ThreadGroup.dc.html / ThreadPerson.dc.html
 * put this at 15px, distinctly larger than the 12px sender name / 10px
 * mono age around it. Rounds UP to 16 (this header's own never-round-down
 * rule); name/age/the compact CREW event one-liner stay on
 * FF_THEME_FONT_CHIP (14px), already smaller, matching the canvas's own
 * hierarchy. */
#define FF_THEME_FONT_MSG_BODY (&lv_font_montserrat_16) /* mockup: 15px thread body text */

/* -------------------------------------------------------------------
 * Layout — the puck IS the physical round panel: the Waveshare
 * ESP32-S3-Touch-LCD-1.46 glass is 412x412 (CLAUDE.md's authoritative
 * "mockup geometry (412x412 circle)"). S15 slice c (docs/specs/S15c-fit-412.md)
 * corrected the earlier 440/456 drift that overflowed the panel by ~28px and
 * made the display free-scroll on real hardware.
 *
 * FF_THEME_WINDOW_PX == FF_THEME_PUCK_PX == 412: the sim renders the puck at
 * the panel's exact size with NO surrounding window margin, so the sim
 * framebuffer and the device framebuffer are the same 412x412 pixels — sim
 * and glass match pixel-for-pixel (the spec's stated goal). The
 * hit-target sweep derives its circle from these two constants
 * (test_face_hit_targets.c: margin = (WINDOW_PX - PUCK_PX) / 2 == 0, center
 * (206,206), radius 206), and targets/sim/main.c sizes its SDL window /
 * headless framebuffer to FF_THEME_WINDOW_PX so the goldens are that same
 * 412x412 frame.
 * ------------------------------------------------------------------- */

#define FF_THEME_WINDOW_PX 412
#define FF_THEME_PUCK_PX   412 /* the device panel is 412x412; the puck fills it */
#define FF_THEME_PUCK_RADIUS_PX (FF_THEME_PUCK_PX / 2)

/* The VISIBLE glass, measured on the Waveshare ESP32-S3-Touch-LCD-1.46 with the
 * CONFIG_FF_GLASS_RULER pattern (docs/hardware/glass-offset.md, 2026-09-02):
 * the round bezel window sits ~5 px right of the 412-wide pixel array, so
 * columns ~0-5 are under the left bezel and the glass shows ≈ [6..411].
 * The panel cannot be shifted (esp_lcd_panel_set_gap pushes the last
 * columns off the array and they paint white), so edge-hugging elements
 * (the Radar rim tint, anything that must read as concentric with the
 * bezel) centre on THIS point and stay inside THIS radius instead of the
 * framebuffer's (206, 206)/206. Content away from the edge keeps using the
 * puck centre - a 2 px asymmetry is invisible there. Re-measure on any new
 * board; do not assume the SKU is uniform. */
#define FF_THEME_GLASS_CX 208
#define FF_THEME_GLASS_CY 206
#define FF_THEME_GLASS_R  200 /* 203 measured; pulled in 3 px so a ring on it clears the bezel lip on glass (maintainer, 2026-09-02) */

/**
 * ff_theme_glass_cx / ff_theme_glass_cy — the visible-glass centre, as a
 * function of the SCREEN flip (format v8 amendment, maintainer ask,
 * 2026-09-02: the Fusion-designed case mounts the puck upside-down).
 *
 * FF_THEME_GLASS_CX/CY above were measured with the panel in its NORMAL
 * (un-mirrored) orientation: the bezel hides the PHYSICAL left ~5 px of
 * the 412-wide pixel array, so the visible centre sits 2 px right of the
 * framebuffer centre (208 vs. 206). `ff_display_set_flip` (device HAL)
 * flips the panel with a HARDWARE mirror (`esp_lcd_panel_mirror`), not a
 * software/coordinate transform — so from the framebuffer's own point of
 * view nothing moves; what moves is which PHYSICAL edge each framebuffer
 * column ends up on. The bezel itself does not move with the mirror (the
 * case is what flipped, not the glass), so the physical ~5 px the bezel
 * hides is now hiding the framebuffer's RIGHT ~5 px instead of its left —
 * the mirrored visible centre is `FF_THEME_PUCK_PX - FF_THEME_GLASS_CX`
 * (412 - 208 = 204), the reflection of the un-flipped centre across the
 * framebuffer's own midline. FF_THEME_GLASS_CY needs the same treatment
 * for consistency even though it is numerically a no-op here: CY (206)
 * already equals FF_THEME_PUCK_PX/2 (the measured vertical offset was 0 —
 * see docs/hardware/glass-offset.md's board-2 measurement), so its
 * mirror (412 - 206 = 206) is itself; a future board with a genuine
 * non-zero CY offset gets the same correction for free from this one
 * function rather than a hand-derived constant.
 *
 * `flip` is an explicit parameter, not a hidden global — same convention
 * (and the same reasoning) as `ff_theme_crew_color`'s `colorblind`
 * parameter above: this header is a plain, dependency-light, header-only
 * inline function included from multiple independent translation units
 * (scr_radar.c today), so a file-static "current orientation" variable
 * would desync across them. Callers already have `ff_app_settings_t`/the
 * shell's settings one frame away (scr_nav.c threads `screen_flip`
 * through to `ff_scr_radar_build`, exactly as it already does for
 * `colorblind`). GLASS_R is unaffected by flip (a mirror doesn't change
 * a *radius*), so it has no function form — the plain #define still
 * applies in both orientations.
 */
static inline int32_t ff_theme_glass_cx(bool flip)
{
    return flip ? (FF_THEME_PUCK_PX - FF_THEME_GLASS_CX) : FF_THEME_GLASS_CX;
}

static inline int32_t ff_theme_glass_cy(bool flip)
{
    return flip ? (FF_THEME_PUCK_PX - FF_THEME_GLASS_CY) : FF_THEME_GLASS_CY;
}

/* Arrow/ring/dot placement geometry (arrow length & taper, ring radius,
 * dot size) moved to app/screens/radar_layout.h as of PR #16's round-4
 * rework — that module is the single source of truth for every number
 * that also has to be collision-tested (RADAR_LAYOUT_ARROW_LEN_PX,
 * RADAR_LAYOUT_RING_RADIUS_PX, RADAR_LAYOUT_DOT_PX, etc.). scr_radar.c
 * includes radar_layout.h directly for those; nothing in this header
 * duplicates them anymore, precisely so they can't drift apart from what
 * the resolver actually tests against.
 */

#define FF_THEME_FLARE_BTN_H_PX 48 /* S06 spec: "FLARE button (48 px high, full hit area)" */

/* docs/review/ux-raver.md checklist item 2: "every tappable thing >= 44px
 * equivalent". Named here so every tap target in app/screens can be
 * checked against one constant instead of a repeated magic number. */
#define FF_THEME_MIN_HIT_PX 44

/**
 * FF_HIT_MIN_GAP_PX — S17 slice b (docs/specs/S17-usability-hardening.md,
 * AC2): the minimum clearance required between two INDEPENDENT interactive
 * elements' hit-rects, so a fat/gloved/kandi'd thumb can't straddle both
 * and trigger the wrong one. Enforced by
 * targets/sim/tests/test_face_hit_targets.c's sweep, the same build-gate
 * home as FF_THEME_MIN_HIT_PX above.
 *
 * ## Edge-to-edge, not centre-to-centre
 * The sweep measures the shortest distance between the two rects'
 * BOUNDARIES (0 if they overlap on an axis), not the distance between
 * their centers. Centre-to-centre is the wrong quantity for this
 * question: it's biased by each control's own SIZE, not by the dead space
 * between them — a huge chip sitting right next to a tiny icon can have a
 * large center-to-center distance while their edges nearly touch (a real
 * mis-tap risk hidden by size), and conversely two same-size controls
 * read consistently under centre-to-centre only because they happen to
 * match. Edge-to-edge instead measures exactly the physical no-man's-land
 * a thumb would need to land inside of to miss BOTH controls — the literal
 * quantity "can a thumb hit both" is asking about — independent of either
 * control's own footprint.
 *
 * ## Why 8, not 24
 * 8px matches the widely-cited minimum (Google's Material Design touch-
 * target accessibility guidance: "at least 8dp of spacing between two
 * adjacent touch targets") for the smallest gap that reliably avoids an
 * ambiguous double-hit — this is a FLOOR, not a comfort target. It is
 * deliberately smaller than `FF_SETTINGS_CHIP_GAP` (24px, scr_settings.c)
 * — PR #68's fix for the specific double-chip mis-tap Bailey caught by
 * eye — because that 24px was chosen as a GENEROUS separation for two
 * chips sitting directly side by side on the same row, not re-derived as
 * the universal minimum every control everywhere must clear. Call sites
 * remain free (and, per #68, encouraged) to use more than the floor;
 * this constant only draws the line below which a gap is a genuine
 * mis-tap trap the sweep must fail on, at ~0.7mm on this display's own
 * ~11px/mm scale (docs/review/ux-raver.md: "412 px ~= 11 px/mm").
 *
 * ## Scope: independent SIBLING controls only
 * The sweep only compares two clickable elements that (a) share the same
 * immediate LVGL parent and (b) are not the SAME logical control wearing
 * two hit-rects — e.g. scr_settings.c's WATER NUDGE/QUIET HOURS rows,
 * where the dim row LABEL and its own value CHIP are two separate LVGL
 * objects that both forward to the identical callback+argument (one
 * setting, one action, two places to tap it). Two hit-rects for the same
 * action are not a mis-tap risk the way two hit-rects for two DIFFERENT
 * actions are — see test_face_hit_targets.c's own comment for exactly how
 * that's detected (matching registered click-callback + user_data, not a
 * per-screen exception list), so a legitimately tight label/chip pairing
 * can't false-positive this sweep. */
#define FF_HIT_MIN_GAP_PX 8

#ifdef __cplusplus
}
#endif

#endif /* FF_THEME_H */
