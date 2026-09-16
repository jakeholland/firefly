/**
 * scr_settings.c — see scr_settings.h.
 *
 * ## Settings audit 2026-09-03 — four rows hidden, the list sectioned
 * A read-only audit (verified by grep, docs/specs/S11-settings.md's own
 * "Amendments" and docs/specs/S21-settings-rework.md's own "Amendments" both
 * carry the finding) found four rows with NO effect on ANY target:
 *   - **SHARE** (LIVE|GHOST) — `share_mode` is written and projected but
 *     nothing gates outbound position sharing; S11's slice c ("GHOST
 *     admin-message wiring") never landed.
 *   - **HAPTICS** — `cfg.haptic` is NULL on the device and unwired in the
 *     sim; the board has no vibration motor at all.
 *   - **GLOW** — `night_glow` has zero consumers; the launcher's ambient
 *     glow renders unconditionally regardless of this setting.
 *   - **WATER NUDGE** — `ff_water_tick` (core) is implemented and unit-
 *     tested but `ff_shell` never calls it.
 * Rather than delete these rows, each is gated behind its own
 * `FF_SETTINGS_ROW_ENABLE_*` compile-time flag (below) — the persisted
 * field, the `FF_SETTING_*` id, and the shell-level intent handler all stay
 * wired exactly as before; flipping a flag back to 1 is the entire
 * re-enable, no other code change. See each flag's own comment for the
 * spec slice (or the missing hardware) that would re-enable it for real.
 *
 * With those four gone, the remaining rows are grouped under lightweight,
 * non-interactive section headers — DISPLAY (BRIGHTNESS, CLOCK, SCREEN,
 * COLORBLIND), SOUND (SOUNDS, UI TICKS, QUIET HOURS), UNITS (UNITS), DEVICE
 * (CALIBRATE TOUCH) — see `settings_build_section_header`'s own doc comment
 * for the header footprint and why it stays under the ~28px/header target.
 *
 * ## One scrolling list, pinned centered header (S21 restyle)
 * A settings screen is a single vertically-scrolling list of rows, with the
 * header (back button + "SETTINGS" + name) PINNED at the top so "back" is
 * always reachable no matter how far the rows are scrolled. #105 paginated
 * instead, purely because the tap-target sweep
 * (`targets/sim/tests/test_face_hit_targets.c`) read each clickable's
 * ABSOLUTE, scroll-shifted rect and failed any row scrolled off-glass; S21
 * makes that sweep scroll-aware (checks a scroll row against the scroll
 * VIEWPORT, not its momentary absolute position), which removes the only
 * reason pagination existed.
 *
 * ## Round-safe framing — the key constraint on a 412 ROUND puck
 * NOTHING may cross the round glass edge. Two devices enforce that here:
 *   - The header group (title + name) is CENTERED in the top band, not
 *     tucked in a top-left corner — the corners of the square are off-glass
 *     on the physical circle, so a corner-anchored control is clipped by
 *     the bezel. The title and name are built into ONE flex-COLUMN
 *     container, cross-axis centered, so both share the glass's own
 *     vertical axis regardless of their own (different) natural text
 *     widths — see the header-alignment-fix note above `FF_SETTINGS_HDR_Y`.
 *     The container's width is queried from `ff_layout_safe_margin_x`
 *     (same primitive the scroll list below uses), placed low enough that
 *     it clears the r=206 circle at FF_SETTINGS_HDR_Y.
 *   - The scroll rows live in ONE `lv_obj` list container positioned as a
 *     rectangle INSCRIBED in the round glass across its whole height (its
 *     x-inset is `ff_layout_safe_margin_x` evaluated over the container's
 *     full vertical span, i.e. bound by its lower edge — the point nearest
 *     the bottom pole). Every row is a child placed in container-relative
 *     coordinates with a uniform inner width, so any row shown at any scroll
 *     position is on-glass by construction, with no per-row margin math.
 * An OPAQUE BG band behind the header (settings_build_header_band, #bug5)
 * occludes the region above the list viewport with solid ink; the list
 * clips its own children, so a row scrolling to the top ends cleanly at the
 * viewport edge. (It REPLACES the former BG->transparent edge-fade scrims,
 * whose gradient left a hard amber banding edge on the RGB565 panel.) The
 * band is non-clickable chrome and never affects the sweep.
 *
 * ## Scroll position survives an in-place rebuild (#bug4)
 * A settings-change intent tears down and rebuilds the whole screen. The
 * list's scroll offset is remembered (LV_EVENT_SCROLL) and restored after
 * each rebuild, so toggling a row does not jump back to the top. A FRESH
 * entry from another face resets it (ff_scr_settings_reset_scroll, called by
 * the face dispatcher on the not-Settings -> Settings transition).
 *
 * ## One consistent row language
 * Every row reads label LEFT (muted, uppercase), control RIGHT. Controls are
 * PILLS (rounded ~12px):
 *   - Toggle pairs (UNITS FT|MI, CLOCK 12H|24H, SCREEN NORMAL|FLIPPED,
 *     SOUNDS ON|OFF, UI TICKS ON|OFF (S27), COLORBLIND ON|OFF — plus the
 *     hidden SHARE LIVE|GHOST, HAPTICS ON|OFF, GLOW ON|OFF rows, still built
 *     by this file behind their own `FF_SETTINGS_ROW_ENABLE_*` flag, see the
 *     "Settings audit 2026-09-03" note above) render two pills; the
 *     ACTIVE one is amber-on-ink, the inactive one surface-on-muted. Both
 *     pills of a pair
 *     forward to the SAME toggle callback with no user_data, so (a) tapping
 *     either flips the two-state setting and (b) the hit-target sweep treats
 *     them as ONE logical control (its composite-control exclusion keys off
 *     matching cb+user_data), letting the pair sit at a tight ~6px gap
 *     without tripping the 8px adjacency floor two INDEPENDENT controls owe.
 *   - Value rows (QUIET HOURS, plus the hidden WATER NUDGE) render one
 *     surface/ink value pill; the row LABEL is itself a tap target
 *     forwarding to the same callback (no dead left half — PR #68), again
 *     composite with its pill.
 *   - CALIBRATE TOUCH is a full-width surface pill with a thin amber border,
 *     an ACTION (amber text), not a stored value.
 * Every control is a bare `FF_INTENT_*` emitter — range validation and
 * persistence are the shell's (`ff_shell.c`), same "screens stay pure
 * renderers" split every face uses.
 *
 * ## Brightness is its own taller row (a −/+ STEPPER, not a slider)
 * A "BRIGHTNESS" caption + "%" value, a non-interactive amber level bar, and a
 * −/+ stepper group (two lv_button pills). #bug2: brightness is NOT a slider —
 * a draggable control inside a vertical scroll list cannot reliably tell a
 * scroll gesture from an adjust one on this touch panel (the slider captured
 * the press so a vertical drag could not scroll; jump-to-press yanked the
 * value). Two discrete tap targets have no drag semantics, so every drag
 * scrolls the list and only a tap steps brightness (settings_brightness_step,
 * 10%/tap, clamped, committed once per real step — see ff_shell.c's brightness
 * handler; brightness stays out of the shell render key so a step updates the
 * level bar in place instead of rebuilding the face).
 *
 * ## UTC offset is NOT a row here
 * The festpack supplies the timezone (`fp_pack_t.utc_offset_min`), so the
 * manual UTC stepper is gone. `ff_settings.utc_offset_min` / `_set` and the
 * wall-clock logic that reads them are untouched — only the Settings UI for
 * it was removed.
 *
 * ## `my_name` is NOT editable in this slice
 * Renaming needs its own live text-entry session and shell-seam draft field;
 * this file renders the current `my_name` as a caption under the title.
 */
#include "scr_settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h> /* strcmp — S12 amendment: does a paired row's display name differ from its short tag? */

#include "ff_crew.h" /* ff_fmt_age — DIAGNOSTICS page's age formatting */
#include "ff_intent.h" /* the emit seam; FF_INTENT_CALIBRATE_TOUCH */
#include "ff_layout.h"
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST, FF_BRIGHTNESS_*_PCT */
#include "ff_theme.h"
#include "scr_nav.h"     /* S12/S04 — ff_scr_button_create for the CREW page's back button */
#include "scr_widgets.h" /* ff_scr_pill_create — the shared pill factory (S17 debt cleanup) */

/* ---------------------------------------------------------------------
 * Palette roles (redesign spec).
 * ------------------------------------------------------------------- */
#define FF_SETTINGS_PILL_RADIUS 12

/* Active/selected toggle pill: amber fill, near-black ink. */
#define FF_SETTINGS_PILL_ON_BG  FF_THEME_COLOR_AMBER
#define FF_SETTINGS_PILL_ON_FG  FF_THEME_COLOR_BG
/* Inactive toggle pill: surface fill, muted text. */
#define FF_SETTINGS_PILL_OFF_BG FF_THEME_COLOR_SURFACE
#define FF_SETTINGS_PILL_OFF_FG FF_THEME_COLOR_MUTED
/* Value pill: surface fill, primary ink. */
#define FF_SETTINGS_PILL_VAL_BG FF_THEME_COLOR_SURFACE
#define FF_SETTINGS_PILL_VAL_FG FF_THEME_COLOR_INK

/* ---------------------------------------------------------------------
 * Layout constants.
 * ------------------------------------------------------------------- */

#define FF_SETTINGS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

/* --- Pinned header: SETTINGS + the owner's name, stacked as ONE centered
 * block on the glass's own vertical axis (round-safe framing note above).
 *
 * Header-alignment fix: S21's horizontal-carousel rework retired the back
 * button (Settings is a swipe tile now, left by swiping — see this file's
 * top comment), but the header kept reserving that button's gutter and
 * left-anchoring both lines against it — so the two lines merely happened
 * to sit near the puck's center rather than actually being centered on it,
 * and "SETTINGS" (an unconstrained-width label, whose glyph width differs
 * from the fixed text column) drifted visibly right of the shorter, truly
 * left-anchored name below it (`firmware/tests/golden/settings_default.png`
 * before this fix: the name sits left of the title's center). Fixed by
 * building the two labels into ONE flex COLUMN container, cross-axis
 * centered (`LV_FLEX_ALIGN_CENTER`) — each label's own natural width is
 * centered independently within the shared column, so a short name and a
 * wider title share one true vertical axis regardless of glyph width. The
 * container's own width is queried from `ff_layout_safe_margin_x` (same
 * round-safe-framing primitive the scroll list below uses), so it clears
 * the r=206 bezel at FF_SETTINGS_HDR_Y exactly as the old fixed-width
 * block did — just centered on the puck instead of offset for a button
 * that no longer exists. */
#define FF_SETTINGS_HDR_Y       34
#define FF_SETTINGS_HDR_H       52 /* generous band covering title+name, for the safe-margin query below */
#define FF_SETTINGS_HDR_ROW_GAP 2  /* title -> name vertical gap inside the stacked block */

/* The scroll viewport: an inscribed rectangle spanning the MIDDLE band, well
 * clear of the header above and the bottom curve below. Its x-inset is the
 * round-safe margin over its whole span (bound by the lower edge, nearest the
 * bottom pole) so a row shown anywhere in the viewport is on-glass. At the
 * lower edge (y=356) the circle half-width is ~141px -> inner width ~262px
 * after the safety inset — ample for the rows; lower rows simply scroll. */
#define FF_SETTINGS_LIST_Y 100
#define FF_SETTINGS_LIST_H 256 /* 100..356 */

/* Rows — usability-review slice 3 (docs/reviews/puck-ux-usability-2026-09-15.md
 * finding 5; docs/hardware/tap-targets.md's own "Settings list rows ...
 * should be 80, and the face can afford it (the list scrolls)" follow-up)
 * raises this from 48px (4.2mm, guarded only by the 44px absolute floor)
 * to FF_THEME_HIT_PRIMARY_PX (80px, 7.0mm) — the owner's floor for list
 * rows. The list scrolls, so the cost is a shorter visible page, not a
 * clipped one; the CREW row promotion (see ff_scr_settings_build) ships
 * in the SAME change specifically because raising this without it would
 * have made CREW's ~830px scroll worse, not better (finding 9). 14px
 * inter-row gap clears the 8px adjacency floor with real slack, unchanged.
 *
 * Shared with three sub-faces that used to tie their own button height to
 * this constant (SHOW CODE's BACK, the crew-op confirms) — both of those
 * are ALSO named by this slice and grow with it. Two more one-time users
 * of the OLD 48px value did NOT get a slice-3 ask and would break their
 * own (unrelated, untouched-goldens) geometry if left wired to this
 * constant: the CREW sub-page's own action pills/rows
 * (FF_CREW_PAGE_ROW_H, own constant below) and the compass-cal ritual's
 * CANCEL/DONE (FF_CALCAL_BTN_H, own constant below) — both deliberately
 * decoupled, same reasoning as the flare takeover's 80px buttons being
 * "a real design trade, not a free win" in the review: growing either one
 * here collides with content already pinned above it, and neither was
 * asked for by this slice. */
#define FF_SETTINGS_ROW_H   FF_THEME_HIT_PRIMARY_PX
#define FF_SETTINGS_ROW_GAP 14
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP)
/* Every pill this file builds (settings_make_pill) uses FF_SETTINGS_ROW_H as
 * its height, and every pill's width (TOGGLE/SCREEN/VALUE, all below) is
 * wider than that — so this one assert is the binding shorter-dimension
 * floor check for all of them. */
_Static_assert(FF_SETTINGS_ROW_H >= FF_THEME_MIN_HIT_PX, "settings pill rows must clear the 44px hit-target floor");

/* Toggle-pair pills: two >=44px pills at a tight 6px gap (safe because a
 * pair shares one callback — see sweep composite-control exclusion).
 * 58 is sized for "GHOST" (51px at FF_THEME_FONT_CHIP, measured), the
 * longest label any pair other than SCREEN's carries. */
#define FF_SETTINGS_TOGGLE_PILL_W 58
#define FF_SETTINGS_TOGGLE_GAP    6

/* ON/OFF pairs get their OWN, narrower width — PR #311 review,
 * N-series ("Settings pill groups moved 11px left; restore the
 * COLORBLIND caption clearance").
 *
 * The measurement. This pass re-framed every Settings band against the
 * BEZEL's glass instead of the framebuffer's circle, which narrowed the
 * list rows from 262px to 240px — so each right-aligned pill group's
 * LEFT edge moved 22px left (11px per side). Every caption cleared it
 * except the longest one on a toggle row: "COLORBLIND" renders 116px
 * wide at FF_THEME_FONT_LABEL with this file's 2px letter-spacing
 * (lv_text_get_size, measured), and the shared 58px pair puts `grp_x` at
 * 240 - (2*58 + 6) = 118 — a 2px gap, visible as a collision in
 * settings_scrolled_mid.png. It was 24px before the pass.
 *
 * The fix is a width sized to the labels it actually carries, not a
 * per-row nudge: "ON"/"OFF" measure 23px and 30px, so 48px is 9px of
 * padding on the widest of them and still 4px over FF_THEME_MIN_HIT_PX.
 * That puts grp_x at 240 - (2*48 + 6) = 138, clearing "COLORBLIND" by
 * 22px — back above the pre-pass clearance, and checked on the rendered
 * screen by S_SET_every_settings_caption_clears_its_control_group
 * (test_scr_intent.c) rather than by this arithmetic alone. */
#define FF_SETTINGS_ONOFF_PILL_W 48
_Static_assert(FF_SETTINGS_ONOFF_PILL_W >= FF_THEME_MIN_HIT_PX,
               "ON/OFF pills must still clear the 44px hit floor in their short axis");
#define FF_SETTINGS_TOGGLE_GRP_W  (2 * FF_SETTINGS_TOGGLE_PILL_W + FF_SETTINGS_TOGGLE_GAP) /* 122 */

/* SCREEN's own pill width (format v8 amendment): "NORMAL"/"FLIPPED" are
 * longer than every other toggle-pair's label ("GHOST", this row's own
 * previous longest at 5 glyphs, is the runner-up) — at the shared
 * FF_SETTINGS_TOGGLE_PILL_W (58px, sized for "GHOST"), "FLIPPED" visibly
 * overflows the pill on the FF_THEME_FONT_CHIP (Montserrat 14) render (a
 * real, sim-caught defect — not a hypothetical). Widened just for this
 * row's pill pair, right-aligned the same as every other toggle row (only
 * `grp_x` moves left to fit); every other toggle row's pill width and
 * alignment is untouched. Not a hardcoded guess: the row is wide enough
 * (see FF_SETTINGS_LIST_H's own inscribed-rectangle margin) that widening
 * only this row's control group cannot crowd the "SCREEN" caption or cross
 * the round glass edge — the sim's own scroll-aware hit-target sweep
 * (test_face_hit_targets.c) still verifies both pills clear the ≥44px
 * floor and the round-glass containment at this width. */
/* Tap-target sizing pass (2026-09-14): 84 -> 78. The settings rows'
 * width now comes from the BEZEL's glass circle rather than the
 * framebuffer's (settings_safe_margin_x), narrowing every row from 262px
 * to 240px — 11px per side — and this row's pill group is the widest on
 * the face. At 84 that put grp_x at 240 - (2*84 + 6) = 66 against a
 * "SCREEN" caption measuring 68px, a 2px overlap visible in the rendered
 * golden. At 78, grp_x is 78 and the gap is 10px. "FLIPPED" measures
 * 62px at FF_THEME_FONT_CHIP, so it still renders in full with 8px of
 * padding a side, and both pills stay well over the hit floor.
 * (Numbers re-measured for the PR #311 review, which caught the first
 * pass quoting "~70px caption / 4px overlap / 8px gap" from arithmetic
 * rather than from lv_text_get_size.) */
#define FF_SETTINGS_SCREEN_PILL_W 76

/* Value pill (WATER/QUIET): one pill wide enough for "120 MIN"/"4A-10A". */
#define FF_SETTINGS_VALUE_PILL_W 96
#define FF_SETTINGS_VALUE_GAP    12

/* --- Container-relative row y-positions (0 = top of the scroll content). ---
 * BRIGHTNESS leads (its own taller block: caption over a slider); its
 * INTERNAL geometry (cap/slider/stepper, all base-relative) is unchanged by
 * the audit below — only where the whole block SITS in the list moved, and
 * that is now a build-time cursor (see ff_scr_settings_build), not a macro,
 * because the row set below it is no longer a fixed chain (four rows are
 * conditionally compiled out — see FF_SETTINGS_ROW_ENABLE_* below — and the
 * remaining rows are grouped under section headers rather than laid out as
 * one flat run). */
#define FF_SETTINGS_REL_BRIGHT_CAP_Y 0
#define FF_SETTINGS_BRIGHT_CAP_H     22
#define FF_SETTINGS_REL_SLIDER_Y     30
/* Transparent hit strip. Raised from the 44 floor to 56 after field-test:
 * the minimum-size strip was hard to land a drag on. Usability-review
 * slice 3 (docs/reviews/puck-ux-usability-2026-09-15.md, finding 5) grows
 * the -/+ stepper pills to FF_SETTINGS_ROW_H (80) with the rest of the
 * Settings pills, so this control-area height is DERIVED from that
 * instead of restated as its own number — the old relationship (56 vs.
 * the pre-pass 48px row) left 8px of breathing room above/below the
 * pills, and this keeps that same ratio rather than either flush-fitting
 * them or hand-picking a new constant that could silently drift from the
 * row height again. */
#define FF_SETTINGS_SLIDER_H         (FF_SETTINGS_ROW_H + 8)
#define FF_SETTINGS_BRIGHT_BLOCK_H   (FF_SETTINGS_REL_SLIDER_Y + FF_SETTINGS_SLIDER_H)

/* ---------------------------------------------------------------------
 * Settings audit 2026-09-03 — row visibility table.
 *
 * Four rows have NO effect on any target (verified by grep; see the file
 * header comment above and docs/specs/S11-settings.md / S21-settings-
 * rework.md's own "Amendments" for the full audit writeup). Rather than
 * delete them, each is gated behind its own flag here: the persisted
 * `ff_settings_t` field, the `FF_SETTING_*` id, and the shell-level
 * `FF_INTENT_SETTING_SET` handling all stay wired exactly as before —
 * flipping a flag to 1 is the ENTIRE re-enable for the row's own
 * visibility (the flag's own comment below says what else would need to
 * land for the setting to actually do something once visible again).
 * ------------------------------------------------------------------- */
/* share_mode is written and projected but nothing gates outbound position
 * sharing — S11 slice c, "GHOST admin-message wiring" (docs/specs/
 * S11-settings.md), never landed. Re-enable once that slice ships. */
#define FF_SETTINGS_ROW_ENABLE_SHARE   0
/* cfg.haptic is NULL on the device and unwired in the sim; the Waveshare
 * ESP32-S3-Touch-LCD-1.46 board this repo targets has no vibration motor at
 * all. No spec slice re-enables this — it needs a haptic driver added to
 * the BOM first. */
#define FF_SETTINGS_ROW_ENABLE_HAPTICS 0
/* night_glow has zero consumers — the launcher's ambient glow renders
 * unconditionally regardless of this setting. No spec slice re-enables
 * this — it needs the launcher's glow effect gated on the flag first. */
#define FF_SETTINGS_ROW_ENABLE_GLOW    0
/* ff_water_tick (core/ff_water.h) is implemented and unit-tested but
 * ff_shell never calls it — S11's own "Water nudge" behavior section
 * ("haptic + toast every water_min while awake, suppressed in quiet
 * hours") describes the wiring that would re-enable this; it never
 * landed. */
#define FF_SETTINGS_ROW_ENABLE_WATER   0

/* ---------------------------------------------------------------------
 * Section header footprint (settings_build_section_header below): a single
 * small-caps text line plus the gap to its section's first row. The 14px
 * gap ABOVE a header (separating it from the previous section's last row)
 * reuses FF_SETTINGS_ROW_GAP — the same gap that already sits between any
 * two rows, not an extra cost, so it is not counted below. What a header
 * actually ADDS over "no header, just a row gap" is HDR_H + HDR_GAP:
 * 18 + 8 = 26px, under the ~28px/header target. HDR_GAP (not HDR_H) is
 * sized to the sweep's own 8px adjacency floor (`FF_HIT_MIN_GAP_PX`,
 * ff_theme.h) — a header isn't CLICKABLE so the sweep never checks it
 * directly, but 8px keeps the header from ever visually reading as part of
 * the control below it, the same property the floor enforces between two
 * real controls. ------------------------------------------------------- */
#define FF_SETTINGS_SECTION_HDR_H   18
#define FF_SETTINGS_SECTION_HDR_GAP 8
#define FF_SETTINGS_SECTION_BLOCK_H (FF_SETTINGS_SECTION_HDR_H + FF_SETTINGS_SECTION_HDR_GAP) /* 26 */

/**
 * settings_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout_safe_margin_x, bound to this puck's own center/radius and this
 * file's safety buffer — identical shape to scr_compose.c's
 * compose_safe_margin_x. Called ONCE, over the scroll container's whole
 * vertical span, to inset the viewport rectangle inside the round glass (its
 * lower edge, nearest the bottom pole, binds).
 */
static int32_t settings_safe_margin_x(int32_t top_y, int32_t h)
{
    /* Tap-target sizing pass (2026-09-14): measured against the BEZEL's
     * visible glass (FF_THEME_GLASS_CX/CY/R = 208/206/200), not the
     * framebuffer's own inscribed circle (206,206,206). This is the fix
     * for the finding Maya's puck review named on this exact face — "the
     * SETTINGS title card in settings_default.png has visible flat
     * 'shoulders' near the top corners that would be sliced by the bezel
     * edge": the header band sits at y=34, where the two circles differ
     * by ~11px of half-width, so the card was framed to a circle 6px
     * larger than the one the user can actually see. Every band this
     * helper insets (the header card, the scroll viewport, CREW, the
     * name editor, diagnostics, compass-cal) narrows by the same honest
     * amount; nothing moves vertically. See ff_layout_bezel_margin_x. */
    float margin = ff_layout_bezel_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                             (float)FF_THEME_GLASS_CX, (float)FF_THEME_GLASS_CY,
                                             (float)FF_THEME_GLASS_R, FF_SETTINGS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Static build-time snapshot — same convention as scr_compose.c's `s_mode`:
 * every callback computes "current -> next" from the settings this screen was
 * built with; a tap only ever reports through the intent seam.
 * ------------------------------------------------------------------- */
static ff_app_settings_t s_settings;

/* ---------------------------------------------------------------------
 * Scroll-position preservation across the in-place rebuild every
 * settings-change intent triggers (#bug4). `ff_scr_settings_build` fully
 * tears down and rebuilds at scroll 0, so without this a toggle jumped the
 * list back to the top. We remember the live list's scroll offset (updated
 * on every LV_EVENT_SCROLL) and restore it after each rebuild. A FRESH
 * entry into Settings from another face resets it to 0 via
 * ff_scr_settings_reset_scroll (called by the face dispatcher on the
 * not-Settings -> Settings transition) — see this file's callers.
 * ------------------------------------------------------------------- */
static lv_obj_t *s_list;      /* the live scroll list (NULL before first build / after teardown) */
static int32_t s_scroll_y;    /* last observed vertical scroll offset, restored on rebuild */

/* fix/diag-scroll-persist — the SAME #bug4 preservation, a separate
 * offset for the DIAGNOSTICS page's OWN list. Not folded into
 * `s_scroll_y`/`s_list` above even though `settings_build_diag_page`
 * also points `s_list` at its own list (a harmless share — see that
 * assignment's own comment, it only feeds the sim scroll-hint hook):
 * `s_scroll_y` is restored by the PLAIN list's build only (this file's
 * final `lv_obj_t *puck = ...` branch), so if Diagnostics wrote into it
 * too, leaving Diagnostics scrolled and returning to the plain list (a
 * shorter, differently-laid-out list) would restore the WRONG list's
 * offset the moment the user backs out — a cross-page bleed neither
 * page's content has anything to do with the other's. A page of its
 * own offset avoids that by construction. */
static int32_t s_diag_scroll_y;

/* ---------------------------------------------------------------------
 * Brightness stepper (#bug2). Brightness is a −/+ stepper, NOT a slider: a
 * draggable control inside a vertical scroll list fights the list's own scroll
 * gesture (the slider captured the press, so a vertical drag starting on it
 * could not scroll — many device rounds confirmed no reliable way to
 * disambiguate). Two discrete tap targets (lv_button CLICKED) have no drag
 * semantics at all, so every drag scrolls the list and only a tap steps
 * brightness — the conflict cannot exist. A non-interactive amber level bar +
 * "%" label show the value; both are updated on each step.
 * ------------------------------------------------------------------- */
static lv_obj_t *s_bright_fill; /* amber level-bar fill (width ∝ pct); non-interactive */
static lv_obj_t *s_bright_pct;  /* the "NN%" label */
static int32_t s_bright_bar_w;  /* the level bar's full width (the fill spans a fraction of it) */
#define FF_SETTINGS_BRIGHT_STEP 10 /* percent added/removed per −/+ tap */
#define FF_SETTINGS_DRAG_LOCK_PX 8     /* horizontal travel before a drag counts as a brightness adjust */

/* LV_EVENT_SCROLL — remember where the user scrolled to, so the next in-place
 * rebuild (a settings-change intent tears down and rebuilds the whole screen)
 * can restore it instead of snapping to the top (#bug4). Kept light: no repaint
 * here, so scrolling stays smooth (an every-frame full-screen invalidate made
 * scrolling laggy). */
static void settings_scroll_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target(e);
    s_scroll_y = lv_obj_get_scroll_y(list);
    /* #bug5 — repaint each scroll frame so the moving amber elements (brightness
     * fill, an active pill, the Calibrate border) leave no partial-strip-flush
     * residue during an active scroll. A LIST-only repaint left residue in the
     * round-glass margins BESIDE the list (the amber bleeds past the row edges),
     * and a whole-SCREEN repaint per frame was laggy — so repaint a FULL-WIDTH
     * band at just the list's height: it covers the sideways bleed but skips the
     * header, staying smooth. */
    lv_area_t band = {.x1 = 0,
                      .y1 = FF_SETTINGS_LIST_Y,
                      .x2 = FF_THEME_PUCK_PX - 1,
                      .y2 = FF_SETTINGS_LIST_Y + FF_SETTINGS_LIST_H - 1};
    lv_obj_invalidate_area(lv_screen_active(), &band);
}

/* LV_EVENT_SCROLL_END — the scroll has settled. Repaint the whole screen ONCE
 * so any partial-strip-flush residue on the device is overpainted, without the
 * per-frame cost that made scrolling laggy (#bug5). Sim-invisible (goldens
 * render at a fixed offset with no live scroll). */
static void settings_scroll_end_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_invalidate(lv_screen_active());
}

/* fix/diag-scroll-persist — the DIAGNOSTICS page's own LV_EVENT_SCROLL
 * handler, into `s_diag_scroll_y` rather than `settings_scroll_cb`'s
 * `s_scroll_y` (see that variable's own doc comment for why the two
 * pages need separate offsets). No per-frame band invalidate here: that
 * repaint in `settings_scroll_cb` exists ONLY to cover moving amber
 * decorations (brightness fill, active pill, the Calibrate border)
 * bleeding past their row edges (#bug5) — Diagnostics renders nothing
 * but plain clipped label text (`settings_diag_line`, LV_LABEL_LONG_DOT,
 * SCROLLABLE cleared on every row), so LVGL's own scroll-invalidate
 * already repaints it correctly with no residue to chase. */
static void settings_diag_scroll_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target(e);
    s_diag_scroll_y = lv_obj_get_scroll_y(list);
}

/* ---------------------------------------------------------------------
 * Generic int-setting emitter — every control below funnels through this.
 * ------------------------------------------------------------------- */
static void settings_emit_int(ff_setting_id_t id, int32_t v)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = id;
    in.u.setting.v.i = v;
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Pill widget: a rounded button with a centered label. bg/fg carry the pill
 * role (active / inactive / value). `letter_space` is applied to the label.
 * A thin adapter over the shared `ff_scr_pill_create` (scr_widgets.h, S17
 * debt cleanup) and absolute positioning (settings lays out every row in
 * list-relative coords, not centered-with-offset like scr_flare.c/
 * scr_power_menu.c's pills).
 *
 * puck-ux-usability-review slice 1, finding 2: this used to build every
 * pill with `FF_SCR_PILL_PRESS_NONE` — "no press-feedback style", the
 * exact gap the review measured as "22 controls on settings_default
 * alone" with nothing between finger-down and the screen changing.
 * `filled` is unconditionally `true` here (this function never builds
 * an outlined pill — see `settings_make_toggle_pill` for the one shape
 * that does), so `FF_SCR_PILL_PRESS_DIM` (the ink-wash-on-press
 * convention every other filled pill in this codebase already uses) is
 * the right, universal choice — not a per-call-site decision. Rest-state
 * rendering (what every committed golden captures) is untouched: a
 * press style only ever paints at LV_STATE_PRESSED.
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_pill(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t bg_hex, uint32_t fg_hex, int32_t letter_space, lv_event_cb_t cb,
                                     void *user_data)
{
    ff_scr_pill_cfg_t cfg = {
        .w = w,
        .h = h,
        .use_pos = true,
        .x = x,
        .y = y,
        .radius = FF_SETTINGS_PILL_RADIUS,
        .filled = true,
        .bg_hex = bg_hex,
        .fg_hex = fg_hex,
        .press = FF_SCR_PILL_PRESS_DIM,
        .font = FF_THEME_FONT_CHIP,
        .letter_space = letter_space,
        .cb = cb,
        .user_data = user_data,
    };
    return ff_scr_pill_create(parent, text, &cfg);
}

/* ---------------------------------------------------------------------
 * Toggle-segment pill — the puck-ux-usability-review slice 1 fix
 * (docs/reviews/puck-ux-usability-2026-09-15.md, findings 2 + 3 + the
 * §5 "SURFACE chip/card is invisible" finding, bundled into slice 1's
 * own fix-plan bullet list). Unlike `settings_make_pill` above (used by
 * every OTHER pill in this file: value chips, nav rows, keys — none of
 * which change here), a toggle segment now carries real state:
 *
 *  - `active`: the amber, solid-filled segment (`FF_SCR_PILL_PRESS_DIM`
 *    — an ink wash on press, same convention as every other filled pill
 *    in this codebase).
 *  - `!active`: an OUTLINED pill (surface fill + a 1px MUTED border)
 *    instead of the old flat SURFACE fill with no border at all. The
 *    review measured `SURFACE` on `BG` at 1.07:1 — "you cannot see
 *    where the button is, only where the word is" — `MUTED` at 5.78:1
 *    fixes that. Presses tint toward AMBER (`FF_SCR_PILL_PRESS_TINT`):
 *    what a tap on the inactive segment is about to make it become.
 *
 * Both segments of a pair still share `cb` with `user_data = NULL` —
 * see settings_build_toggle_row_ex's own doc comment on why that
 * identity must not change. */
static lv_obj_t *settings_make_toggle_pill(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w,
                                           int32_t h, bool active, lv_event_cb_t cb)
{
    ff_scr_pill_cfg_t cfg = {
        .w = w,
        .h = h,
        .use_pos = true,
        .x = x,
        .y = y,
        .radius = FF_SETTINGS_PILL_RADIUS,
        .filled = active,
        .border_width = active ? 0 : 1,
        .bg_hex = active ? FF_SETTINGS_PILL_ON_BG : FF_THEME_COLOR_MUTED,
        .fg_hex = active ? FF_SETTINGS_PILL_ON_FG : FF_SETTINGS_PILL_OFF_FG,
        .press = active ? FF_SCR_PILL_PRESS_DIM : FF_SCR_PILL_PRESS_TINT,
        .press_tint_hex = FF_THEME_COLOR_AMBER,
        .font = FF_THEME_FONT_CHIP,
        .cb = cb,
        .user_data = NULL,
    };
    return ff_scr_pill_create(parent, text, &cfg);
}

/* ---------------------------------------------------------------------
 * Row container — a transparent, non-clickable, non-scrolling box the row's
 * label + control(s) live inside, positioned in list-relative coords.
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_SETTINGS_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* A plain left caption (uppercase, muted), vertically centered in the row. */
static void settings_row_caption(lv_obj_t *row, char const *text)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

/* ---------------------------------------------------------------------
 * Toggle-pair row: label + two pills [left|right], the active one amber.
 * BOTH pills share `cb` with NULL user_data — one logical two-state control
 * (tap either to SET it — see each `cb`'s own doc comment, puck-ux-
 * usability-review slice 1 finding 3) and one composite pair to the
 * adjacency sweep (`test_face_hit_targets.c`'s Exclusion 1: same
 * callback + same user_data == one logical control, so the pair's 6px
 * gap stays legal against the 8px floor). Resolving WHICH segment was
 * pressed inside `cb` via `user_data` instead would break that identity
 * — see settings_toggle_pressed_is_left's own doc comment.
 * active_side: 0 = left pill active, 1 = right, -1 = neither (honest render
 * of a persisted value that maps to neither shown option).
 * ------------------------------------------------------------------- */
static void settings_build_toggle_row_ex(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                         char const *left_text, char const *right_text, int active_side,
                                         int32_t pill_w, lv_event_cb_t cb)
{
    lv_obj_t *row = settings_make_row(list, rel_y, row_w);
    settings_row_caption(row, label);

    int32_t const grp_w = 2 * pill_w + FF_SETTINGS_TOGGLE_GAP;
    int32_t const grp_x = row_w - grp_w;

    settings_make_toggle_pill(row, left_text, grp_x, 0, pill_w, FF_SETTINGS_ROW_H, active_side == 0, cb);
    settings_make_toggle_pill(row, right_text, grp_x + pill_w + FF_SETTINGS_TOGGLE_GAP, 0, pill_w, FF_SETTINGS_ROW_H,
                              active_side == 1, cb);
}

static void settings_build_toggle_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                      char const *left_text, char const *right_text, int active_side,
                                      lv_event_cb_t cb)
{
    settings_build_toggle_row_ex(list, rel_y, row_w, label, left_text, right_text, active_side,
                                 FF_SETTINGS_TOGGLE_PILL_W, cb);
}

/* settings_hit_add_press_feedback — puck-ux-usability-review slice 1,
 * finding 2: two rows (this value-row caption wrapper and
 * settings_build_name_row's own identical wrapper below) build their
 * clickable hit box as a bare `lv_obj_create` + `lv_obj_remove_style_all`
 * — completely transparent at rest, with no `ff_scr_pill_create` under
 * it to carry a press style. Same ink-wash convention every filled pill
 * in this codebase uses (`FF_SCR_PILL_PRESS_DIM`'s own INK @ LV_OPA_20),
 * and the same "the invisible hit box IS the press overlay" shape
 * `scr_inbox.c`'s caption-wrapping hit box already uses — nothing here
 * paints at rest (still LV_OPA_TRANSP there), only at LV_STATE_PRESSED. */
static void settings_hit_add_press_feedback(lv_obj_t *hit)
{
    lv_obj_set_style_bg_color(hit, lv_color_hex(FF_THEME_COLOR_INK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hit, LV_OPA_20, LV_STATE_PRESSED);
}

/* ---------------------------------------------------------------------
 * Value row: a clickable label (no dead left half) + one value pill, both
 * wired to the same `cb` (composite to the sweep). `dim` renders the pill
 * muted instead of ink (an honest "off"/unset value).
 * ------------------------------------------------------------------- */
static void settings_build_value_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                     char const *value, bool dim, lv_event_cb_t cb)
{
    lv_obj_t *row = settings_make_row(list, rel_y, row_w);

    int32_t const label_w = row_w - FF_SETTINGS_VALUE_PILL_W - FF_SETTINGS_VALUE_GAP;

    /* Clickable hit box wrapping the caption (its DIRECT child is the label —
     * the label-tap test keys off parent(label) being clickable). */
    lv_obj_t *hit = lv_obj_create(row);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, label_w, FF_SETTINGS_ROW_H);
    lv_obj_set_pos(hit, 0, 0);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);
    settings_hit_add_press_feedback(hit);

    lv_obj_t *lbl = lv_label_create(hit);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    uint32_t const fg = dim ? FF_THEME_COLOR_MUTED : FF_SETTINGS_PILL_VAL_FG;
    settings_make_pill(row, value, row_w - FF_SETTINGS_VALUE_PILL_W, 0, FF_SETTINGS_VALUE_PILL_W, FF_SETTINGS_ROW_H,
                       FF_SETTINGS_PILL_VAL_BG, fg, 0, cb, NULL);
}

/* ---------------------------------------------------------------------
 * settings_toggle_pressed_is_left — puck-ux-usability-review slice 1,
 * finding 3: "tapping the already-active segment of a Settings toggle
 * inverts the setting" (SCREEN NORMAL/FLIPPED's worst case: one
 * confirming tap on the already-lit NORMAL flips the display upside
 * down). Every toggle callback below must SET the tapped segment's
 * value, never invert the current one — which means each callback needs
 * to know WHICH of its row's two pills was actually pressed.
 *
 * The obvious fix — give the two pills distinct `user_data` — is the
 * trap the review calls out by name (§5): `test_face_hit_targets.c`'s
 * adjacency sweep identifies "one logical composite control" by an
 * EXACT (callback, user_data) match (its Exclusion 1), which is the
 * only reason the pair's 6px gap is legal against the 8px floor. Two
 * different `user_data` values would make the sweep see two
 * independent controls 6px apart — a fresh violation with zero pixels
 * moved. So `cb` and `user_data` (always NULL) stay exactly as they
 * are, and this resolves the pressed side from the EVENT TARGET's own
 * position instead: `settings_build_toggle_row_ex` always builds
 * exactly two pills, left then right, as the LAST two children it adds
 * to `row` (the caption label is added first and never after) — so the
 * left pill's `lv_obj_get_index()` is always exactly one less than the
 * right pill's, and the right pill is always the row's last child.
 * Comparing the pressed object's index against "is it the row's last
 * child" tells left from right without needing to know either pill's
 * absolute index. */
static bool settings_toggle_pressed_is_left(lv_event_t *e)
{
    lv_obj_t *pressed = lv_event_get_target(e);
    lv_obj_t *row = lv_obj_get_parent(pressed);
    int32_t const last_idx = (int32_t)lv_obj_get_child_cnt(row) - 1;
    return lv_obj_get_index(pressed) != last_idx;
}

/* settings_emit_toggle — the SET-not-invert core. Emits
 * FF_INTENT_SETTING_SET only when the tapped segment's resolved value
 * actually DIFFERS from the setting's current value — pressing the
 * already-active segment is a genuine no-op (slice 1 acceptance
 * criterion 2: "a synthetic press on the active segment ... emits ZERO
 * FF_INTENT_SETTING_SET"), not an emit-with-the-same-value that merely
 * looks like one from the outside. */
static void settings_emit_toggle(ff_setting_id_t id, int32_t current, int32_t v)
{
    if (v == current) {
        return;
    }
    settings_emit_int(id, v);
}

/* ---------------------------------------------------------------------
 * UNITS (FT|MI). left=FT (imperial=1), right=M (imperial=0) — matches
 * settings_build_toggle_row's own active_side argument at this row's
 * call site (`imperial ? 0 : 1`: imperial true lights the LEFT/FT pill).
 * ------------------------------------------------------------------- */
static void settings_units_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_IMPERIAL, s_settings.imperial ? 1 : 0, settings_toggle_pressed_is_left(e) ? 1 : 0);
}

/* ---------------------------------------------------------------------
 * CLOCK (12H|24H) — S21 amendment. left=12H (clock_24h=0), right=24H
 * (clock_24h=1) — matches this row's `clock_24h ? 1 : 0` active_side.
 * ------------------------------------------------------------------- */
static void settings_clock_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_CLOCK_24H, s_settings.clock_24h ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * SCREEN (NORMAL|FLIPPED) — format v8 amendment (maintainer ask,
 * 2026-09-02): the Fusion-designed case mounts the puck upside-down.
 * left=NORMAL (screen_flip=0), right=FLIPPED (screen_flip=1) — matches
 * this row's `screen_flip ? 1 : 0` active_side. The device applies a
 * HARDWARE panel mirror on change (app_main.c reads the shell's
 * projected screen_flip every tick, same pattern brightness_pct's live
 * apply already uses) — this row only ever emits the intent, never
 * touches display HAL.
 * ------------------------------------------------------------------- */
static void settings_screen_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_SCREEN_FLIP, s_settings.screen_flip ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * SHARE (LIVE|GHOST). left=LIVE, right=GHOST — a direct SET now (see
 * settings_toggle_pressed_is_left's doc comment), which also gives the
 * ZONES-persisted case (PR #68 UX review, blocking finding 1: ZONES
 * doesn't change sharing behavior from LIVE in v1, so this row only ever
 * shows LIVE/GHOST) an honest tap-to-set instead of the old "tapping
 * EITHER pill from ZONES always lands on GHOST" cycle. (ZONES itself
 * never equals either resolved value, so a ZONES-persisted tap always
 * emits — there is no "already active" segment to no-op against.)
 *
 * Settings audit 2026-09-03: this row is HIDDEN
 * (FF_SETTINGS_ROW_ENABLE_SHARE == 0, see that flag's own comment) — the
 * callback below is only ever wired by the row builder inside that same
 * #if, so it is guarded identically here (an unused static function is a
 * build error under -Werror). Re-enabling the row (flip the flag) brings
 * this callback back automatically; no separate step.
 * ------------------------------------------------------------------- */
#if FF_SETTINGS_ROW_ENABLE_SHARE
static void settings_share_cb(lv_event_t *e)
{
    bool const left = settings_toggle_pressed_is_left(e);
    int32_t const v = left ? FF_SHARE_LIVE : FF_SHARE_GHOST;
    settings_emit_toggle(FF_SETTING_SHARE_MODE, (int32_t)s_settings.share_mode, v);
}
#endif

/* ---------------------------------------------------------------------
 * HAPTICS / SOUNDS / UI TICKS / GLOW / COLORBLIND — plain booleans, all
 * built "left=ON, right=OFF" with `active_side = <field> ? 0 : 1` at
 * their call sites (true lights the LEFT/ON pill) — so left pressed
 * means SET true, right pressed means SET false, for every one of them.
 * HAPTICS and GLOW are hidden rows (settings audit 2026-09-03); their
 * callbacks are guarded the same way SHARE's is above.
 * ------------------------------------------------------------------- */
#if FF_SETTINGS_ROW_ENABLE_HAPTICS
static void settings_haptics_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_HAPTICS, s_settings.haptics ? 1 : 0, settings_toggle_pressed_is_left(e) ? 1 : 0);
}
#endif

/* SOUNDS (S27, docs/specs/S27-sounds.md) — the master switch for every
 * sound this puck plays. Same two-state toggle shape as HAPTICS above. */
static void settings_sounds_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_SOUNDS_ON, s_settings.sounds_on ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 1 : 0);
}

/* UI TICKS (S27) — the second, TAP-only gate. Same two-state toggle
 * shape; defaults OFF (ff_settings.h's doc comment on the field). */
static void settings_ui_ticks_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_UI_TICKS, s_settings.ui_ticks ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 1 : 0);
}

#if FF_SETTINGS_ROW_ENABLE_GLOW
static void settings_night_glow_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_NIGHT_GLOW, s_settings.night_glow ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 1 : 0);
}
#endif

static void settings_colorblind_cb(lv_event_t *e)
{
    settings_emit_toggle(FF_SETTING_COLORBLIND, s_settings.colorblind ? 1 : 0,
                         settings_toggle_pressed_is_left(e) ? 1 : 0);
}

/* ---------------------------------------------------------------------
 * WATER NUDGE — label + value pill cycling the spec's v1 presets.
 *
 * Settings audit 2026-09-03: this row is HIDDEN
 * (FF_SETTINGS_ROW_ENABLE_WATER == 0) — every symbol below is only used by
 * its own row builder inside that same #if, so the whole block is guarded
 * together (an unused static function/array is a build error under
 * -Werror). Re-enabling the row (flip the flag) brings all of it back.
 * ------------------------------------------------------------------- */
#if FF_SETTINGS_ROW_ENABLE_WATER
static uint16_t const kWaterPresets[] = {0, 45, 90, 120};
enum { N_WATER_PRESETS = sizeof(kWaterPresets) / sizeof(kWaterPresets[0]) };

static uint16_t settings_next_water(uint16_t current)
{
    for (size_t i = 0; i < N_WATER_PRESETS; i++) {
        if (kWaterPresets[i] == current) return kWaterPresets[(i + 1) % N_WATER_PRESETS];
    }
    return kWaterPresets[0]; /* a persisted value outside the v1 cycle resets onto it */
}

static void settings_water_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_WATER_MIN, settings_next_water(s_settings.water_min));
}

static void settings_water_label(char *buf, size_t n, uint16_t water_min)
{
    if (water_min == 0) {
        snprintf(buf, n, "OFF");
    } else {
        snprintf(buf, n, "%u MIN", (unsigned)water_min);
    }
}
#endif

/* ---------------------------------------------------------------------
 * QUIET HOURS — label + value pill cycling the spec's v1 presets. Each
 * preset sets BOTH quiet_from_min/quiet_to_min, so a tap emits two
 * FF_INTENT_SETTING_SET; the shell persists once per changed field.
 * ------------------------------------------------------------------- */
typedef struct {
    uint16_t from_min;
    uint16_t to_min;
    char const *label;
} settings_quiet_preset_t;

static settings_quiet_preset_t const kQuietPresets[] = {
    {0, 0, "OFF"},
    {120, 480, "2A-8A"},
    {240, 600, "4A-10A"},
};
enum { N_QUIET_PRESETS = sizeof(kQuietPresets) / sizeof(kQuietPresets[0]) };

static settings_quiet_preset_t const *settings_next_quiet(uint16_t from_min, uint16_t to_min)
{
    for (size_t i = 0; i < N_QUIET_PRESETS; i++) {
        if (kQuietPresets[i].from_min == from_min && kQuietPresets[i].to_min == to_min) {
            return &kQuietPresets[(i + 1) % N_QUIET_PRESETS];
        }
    }
    return &kQuietPresets[0];
}

static settings_quiet_preset_t const *settings_current_quiet(uint16_t from_min, uint16_t to_min)
{
    for (size_t i = 0; i < N_QUIET_PRESETS; i++) {
        if (kQuietPresets[i].from_min == from_min && kQuietPresets[i].to_min == to_min) {
            return &kQuietPresets[i];
        }
    }
    return NULL; /* a persisted value outside the v1 cycle: render honestly, don't fake a preset name */
}

static void settings_quiet_cb(lv_event_t *e)
{
    (void)e;
    settings_quiet_preset_t const *next = settings_next_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    settings_emit_int(FF_SETTING_QUIET_FROM_MIN, next->from_min);
    settings_emit_int(FF_SETTING_QUIET_TO_MIN, next->to_min);
}

/* ---------------------------------------------------------------------
 * BRIGHTNESS — a "BRIGHTNESS" caption + "%" value, a non-interactive amber
 * level bar, and a −/+ stepper (two lv_button pills). Each −/+ tap steps the
 * value and emits it committed; see settings_brightness_step (#bug2).
 * ------------------------------------------------------------------- */
static uint8_t settings_brightness_clamped(void)
{
    uint32_t v = s_settings.brightness_pct;
    if (v < FF_BRIGHTNESS_MIN_PCT) v = FF_BRIGHTNESS_MIN_PCT;
    if (v > FF_BRIGHTNESS_MAX_PCT) v = FF_BRIGHTNESS_MAX_PCT;
    return (uint8_t)v;
}

/* Emit a brightness setting. The `transient` flag (#bug1) distinguishes a live
 * preview (the shell applies it to the projected value so the backlight follows,
 * but does NOT write NVS) from a committed value (persisted once). The −/+
 * stepper always emits COMMITTED (discrete taps can't thrash NVS the way a live
 * drag would); the transient path is retained on the intent for a possible
 * future live control. Either way brightness is kept OUT of the shell render key
 * (see ff_shell.c's FF_SETTING_BRIGHTNESS handler + shell_render_key note) so a
 * value change reprograms the backlight without forcing a face rebuild. */
static void settings_emit_brightness(int32_t v, bool transient)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = FF_SETTING_BRIGHTNESS;
    in.u.setting.v.i = v;
    in.u.setting.transient = transient;
    ff_intent_emit(&in);
}

/* Update the amber level-bar fill width and the "%" label to `pct`. Pure screen
 * work; the fill spans frac(pct) of the bar's full width. */
static void settings_brightness_update_level(uint8_t pct)
{
    if (s_bright_pct != NULL) {
        char pctbuf[8];
        snprintf(pctbuf, sizeof(pctbuf), "%u%%", (unsigned)pct);
        lv_label_set_text(s_bright_pct, pctbuf);
    }
    if (s_bright_fill != NULL) {
        float const frac =
            (float)(pct - FF_BRIGHTNESS_MIN_PCT) / (float)(FF_BRIGHTNESS_MAX_PCT - FF_BRIGHTNESS_MIN_PCT);
        int32_t w = (int32_t)lroundf(frac * (float)s_bright_bar_w);
        if (w < 1) w = 1;
        lv_obj_set_width(s_bright_fill, w);
    }
}

/* A −/+ tap steps brightness by `delta`, clamped to [MIN, MAX]. Each tap is a
 * committed change (persisted once); discrete taps can't thrash NVS the way a
 * drag would, so there is no transient/commit split here. Brightness stays out
 * of the shell render key (#bug1), so this updates the level in place rather
 * than rebuilding — and repaints once so the shrinking fill leaves no residue
 * (#bug5). */
static void settings_brightness_step(int32_t delta)
{
    uint8_t const cur = settings_brightness_clamped();
    int32_t v = (int32_t)cur + delta;
    if (v < (int32_t)FF_BRIGHTNESS_MIN_PCT) v = (int32_t)FF_BRIGHTNESS_MIN_PCT;
    if (v > (int32_t)FF_BRIGHTNESS_MAX_PCT) v = (int32_t)FF_BRIGHTNESS_MAX_PCT;
    if ((uint8_t)v == cur) {
        return; /* boundary no-op (− at MIN, + at MAX): nothing changed, don't emit/persist */
    }
    s_settings.brightness_pct = (uint8_t)v; /* keep the local copy in step for the next tap */
    settings_brightness_update_level((uint8_t)v);
    settings_emit_brightness(v, false /* committed: each step persists once */);
    lv_obj_invalidate(lv_screen_active());
}

static void settings_brightness_minus_cb(lv_event_t *e)
{
    (void)e;
    settings_brightness_step(-FF_SETTINGS_BRIGHT_STEP);
}

static void settings_brightness_plus_cb(lv_event_t *e)
{
    (void)e;
    settings_brightness_step(+FF_SETTINGS_BRIGHT_STEP);
}

/* A bare non-clickable decoration box. */
static lv_obj_t *settings_deco_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg_hex,
                                   int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void settings_build_brightness(lv_obj_t *list, int32_t row_w, int32_t rel_y)
{
    uint8_t const pct = settings_brightness_clamped();

    /* A full-width base covering the whole brightness block, exactly like the
     * toggle rows' settings_make_row (#bug2). Its SCROLL_CHAIN (default, not
     * cleared) means a press ANYWHERE on the block chains to the list scroll —
     * without it, the block's only objects are a 6px level bar and some labels,
     * so a drag on the empty space around them landed on nothing scrollable and
     * the list would not scroll while the brightness row was on screen. The −/+
     * pills sit on top and still take their taps. `rel_y` is this block's
     * position in the LIST (the caller's running cursor); every child inside
     * `base` below stays positioned relative to `base` itself
     * (FF_SETTINGS_REL_BRIGHT_CAP_Y etc., unchanged, always 0-based). */
    lv_obj_t *base = lv_obj_create(list);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, row_w, FF_SETTINGS_BRIGHT_BLOCK_H);
    lv_obj_set_pos(base, 0, rel_y);
    lv_obj_set_style_pad_all(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(base);
    lv_label_set_text(cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(cap, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_pos(cap, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);

    char pctbuf[8];
    snprintf(pctbuf, sizeof(pctbuf), "%u%%", (unsigned)pct);
    lv_obj_t *pctlbl = lv_label_create(base);
    lv_label_set_text(pctlbl, pctbuf);
    lv_obj_set_style_text_font(pctlbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(pctlbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(pctlbl, LV_ALIGN_TOP_RIGHT, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);
    s_bright_pct = pctlbl; /* a step updates this label */

    /* --- Control row: a non-interactive amber level bar (left) + a −/+ stepper
     * group (right). The stepper is two lv_button pills — CLICKED fires only on
     * a tap, so a drag anywhere scrolls the list natively and the brightness
     * control never fights the scroll (#bug2). --- */
    int32_t const ctrl_h = FF_SETTINGS_SLIDER_H; /* control-area height */
    /* −/+ are DISTINCT controls (unlike a toggle's paired pills, which share a
     * callback and are excluded from the adjacency sweep as one composite), so
     * they need the full 8px hit-target adjacency floor between them. */
    int32_t const step_gap = 8;
    int32_t const grp_w = 2 * FF_SETTINGS_TOGGLE_PILL_W + step_gap;
    int32_t const grp_x = row_w - grp_w;
    int32_t const pill_h = FF_SETTINGS_ROW_H;
    int32_t const pill_y = FF_SETTINGS_REL_SLIDER_Y + (ctrl_h - pill_h) / 2;

    /* Level bar: a thin surface track with an amber fill spanning frac(pct). */
    int32_t const track_h = 6;
    int32_t const track_y = FF_SETTINGS_REL_SLIDER_Y + (ctrl_h - track_h) / 2;
    int32_t const bar_w = grp_x - 16; /* stop short of the stepper group */
    s_bright_bar_w = (bar_w > 0) ? bar_w : 1;
    settings_deco_box(base, 0, track_y, s_bright_bar_w, track_h, FF_THEME_COLOR_SURFACE, 3); /* track */
    float const frac = (float)(pct - FF_BRIGHTNESS_MIN_PCT) / (float)(FF_BRIGHTNESS_MAX_PCT - FF_BRIGHTNESS_MIN_PCT);
    int32_t fill_w = (int32_t)lroundf(frac * (float)s_bright_bar_w);
    if (fill_w < 1) fill_w = 1;
    s_bright_fill = settings_deco_box(base, 0, track_y, fill_w, track_h, FF_THEME_COLOR_AMBER, 3);

    /* −/+ stepper pills. */
    lv_obj_t *minus = settings_make_pill(base, "-", grp_x, pill_y, FF_SETTINGS_TOGGLE_PILL_W, pill_h,
                                         FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, 0, settings_brightness_minus_cb,
                                         NULL);
    lv_obj_t *plus = settings_make_pill(base, "+", grp_x + FF_SETTINGS_TOGGLE_PILL_W + step_gap, pill_y,
                                        FF_SETTINGS_TOGGLE_PILL_W, pill_h, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK,
                                        0, settings_brightness_plus_cb, NULL);
    /* Bump the −/+ glyphs up from the small CHIP font so they read as real
     * buttons, not tiny marks. */
    lv_obj_set_style_text_font(lv_obj_get_child(minus, 0), FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), FF_THEME_FONT_HEADLINE, 0);
}

/* ---------------------------------------------------------------------
 * CALIBRATE TOUCH — full-width surface pill, thin amber border, amber text.
 * On tap emits the shell-owned FF_INTENT_CALIBRATE_TOUCH (the shell runs the
 * device crosshair flow; a no-op in the sim). An action, not a stored value.
 * ------------------------------------------------------------------- */
static void settings_calibrate_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_calibrate_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *pill = settings_make_pill(list, "CALIBRATE TOUCH", 0, rel_y, row_w, FF_SETTINGS_ROW_H,
                                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER, 2, settings_calibrate_cb, NULL);
    lv_obj_set_style_border_width(pill, 2, 0); /* ~1.5px, rounded up to a device pixel */
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_40 + LV_OPA_10 / 2 /* ~45% */, 0);
}

/* ---------------------------------------------------------------------
 * COMPASS — S12 step 3. A value row (settings_build_value_row's "label +
 * status pill, both tappable" shape — QUIET HOURS' own shape, not
 * CALIBRATE TOUCH's full-width pill): the LABEL side is the action (tap
 * to open the calibration ritual), the PILL side is an honest STATUS
 * readout, dimmed when uncalibrated (the same "dim = an honest off/
 * unset value" convention QUIET HOURS' `quiet_off` already uses), read
 * from `ff_app_compass_cal_t.cal_valid` (S12 step 3's shell projection,
 * always populated regardless of `subview` — see that struct's own doc
 * comment in ff_app_state.h).
 *
 * Label reads "COMPASS", not "CALIBRATE COMPASS" — matching this row
 * shape's own single-word label convention (UNITS, CLOCK, SCREEN — every
 * existing `settings_build_value_row`/`settings_build_toggle_row` caller
 * uses one word) and, measured, load-bearing: at this row's label
 * column width (row_w - FF_SETTINGS_VALUE_PILL_W - FF_SETTINGS_VALUE_GAP)
 * "CALIBRATE COMPASS" (18 chars) clipped mid-word against the value pill
 * (caught rendering `settings_scrolled_bottom`'s golden — see the PR
 * body). The status pill reads "SET"/"UNSET" for the same fixed-96px-
 * width reason: "CALIBRATED"/"UNCALIBRATED" overflowed the pill and
 * bled into the row to its right. Honest either way — SET/UNSET says
 * exactly the same fact "calibrated"/"uncalibrated" would, just in the
 * width this row shape actually has (the ritual page itself, reached by
 * tapping this row, spells it out in full: "CALIBRATE COMPASS").
 *
 * Tapping EITHER half emits FF_INTENT_COMPASS_CAL_START (the shell
 * decides: a no-op if a session is somehow already active, opens the
 * ritual otherwise), never a cycle-through-presets action the pill
 * shape sometimes implies elsewhere (QUIET HOURS) — this status can
 * only be changed by completing (or clearing) the ritual itself.
 * ------------------------------------------------------------------- */
static void settings_compass_cal_open_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_COMPASS_CAL_START, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_compass_cal_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, bool cal_valid)
{
    settings_build_value_row(list, rel_y, row_w, "COMPASS", cal_valid ? "SET" : "UNSET",
                             /*dim=*/!cal_valid, settings_compass_cal_open_cb);
}

/* ---------------------------------------------------------------------
 * DIAGNOSTICS — a full-width action pill, same shape as CALIBRATE
 * TOUCH/CREW above, that opens the DIAGNOSTICS sub-view
 * (FF_INTENT_SETTINGS_OPEN_DIAGNOSTICS, no payload — the shell decides,
 * this file only asks). An ACTION, not a stored value.
 * ------------------------------------------------------------------- */
static void settings_diag_open_row_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_OPEN_DIAGNOSTICS, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_diag_open_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *pill = settings_make_pill(list, "DIAGNOSTICS", 0, rel_y, row_w, FF_SETTINGS_ROW_H,
                                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER, 2, settings_diag_open_row_cb,
                                        NULL);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_40 + LV_OPA_10 / 2 /* ~45% */, 0);
}

/* ---------------------------------------------------------------------
 * POWER — field-hardening ahead of Lost Lands (S26 slice b amendment,
 * docs/specs/S26-device-lifecycle.md "(b) Power button -> power menu ->
 * soft power-off"): the printed case's physical PWR button does not
 * actuate reliably, so the power menu (Power off / Reboot / Cancel)
 * needs a second, on-glass way in. A full-width action pill, same shape
 * as CALIBRATE TOUCH/CREW/DIAGNOSTICS above, that emits the exact same
 * `FF_INTENT_POWER_MENU_OPEN` the esp32s3 target's PWR long-press
 * already dispatches (ff_intent.h's own doc comment on that intent is
 * amended alongside this row) — it lands on the identical
 * `ff_route_push_modal(&sh->route, FF_APP_FACE_POWER_MENU)` call, so
 * this REUSES the existing modal (`ff_scr_power_menu_build`) and its
 * handlers verbatim rather than building a second power menu. Tapping
 * this row only OPENS the menu; Power off still needs its own
 * confirming tap inside it, same as a PWR long-press today — no new way
 * to actually power off exists here. An ACTION, not a stored value. */
static void settings_power_open_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_POWER_MENU_OPEN, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_power_open_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *pill = settings_make_pill(list, "POWER", 0, rel_y, row_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                                        FF_THEME_COLOR_AMBER, 2, settings_power_open_cb, NULL);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_40 + LV_OPA_10 / 2 /* ~45% */, 0);
}

/* ---------------------------------------------------------------------
 * NAME — the puck's own identity, tap-to-edit. A value row
 * (settings_build_value_row's "label + status pill, both tappable"
 * shape), NOT reused verbatim: unlike every other row's label (a fixed
 * uppercase caption — "COMPASS", "UNITS"), this row's LABEL IS THE
 * STORED VALUE — the whole point of the row is to show the current
 * name — so it needs its own DOTS-ellipsized, width-bounded render (the
 * `compose_to`/S08 precedent for a live value that must never bleed
 * into a neighbour, scr_compose.c's own header comment on the TO row)
 * rather than the shared helper's fixed-caption label.
 *
 * The pill is the small "mesh: NAME OK / pending" state the feature
 * brief asks for — deliberately NOT the name repeated a second time
 * (no room, and no value in it): a checkmark (`LV_SYMBOL_OK`) once
 * `ff_shell_mesh_name_status`'s `confirmed` is true, "..." while
 * pending, "N/A" when there is nothing to confirm yet (name unset) —
 * not a bare "-", which collides with the brightness stepper's own
 * "-" pill label under this test file's tree-order `find_button_with_
 * label` search (test_scr_intent.c's own S_name_row_unset_* test found
 * this the hard way).
 * Confirmed is NEVER assumed from a push merely having been attempted —
 * only a matching self NodeInfo flips it (this repo's honest-data rule;
 * see that getter's own doc comment, ff_shell.h).
 * ------------------------------------------------------------------- */
static void settings_name_open_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_OPEN_NAME_EDIT, .u = {0}};
    ff_intent_emit(&in);
}

/**
 * `push_failed` — confirmation-fix follow-up: a routing NAK for the
 * CURRENT push (`ff_app_settings_t.mesh_name_push_failed`'s own doc
 * comment has the full rationale). Renders as "!" in amber — distinct
 * from both the checkmark and the plain "..." pending dots, because
 * "the mesh reported this write failed" is a stronger, more actionable
 * claim than "still waiting" and this repo's honest-data rule says a
 * silent identical-looking pending state would bury it. `confirmed`
 * takes precedence when both are true (a later retry that DID succeed
 * always wins over an earlier NAK) — checked first, below.
 *
 * `mismatch` — confirmation-fix round 2: a fresh reply/self-NodeInfo for
 * the CURRENT push reporting a DIFFERENT owner than was pushed
 * (`ff_app_settings_t.mesh_name_mismatch`'s own doc comment). Deliberately
 * rendered with the SAME "!" amber glyph as `push_failed` — both are
 * "something about this push needs the wearer's attention, don't read
 * the ... dots as ordinary pending" anomalies, and this row has no
 * spare pixels for a second distinct warning glyph — but the two are
 * tracked as SEPARATE booleans (never collapsed into one) because they
 * are different facts at the shell/console layer: a routing NAK vs. the
 * admin module answering with the wrong name. `confirmed` still takes
 * precedence over either.
 */
static void settings_build_name_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *my_name,
                                    bool confirmed, bool push_failed, bool mismatch)
{
    lv_obj_t *row = settings_make_row(list, rel_y, row_w);
    int32_t const label_w = row_w - FF_SETTINGS_VALUE_PILL_W - FF_SETTINGS_VALUE_GAP;
    bool const has_name = (my_name != NULL) && (my_name[0] != '\0');

    lv_obj_t *hit = lv_obj_create(row);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, label_w, FF_SETTINGS_ROW_H);
    lv_obj_set_pos(hit, 0, 0);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, settings_name_open_cb, LV_EVENT_CLICKED, NULL);
    settings_hit_add_press_feedback(hit);

    lv_obj_t *lbl = lv_label_create(hit);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(lbl, label_w);
    lv_label_set_text(lbl, has_name ? my_name : "(unset)");
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(has_name ? FF_THEME_COLOR_INK : FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    bool const show_ok = has_name && confirmed;
    bool const show_failed = has_name && !show_ok && (push_failed || mismatch);
    char const *pill_text = has_name ? (show_ok ? LV_SYMBOL_OK : (show_failed ? "!" : "...")) : "N/A";
    uint32_t const pill_fg =
        show_ok ? FF_SETTINGS_PILL_VAL_FG : (show_failed ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_MUTED);
    settings_make_pill(row, pill_text, row_w - FF_SETTINGS_VALUE_PILL_W, 0, FF_SETTINGS_VALUE_PILL_W,
                       FF_SETTINGS_ROW_H, FF_SETTINGS_PILL_VAL_BG, pill_fg, 0, settings_name_open_cb, NULL);
}

/* ---------------------------------------------------------------------
 * CREW — S12/S04: a full-width action pill, same shape as CALIBRATE
 * TOUCH above, that opens the CREW sub-view (FF_INTENT_SETTINGS_OPEN_
 * CREW, no payload — the shell decides, this file only asks). An
 * ACTION, not a stored value, same as CALIBRATE TOUCH.
 * ------------------------------------------------------------------- */
static void settings_crew_open_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_OPEN_CREW, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_crew_open_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *pill = settings_make_pill(list, "CREW", 0, rel_y, row_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                                        FF_THEME_COLOR_AMBER, 2, settings_crew_open_cb, NULL);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_40 + LV_OPA_10 / 2 /* ~45% */, 0);
}

/* ---------------------------------------------------------------------
 * Opaque header band (#bug5). REPLACES the former BG->transparent edge-fade
 * scrims. On the RGB565 round panel a gradient scrim leaves a HARD amber
 * edge (colour-step banding) instead of hiding a row that has scrolled to
 * the viewport top — the maintainer's device photo showed a stray amber
 * pill fragment bleeding in just below the pinned header. A SOLID BG band
 * behind the header occludes that region outright with no gradient to band.
 *
 * Spanning the whole top region ABOVE the list viewport (y=0 .. LIST_Y), it
 * paints solid ink behind the header while leaving the list's first row
 * (at the viewport top, y=LIST_Y) fully visible — the list clips its own
 * children to its rectangle (LVGL default; the list never sets
 * LV_OBJ_FLAG_OVERFLOW_VISIBLE), so a row scrolling up ends cleanly at the
 * viewport top with the opaque band above it, no translucent overlap. Drawn
 * on the puck BEFORE the header controls so they render on top of it.
 *
 * NOTE: the sim renders XRGB8888 (8-bit/channel), so it cannot reproduce
 * the device's RGB565 gradient banding — the scrolled sim golden renders
 * clean either way. This band is nonetheless the correct DEVICE fix (no
 * gradient => no banding); the on-glass result is verified on hardware.
 * ------------------------------------------------------------------- */
static void settings_build_header_band(lv_obj_t *puck, int32_t x, int32_t w, int32_t h)
{
    lv_obj_t *band = lv_obj_create(puck);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, w, h);
    lv_obj_set_pos(band, x, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(band, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0); /* fully opaque: occlude, don't fade */
}

/* ---------------------------------------------------------------------
 * Section header (settings audit 2026-09-03) — a lightweight, NON-clickable
 * small-caps label grouping the rows below it (DISPLAY / SOUND / UNITS /
 * DEVICE). Deliberately NOT a control: no hit-target obligations, so it
 * carries no click callback and is explicitly cleared of both CLICKABLE and
 * SCROLLABLE (a plain lv_label defaults to neither, but this states the
 * intent rather than relying on the default). Dimmer opacity and wider
 * letter-spacing than a row caption (settings_row_caption) so it reads as
 * chrome grouping rows, not a row itself.
 *
 * `y` is the caller's running layout cursor, in LIST-relative coordinates;
 * `first` is true only for the very first header in the list (DISPLAY),
 * which sits at the top of the content with nothing above it to separate
 * from, so it skips the leading FF_SETTINGS_ROW_GAP every later header adds
 * to clear the previous section's last row. Returns the y position of the
 * section's first row — callers assign this straight back to their cursor:
 *   y = settings_build_section_header(list, y, row_w, "DISPLAY", true);
 *   settings_build_brightness(list, row_w, y); y += FF_SETTINGS_BRIGHT_BLOCK_H;
 *   ...
 * ------------------------------------------------------------------- */
static int32_t settings_build_section_header(lv_obj_t *list, int32_t y, int32_t row_w, char const *text, bool first)
{
    if (!first) {
        y += FF_SETTINGS_ROW_GAP; /* separate from the previous section's last row — the SAME gap
                                    * any two rows already carry, not an extra cost (see this
                                    * function's own doc comment / the FF_SETTINGS_SECTION_BLOCK_H
                                    * comment above for the accounting). */
    }

    lv_obj_t *lbl = lv_label_create(list);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_set_width(lbl, row_w);
    lv_obj_set_height(lbl, FF_SETTINGS_SECTION_HDR_H);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    /* puck-ux-usability-2026-09-15 slice 4 (finding 8's own "measure,
     * don't reason harder" lesson, extended past the DIM constant itself
     * by test_text_contrast_all_faces.c's whole-fixture sweep): MUTED at
     * LV_OPA_60 measures 2.76:1 against BG — MUTED's own 5.78:1 needs
     * >= ~86% opacity (219/255) just to clear 4.5:1, at which point the
     * "dimmer than a row caption" effect this opacity existed for is
     * barely perceptible anyway. Full opacity instead: still visually
     * SECONDARY to a row's own INK caption (MUTED is a dimmer colour to
     * begin with), just not dimmed a second time past AA. Letter-tracking
     * (the actual visual distinguisher between a section header and a
     * row caption) is unchanged. */
    lv_obj_set_style_text_letter_space(lbl, 3, 0); /* wider tracking than a row caption's 2px */
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);

    return y + FF_SETTINGS_SECTION_HDR_H + FF_SETTINGS_SECTION_HDR_GAP;
}

/* ---------------------------------------------------------------------
 * CREW page (S12/S04, docs/specs/S04-firefly-protocol.md's "pairing v1 =
 * channel membership + explicit crew list"; the S22 note's "pairing
 * screen ... unbuilt", finally built here) — the Settings SUB-VIEW the
 * CREW row above opens (ff_settings_subview_t, ff_app_state.h). Two
 * lists: PAIRED (name + honest presence chip, reused from S24's
 * ff_sigview_presence, never reimplemented + REMOVE) and HEARD (name or
 * an honest short-id fallback + seen-age + ADD). Pure renderer of
 * `ff_app_crew_page_t`: ADD/REMOVE emit FF_INTENT_CREW_PAIR/_UNPAIR with
 * the target node id, the header's "<" emits FF_INTENT_BACK — the shell
 * decides every transition (same "screens stay pure renderers" split
 * this whole file already follows for FF_INTENT_SETTING_SET).
 *
 * Layout deliberately mirrors the plain settings list's own geometry
 * (list_margin/row_w via settings_safe_margin_x, FF_SETTINGS_LIST_Y/H)
 * so the two pages read as one visual family rather than two unrelated
 * screens; the header mirrors scr_inbox.c's picker/thread back-button
 * convention (a real FF_THEME_MIN_HIT_PX circle at its own chord
 * margin) since THIS page, unlike the plain settings list, is reached
 * by drilling in rather than by swiping to a base face, so it needs its
 * own way back.
 * ------------------------------------------------------------------- */
#define FF_CREW_BACK_Y  30
#define FF_CREW_BACK_PX FF_THEME_MIN_HIT_PX
#define FF_CREW_HDR_Y   (FF_CREW_BACK_Y + (FF_CREW_BACK_PX - 24) / 2) /* optically centered against the back circle */

#define FF_CREW_LIST_Y 100
#define FF_CREW_LIST_H 256 /* same inscribed-viewport band as the plain settings list */

/* Rows are taller than a plain settings row (S12's two-line "name" +
 * "presence/age" content, vs. one label) but still clear the 44px hit
 * floor with real margin; the inter-row gap clears the 8px adjacency
 * floor with the same slack the plain list's FF_SETTINGS_ROW_GAP uses. */
#define FF_CREW_ROW_H    56
#define FF_CREW_ROW_GAP  14
#define FF_CREW_ROW_STEP (FF_CREW_ROW_H + FF_CREW_ROW_GAP)
_Static_assert(FF_CREW_ROW_H >= FF_THEME_MIN_HIT_PX, "crew rows must clear the 44px hit-target floor");

/* PR #311 review (N-series): the crew row's action pill gets its OWN
 * width instead of borrowing FF_SETTINGS_VALUE_PILL_W (96).
 *
 * #307's own comment records why this number matters: #303 replaced
 * "LOST" with "NO SIGNAL 15 MIN", and the crew row's label column has to
 * be wide enough to render that status in full — a truncated "NO SIGNAL
 * 15 ..." is the exact defect that PR sized this pill to avoid. This PR
 * then narrowed every Settings band from 262px to 240px (the bezel's
 * glass, not the framebuffer's circle), which took the label column from
 * 154px to 132px and truncated it again in crew_default.png.
 *
 * Measured, at FF_THEME_FONT_CHIP with this row's 1px letter-spacing:
 * the widest status ff_fmt_age can produce is "NO SIGNAL 48 MIN" at
 * 150px (swept over all two-digit minute values; "NO SIGNAL 23 HR" is
 * 139, "off your radar" 111, "NOT SEEN YET" 114). The widest action word
 * is "UNHIDE" at 58px. So the pill takes 76 — 9px of padding either side
 * of "UNHIDE", still 32px over FF_THEME_MIN_HIT_PX — and leaves the
 * label column 240 - 76 - 12 = 152px, clear of the worst case by 2px.
 * Held there by S_CREW_worst_case_status_renders_in_full
 * (test_scr_intent.c), which renders the row and asserts no ellipsis
 * rather than trusting this arithmetic. */
#define FF_CREW_ACTION_PILL_W 76
_Static_assert(FF_CREW_ACTION_PILL_W >= FF_THEME_MIN_HIT_PX,
               "the crew row's action pill must still clear the 44px hit floor");
#define FF_CREW_ACTION_GAP    FF_SETTINGS_VALUE_GAP

/* Usability-review slice 3 (docs/reviews/puck-ux-usability-2026-09-15.md)
 * raised FF_SETTINGS_ROW_H to 80 for the plain Settings list. This CREW
 * sub-page borrowed that same constant (pre-pass, when it was 48) for its
 * own full-width action pills (SHOW CODE / START CREW / LEAVE CREW) and
 * for the HIDE/UNHIDE/ADD/FULL(8) pills inside FF_CREW_ROW_H (56)-tall
 * member rows — deliberately DECOUPLED here rather than following
 * FF_SETTINGS_ROW_H up to 80, for two independent reasons: (1) this
 * slice's own "Do" list names the plain Settings list, SHOW CODE's BACK
 * and the crew-op confirms, not this page's rows — growing them was never
 * asked for; (2) it does not fit without its own redesign anyway — an
 * 80px pill centered in a 56px-tall FF_CREW_ROW_H row would overflow the
 * row by 24px into its neighbours, and the SHOW CODE/START CREW/LEAVE
 * CREW pills are full-width rows in their own right (not embedded in a
 * taller row), so growing THEM would just re-flow this page's whole
 * layout — a legitimate follow-up, but its own slice, the same call the
 * flare takeover's 80px buttons got ("a real design trade, not a free
 * win — state it and let Jake pick"). Kept at the pre-pass 48px so this
 * page's own goldens (crew_default, crew_full, crew_hidden, crew_show_
 * code's CALLER page, etc.) stay byte-identical; only the sub-faces this
 * slice actually names (SHOW CODE's own BACK button, the crew-op
 * confirm/status pages) grow. */
#define FF_CREW_PAGE_ROW_H 48
_Static_assert(FF_CREW_PAGE_ROW_H >= FF_THEME_MIN_HIT_PX,
               "the crew page's own action pills must still clear the 44px hit floor");

/* [api] A02 slice D — the PAIRED row's action is **HIDE**, and REMOVE is
 * gone from this row. Both halves of that are decisions, so both are
 * written down (AGENTS.md: note the interpretation).
 *
 * WHY REMOVE GOES. A02 §4.7 retires the add/remove vocabulary outright:
 * having the code IS membership, so "remove" cannot mean what it used
 * to. Concretely, under auto-crew a plain unpair is not permanent — the
 * person's next qualifying packet re-admits them (see
 * `shell_try_admit`'s own comment, ff_shell.c). A button that silently
 * undoes itself a few seconds later is worse than no button, and it is
 * exactly the confidently-wrong control this project refuses elsewhere.
 * `FF_INTENT_CREW_UNPAIR` itself stays — the bench console and the
 * intent tests still exercise it — it is just no longer offered here.
 *
 * WHY NOT BOTH. Two 74 px pills side by side were tried and shipped a
 * measurable regression: `polish/puck-plain-faces` (#303) had just
 * replaced "LOST" with "NO SIGNAL 15 MIN", and the narrowed label column
 * truncated it to "NO SIGNA...". One control at the page's existing
 * FF_CREW_ACTION_PILL_W keeps that wording legible, which is the whole
 * point of #303.
 *
 * The HIDE pill reuses FF_CREW_ACTION_PILL_W above — no new geometry. */

static void settings_crew_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_crew_pair_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CREW_PAIR, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

/* [api] A02 slice D — the CREW page's three new controls. Same
 * pure-renderer shape as PAIR/UNPAIR above: the row carries the target
 * node id, the intent carries it to the shell, and the shell decides. */
static void settings_crew_hide_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CREW_HIDE, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

static void settings_crew_unhide_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CREW_UNHIDE, .u = {0}};
    in.u.node_id = (uint32_t)node;
    ff_intent_emit(&in);
}

static void settings_crew_show_code_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_OPEN_CREW_CODE, .u = {0}};
    ff_intent_emit(&in);
}

/* [api] A02 slice D2 — START CREW / LEAVE CREW, each in two beats. The
 * pill only ASKS (the confirm face is the consent); the confirm face's
 * own button is the one tap that writes to the radio. Same pure-renderer
 * shape as every other control on this page: a bare intent, and the
 * shell decides. */
static void settings_crew_start_req_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CREW_START_REQUEST, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_crew_leave_req_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CREW_LEAVE_REQUEST, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_crew_start_confirm_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CREW_START_CONFIRM, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_crew_leave_confirm_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CREW_LEAVE_CONFIRM, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_crew_dismiss_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CREW_DISMISS, .u = {0}};
    ff_intent_emit(&in);
}

/* Honest presence text/color — S24's ff_sigview_presence vocabulary,
 * REUSED verbatim (mirrors scr_inbox.c's inbox_presence_text: same
 * words, same color roles, not reimplemented independently). */
static void settings_crew_presence_text(ff_sigview_presence_t presence, uint32_t age_ms, char *buf, size_t n,
                                        uint32_t *out_color)
{
    switch (presence) {
    case FF_PRESENCE_SEEN: {
        char age_buf[16];
        ff_fmt_age(age_buf, sizeof(age_buf), age_ms);
        snprintf(buf, n, "SEEN %s", age_buf);
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    }
    case FF_PRESENCE_LOST: {
        char age_buf[16];
        ff_fmt_age(age_buf, sizeof(age_buf), age_ms);
        snprintf(buf, n, "NO SIGNAL %s", age_buf);
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    }
    case FF_PRESENCE_LINKED:
    default:
        snprintf(buf, n, "NOT SEEN YET");
        *out_color = FF_THEME_COLOR_MUTED;
        break;
    }
}

/* Two-line row content (name/id on top, status/age below), left-anchored
 * in the label column; the ADD/REMOVE pill sits to its right, same
 * column split settings_build_value_row already uses. */
static lv_obj_t *settings_crew_row_labels(lv_obj_t *row, int32_t label_w, char const *top, char const *bottom,
                                          uint32_t bottom_color)
{
    lv_obj_t *top_lbl = lv_label_create(row);
    lv_obj_set_width(top_lbl, label_w);
    /* 2026-09-06 crew long names: DOTS/DOT truncates to ONE line only
     * when the label's HEIGHT is ALSO bounded — width alone makes LVGL
     * wrap instead (scr_banner.c's/scr_inbox.c's documented lesson,
     * newly load-bearing here now a display name + "(SHORT)" tag can run
     * well past this column's width, where a short name rarely did). */
    lv_obj_set_height(top_lbl, lv_font_get_line_height(FF_THEME_FONT_LABEL));
    lv_label_set_long_mode(top_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(top_lbl, top);
    lv_obj_set_style_text_font(top_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(top_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_letter_space(top_lbl, 1, 0);
    lv_obj_align(top_lbl, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *bot_lbl = lv_label_create(row);
    lv_obj_set_width(bot_lbl, label_w);
    lv_obj_set_height(bot_lbl, lv_font_get_line_height(FF_THEME_FONT_CHIP));
    lv_label_set_long_mode(bot_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(bot_lbl, bottom);
    lv_obj_set_style_text_font(bot_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(bot_lbl, lv_color_hex(bottom_color), 0);
    lv_obj_set_style_text_letter_space(bot_lbl, 1, 0);
    lv_obj_align(bot_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    return top_lbl;
}

static void settings_crew_build_paired_row(lv_obj_t *list, int32_t rel_y, int32_t row_w,
                                           ff_app_crew_paired_row_t const *m)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_CREW_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int32_t const label_w = row_w - FF_CREW_ACTION_PILL_W - FF_CREW_ACTION_GAP;

    char status[24];
    uint32_t color = FF_THEME_COLOR_MUTED;
    settings_crew_presence_text(m->presence, m->presence_age_ms, status, sizeof(status), &color);

    /* Identity is never fabricated (CLAUDE.md): a paired member with no
     * name yet (NodeInfo hasn't arrived) renders an honest node-id
     * fallback, never a placeholder word like "unnamed".
     *
     * S12 CREW page amendment (2026-09-06, crew long names): `m->name`
     * is the DISPLAY name (long when known, else short — `ff_crew_
     * display_name`, applied in `shell_project_crew_page`); `m->short_
     * name` is always the short one. When the two differ — i.e. a real
     * long name is known — the short name rides along as a muted "(TAYL)"
     * tag, via LVGL's `#RRGGBB text#` recolor markup (the same mechanism
     * `scr_compose.c`'s pending-character highlight uses). Recolor is
     * enabled ONLY in that case: a plain name (no tag) never risks a
     * literal '#' in someone's name being misread as markup. */
    char top[FF_APP_LONG_NAME_LEN + FF_APP_NAME_LEN + 24];
    bool has_tag = (m->name[0] != '\0') && (m->short_name[0] != '\0') && (strcmp(m->name, m->short_name) != 0);
    if (m->name[0] != '\0') {
        if (has_tag) {
            snprintf(top, sizeof(top), "%s #%06x (%s)#", m->name, (unsigned)FF_THEME_COLOR_MUTED, m->short_name);
        } else {
            snprintf(top, sizeof(top), "%s", m->name);
        }
    } else {
        snprintf(top, sizeof(top), "#%04x", (unsigned)(m->node_id & 0xFFFFu));
    }

    lv_obj_t *top_lbl = settings_crew_row_labels(row, label_w, top, status, color);
    lv_label_set_recolor(top_lbl, has_tag);

    settings_make_pill(row, "HIDE", row_w - FF_CREW_ACTION_PILL_W, (FF_CREW_ROW_H - FF_CREW_PAGE_ROW_H) / 2,
                       FF_CREW_ACTION_PILL_W, FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE,
                       FF_THEME_COLOR_STALE_AMBER, 0, settings_crew_hide_cb,
                       (void *)(uintptr_t)m->node_id);
}

/* [api] A02 slice D — one HIDDEN-section row. Same two-line shape as a
 * PAIRED row minus the presence chip: a hidden node is deliberately NOT
 * in the roster (hide is unpair + remember), so there is no presence to
 * report and inventing one would be a claim about somebody the wearer
 * asked not to see. The subtitle says what hiding actually does, in the
 * words the amendment uses, so the wearer is never guessing whether
 * their messages still arrive. */
static void settings_crew_build_hidden_row(lv_obj_t *list, int32_t rel_y, int32_t row_w,
                                            ff_app_crew_hidden_row_t const *h)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_CREW_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int32_t const label_w = row_w - FF_CREW_ACTION_PILL_W - FF_CREW_ACTION_GAP;

    char top[FF_APP_NAME_LEN + 4];
    if (h->has_name && h->name[0] != '\0') {
        snprintf(top, sizeof(top), "%s", h->name);
    } else {
        snprintf(top, sizeof(top), "#%s", h->short_id);
    }

    settings_crew_row_labels(row, label_w, top, "off your radar", FF_THEME_COLOR_MUTED);

    settings_make_pill(row, "UNHIDE", row_w - FF_CREW_ACTION_PILL_W, (FF_CREW_ROW_H - FF_CREW_PAGE_ROW_H) / 2,
                       FF_CREW_ACTION_PILL_W, FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER,
                       0, settings_crew_unhide_cb, (void *)(uintptr_t)h->node_id);
}

/* [api] A02 slice D — one NOT TRACKED row: a sender that proved it holds
 * the crew key but arrived at a full 8/8 roster. Carries its REAL
 * last-heard age (never "just now" filler) and no action of its own —
 * the way to make room is to hide somebody, which is what the section's
 * caption says. */
static void settings_crew_build_overflow_row(lv_obj_t *list, int32_t rel_y, int32_t row_w,
                                              ff_app_crew_heard_row_t const *h)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_CREW_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char age_buf[16];
    ff_fmt_age(age_buf, sizeof(age_buf), h->age_ms);
    char status[32];
    snprintf(status, sizeof(status), "heard %s", age_buf);

    /* A02 §4.3/§4.4 — these people ARE crew (they proved possession of
     * the key); they are only untracked because the roster is full. The
     * spec is explicit for exactly this row: "with their name if
     * NodeInfo arrived, `New crew member` otherwise", and §4.4's rule is
     * absolute — "Never blank, never a hex id, on any screen". The hex
     * fallback below it in HEARD is right for a stranger and wrong here:
     * it reads as a fault, and it is the only thing on this page that
     * makes a crew member look like a radio. */
    char top[FF_APP_NAME_LEN + 4];
    if (h->has_name && h->name[0] != '\0') {
        snprintf(top, sizeof(top), "%s", h->name);
    } else {
        snprintf(top, sizeof(top), "NEW CREW MEMBER");
    }

    settings_crew_row_labels(row, row_w, top, status, FF_THEME_COLOR_STALE_AMBER);
}

/* A wrapped caption under a section header — the honest "why is this
 * here / what do I do" line. Returns the y past it. */
static int32_t settings_crew_caption(lv_obj_t *list, int32_t y, int32_t row_w, char const *text)
{
    lv_obj_t *lbl = lv_label_create(list);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_set_width(lbl, row_w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(lbl);
    int32_t const h = lv_obj_get_height(lbl);
    return y + (h > 0 ? h : FF_CREW_PAGE_ROW_H) + FF_SETTINGS_ROW_GAP;
}

static void settings_crew_build_heard_row(lv_obj_t *list, int32_t rel_y, int32_t row_w,
                                          ff_app_crew_heard_row_t const *h, bool roster_full)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_CREW_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int32_t const label_w = row_w - FF_CREW_ACTION_PILL_W - FF_CREW_ACTION_GAP;

    char age_buf[16];
    ff_fmt_age(age_buf, sizeof(age_buf), h->age_ms);
    char status[24];
    snprintf(status, sizeof(status), "SEEN %s", age_buf);

    /* Honest short-id fallback (never a fabricated name) when this heard
     * node has never sent a NodeInfo we caught a name from. */
    char top[FF_APP_NAME_LEN + 4];
    if (h->has_name && h->name[0] != '\0') {
        snprintf(top, sizeof(top), "%s", h->name);
    } else {
        snprintf(top, sizeof(top), "#%s", h->short_id);
    }

    settings_crew_row_labels(row, label_w, top, status, FF_THEME_COLOR_MUTED);

    /* S12 AC — roster full: ADD disabled with an honest "FULL (8)" state
     * (interpretation call, PR body: the brief's "crew full (8)" wording
     * is rendered compactly here since the pill column is only
     * FF_SETTINGS_VALUE_PILL_W=96px wide). */
    lv_obj_t *pill;
    if (roster_full) {
        pill = settings_make_pill(row, "FULL (8)", row_w - FF_CREW_ACTION_PILL_W,
                                  (FF_CREW_ROW_H - FF_CREW_PAGE_ROW_H) / 2, FF_CREW_ACTION_PILL_W,
                                  FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED, 0, NULL, NULL);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    } else {
        pill = settings_make_pill(row, "ADD", row_w - FF_CREW_ACTION_PILL_W,
                                  (FF_CREW_ROW_H - FF_CREW_PAGE_ROW_H) / 2, FF_CREW_ACTION_PILL_W,
                                  FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE,
                                  FF_THEME_COLOR_AMBER, 0, settings_crew_pair_cb, (void *)(uintptr_t)h->node_id);
    }
    (void)pill;
}

/* Honest empty state for the HEARD list (S12 AC): the comms-brain-down
 * hint ONLY when the link genuinely isn't up (never as generic filler —
 * CLAUDE.md's "unknown = explicitly unknown" extends to WHY a list is
 * empty, not just whether it is). */
static void settings_crew_build_heard_empty(lv_obj_t *list, int32_t rel_y, int32_t row_w, bool link_connected)
{
    lv_obj_t *lbl = lv_label_create(list);
    lv_obj_set_pos(lbl, 0, rel_y);
    lv_obj_set_width(lbl, row_w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, link_connected ? "nobody heard yet" : "No crew yet. Add people from HEARD once your radio is on.");
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
}

/* Defined with the STATUS face below, and shared with the CREW page's
 * blocked-reason caption so the same failure never gets two wordings. */
static char const *settings_crew_fail_text(ff_app_crew_fail_t f);

static void settings_build_crew_page(lv_obj_t *parent, ff_app_crew_page_t const *cw)
{
    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);

    /* Header: a real back circle (scr_inbox.c's picker/thread back-button
     * convention) + a centered "CREW" title. */
    int32_t const back_margin = settings_safe_margin_x(FF_CREW_BACK_Y, FF_CREW_BACK_PX);
    lv_obj_t *back = ff_scr_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_CREW_BACK_PX, FF_CREW_BACK_PX);
    lv_obj_set_pos(back, back_margin, FF_CREW_BACK_Y);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    /* puck-ux-usability-review slice 1, finding 2 (review fix, confirmed
     * by test_press_feedback_all_faces.c's corrected sweep): this back
     * circle is the same "raw ff_scr_button_create wrapper, no
     * ff_scr_pill_create under it" shape as the value/name row hit boxes
     * this PR already fixed via settings_hit_add_press_feedback — same
     * fix, same convention. */
    settings_hit_add_press_feedback(back);
    lv_obj_add_event_cb(back, settings_crew_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *glyph = lv_label_create(back);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(glyph, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "CREW");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, FF_CREW_HDR_Y);

    int32_t list_margin = settings_safe_margin_x(FF_CREW_LIST_Y, FF_CREW_LIST_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

    lv_obj_t *list = lv_obj_create(puck);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, row_w, FF_CREW_LIST_H);
    lv_obj_set_pos(list, list_margin, FF_CREW_LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE); /* #bug2 precedent — see the plain list's own comment */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    s_list = list; /* shares the plain list's scroll-hint hook (ff_scr_settings_apply_scroll_hint) — only
                    * one of the two lists is ever built at a time per subview. */

    int32_t y = 0;

    /* [api] A02 slice D — SHOW CODE. A full-width pill above the lists,
     * present whether or not a code has resolved: the face it opens says
     * "no crew code yet" with the reason, which teaches more than a
     * control that silently does nothing. */
    settings_make_pill(list, "SHOW CODE", 0, y, row_w, FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE,
                       FF_THEME_COLOR_AMBER, 2, settings_crew_show_code_cb, NULL);
    y += FF_CREW_PAGE_ROW_H + FF_SETTINGS_ROW_GAP;

    /* [api] A02 slice D2 — START CREW / LEAVE CREW.
     *
     * Exactly one of the two is ever offered, and only when the shell
     * says the puck could actually carry it out (`can_start`/
     * `can_leave`, computed once — see `ff_shell_crew_op_status`). When
     * neither is offered the row is a muted SENTENCE, not a greyed
     * button: a disabled control tells a wearer that something is
     * possible and withheld, when the truth is that the puck cannot see
     * its radio's channels yet. Saying that is more use than a button
     * that would fail.
     *
     * Same full-width geometry as SHOW CODE above — no new sizes, and
     * clear of the 44 px hit floor by construction. */
    if (cw->can_start) {
        settings_make_pill(list, "START CREW", 0, y, row_w, FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE,
                           FF_THEME_COLOR_AMBER, 2, settings_crew_start_req_cb, NULL);
        y += FF_CREW_PAGE_ROW_H + FF_SETTINGS_ROW_GAP;
    } else if (cw->can_leave) {
        settings_make_pill(list, "LEAVE CREW", 0, y, row_w, FF_CREW_PAGE_ROW_H, FF_THEME_COLOR_SURFACE,
                           FF_THEME_COLOR_STALE_AMBER, 2, settings_crew_leave_req_cb, NULL);
        y += FF_CREW_PAGE_ROW_H + FF_SETTINGS_ROW_GAP;
    } else if (cw->region_unset) {
        /* S02's D2 amendment §A.1 requires this refusal be reported as
         * ITSELF, in these words. Without this branch the sentence below
         * would claim the puck is "still reading" a radio that has
         * finished answering and cannot legally transmit — a sentence
         * that is false and never resolves, over the one fact the wearer
         * could act on. */
        y = settings_crew_caption(list, y, row_w, settings_crew_fail_text(FF_APP_CREW_FAIL_REGION_UNSET));
    } else {
        y = settings_crew_caption(list, y, row_w,
                                   cw->link_connected
                                       ? "Your puck is still reading your radio's settings."
                                       : "Your puck can't reach its radio, so it can't change crews.");
    }

    y = settings_build_section_header(list, y, row_w, "PAIRED", /*first=*/true);
    for (uint8_t i = 0; i < cw->paired_count; i++) {
        settings_crew_build_paired_row(list, y, row_w, &cw->paired[i]);
        y += FF_CREW_ROW_STEP;
    }

    /* [api] A02 slice D §E — NOT TRACKED. Only ever built when there IS
     * an overflow: an empty section here would imply a cap problem that
     * does not exist. */
    if (cw->overflow_count > 0u) {
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "NOT TRACKED (%u)", (unsigned)cw->overflow_count);
        y = settings_build_section_header(list, y, row_w, hdr, /*first=*/false);
        y = settings_crew_caption(list, y, row_w,
                                   "More people are on this crew than your puck can track (8 is "
                                   "the limit). Hide someone to make room.");
        for (uint8_t i = 0; i < cw->overflow_count; i++) {
            settings_crew_build_overflow_row(list, y, row_w, &cw->overflow[i]);
            y += FF_CREW_ROW_STEP;
        }
    }

    /* [api] A02 slice D §C — HIDDEN. Same rule: no section when nobody
     * is hidden. */
    if (cw->hidden_count > 0u) {
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "HIDDEN (%u)", (unsigned)cw->hidden_count);
        y = settings_build_section_header(list, y, row_w, hdr, /*first=*/false);
        if (cw->hidden_full) {
            y = settings_crew_caption(list, y, row_w,
                                       "You've hidden as many people as your puck can remember "
                                       "(16). Unhide someone first.");
        }
        for (uint8_t i = 0; i < cw->hidden_count; i++) {
            settings_crew_build_hidden_row(list, y, row_w, &cw->hidden[i]);
            y += FF_CREW_ROW_STEP;
        }
    }

    y = settings_build_section_header(list, y, row_w, "HEARD", /*first=*/false);
    if (cw->heard_count == 0) {
        settings_crew_build_heard_empty(list, y, row_w, cw->link_connected);
        y += FF_CREW_PAGE_ROW_H;
    } else {
        for (uint8_t i = 0; i < cw->heard_count; i++) {
            settings_crew_build_heard_row(list, y, row_w, &cw->heard[i], cw->roster_full);
            y += FF_CREW_ROW_STEP;
        }
    }

    lv_obj_update_layout(list);
}

/* A02 slice D — SHOW CODE needs LVGL's optional QR widget. Both targets
 * ask for it (firmware/lv_conf.h; targets/esp32s3/sdkconfig.defaults),
 * but on ESP-IDF `sdkconfig.defaults` only seeds a sdkconfig that does
 * not exist yet: a board configured BEFORE this change still carries
 * `# CONFIG_LV_USE_QRCODE is not set`, that file wins, and the build
 * dies six lines deep in implicit-declaration errors that name LVGL
 * rather than the config. Say it once, in words that name the fix.
 * (Measured on the bring-up board's own sdkconfig, 2026-09-13.) */
#if !defined(LV_USE_QRCODE) || LV_USE_QRCODE == 0
#error "A02 slice D needs LV_USE_QRCODE. ESP-IDF: an EXISTING firmware/targets/esp32s3/sdkconfig overrides sdkconfig.defaults, so a board configured before this change still has it off - enable it in `idf.py menuconfig` (Component config -> LVGL configuration -> 3rd party libraries -> QR code), or delete that sdkconfig and rebuild. Sim: see firmware/lv_conf.h."
#endif

/* ---------------------------------------------------------------------
 * SHOW CODE face — A02 slice D (docs/specs/S02-core-crew.md's 2026-09-13
 * amendment §D; the copy is A02 §1.8's; the QR's own payload is the
 * 2026-09-15 amendment directly below)
 * ---------------------------------------------------------------------
 *
 *              [ QR of FIRE-4K9M7X ]
 *
 *                    FIRE-4K9M7X
 *
 *        Anyone who scans or types this is in your crew.
 *                       exact positions
 *                        [ BACK ]
 *
 * The code is DERIVED from the radio's own channel name (A02 §1.3 makes
 * `ChannelSettings.name` and the canonical code the same 11 bytes), so
 * there is no second source of truth and nothing extra to persist. A
 * channel name that is not a valid code reads as "no crew code yet" —
 * never rendered as one, because a fabricated code on this face is a
 * code somebody will type into their phone and then stand around
 * wondering why nobody appeared.
 *
 * **2026-09-15 amendment — the QR encodes the bare code, not the deep
 * link.** Owner report: the phone's scanner struggled with this face's
 * QR up close, decoding only from further away than a wearer showing a
 * puck across a tent has room for. Cause: this face encoded
 * `cw->invite_url` (`firefly://crew?v=1&code=FIRE-4K9M7X`, 35 ASCII
 * bytes), which LVGL's `lv_qrcode` — always BYTE mode, see below — puts
 * at QR version 3 (29x29 modules) in the 170px canvas below, under 6px a
 * module on this 1.46" glass. The bare canonical code alone
 * (`cw->crew_code`, `FIRE-4K9M7X`, 11 bytes) is what this face actually
 * needs the phone to read back — A02 §1.2's `CrewCode.parse` already
 * accepts it typed, scanned, or read aloud, tag and all — and it fits QR
 * version 1 (21x21 modules) in the same canvas, ~40% more px per module.
 * `cw->invite_url` is UNCHANGED and still built every projection
 * (`ff_shell.c`) for any consumer that does want the full deep link (the
 * app's own Start/Join QR keeps it, A02 §1.8; a future NFC share or
 * console dump could reuse it) — this face is simply no longer one of
 * them. The `_Static_assert` below pins the bare code's byte-mode fit
 * inside QR v1-M so a future change to `FF_CREWCODE_LEN` fails the build
 * instead of silently growing the version back up.
 *
 * The PRECISION line (S02's 2026-09-14 amendment, #47) is the one other
 * honest fact this face makes: "exact positions" when the crew
 * channel's own `position_precision` is proven to be exactly 32, and
 * "positions coarse — start the crew again" for every other case folded
 * into one — stated-but-wrong, never reported, or no crew channel at
 * all read the SAME to a wearer deciding whether to trust the puck's
 * positions. Sourced from `ff_app_crew_page_t.precision_exact`
 * (`shell_project_crew_page`), a live fact about the radio's channel
 * table — true the moment the crew's own row proves it, whether or not
 * a `ff_crewstart` START ever ran this session.
 *
 * Geometry, against the round 412 glass — MEASURED off the committed
 * golden, not eyeballed. The `lv_qrcode` canvas is 170 px (trimmed from
 * slice D's original 190 px to make room for the PRECISION line below
 * the caption — comfortably above the ~120 px floor a phone camera needs
 * even for the longer pre-amendment payload) and carries a 6 px light
 * border as its quiet zone, so the white ground the camera actually
 * sees is 170 + 2*6 = 182 px square. That leaves the code line, the
 * caption, the PRECISION line and a real 44 px BACK target room below
 * it inside the circle. `lv_qrcode` draws into a canvas of exactly the
 * size it is given, so the module size is whatever 170 / (modules +
 * 2*quiet-zone) works out to for this payload's version — LVGL handles
 * that; what matters here is that the dark-on-light polarity is NOT
 * inverted (a scanner expects dark modules on a light ground, and a
 * clever dark-theme QR is a QR that does not scan).
 *
 * `lv_qrcode_update` always calls `qrcodegen_encodeBinary` — BYTE mode,
 * unconditionally, never the alphanumeric mode qrcodegen also offers
 * (`src/libs/qrcode/lv_qrcode.c`, LVGL v9.5.0 pinned by
 * `firmware/CMakeLists.txt`). The 11-byte bare code still lands at
 * version 1 in that mode: QR version 1's ECC-MEDIUM byte-mode capacity
 * is 14 bytes (`qrcodegen_getMinFitVersion`), and 11 <= 14. Alphanumeric
 * mode is not in play here despite the code's alphabet being a subset of
 * it — worth pinning so nobody "fixes" a future regression by chasing a
 * mode switch that was never happening.
 *
 * The square is centred on the GLASS (FF_THEME_GLASS_CX = 208), not on
 * the 412 pixel array (206). That 2 px is not cosmetic: the panel sits
 * ~5 px left of the bezel's optical centre (ff_theme.h), and a square
 * hung off the panel centre instead puts its top-LEFT corner further
 * from the glass centre than one centred on the glass itself — outside
 * FF_THEME_GLASS_R on a large enough square, which is itself already
 * pulled 3 px in from the measured 203. Centred on the glass both top
 * corners land at the same radius and the asymmetry is gone. Pinned by
 * the assert below rather than by the golden, because a golden is a
 * pixel-diff against itself and would happily keep a corner over the
 * bezel lip forever.
 * ------------------------------------------------------------------- */
#define FF_CREWCODE_QR_PX     170
#define FF_CREWCODE_QR_BORDER 6 /* the QR's own quiet zone, in its light colour */
#define FF_CREWCODE_QR_BOX    (FF_CREWCODE_QR_PX + 2 * FF_CREWCODE_QR_BORDER)
/* TOP_MID aligns on the pixel array's centre; this nudges to the glass's. */
#define FF_CREWCODE_QR_DX     (FF_THEME_GLASS_CX - FF_THEME_PUCK_PX / 2)
#define FF_CREWCODE_QR_Y      34
/* Usability-review slice 3 (docs/reviews/puck-ux-usability-2026-09-15.md
 * §2.3 "SHOW CODE BACK") grows FF_CREWCODE_BTN_H to 80 (via
 * FF_SETTINGS_ROW_H) and moves FF_CREWCODE_BTN_Y up to make room inside
 * the glass — see that constant's own comment. That leaves 6 fewer
 * pixels above the BACK button for the code/caption/precision stack, so
 * this trailing gap after the QR (14 -> 8) gives them back: a code/
 * caption/precision block whose own internal spacing is otherwise
 * untouched, just started 6px earlier. */
#define FF_CREWCODE_CODE_Y    (FF_CREWCODE_QR_Y + FF_CREWCODE_QR_PX + 8)
#define FF_CREWCODE_CAPTION_Y (FF_CREWCODE_CODE_Y + 46)
/* A02 slice D2 amendment (#47) — the one PRECISION line, below the
 * (up to two-line) caption. */
#define FF_CREWCODE_PRECISION_Y (FF_CREWCODE_CAPTION_Y + 36)
#define FF_CREWCODE_BTN_W     120
#define FF_CREWCODE_BTN_H     FF_SETTINGS_ROW_H /* 80 (was 48) — usability review slice 3, finding 5 */
/* 326 -> 314: at 120px wide the BACK pill's farthest corner (its LEFT
 * edge — the button is centred on the framebuffer's x=206, 2px left of
 * the glass centre's 208) sits |dx|=62 from FF_THEME_GLASS_CX, so
 * FF_THEME_GLASS_R (200) caps |dy| at sqrt(200^2 - 62^2) = 190.2 — the
 * button's bottom can reach glass y <= 206 + 190.2 = 396.2. At BTN_Y=314,
 * H=80, bottom=394, comfortably inside. See the containment assert below,
 * which is the actual gate — this comment is the derivation, not the
 * check. */
#define FF_CREWCODE_BTN_Y     314

_Static_assert(FF_CREWCODE_QR_PX <= 220, "A02 slice D: the QR must stay <= 220px to fit the round glass");
/* 2026-09-15 amendment — this face's QR now encodes `cw->crew_code`
 * (FF_CREWCODE_LEN == 11 ASCII bytes), not `cw->invite_url`, precisely
 * so it stays QR version 1 (21x21 modules) instead of the deep link's
 * version 3 (29x29) — see the big comment above. `lv_qrcode_update`
 * always uses BYTE mode (`qrcodegen_encodeBinary`, never the
 * alphanumeric mode qrcodegen also offers) at ECC MEDIUM, whose QR
 * version 1 byte-mode capacity is 14 bytes
 * (`qrcodegen_getMinFitVersion`/the QR spec's own table) — so this
 * assert is the load-bearing one: a future change to `FF_CREWCODE_LEN`
 * (or a switch back to encoding a longer string here) that crosses 14
 * bytes silently buys back the exact module-size regression this
 * amendment fixes, and this fails the build instead. Cross-checked
 * against the live encoder, not just this arithmetic, by
 * `test_scr_crewcode_qr.c`. */
_Static_assert(FF_CREWCODE_LEN <= 14u,
               "SHOW CODE's QR payload (the bare crew code) must fit QR v1-M's 14-byte byte-mode "
               "capacity, or the code face silently grows past QR version 1 (21x21 modules)");
/* The QR's light ground must stay INSIDE the glass, corners included —
 * the quiet zone is part of the symbol, and a scanner that loses it
 * loses the symbol. Its top corners are the worst case; stated as
 * arithmetic, the same way the BACK pill's containment is below. */
_Static_assert((FF_CREWCODE_QR_BOX / 2) * (FF_CREWCODE_QR_BOX / 2) +
                       (FF_THEME_GLASS_CY - FF_CREWCODE_QR_Y) * (FF_THEME_GLASS_CY - FF_CREWCODE_QR_Y) <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "SHOW CODE's QR (quiet zone included) must stay inside FF_THEME_GLASS_R");
/* The PRECISION line has to land clear of the BACK pill above it — a
 * one-line chip-font label needs about 20px, stated conservatively
 * rather than measured off one render. */
_Static_assert(FF_CREWCODE_PRECISION_Y + 20 <= FF_CREWCODE_BTN_Y,
               "SHOW CODE's PRECISION line must clear the BACK button above it");
_Static_assert(FF_CREWCODE_BTN_H >= FF_THEME_MIN_HIT_PX, "SHOW CODE's BACK button must clear the 44px hit floor");
/* The BACK pill's lowest corners have to stay inside the GLASS, not the
 * framebuffer — usability-review slice 3 rewrote this onto
 * FF_THEME_GLASS_CX/CY/R for the same reason docs/hardware/tap-targets.md's
 * crew-op assert was: an assert written against the framebuffer's own
 * (206,206,206) circle can pass a layout whose corner is already under
 * the bezel lip, because the bezel eats asymmetrically (2px off the
 * LEFT). The button is centred on the framebuffer's x=206, so its LEFT
 * edge (146) is the one farther from the glass centre (208) — both are
 * checked so this can't quietly depend on which one happens to bind. */
#define FF_CREWCODE_BTN_DX_L (FF_THEME_GLASS_CX - (FF_THEME_PUCK_PX - FF_CREWCODE_BTN_W) / 2)
#define FF_CREWCODE_BTN_DX_R ((FF_THEME_PUCK_PX + FF_CREWCODE_BTN_W) / 2 - FF_THEME_GLASS_CX)
#define FF_CREWCODE_BTN_DY_B (FF_CREWCODE_BTN_Y + FF_CREWCODE_BTN_H - FF_THEME_GLASS_CY)
_Static_assert(FF_CREWCODE_BTN_DX_L * FF_CREWCODE_BTN_DX_L + FF_CREWCODE_BTN_DY_B * FF_CREWCODE_BTN_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "SHOW CODE's BACK button (left corner) must stay inside the round glass");
_Static_assert(FF_CREWCODE_BTN_DX_R * FF_CREWCODE_BTN_DX_R + FF_CREWCODE_BTN_DY_B * FF_CREWCODE_BTN_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "SHOW CODE's BACK button (right corner) must stay inside the round glass");

static void settings_crew_code_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* The puck disc every full-screen settings sub-face draws onto. One
 * definition, used by SHOW CODE below and by the two crew-operation
 * faces after it — the comment used to claim this factoring while the
 * SHOW CODE face still kept its own verbatim copy. */
static lv_obj_t *settings_face_disc(lv_obj_t *parent)
{
    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);
    return puck;
}

static void settings_build_crew_code_page(lv_obj_t *parent, ff_app_crew_page_t const *cw)
{
    lv_obj_t *puck = settings_face_disc(parent);

    bool const have_code = (cw->crew_code[0] != '\0') && (cw->invite_url[0] != '\0');

    if (have_code) {
        /* Dark modules on a light ground, always. The rest of this app
         * is a dark theme; a QR is not decoration, it is something a
         * phone camera has to read in a dark field, and inverting it to
         * match the theme would make it fail on a good fraction of
         * scanners. */
        lv_obj_t *qr = lv_qrcode_create(puck);
        lv_qrcode_set_size(qr, FF_CREWCODE_QR_PX);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
        /* 2026-09-15 amendment: the bare code, not `cw->invite_url` — see
         * the SHOW CODE header comment above and the QR v1 capacity
         * assert below. `cw->invite_url` is still built (ff_shell.c) and
         * still exactly what A02 §1.2 parses; this face just no longer
         * needs its extra 24 bytes on the glass. */
        lv_qrcode_update(qr, cw->crew_code, strlen(cw->crew_code));
        lv_obj_align(qr, LV_ALIGN_TOP_MID, FF_CREWCODE_QR_DX, FF_CREWCODE_QR_Y);
        /* A quiet zone in the QR's own light colour: the module pattern
         * has to be surrounded by light, and the puck behind it is
         * near-black. */
        lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(qr, FF_CREWCODE_QR_BORDER, 0);
        lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *code = lv_label_create(puck);
        lv_label_set_text(code, cw->crew_code);
        lv_obj_set_style_text_font(code, FF_THEME_FONT_DISTANCE, 0); /* the biggest face there is */
        lv_obj_set_style_text_color(code, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_text_letter_space(code, 2, 0);
        lv_obj_align(code, LV_ALIGN_TOP_MID, 0, FF_CREWCODE_CODE_Y);
        lv_obj_clear_flag(code, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *cap = lv_label_create(puck);
        lv_obj_set_width(cap, FF_THEME_PUCK_PX - 120);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
        lv_label_set_text(cap, "Anyone who scans or types this is in your crew.");
        lv_obj_set_style_text_font(cap, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, FF_CREWCODE_CAPTION_Y);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);

        /* A02 slice D2 amendment (#47) — the one other honest fact this
         * face makes. "exact positions" only when the crew channel's OWN
         * position_precision is PROVEN to be exactly 32; every other case
         * (stated-but-wrong, never reported, or the radio simply hasn't
         * answered this question yet) reads the same to a wearer deciding
         * whether to trust what the map shows — "coarse", with the one
         * thing that fixes it. */
        lv_obj_t *prec = lv_label_create(puck);
        lv_obj_set_width(prec, FF_THEME_PUCK_PX - 100);
        lv_label_set_long_mode(prec, LV_LABEL_LONG_WRAP);
        if (cw->precision_exact) {
            lv_label_set_text(prec, "exact positions");
            lv_obj_set_style_text_color(prec, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
        } else {
            lv_label_set_text(prec, "positions coarse - start the crew again");
            lv_obj_set_style_text_color(prec, lv_color_hex(FF_THEME_COLOR_STALE_AMBER), 0);
        }
        lv_obj_set_style_text_font(prec, FF_THEME_FONT_CHIP, 0);
        lv_obj_set_style_text_align(prec, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(prec, LV_ALIGN_TOP_MID, 0, FF_CREWCODE_PRECISION_Y);
        lv_obj_clear_flag(prec, LV_OBJ_FLAG_CLICKABLE);
    } else {
        /* The honest empty state. No QR, no placeholder code, and a
         * reason — a blank square with "----" under it would look like a
         * bug, and a fabricated code would be worse than one. */
        lv_obj_t *msg = lv_label_create(puck);
        lv_obj_set_width(msg, FF_THEME_PUCK_PX - 140);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_label_set_text(msg, "No crew code yet - start one on the phone");
        lv_obj_set_style_text_font(msg, FF_THEME_FONT_HEADLINE, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(FF_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, -20);
        lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
    }

    settings_make_pill(puck, "BACK", (FF_THEME_PUCK_PX - FF_CREWCODE_BTN_W) / 2, FF_CREWCODE_BTN_Y,
                       FF_CREWCODE_BTN_W, FF_CREWCODE_BTN_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED,
                       2, settings_crew_code_back_cb, NULL);
}

/* ---------------------------------------------------------------------
 * START CREW / LEAVE CREW — A02 slice D2 (docs/specs/S02-core-crew.md's
 * 2026-09-14 amendment)
 * ---------------------------------------------------------------------
 *
 * Two faces, both full-screen, both reached from the CREW page:
 *
 *   CONFIRM                          STATUS
 *   ------------------------         ------------------------
 *   Start a new crew?                SAVING TO YOUR RADIO
 *   Your radio saves it and          Your radio restarts for a
 *   restarts for a few seconds.      few seconds.
 *
 *   [ NOT NOW ]  [ START CREW ]      (no button while it works)
 *
 * The copy is plain-words on purpose (#303's "plain faces" pass): no
 * "channel", no "PSK", no "admin write". A wearer standing in a field
 * needs to know what is about to happen to their radio and roughly how
 * long it will be off, and that is all this face claims.
 *
 * The STATUS face's vocabulary is the RADIO's truth rather than a
 * reassuring summary — "checking it saved" is a real step that can
 * really fail, and it says so. READY is only ever reached by a read-back
 * that matched (ff_crewstart.h); nothing on this face can show DONE
 * because a frame was sent.
 * ------------------------------------------------------------------- */
#define FF_CREWOP_TITLE_Y   122
#define FF_CREWOP_BODY_Y    172
#define FF_CREWOP_BODY_W    (FF_THEME_PUCK_PX - 150)
#define FF_CREWOP_CODE_Y    168
#define FF_CREWOP_CODE_BODY_Y 212
/* Usability-review slice 3 (docs/reviews/puck-ux-usability-2026-09-15.md
 * finding 5 / finding 10 / §2.3 "Crew-op confirm buttons") grows the
 * confirm buttons to FF_SETTINGS_ROW_H (80) and the destructive-adjacent
 * gap to FF_HIT_MIN_GAP_DESTRUCTIVE_PX (16, matching the flare takeover's
 * own GO/DISMISS separation) — narrowing FF_CREWOP_BTN_W to 124 pays for
 * both, same trade docs/hardware/tap-targets.md's Radar FLARE derivation
 * made (narrower buys height/gap on a round face; see that file's
 * "FLARE could not reach 80px tall" section for the same shape of trade).
 *
 * FF_CREWOP_BTN_Y (the single-pill BACK/DONE position — no-snapshot's
 * BACK, FAILED's BACK, READY-leaving's DONE, READY-not-leaving's DONE)
 * does NOT move: a single 124px pill at y=300, height 80, is nowhere near
 * the glass edge (see the containment assert below) so it never needed
 * to.
 *
 * The two-pill CONFIRM row (NOT NOW / LEAVE, NOT NOW / START — the one
 * finding 10 actually names) gets its OWN, higher Y instead of sharing
 * FF_CREWOP_BTN_Y: at the new width/gap the row is 264px wide, and its
 * farthest corner (bottom-LEFT — the row is centred on the framebuffer's
 * x=206, 2px left of the glass centre's 208, so the LEFT edge is farther)
 * does not clear FF_THEME_GLASS_R at y=300 (see the math this constant's
 * containment assert states directly) the way it did at the old 48px
 * height. FF_CREWOP_CONFIRM_BTN_Y is the highest the row can sit while
 * still landing directly under FF_CREWOP_BODY_Y's text, derived the same
 * "measure the actual corner" way as every other constant in this file's
 * geometry — not shared with the single-pill rows, which have no reason
 * to move up into content that is not there on those faces.
 *
 * READY-not-leaving's SHOW CODE (BTN2) is deliberately NOT part of this
 * growth: it is not named by this slice's ask, and there is no room for
 * it to grow anyway — the code label above it (FF_CREWOP_CODE_Y) and
 * DONE below it (now at its full 80px) leave exactly the old 48px gap
 * this pill has always used. FF_CREWOP_BTN2_H keeps its own, independent
 * value for this reason (same call as FF_CALCAL_BTN_H / FF_CREW_PAGE_ROW_H
 * above — see either one's comment), and FF_CREWOP_BTN2_Y is now a plain
 * literal rather than derived from FF_CREWOP_BTN_Y, since the two no
 * longer share a height. */
#define FF_CREWOP_BTN_H         FF_SETTINGS_ROW_H
#define FF_CREWOP_BTN_Y         300
#define FF_CREWOP_CONFIRM_BTN_Y 274
#define FF_CREWOP_BTN2_H        48
#define FF_CREWOP_BTN2_Y        242
#define FF_CREWOP_BTN_W         124
#define FF_CREWOP_BTN_GAP       FF_HIT_MIN_GAP_DESTRUCTIVE_PX

_Static_assert(FF_CREWOP_BTN_H >= FF_THEME_MIN_HIT_PX, "crew confirm buttons must clear the 44px hit floor");
_Static_assert(FF_CREWOP_BTN2_H >= FF_THEME_MIN_HIT_PX, "the READY page's SHOW CODE pill must clear the 44px hit floor");
_Static_assert(FF_CREWOP_BTN_GAP >= FF_HIT_MIN_GAP_DESTRUCTIVE_PX,
               "NOT NOW / LEAVE is a destructive-adjacent-to-cancel pair; its gap must clear the destructive "
               "floor, matching the flare takeover's 16px (finding 10)");

/* Containment, against the GLASS and not against the 412 pixel array.
 *
 * The two are not the same circle: the panel sits ~5 px left of the
 * bezel's optical centre, so FF_THEME_GLASS_CX is 208 against the
 * array's 206, and FF_THEME_GLASS_R is 200 (203 measured, pulled in 3)
 * against the array's 206 — see ff_theme.h, and the SHOW CODE face's own
 * assert above, which was rewritten onto these constants for exactly
 * this reason. An assert written against 206/206 passes a layout whose
 * bottom-LEFT corner is already under the bezel lip, because the left
 * side is the side the offset eats.
 *
 * The pill row is `2*W + GAP` wide, centred on the array
 * (LV_ALIGN_TOP_MID), so its corners are at
 *   x = (FF_THEME_PUCK_PX -/+ row_w) / 2,  y = FF_CREWOP_CONFIRM_BTN_Y + BTN_H
 * and the bottom-left corner is the worst case. Stated as arithmetic
 * rather than eyeballed off a render: a golden is a pixel-diff against
 * itself and would keep a corner over the bezel forever. (The pills are
 * drawn with FF_SETTINGS_PILL_RADIUS, so the real corner is rounded and
 * this square-corner test is the conservative one.) */
#define FF_CREWOP_ROW_W  (2 * FF_CREWOP_BTN_W + FF_CREWOP_BTN_GAP)
#define FF_CREWOP_DX_L   (FF_THEME_GLASS_CX - (FF_THEME_PUCK_PX - FF_CREWOP_ROW_W) / 2)
#define FF_CREWOP_DX_R   ((FF_THEME_PUCK_PX + FF_CREWOP_ROW_W) / 2 - FF_THEME_GLASS_CX)
#define FF_CREWOP_DY_B   (FF_CREWOP_CONFIRM_BTN_Y + FF_CREWOP_BTN_H - FF_THEME_GLASS_CY)

_Static_assert(FF_CREWOP_DX_L * FF_CREWOP_DX_L + FF_CREWOP_DY_B * FF_CREWOP_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "the crew confirm button row's bottom-LEFT corner must stay inside FF_THEME_GLASS_R");
_Static_assert(FF_CREWOP_DX_R * FF_CREWOP_DX_R + FF_CREWOP_DY_B * FF_CREWOP_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "the crew confirm button row's bottom-RIGHT corner must stay inside FF_THEME_GLASS_R");
/* The single-pill BACK/DONE rows (FF_CREWOP_BTN_Y, unmoved) — checked
 * directly rather than assumed, now that they no longer share a Y with
 * the two-pill row above. */
#define FF_CREWOP_SOLO_DX ((FF_CREWOP_BTN_W + 1) / 2 + (FF_THEME_GLASS_CX - FF_THEME_PUCK_PX / 2))
#define FF_CREWOP_SOLO_DY_B (FF_CREWOP_BTN_Y + FF_CREWOP_BTN_H - FF_THEME_GLASS_CY)
_Static_assert(FF_CREWOP_SOLO_DX * FF_CREWOP_SOLO_DX + FF_CREWOP_SOLO_DY_B * FF_CREWOP_SOLO_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "the crew confirm/status page's single BACK/DONE pill must stay inside FF_THEME_GLASS_R");
/* The READY page's SHOW CODE pill (BTN2) — its own, smaller, unmoved
 * geometry; checked directly for the same reason. */
#define FF_CREWOP_BTN2_DX ((FF_CREWOP_BTN_W + 1) / 2 + (FF_THEME_GLASS_CX - FF_THEME_PUCK_PX / 2))
#define FF_CREWOP_BTN2_DY_B (FF_CREWOP_BTN2_Y + FF_CREWOP_BTN2_H - FF_THEME_GLASS_CY)
_Static_assert(FF_CREWOP_BTN2_DX * FF_CREWOP_BTN2_DX + FF_CREWOP_BTN2_DY_B * FF_CREWOP_BTN2_DY_B <=
                   FF_THEME_GLASS_R * FF_THEME_GLASS_R,
               "the crew READY face's SHOW CODE pill must stay inside FF_THEME_GLASS_R");
/* BTN2 (SHOW CODE) and the single-pill DONE below it on the same READY
 * face must not collide now that they no longer share one derived
 * height — pinned explicitly since the old formula that guaranteed this
 * for free is gone. */
_Static_assert(FF_CREWOP_BTN2_Y + FF_CREWOP_BTN2_H + 10 <= FF_CREWOP_BTN_Y,
               "the READY face's SHOW CODE pill must clear DONE below it by at least 10px");

static void settings_crewop_title(lv_obj_t *puck, char const *text, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(puck);
    lv_obj_set_width(lbl, FF_CREWOP_BODY_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, FF_CREWOP_TITLE_Y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void settings_crewop_body(lv_obj_t *puck, char const *text, int32_t y)
{
    lv_obj_t *lbl = lv_label_create(puck);
    lv_obj_set_width(lbl, FF_CREWOP_BODY_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_MSG_BODY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

/* A centred row of one or two pills. `right_text == NULL` centres the
 * single pill, rather than leaving it lopsided where a pair would be.
 * `h` is explicit (not always FF_CREWOP_BTN_H) because this face now has
 * TWO button heights: the grown 80px primary/confirm rows this slice
 * raised, and the READY page's own un-grown SHOW CODE pill (BTN2) — see
 * FF_CREWOP_BTN2_H's own comment for why that one stays behind. */
static void settings_crewop_buttons_h(lv_obj_t *puck, int32_t y, int32_t h, char const *left_text, uint32_t left_fg,
                                       lv_event_cb_t left_cb, char const *right_text, uint32_t right_fg,
                                       lv_event_cb_t right_cb)
{
    if (right_text == NULL) {
        settings_make_pill(puck, left_text, (FF_THEME_PUCK_PX - FF_CREWOP_BTN_W) / 2, y, FF_CREWOP_BTN_W, h,
                           FF_THEME_COLOR_SURFACE, left_fg, 2, left_cb, NULL);
        return;
    }
    int32_t const total = 2 * FF_CREWOP_BTN_W + FF_CREWOP_BTN_GAP;
    int32_t const x0 = (FF_THEME_PUCK_PX - total) / 2;
    settings_make_pill(puck, left_text, x0, y, FF_CREWOP_BTN_W, h, FF_THEME_COLOR_SURFACE,
                       left_fg, 2, left_cb, NULL);
    settings_make_pill(puck, right_text, x0 + FF_CREWOP_BTN_W + FF_CREWOP_BTN_GAP, y, FF_CREWOP_BTN_W, h,
                       FF_THEME_COLOR_SURFACE, right_fg, 2, right_cb, NULL);
}

/* The common case — every caller except the READY page's stacked SHOW
 * CODE pill wants the primary FF_CREWOP_BTN_H (80). */
static void settings_crewop_buttons(lv_obj_t *puck, int32_t y, char const *left_text, uint32_t left_fg,
                                     lv_event_cb_t left_cb, char const *right_text, uint32_t right_fg,
                                     lv_event_cb_t right_cb)
{
    settings_crewop_buttons_h(puck, y, FF_CREWOP_BTN_H, left_text, left_fg, left_cb, right_text, right_fg, right_cb);
}

static void settings_build_crew_confirm_page(lv_obj_t *parent, ff_app_crew_page_t const *cw)
{
    lv_obj_t *puck = settings_face_disc(parent);

    if (cw->op == FF_APP_CREW_OP_LEAVE) {
        if (!cw->has_snapshot) {
            /* The honest refusal, up front. "Your radio goes back to its
             * old settings" is a promise this puck cannot keep when it
             * never recorded them — a puck handed a crew channel by
             * somebody else's CLI, say — and offering the button anyway
             * would be offering to do something else entirely (a reset
             * to the factory default) under that sentence. */
            settings_crewop_title(puck, "Can't leave yet", FF_THEME_COLOR_INK);
            settings_crewop_body(puck,
                                  "Your puck has no record of your radio's old settings, so it can't put "
                                  "them back. Change crews from the phone instead.",
                                  FF_CREWOP_BODY_Y);
            settings_crewop_buttons(puck, FF_CREWOP_BTN_Y, "BACK", FF_THEME_COLOR_MUTED,
                                     settings_crew_dismiss_cb, NULL, 0, NULL);
            return;
        }
        settings_crewop_title(puck, "Leave the crew?", FF_THEME_COLOR_INK);
        settings_crewop_body(puck, "Your radio goes back to its old settings.", FF_CREWOP_BODY_Y);
        /* The destructive confirm pair (finding 10) — its own, higher Y;
         * see FF_CREWOP_CONFIRM_BTN_Y's comment for why it can't share
         * FF_CREWOP_BTN_Y with the single-pill rows any more. */
        settings_crewop_buttons(puck, FF_CREWOP_CONFIRM_BTN_Y, "NOT NOW", FF_THEME_COLOR_MUTED,
                                 settings_crew_dismiss_cb, "LEAVE", FF_THEME_COLOR_STALE_AMBER,
                                 settings_crew_leave_confirm_cb);
        return;
    }

    settings_crewop_title(puck, "Start a new crew?", FF_THEME_COLOR_INK);
    settings_crewop_body(puck, "Your radio saves it and restarts for a few seconds.", FF_CREWOP_BODY_Y);
    settings_crewop_buttons(puck, FF_CREWOP_CONFIRM_BTN_Y, "NOT NOW", FF_THEME_COLOR_MUTED, settings_crew_dismiss_cb,
                             "START", FF_THEME_COLOR_AMBER, settings_crew_start_confirm_cb);
}

/* One sentence per failure. Every one of these is something a wearer can
 * either act on or at least understand — "it didn't work" is not a
 * report, and a bare error code on a festival puck is worse. */
static char const *settings_crew_fail_text(ff_app_crew_fail_t f)
{
    switch (f) {
    case FF_APP_CREW_FAIL_NO_LINK: return "Your puck can't reach its radio right now.";
    case FF_APP_CREW_FAIL_REGION_UNSET: return "Set the radio region on the phone first.";
    case FF_APP_CREW_FAIL_NO_ENTROPY: return "This puck can't make a safe code.";
    case FF_APP_CREW_FAIL_NO_SNAPSHOT: return "No record of your radio's old settings.";
    case FF_APP_CREW_FAIL_SEND: return "Your radio wouldn't take the change.";
    case FF_APP_CREW_FAIL_NAK: return "Your radio refused the change.";
    case FF_APP_CREW_FAIL_TIMEOUT_ACK: return "Your radio never answered.";
    case FF_APP_CREW_FAIL_TIMEOUT_VERIFY: return "Your radio didn't come back to be checked.";
    case FF_APP_CREW_FAIL_MISMATCH: return "Your radio saved something else.";
    case FF_APP_CREW_FAIL_NONE: break;
    }
    /* Unreachable while phase == FAILED, and deliberately not a cheerful
     * blank: a face with no reason on it is the thing this enum exists
     * to prevent. */
    return "Something went wrong.";
}

static void settings_build_crew_status_page(lv_obj_t *parent, ff_app_crew_page_t const *cw)
{
    lv_obj_t *puck = settings_face_disc(parent);
    bool const leaving = (cw->op == FF_APP_CREW_OP_LEAVE);

    switch (cw->phase) {
    case FF_APP_CREW_PHASE_IDLE:
    case FF_APP_CREW_PHASE_GENERATING:
        settings_crewop_title(puck, "MAKING A CODE", FF_THEME_COLOR_AMBER);
        settings_crewop_body(puck, "One moment.", FF_CREWOP_BODY_Y);
        return;

    case FF_APP_CREW_PHASE_WRITING:
        settings_crewop_title(puck, "SAVING TO YOUR RADIO", FF_THEME_COLOR_AMBER);
        settings_crewop_body(puck, "Your radio restarts for a few seconds.", FF_CREWOP_BODY_Y);
        return;

    case FF_APP_CREW_PHASE_VERIFYING:
        /* Named out loud because it is a real step that can really fail.
         * A face that said "saving..." through the read-back would be
         * hiding the only part of this that proves anything. */
        settings_crewop_title(puck, "CHECKING IT SAVED", FF_THEME_COLOR_AMBER);
        settings_crewop_body(puck, "Reading your radio back to make sure.", FF_CREWOP_BODY_Y);
        return;

    case FF_APP_CREW_PHASE_READY:
        settings_crewop_title(puck, leaving ? "LEFT THE CREW" : "CREW STARTED", FF_THEME_COLOR_LIVE_GREEN);
        if (leaving) {
            settings_crewop_body(puck, "Your radio is back on its old settings.", FF_CREWOP_BODY_Y);
            settings_crewop_buttons(puck, FF_CREWOP_BTN_Y, "DONE", FF_THEME_COLOR_MUTED,
                                     settings_crew_dismiss_cb, NULL, 0, NULL);
            return;
        }
        /* One short line here, not the SHOW CODE face's full sentence:
         * this face has to fit the code, a caption and two 44 px
         * targets between the title and the bottom of the glass, and a
         * two-line caption is what pushes the code into the buttons. */
        settings_crewop_body(puck, "Show this to your crew.", FF_CREWOP_CODE_BODY_Y);
        /* The code, from the RUN that just succeeded. Rendered only
         * here, on READY — the machine does not fill `pending_code` for
         * a leave, and showing a minted code before the radio accepted
         * it is exactly the confidently-wrong screen the verify step
         * exists to prevent. */
        if (cw->pending_code[0] != '\0') {
            lv_obj_t *code = lv_label_create(puck);
            lv_label_set_text(code, cw->pending_code);
            lv_obj_set_style_text_font(code, FF_THEME_FONT_NAME, 0);
            lv_obj_set_style_text_color(code, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
            lv_obj_set_style_text_letter_space(code, 2, 0);
            lv_obj_align(code, LV_ALIGN_TOP_MID, 0, FF_CREWOP_CODE_Y);
            lv_obj_clear_flag(code, LV_OBJ_FLAG_CLICKABLE);
        }
        /* BTN2 keeps its own, pre-pass height — see FF_CREWOP_BTN2_H's
         * comment for why this one pill is not part of this slice's
         * growth. */
        settings_crewop_buttons_h(puck, FF_CREWOP_BTN2_Y, FF_CREWOP_BTN2_H, "SHOW CODE", FF_THEME_COLOR_AMBER,
                                   settings_crew_show_code_cb, NULL, 0, NULL);
        settings_crewop_buttons(puck, FF_CREWOP_BTN_Y, "DONE", FF_THEME_COLOR_MUTED,
                                 settings_crew_dismiss_cb, NULL, 0, NULL);
        return;

    case FF_APP_CREW_PHASE_FAILED:
        settings_crewop_title(puck, leaving ? "COULDN'T LEAVE" : "COULDN'T START", FF_THEME_COLOR_STALE_AMBER);
        settings_crewop_body(puck, settings_crew_fail_text(cw->fail), FF_CREWOP_BODY_Y);
        settings_crewop_buttons(puck, FF_CREWOP_BTN_Y, "BACK", FF_THEME_COLOR_MUTED,
                                 settings_crew_dismiss_cb, NULL, 0, NULL);
        return;
    }
}

/* ---------------------------------------------------------------------
 * COMPASS CAL ritual page — S12 step 3 (docs/specs/S12-first-run.md
 * Step 3, the compass calibration figure-eight). A small fixed set of
 * centered elements on a self-contained puck — scr_power_menu.c's
 * `ff_scr_power_menu_build` shape, not CREW's scrolling list; nothing
 * here needs to scroll.
 * ------------------------------------------------------------------- */
#define FF_CALCAL_TITLE_Y    34
/* PR #311 review (N-series): 78 -> 84. The instruction line is one
 * sentence, "Rotate the puck slowly in a figure eight", which measures
 * 289px at FF_THEME_FONT_CHIP (lv_text_get_size). Re-framing this band
 * against the BEZEL's glass narrowed it from 302px to 282px, so the line
 * started wrapping — and wrapping a 39-character sentence at 282px
 * leaves "eight" alone on the second row, which is what
 * compass_cal_ritual.png showed.
 *
 * The band's own top edge is what binds here (it is 128px above the
 * glass centre; the bottom edge is nearer), so the fix is 6px of
 * descent, not a narrower font or shorter copy: at y 84 the chord gives
 * 292px, the sentence fits on one line again, and the line's far corner
 * sits sqrt(148^2 + 122^2) = 191.8px from the glass centre — 8px inside
 * FF_THEME_GLASS_R. The ring below starts at y 130, so a 16px line at 84
 * still clears it by 30px. */
#define FF_CALCAL_INSTR_Y    84
#define FF_CALCAL_INSTR_H    50
#define FF_CALCAL_RING_DIAM  140
#define FF_CALCAL_RING_Y     130
#define FF_CALCAL_SAMPLES_Y  (FF_CALCAL_RING_Y + FF_CALCAL_RING_DIAM + 10)
#define FF_CALCAL_BTN_W      120
/* Deliberately its OWN constant, not FF_SETTINGS_ROW_H, since usability-
 * review slice 3 raised that to 80 and this pair of buttons does not have
 * the room to follow: at FF_CALCAL_BTN_Y (below) the two-pill row's own
 * band is already flush against FF_CALCAL_SAMPLES_Y's text above it (the
 * pre-pass 48px height leaves this face's tightest margin on the whole
 * device — see that constant's own derivation comment). Growing to 80
 * needs either narrower buttons (a real width cut, this face's own
 * version of the flare takeover's "costs the starburst" trade) or moved
 * content above, and this slice's ask did not name this face — kept at
 * 48 (still clears the 44px absolute floor) rather than silently
 * reflowing a screen nobody asked to change. Same call as the CREW page's
 * FF_CREW_PAGE_ROW_H just above; see that constant's comment. */
#define FF_CALCAL_BTN_H      48
#define FF_CALCAL_BTN_GAP    16
/* Measured against the round glass (test_face_hit_targets.c's own
 * sweep, not hand math — a first pass at Y=324 shipped off-glass
 * buttons, caught by that sweep against this fixture): at this Y/height
 * the inscribed-circle chord is ~278px wide (ff_layout_safe_margin_x,
 * FF_SETTINGS_SAFETY_PX=10 buffer), comfortably over the 256px both
 * buttons + gap need, with ~20px to spare each side. */
#define FF_CALCAL_BTN_Y      300

_Static_assert(FF_CALCAL_BTN_H >= FF_THEME_MIN_HIT_PX, "compass-cal buttons must clear the 44px hit-target floor");

static void settings_calcal_cancel_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_COMPASS_CAL_CANCEL, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_calcal_finish_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_COMPASS_CAL_FINISH, .u = {0}};
    ff_intent_emit(&in);
}

/* A ring or ring-segment centered horizontally with its TOP at `top_y` —
 * same "thin lv_arc wrapper, MAIN part only, no clickable/scrollable"
 * shape scr_launcher.c's `launcher_mk_arc`/scr_radar.c's
 * `radar_make_cluster_wedge` already establish (not a fourth
 * reimplementation). Rotated -90 so `[0, sweep_deg)` starts at 12
 * o'clock and sweeps clockwise, the same "progress reads like a clock"
 * convention this codebase's wall-clock/battery displays use, rather
 * than lv_arc's own native 0=3-o'clock zero point. */
static void settings_calcal_ring(lv_obj_t *parent, int32_t top_y, float sweep_deg, uint32_t color_hex)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, FF_CALCAL_RING_DIAM, FF_CALCAL_RING_DIAM);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, top_y);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0.0f, (lv_value_precise_t)sweep_deg);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
}

static void settings_build_compass_cal_page(lv_obj_t *parent, ff_app_compass_cal_t const *cc)
{
    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "CALIBRATE COMPASS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, FF_CALCAL_TITLE_Y);

    int32_t const instr_margin = settings_safe_margin_x(FF_CALCAL_INSTR_Y, FF_CALCAL_INSTR_H);
    int32_t const instr_w = FF_THEME_PUCK_PX - 2 * instr_margin;
    lv_obj_t *instr = lv_label_create(puck);
    lv_label_set_long_mode(instr, LV_LABEL_LONG_WRAP);
    lv_label_set_text(instr, "Rotate the puck slowly in a figure eight");
    lv_obj_set_width(instr, instr_w);
    lv_obj_set_style_text_font(instr, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(instr, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(instr, instr_margin, FF_CALCAL_INSTR_Y);

    /* Progress ring: a dim full-circle track, then an amber arc spanning
     * `progress_pct` of it. `cc->active` is true for every real render
     * of this page (the shell only sets FF_SETTINGS_SUB_COMPASS_CAL from
     * FF_INTENT_COMPASS_CAL_START, which starts the session in the same
     * dispatch) — the `!active` fallback (0%) only matters for a
     * fixture/golden that renders this page with no live session. */
    settings_calcal_ring(puck, FF_CALCAL_RING_Y, 359.9f, FF_THEME_COLOR_SURFACE);
    int const pct = cc->active ? cc->progress_pct : 0;
    float const sweep_deg = (float)pct * 3.6f; /* 100% == 360 degrees */
    if (sweep_deg > 0.05f) {
        settings_calcal_ring(puck, FF_CALCAL_RING_Y, sweep_deg, FF_THEME_COLOR_AMBER);
    }

    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    lv_obj_t *pct_lbl = lv_label_create(puck);
    lv_label_set_text(pct_lbl, pct_buf);
    lv_obj_set_style_text_font(pct_lbl, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(pct_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(pct_lbl, LV_ALIGN_TOP_MID, 0, FF_CALCAL_RING_Y + FF_CALCAL_RING_DIAM / 2 - 12);

    char samp_buf[32];
    snprintf(samp_buf, sizeof(samp_buf), "%u samples", cc->active ? cc->sample_count : 0u);
    lv_obj_t *samp_lbl = lv_label_create(puck);
    lv_label_set_text(samp_lbl, samp_buf);
    lv_obj_set_style_text_font(samp_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(samp_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(samp_lbl, LV_ALIGN_TOP_MID, 0, FF_CALCAL_SAMPLES_Y);

    /* CANCEL always; DONE only once the session itself says it would
     * succeed (`can_finish`) — a shown button never calls into a finish
     * attempt the session's own state already knows will fail. */
    bool const show_done = cc->active && cc->can_finish;
    int32_t const total_w = show_done ? (2 * FF_CALCAL_BTN_W + FF_CALCAL_BTN_GAP) : FF_CALCAL_BTN_W;
    int32_t const start_x = (FF_THEME_PUCK_PX - total_w) / 2;

    ff_scr_pill_cfg_t cancel_cfg = {
        .w = FF_CALCAL_BTN_W,
        .h = FF_CALCAL_BTN_H,
        .use_pos = true,
        .x = start_x,
        .y = FF_CALCAL_BTN_Y,
        .radius = LV_RADIUS_CIRCLE,
        .filled = false,
        .border_width = 3,
        .bg_hex = FF_THEME_COLOR_MUTED,
        .fg_hex = FF_THEME_COLOR_INK,
        .press = FF_SCR_PILL_PRESS_TINT,
        .press_tint_hex = FF_THEME_COLOR_MUTED,
        .font = FF_THEME_FONT_NAME,
        .letter_space = 0,
        .cb = settings_calcal_cancel_cb,
        .user_data = NULL,
    };
    ff_scr_pill_create(puck, "CANCEL", &cancel_cfg);

    if (show_done) {
        ff_scr_pill_cfg_t done_cfg = cancel_cfg;
        done_cfg.x = start_x + FF_CALCAL_BTN_W + FF_CALCAL_BTN_GAP;
        done_cfg.filled = true;
        done_cfg.border_width = 0;
        done_cfg.bg_hex = FF_THEME_COLOR_AMBER;
        done_cfg.fg_hex = FF_THEME_COLOR_BG;
        done_cfg.press = FF_SCR_PILL_PRESS_DIM;
        done_cfg.cb = settings_calcal_finish_cb;
        ff_scr_pill_create(puck, "DONE", &done_cfg);
    }
}

/* ---------------------------------------------------------------------
 * NAME editor — the "NAME" row's full-screen T9 page (reuses core's
 * `ff_t9.h` engine via the shell-owned `name_draft`; NOT the Compose
 * screen's own keypad renderer — see ff_intent.h's FF_INTENT_
 * SETTINGS_OPEN_NAME_EDIT doc comment for why this feature keeps its
 * own small, independent keypad rather than branching scr_compose.c's
 * already-heavily-amended ABC/123/SYM/PRED rendering on "which draft is
 * this"). Letters/digits/space only (S12's own charset rule) — two
 * pages, ABC and 123, no SYM/PRED.
 *
 * Header row: a BACK circle (CREW's own convention, `settings_crew_
 * back_cb` — genuinely reused, not re-implemented, since a BACK press
 * means exactly the same thing on either page) at the safe left margin,
 * a DONE pill at the safe right margin — the "SEND relocation" idea S08
 * settled on for Compose (a commit action belongs in the header, not
 * fighting the keypad for room below).
 *
 * Below that: the live draft text (committed + pending, straight from
 * `ff_app_name_edit_t.text`), a MODE indicator, then a 3x3 letter/digit
 * grid and a bottom DEL/SPACE/MODE row. Every row's width comes from
 * `settings_safe_margin_x` at that row's own Y — the same "never hand
 * math" discipline every other row in this file uses — so the sweep
 * (test_face_hit_targets.c) is the real gate on whether this geometry
 * is actually safe, not eyeballing it.
 * ------------------------------------------------------------------- */
#define FF_NAMEEDIT_HDR_Y    20
#define FF_NAMEEDIT_HDR_PX   FF_THEME_MIN_HIT_PX /* 44 — back circle + DONE pill height */
#define FF_NAMEEDIT_DONE_W   64
#define FF_NAMEEDIT_TITLE_Y  74
#define FF_NAMEEDIT_TEXT_Y   98
#define FF_NAMEEDIT_TEXT_H   32
#define FF_NAMEEDIT_MODE_Y   130
#define FF_NAMEEDIT_GRID_Y   142
#define FF_NAMEEDIT_KEY_H    FF_THEME_MIN_HIT_PX /* 44 */
#define FF_NAMEEDIT_ROW_GAP  8
#define FF_NAMEEDIT_ROW_STEP (FF_NAMEEDIT_KEY_H + FF_NAMEEDIT_ROW_GAP) /* 52 */
_Static_assert(FF_NAMEEDIT_KEY_H >= FF_THEME_MIN_HIT_PX, "name-editor keys must clear the 44px hit-target floor");

/* ABC mode's key legends — mirrors scr_compose.c's own kAbcLegends
 * content (a separate, file-static copy: this screen's keypad is
 * deliberately its own small renderer, not a shared component — see
 * this section's own top comment). Index 0 unused (SPACE is its own
 * dedicated bottom-row key, never part of the 1-9 grid here). */
static char const *const kNameAbcLegends[10] = {
    "", ".,?!", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ",
};

static void settings_name_key_pressed(uint8_t key)
{
    ff_intent_t in = {.kind = FF_INTENT_NAME_T9_KEY, .u = {.t9_key = key}};
    if (s_settings.name_edit.mode == FF_APP_NAME_EDIT_123) {
        char digit[2] = {(char)('0' + key), '\0'};
        in.kind = FF_INTENT_NAME_T9_INSERT;
        in.u.text = digit;
        ff_intent_emit(&in); /* emit here: `digit` doesn't outlive this block */
        return;
    }
    ff_intent_emit(&in);
}

static void settings_name_key_click_cb(lv_event_t *e)
{
    uintptr_t key = (uintptr_t)lv_event_get_user_data(e);
    settings_name_key_pressed((uint8_t)key);
}

static void settings_name_space_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_NAME_T9_SPACE, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_name_backspace_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_NAME_T9_BACKSPACE, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_name_mode_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_NAME_T9_MODE, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_name_done_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_NAME_COMMIT, .u = {0}};
    ff_intent_emit(&in);
}

/* One row of the 3x9 grid: keys [first_key, first_key+2], evenly split
 * across the row's own safe width. `mode` picks the legend (ABC letters
 * vs. literal digits). */
static void settings_name_build_grid_row(lv_obj_t *puck, ff_app_name_edit_mode_t mode, int32_t y,
                                         uint8_t first_key)
{
    int32_t const margin = settings_safe_margin_x(y, FF_NAMEEDIT_KEY_H);
    int32_t const row_w = FF_THEME_PUCK_PX - 2 * margin;
    int32_t const key_w = (row_w - 2 * FF_NAMEEDIT_ROW_GAP) / 3;

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t const key = (uint8_t)(first_key + i);
        int32_t const x = margin + (int32_t)i * (key_w + FF_NAMEEDIT_ROW_GAP);
        /* `key` is always 1-9 by construction (the 3x3 grid never calls
         * this with a `first_key` that could reach 10), but `uint8_t`'s
         * full range is 0-255 to GCC's format-truncation checker
         * (-Wformat-truncation, CLAUDE.md: GCC is the build authority,
         * not clang, which stayed silent here) — `% 10u` proves the
         * single-digit bound to the compiler, not just to a human
         * reading the call sites. */
        char digit_buf[2];
        char const *legend = kNameAbcLegends[key];
        if (mode == FF_APP_NAME_EDIT_123) {
            snprintf(digit_buf, sizeof(digit_buf), "%u", (unsigned)key % 10u);
            legend = digit_buf;
        }
        lv_obj_t *btn = settings_make_pill(puck, legend, x, y, key_w, FF_NAMEEDIT_KEY_H, FF_THEME_COLOR_SURFACE,
                                           FF_THEME_COLOR_INK, 0, settings_name_key_click_cb,
                                           (void *)(uintptr_t)key);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), FF_THEME_FONT_CHIP, 0);
    }
}

static void settings_build_name_edit_page(lv_obj_t *parent, ff_app_name_edit_t const *ne)
{
    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);

    /* Header: BACK circle (left) + DONE pill (right), both at the safe
     * margin for this row's own Y band. */
    int32_t const hdr_margin = settings_safe_margin_x(FF_NAMEEDIT_HDR_Y, FF_NAMEEDIT_HDR_PX);

    lv_obj_t *back = ff_scr_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_NAMEEDIT_HDR_PX, FF_NAMEEDIT_HDR_PX);
    lv_obj_set_pos(back, hdr_margin, FF_NAMEEDIT_HDR_Y);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    /* puck-ux-usability-review slice 1, finding 2 (review fix) — see the
     * CREW page's own back-circle comment above (settings_build_crew_page):
     * the identical raw-wrapper gap, the identical fix. */
    settings_hit_add_press_feedback(back);
    lv_obj_add_event_cb(back, settings_crew_back_cb, LV_EVENT_CLICKED, NULL); /* BACK means the same thing everywhere */
    lv_obj_t *glyph = lv_label_create(back);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(glyph, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph);

    ff_scr_pill_cfg_t done_cfg = {
        .w = FF_NAMEEDIT_DONE_W,
        .h = FF_NAMEEDIT_HDR_PX,
        .use_pos = true,
        .x = FF_THEME_PUCK_PX - hdr_margin - FF_NAMEEDIT_DONE_W,
        .y = FF_NAMEEDIT_HDR_Y,
        .radius = LV_RADIUS_CIRCLE,
        .filled = true,
        .bg_hex = FF_THEME_COLOR_AMBER,
        .fg_hex = FF_THEME_COLOR_BG,
        .press = FF_SCR_PILL_PRESS_DIM,
        .font = FF_THEME_FONT_LABEL,
        .letter_space = 1,
        .cb = settings_name_done_cb,
        .user_data = NULL,
    };
    ff_scr_pill_create(puck, "DONE", &done_cfg);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "NAME");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, FF_NAMEEDIT_TITLE_Y);

    /* Live draft preview — committed + pending, straight from the
     * projected view (never re-derived here). DOTS-ellipsized like the
     * NAME row's own label, for the same reason (an at-cap 15-char name
     * must not bleed past its own safe width). */
    int32_t const text_margin = settings_safe_margin_x(FF_NAMEEDIT_TEXT_Y, FF_NAMEEDIT_TEXT_H);
    lv_obj_t *text_lbl = lv_label_create(puck);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(text_lbl, FF_THEME_PUCK_PX - 2 * text_margin);
    lv_label_set_text(text_lbl, (ne->text[0] != '\0') ? ne->text : "(empty)");
    lv_obj_set_style_text_font(text_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_align(text_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text_lbl,
                                lv_color_hex((ne->text[0] != '\0') ? FF_THEME_COLOR_INK : FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_pos(text_lbl, text_margin, FF_NAMEEDIT_TEXT_Y);

    /* Mode indicator — non-interactive (tap any grid key or the bottom
     * row's MODE chip to cycle it; this label just states which page is
     * live, the same "never a mystery toggle" rule S08's own mode-chip
     * amendment established). */
    lv_obj_t *mode_lbl = lv_label_create(puck);
    lv_label_set_text(mode_lbl, (ne->mode == FF_APP_NAME_EDIT_123) ? "123" : "ABC");
    lv_obj_set_style_text_font(mode_lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(mode_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(mode_lbl, 1, 0);
    lv_obj_align(mode_lbl, LV_ALIGN_TOP_MID, 0, FF_NAMEEDIT_MODE_Y);

    settings_name_build_grid_row(puck, ne->mode, FF_NAMEEDIT_GRID_Y, 1);
    settings_name_build_grid_row(puck, ne->mode, FF_NAMEEDIT_GRID_Y + FF_NAMEEDIT_ROW_STEP, 4);
    settings_name_build_grid_row(puck, ne->mode, FF_NAMEEDIT_GRID_Y + 2 * FF_NAMEEDIT_ROW_STEP, 7);

    /* Bottom row: DEL, SPACE, MODE — SPACE is this row's most-tapped
     * key and gets the remainder, the same "most-tapped key gets the
     * remainder" doctrine S08's own bottom-row amendments use. */
    int32_t const bottom_y = FF_NAMEEDIT_GRID_Y + 3 * FF_NAMEEDIT_ROW_STEP;
    int32_t const bottom_margin = settings_safe_margin_x(bottom_y, FF_NAMEEDIT_KEY_H);
    int32_t const bottom_w = FF_THEME_PUCK_PX - 2 * bottom_margin;
    int32_t const del_w = (bottom_w - 2 * FF_NAMEEDIT_ROW_GAP) / 4;
    int32_t const mode_w = del_w;
    int32_t const space_w = bottom_w - del_w - mode_w - 2 * FF_NAMEEDIT_ROW_GAP;

    lv_obj_t *del_btn = settings_make_pill(puck, "DEL", bottom_margin, bottom_y, del_w, FF_NAMEEDIT_KEY_H,
                                           FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, 0,
                                           settings_name_backspace_cb, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(del_btn, 0), FF_THEME_FONT_CHIP, 0);

    lv_obj_t *space_btn =
        settings_make_pill(puck, "SPACE", bottom_margin + del_w + FF_NAMEEDIT_ROW_GAP, bottom_y, space_w,
                           FF_NAMEEDIT_KEY_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, 0, settings_name_space_cb,
                           NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(space_btn, 0), FF_THEME_FONT_CHIP, 0);

    lv_obj_t *mode_btn =
        settings_make_pill(puck, (ne->mode == FF_APP_NAME_EDIT_123) ? "ABC" : "123",
                           bottom_margin + del_w + space_w + 2 * FF_NAMEEDIT_ROW_GAP, bottom_y, mode_w,
                           FF_NAMEEDIT_KEY_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER, 0,
                           settings_name_mode_cb, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(mode_btn, 0), FF_THEME_FONT_CHIP, 0);
}

/* ---------------------------------------------------------------------
 * DIAGNOSTICS page (Settings -> "DIAGNOSTICS" row). A full-screen,
 * scrollable, READ-ONLY status dump of `ff_app_diag_t` — link, my
 * position, mesh link-quality, wall clock, compass, device. Same back-
 * circle-plus-scroll-list SHAPE as the CREW page, reached the same way
 * (drilling in rather than swiping to a base face, so it needs its own
 * way back) — but its OWN header geometry, NOT `FF_CREW_BACK_Y`/`_HDR_Y`
 * directly: "DIAGNOSTICS" (11 chars) at `FF_THEME_FONT_HEADLINE` is wide
 * enough that CREW's `y=30` header row (chosen for CREW/NAME's own
 * 4-char titles) leaves too little width beside the back circle at that
 * height — rendering `settings_diag_full.json` showed the title's own
 * left edge sitting UNDER the back button. `FF_DIAG_BACK_Y` sits lower,
 * where the round glass is measurably wider, so a title box anchored
 * right of the back circle has room for the full word with no wrap and
 * no overlap, by construction (see `settings_build_diag_page`'s own
 * comment on the header). `FF_DIAG_LIST_Y`/`_LIST_H` shift/shrink to
 * match — the list is a scroll container regardless, so a few px less
 * viewport height costs nothing but one more scroll tick.
 *
 * Rows are plain, non-interactive text ("LABEL: value") — this
 * codebase vendors no monospace font (see `FF_THEME_FONT_DISTANCE`'s
 * own doc comment above: "mono in spec... no mono vendored"), so
 * `FF_THEME_FONT_CHIP` stands in, matching every other compact-text
 * row in this file. Rows are far shorter than the 44px hit-target
 * floor because they are not controls — only the BACK circle (this
 * page) and the "DIAGNOSTICS" open row (the plain LIST) are real
 * controls anywhere in this feature.
 *
 * Honest data throughout (CLAUDE.md): every fact renders "--"/"unknown"
 * whenever `ff_app_diag_t`'s own has_-flag (or enum-UNKNOWN member) says
 * it must — this file never guesses at a value the projection did not
 * honestly provide.
 */
#define FF_DIAG_ROW_H    20
#define FF_DIAG_ROW_GAP  6
#define FF_DIAG_ROW_STEP (FF_DIAG_ROW_H + FF_DIAG_ROW_GAP)

#define FF_DIAG_BACK_Y  44 /* lower than FF_CREW_BACK_Y(30) — see this section's own header comment */
#define FF_DIAG_BACK_PX FF_THEME_MIN_HIT_PX
#define FF_DIAG_HDR_Y   (FF_DIAG_BACK_Y + (FF_DIAG_BACK_PX - 24) / 2) /* optically centered against the back circle */
#define FF_DIAG_LIST_Y  114 /* FF_CREW_LIST_Y(100) shifted down by the header's own 14px drop */
#define FF_DIAG_LIST_H  242 /* FF_CREW_LIST_H(256) shortened by that same 14px */

static void settings_diag_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* One "LABEL: value" line, muted label + ink value via LVGL recolor
 * markup — the same `#RRGGBB text#` mechanism the CREW page's short-name
 * tag already uses (settings_crew_build_paired_row, above). Returns the
 * next row's y so callers chain `y = settings_diag_line(...)`. */
static int32_t settings_diag_line(lv_obj_t *list, int32_t y, int32_t row_w, char const *label, char const *value)
{
    lv_obj_t *lbl = lv_label_create(list);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_set_width(lbl, row_w);
    lv_obj_set_height(lbl, lv_font_get_line_height(FF_THEME_FONT_CHIP));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    char buf[128];
    snprintf(buf, sizeof(buf), "#%06x %s:# %s", (unsigned)FF_THEME_COLOR_MUTED, label, value);
    lv_label_set_text(lbl, buf);
    lv_label_set_recolor(lbl, true);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
    return y + FF_DIAG_ROW_STEP;
}

/* Honest age formatting: "--" when the fact this age describes was
 * never observed at all (see `ff_app_diag_t`'s own has_*-flag
 * convention), `ff_fmt_age`'s own coarse human age otherwise. */
static void settings_diag_age(char *buf, size_t n, bool has, uint32_t age_ms)
{
    if (!has) {
        snprintf(buf, n, "--");
        return;
    }
    ff_fmt_age(buf, n, age_ms);
}

/* Boundary-translation name tables — mirror ff_app_state.h's own
 * "ff_app_diag_t's five small enums" doc comment: each name table below
 * is this SCREEN's own vocabulary (lowercase where the fact reads as
 * prose, e.g. "internal GPS"; upper-case where it reads as a state,
 * e.g. "CONNECTED", matching this file's existing link/presence text
 * conventions elsewhere in the file). */
static char const *settings_diag_link_name(ff_app_link_t l)
{
    switch (l) {
    case FF_APP_LINK_RECONNECTING: return "RECONNECTING";
    case FF_APP_LINK_CONNECTED: return "CONNECTED";
    case FF_APP_LINK_NONE:
    default: return "NONE";
    }
}

static char const *settings_diag_pos_src_name(ff_app_pos_src_t s)
{
    switch (s) {
    case FF_APP_POS_SRC_MANUAL: return "manual";
    case FF_APP_POS_SRC_INTERNAL: return "internal GPS";
    case FF_APP_POS_SRC_EXTERNAL: return "external GPS";
    case FF_APP_POS_SRC_UNKNOWN:
    default: return "unknown";
    }
}

static char const *settings_diag_trust_name(ff_app_wall_trust_t t)
{
    switch (t) {
    case FF_APP_WALL_TRUST_TRUSTED: return "trusted";
    case FF_APP_WALL_TRUST_CORROBORATED: return "corroborated";
    case FF_APP_WALL_TRUST_BOOTSTRAP:
    default: return "bootstrap";
    }
}

static char const *settings_diag_mag_kind_name(ff_app_mag_kind_t k)
{
    switch (k) {
    case FF_APP_MAG_QMC5883L: return "QMC5883L";
    case FF_APP_MAG_HMC5883L: return "HMC5883L";
    case FF_APP_MAG_QMC5883P: return "QMC5883P";
    case FF_APP_MAG_NONE:
    default: return "none";
    }
}

static char const *settings_diag_imu_state_name(ff_app_imu_state_t s)
{
    switch (s) {
    case FF_APP_IMU_NO_DATA: return "no-data";
    case FF_APP_IMU_OK: return "ok";
    case FF_APP_IMU_ABSENT:
    default: return "absent";
    }
}

/* fix/render-key-churn (2026-09-07) — this page's own text must show
 * EXACTLY what `shell_render_key` (ff_shell.c) uses to decide whether a
 * rebuild is warranted, or the two silently drift: a value that changes
 * by less than the key's bucket width would then update on screen (this
 * page prints `ff_app_diag_t` fields close to verbatim) with no redraw
 * ever scheduled to show it — a STALE-DISPLAY bug, the opposite failure
 * from the churn these buckets exist to kill. These three helpers
 * duplicate `shell_render_key`'s own bucketing formulas (deliberately
 * duplicated, not shared — this codebase's "one projection, two
 * presentations" precedent, `shell_compute_diag`'s own doc comment,
 * covers computing the FACT once; it does not extend to three one-line
 * roundings that must simply stay textually identical to their
 * ff_shell.c counterparts) so that "the key changed" and "the page would
 * draw differently" are the same statement by construction, in both
 * directions. RSSI (already whole `%d dBm`) and channel/air utilization
 * (already `%.0f%%`, matching the key's own round-to-nearest-percent)
 * needed no display change — only SNR, battery mV, and free heap, all
 * three left byte/mV/dB-precise on the page while the key already
 * bucketed them, get one here. */
static float settings_diag_bucket_snr_db(float v)
{
    /* 0.5 dB buckets, truncating toward zero — same formula as
     * `key->settings.diag.last_snr_db` in shell_render_key. */
    return (float)(int32_t)(v * 2.0f) / 2.0f;
}

static uint16_t settings_diag_bucket_batt_mv(uint16_t v)
{
    /* 10 mV buckets, round-to-nearest — same formula as
     * `key->settings.diag.batt_mv` in shell_render_key. */
    return (uint16_t)(((v + 5u) / 10u) * 10u);
}

static uint32_t settings_diag_bucket_heap_kb(uint32_t bytes)
{
    /* Whole KB, truncating — same 1024-byte grid `key->settings.diag.
     * free_heap_bytes` floors to in shell_render_key before comparing. */
    return bytes / 1024u;
}

static void settings_build_diag_page(lv_obj_t *parent, ff_app_diag_t const *d)
{
    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE);

    /* Header: a real back circle (CREW's own convention, at THIS page's
     * own lower `FF_DIAG_BACK_Y` — see this section's top comment for
     * why) + a title centered in the space to its RIGHT, not puck-wide
     * centering: a fixed-width label starting right after the back
     * button, right-margin mirrored to `back_margin` for symmetry,
     * internally center-aligned — guaranteed clear of the back button by
     * construction regardless of title length, so a future rename to a
     * still-longer string cannot reintroduce the overlap this geometry
     * was found (rendering settings_diag_full.json) to have at CREW's
     * own header height. */
    int32_t const back_margin = settings_safe_margin_x(FF_DIAG_BACK_Y, FF_DIAG_BACK_PX);
    lv_obj_t *back = ff_scr_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_DIAG_BACK_PX, FF_DIAG_BACK_PX);
    lv_obj_set_pos(back, back_margin, FF_DIAG_BACK_Y);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    /* puck-ux-usability-review slice 1, finding 2 (review fix) — see the
     * CREW page's own back-circle comment (settings_build_crew_page): the
     * identical raw-wrapper gap, the identical fix. */
    settings_hit_add_press_feedback(back);
    lv_obj_add_event_cb(back, settings_diag_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *glyph = lv_label_create(back);
    lv_label_set_text(glyph, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(glyph, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph);

    int32_t const title_left = back_margin + FF_DIAG_BACK_PX;
    int32_t const title_w = FF_THEME_PUCK_PX - title_left - back_margin;
    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "DIAGNOSTICS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, title_w);
    lv_obj_set_pos(title, title_left, FF_DIAG_HDR_Y);

    int32_t list_margin = settings_safe_margin_x(FF_DIAG_LIST_Y, FF_DIAG_LIST_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

    lv_obj_t *list = lv_obj_create(puck);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, row_w, FF_DIAG_LIST_H);
    lv_obj_set_pos(list, list_margin, FF_DIAG_LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE); /* #bug2 precedent — see the plain list's own comment */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    s_list = list; /* shares the plain list's scroll-hint hook — only one of these lists is ever built at a time per subview */
    /* fix/diag-scroll-persist — observe this list's own scroll offset
     * (settings_diag_scroll_cb, into s_diag_scroll_y) the same #bug4 shape
     * the plain list already uses for s_scroll_y; restored at the end of
     * this function below instead of the old hardcoded scroll-to-0. */
    lv_obj_add_event_cb(list, settings_diag_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, settings_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    char buf[64];
    int32_t y = 0;

    /* --- 1. Link --- */
    y = settings_build_section_header(list, y, row_w, "LINK", /*first=*/true);
    y = settings_diag_line(list, y, row_w, "STATE", settings_diag_link_name(d->link));
    if (d->my_node_id != 0u) {
        snprintf(buf, sizeof(buf), "!%08x", (unsigned)d->my_node_id);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "NODE", buf);
    snprintf(buf, sizeof(buf), "%s / %s", d->has_short_name ? d->short_name : "--",
             d->has_long_name ? d->long_name : "--");
    y = settings_diag_line(list, y, row_w, "NAME", buf);
    settings_diag_age(buf, sizeof(buf), d->has_last_frame_age, d->last_frame_age_ms);
    y = settings_diag_line(list, y, row_w, "LAST FRAME", buf);
    snprintf(buf, sizeof(buf), "%u ok / %u err", (unsigned)d->frames_ok, (unsigned)d->decode_errors);
    y = settings_diag_line(list, y, row_w, "FRAMES", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)d->reconnects);
    y = settings_diag_line(list, y, row_w, "RECONNECTS", buf);

    /* --- 2. Position (mine) --- */
    y = settings_build_section_header(list, y, row_w, "POSITION", /*first=*/false);
    y = settings_diag_line(list, y, row_w, "SOURCE", settings_diag_pos_src_name(d->pos_src));
    if (d->pos_ok) {
        snprintf(buf, sizeof(buf), "%.5f, %.5f", d->pos_lat, d->pos_lon);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = settings_diag_line(list, y, row_w, "LAT/LON", buf);
    if (d->pos_has_altitude) {
        snprintf(buf, sizeof(buf), "%d m", (int)d->pos_altitude_m);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "ALTITUDE", buf);
    if (d->pos_has_sats) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)d->pos_sats_in_view);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "SATS", buf);
    if (d->pos_has_precision_bits) {
        snprintf(buf, sizeof(buf), "%u bits", (unsigned)d->pos_precision_bits);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "PRECISION", buf);
    settings_diag_age(buf, sizeof(buf), d->pos_has_age, d->pos_age_ms);
    y = settings_diag_line(list, y, row_w, "AGE", buf);

    /* --- 3. Mesh --- */
    y = settings_build_section_header(list, y, row_w, "MESH", /*first=*/false);
    snprintf(buf, sizeof(buf), "%u crew / %u heard", (unsigned)d->crew_count, (unsigned)d->heard_count);
    y = settings_diag_line(list, y, row_w, "ROSTER", buf);
    if (d->has_last_rssi || d->has_last_snr) {
        char rssi_buf[16];
        char snr_buf[16];
        if (d->has_last_rssi) {
            snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", (int)d->last_rssi_dbm);
        } else {
            snprintf(rssi_buf, sizeof(rssi_buf), "--");
        }
        if (d->has_last_snr) {
            snprintf(snr_buf, sizeof(snr_buf), "%.1f dB", (double)settings_diag_bucket_snr_db(d->last_snr_db));
        } else {
            snprintf(snr_buf, sizeof(snr_buf), "--");
        }
        snprintf(buf, sizeof(buf), "%s / %s (%s)", rssi_buf, snr_buf, d->last_rf_direct ? "direct" : "relayed/unk");
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "LAST RF", buf);
    settings_diag_age(buf, sizeof(buf), d->has_last_rf_age, d->last_rf_age_ms);
    y = settings_diag_line(list, y, row_w, "RF AGE", buf);
    if (d->has_chan_util || d->has_air_util_tx) {
        char cu[16];
        char au[16];
        if (d->has_chan_util) {
            snprintf(cu, sizeof(cu), "%.0f%%", (double)d->chan_util_pct);
        } else {
            snprintf(cu, sizeof(cu), "--");
        }
        if (d->has_air_util_tx) {
            snprintf(au, sizeof(au), "%.0f%%", (double)d->air_util_tx_pct);
        } else {
            snprintf(au, sizeof(au), "--");
        }
        snprintf(buf, sizeof(buf), "chan %s / tx %s", cu, au);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "AIRTIME", buf);
    settings_diag_age(buf, sizeof(buf), d->has_telemetry_age, d->telemetry_age_ms);
    y = settings_diag_line(list, y, row_w, "TELEM AGE", buf);
    settings_diag_age(buf, sizeof(buf), d->has_pos_broadcast_age, d->pos_broadcast_age_ms);
    y = settings_diag_line(list, y, row_w, "POS BCAST", buf);

    /* --- 4. Time --- */
    y = settings_build_section_header(list, y, row_w, "TIME", /*first=*/false);
    y = settings_diag_line(list, y, row_w, "LATCHED", d->wall_latched ? "yes" : "no");
    y = settings_diag_line(list, y, row_w, "TRUST",
                           d->wall_has_trust ? settings_diag_trust_name(d->wall_trust) : "unknown");
    if (d->wall_has_src_node) {
        snprintf(buf, sizeof(buf), "!%08x", (unsigned)d->wall_src_node);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "SRC NODE", buf);
    if (d->wall_has_offset) {
        snprintf(buf, sizeof(buf), "%+d min%s", (int)d->wall_offset_min, d->wall_offset_assumed ? " (assumed)" : "");
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "OFFSET", buf);
    y = settings_diag_line(list, y, row_w, "LOCAL", d->has_local_time ? d->local_time_str : "unknown");

    /* --- 5. Compass --- */
    y = settings_build_section_header(list, y, row_w, "COMPASS", /*first=*/false);
    y = settings_diag_line(list, y, row_w, "MAGNETOMETER", settings_diag_mag_kind_name(d->mag_kind));
    y = settings_diag_line(list, y, row_w, "IMU", settings_diag_imu_state_name(d->imu_state));
    if (d->heading_valid) {
        snprintf(buf, sizeof(buf), "%.0f deg", (double)d->heading_deg);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "HEADING", buf);
    y = settings_diag_line(list, y, row_w, "CALIBRATION", d->compass_cal_set ? "set" : "identity");

    /* --- 6. Device --- */
    y = settings_build_section_header(list, y, row_w, "DEVICE", /*first=*/false);
    if (d->has_batt_mv || d->batt_pct >= 0) {
        char mv[16];
        char pct[16];
        if (d->has_batt_mv) {
            snprintf(mv, sizeof(mv), "%u mV", (unsigned)settings_diag_bucket_batt_mv(d->batt_mv));
        } else {
            snprintf(mv, sizeof(mv), "--");
        }
        if (d->batt_pct >= 0) {
            snprintf(pct, sizeof(pct), "%d%%", (int)d->batt_pct);
        } else {
            snprintf(pct, sizeof(pct), "--");
        }
        snprintf(buf, sizeof(buf), "%s / %s", mv, pct);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    y = settings_diag_line(list, y, row_w, "BATTERY", buf);
    {
        unsigned const h = d->uptime_s / 3600u;
        unsigned const m = (d->uptime_s % 3600u) / 60u;
        unsigned const s = d->uptime_s % 60u;
        snprintf(buf, sizeof(buf), "%uh%02um%02us", h, m, s);
    }
    y = settings_diag_line(list, y, row_w, "UPTIME", buf);
    snprintf(buf, sizeof(buf), "%s / %s", (d->fw_git_sha[0] != '\0') ? d->fw_git_sha : "unknown",
             (d->fw_build_date[0] != '\0') ? d->fw_build_date : "unknown");
    y = settings_diag_line(list, y, row_w, "FIRMWARE", buf);
    if (d->has_free_heap) {
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)settings_diag_bucket_heap_kb(d->free_heap_bytes));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    y = settings_diag_line(list, y, row_w, "FREE HEAP", buf);
    /* S30 mic bring-up (docs/specs/S30-audio-input.md) — one more DEVICE
     * row, same "append a new fact to the existing section" precedent
     * this file already followed when DIAGNOSTICS itself was added as
     * DEVICE's own last row. Four honest states, in priority order:
     * absent (init failed, OR the driver's own "stuck/all-zero for 1s"
     * sentinel tripped — this struct does not distinguish the two, and
     * neither reads as a working mic) beats off beats "on, no data yet"
     * (the brief window between `mic on` and the reader task's first
     * frame) beats a real dBFS reading. Never a fabricated number for
     * any state but the last. */
    if (!d->mic_present) {
        snprintf(buf, sizeof(buf), "absent");
    } else if (!d->mic_running) {
        snprintf(buf, sizeof(buf), "off");
    } else if (!d->has_mic_level) {
        snprintf(buf, sizeof(buf), "on");
    } else {
        snprintf(buf, sizeof(buf), "%.0f dBFS", (double)d->mic_envelope_dbfs);
    }
    y = settings_diag_line(list, y, row_w, "MIC", buf);
    /* fix/s31-music-idle-drain (2026-09-09) power diagnostic — a
     * SEPARATE row from MIC just above, deliberately: this bug's own
     * bench evidence was a mic that ran for 6.6 HOURS while the "MIC"
     * row above would have honestly read a normal-looking "on"/dBFS the
     * entire time — nothing about a single point-in-time status read
     * hints at "and it has been like this all night". Unconditional
     * (present or not, running or not — d->mic_on_s's own doc comment,
     * ff_app_state.h) so a stuck-on total from a PRIOR session segment
     * stays visible even after the mic itself has since gone quiet. */
    snprintf(buf, sizeof(buf), "%us", (unsigned)d->mic_on_s);
    y = settings_diag_line(list, y, row_w, "MIC ON-TIME", buf);
    (void)y; /* the final cursor value is only informative */

    /* fix/diag-scroll-persist — restore the offset the previous build left
     * (0 on a fresh entry — s_diag_scroll_y is a zero-initialized static,
     * and ff_scr_settings_reset_scroll below clears it same as s_scroll_y
     * on a fresh Settings-face entry), instead of the old unconditional
     * scroll-to-0 that reset every in-place rebuild — including the
     * routine ones a live stats/link/time tick causes (shell_render_key,
     * ff_shell.c) — back to the top mid-read, and mid-drag. The layout
     * must be resolved first so LVGL knows the scrollable range to clamp
     * against (same ordering the plain list's own #bug4 restore uses). */
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, s_diag_scroll_y, LV_ANIM_OFF);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */
void ff_scr_settings_build(lv_obj_t *parent, ff_app_settings_t const *settings)
{
    if (parent == NULL || settings == NULL) {
        return;
    }

    s_settings = *settings;

    /* S12/S04 — the CREW sub-view replaces the plain list entirely while
     * showing (the ff_scr_inbox_build subview-dispatch precedent: a
     * settings-change intent's rebuild re-enters this function, so the
     * check belongs at the very top, not as a special case bolted onto
     * the list build below). */
    if (settings->subview == FF_SETTINGS_SUB_CREW) {
        settings_build_crew_page(parent, &settings->crew);
        return;
    }
    /* S12 step 3 — same subview-dispatch-at-the-top shape as CREW just
     * above. */
    if (settings->subview == FF_SETTINGS_SUB_COMPASS_CAL) {
        settings_build_compass_cal_page(parent, &settings->compass_cal);
        return;
    }
    /* NAME in Settings — same subview-dispatch-at-the-top shape as
     * CREW/COMPASS_CAL just above. */
    if (settings->subview == FF_SETTINGS_SUB_NAME_EDIT) {
        settings_build_name_edit_page(parent, &settings->name_edit);
        return;
    }
    /* DIAGNOSTICS — same subview-dispatch-at-the-top shape as
     * CREW/COMPASS_CAL/NAME_EDIT just above. */
    if (settings->subview == FF_SETTINGS_SUB_DIAGNOSTICS) {
        settings_build_diag_page(parent, &settings->diag);
        return;
    }
    /* [api] A02 slice D — SHOW CODE, same shape again. */
    if (settings->subview == FF_SETTINGS_SUB_CREW_CODE) {
        settings_build_crew_code_page(parent, &settings->crew);
        return;
    }
    /* [api] A02 slice D2 — the START/LEAVE confirm and status faces. */
    if (settings->subview == FF_SETTINGS_SUB_CREW_CONFIRM) {
        settings_build_crew_confirm_page(parent, &settings->crew);
        return;
    }
    if (settings->subview == FF_SETTINGS_SUB_CREW_STATUS) {
        settings_build_crew_status_page(parent, &settings->crew);
        return;
    }

    lv_obj_t *puck = lv_obj_create(parent);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE); /* base lv_obj defaults clickable; this one is a plain backdrop */

    /* The scroll list's inscribed rectangle — computed here (before the
     * header) so the opaque header band can share its horizontal extent. */
    int32_t list_margin = settings_safe_margin_x(FF_SETTINGS_LIST_Y, FF_SETTINGS_LIST_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

    /* --- Opaque header band FIRST (#bug5), so the header controls below draw
     * on top of it. Occludes everything above the list viewport with solid
     * ink — no gradient, so no RGB565 edge banding on device. --- */
    settings_build_header_band(puck, list_margin, row_w, FF_SETTINGS_LIST_Y);

    /* --- PINNED, centered header: SETTINGS title + name, stacked as ONE
     * flex-column block centered on the glass's own vertical axis (see this
     * constant block's doc comment above for the alignment-fix rationale).
     * Built directly on the puck (never inside the scroll list) so it never
     * scrolls away. There is no BACK control any more — the horizontal-
     * carousel rework made Settings a swipe tile you leave by swiping left,
     * not a modal with a back button (see scr_settings.h). --- */
    int32_t const hdr_margin = settings_safe_margin_x(FF_SETTINGS_HDR_Y, FF_SETTINGS_HDR_H);
    int32_t const hdr_w = FF_THEME_PUCK_PX - 2 * hdr_margin;

    lv_obj_t *hdr = lv_obj_create(puck);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, hdr_w, LV_SIZE_CONTENT);
    lv_obj_set_pos(hdr, hdr_margin, FF_SETTINGS_HDR_Y);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_row(hdr, FF_SETTINGS_HDR_ROW_GAP, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    /* Main axis (vertical, column flow): pack from the top, no extra
     * stretch. Cross axis (horizontal): CENTER each child — this is what
     * puts a short name and a wider title on the same true vertical axis
     * regardless of their own (different) natural widths. */
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);

    lv_obj_t *name_lbl = lv_label_create(hdr);
    lv_obj_set_width(name_lbl, hdr_w);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0); /* centers within its own (container-width) box */
    lv_label_set_text(name_lbl, (s_settings.my_name[0] != '\0') ? s_settings.my_name : "(unset)");
    lv_obj_set_style_text_font(name_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(name_lbl, 1, 0);

    /* --- The scroll list: an inscribed rectangle in the round glass, vertical-
     * only user scroll. --- */
    lv_obj_t *list = lv_obj_create(puck);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, row_w, FF_SETTINGS_LIST_H);
    lv_obj_set_pos(list, list_margin, FF_SETTINGS_LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    /* #bug2 — the list MUST stay CLICKABLE. LVGL only begins a scroll from a
     * press that lands on a hit-testable (clickable) object and then walks up to
     * the scrollable ancestor; a non-clickable object is skipped by hit-test, so
     * a press on empty/caption space would find no target and never scroll. With
     * the list clickable, ANY press inside it (the plain toggle-row captions and
     * the gaps included) initiates the scroll. It carries no CLICKED handler, so
     * a tap on empty space is a harmless no-op; the rows/pills on top still take
     * their own taps. (Previously cleared here as "a plain scroll region" — that
     * was the left-side dead-scroll bug: only the rows with a clickable control
     * on the left, the value rows, would scroll.) */
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF); /* no bar on the round glass */
    /* #bug4 — remember this list and observe its scroll so a rebuild after a
     * settings-change intent restores the offset instead of jumping to top. */
    s_list = list;
    lv_obj_add_event_cb(list, settings_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, settings_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* ---------------------------------------------------------------------
     * Settings audit 2026-09-03 — sectioned row build, running cursor.
     *
     * `y` is this build's ONE layout cursor (list-relative, top of content =
     * 0); every row/header advances it by its own footprint plus the gap the
     * next item needs, so insertion/removal anywhere never requires touching
     * a downstream constant — the failure mode the old REL_*_Y macro chain
     * had (S21/S27's own amendments each had to manually re-derive every
     * later offset by hand). Order: CREW -> DISPLAY (BRIGHTNESS, CLOCK,
     * SCREEN, COLORBLIND) -> SOUND (SOUNDS, UI TICKS, QUIET HOURS) -> UNITS
     * (UNITS) -> DEVICE (CALIBRATE TOUCH) -> NAME, per the audit's
     * maintainer-decided section order EXCEPT for CREW, promoted to the
     * top by usability-review slice 3 (docs/reviews/puck-ux-usability-
     * 2026-09-15.md finding 9 / §4 task 4): SHOW CODE is the #1 day-one
     * task and it used to sit ~830px down a scrolling list — worse, in
     * this same slice's own change, once the rows above it also grew from
     * 48px to 80px each (finding 5's "the two changes have to ship
     * together" — see FF_SETTINGS_ROW_H's own comment). CREW is now
     * reachable with ZERO scrolling from the Settings root. The four
     * hidden rows (SHARE/HAPTICS/GLOW/WATER NUDGE) are NOT assigned a
     * section here — see their own trailing block below this one for
     * why. ------------------------------------------------------------- */
    int32_t y = 0;

    y = settings_build_section_header(list, y, row_w, "CREW", /*first=*/true);
    settings_build_crew_open_row(list, y, row_w);
    y += FF_SETTINGS_ROW_H; /* last (only) row of CREW */

    y = settings_build_section_header(list, y, row_w, "DISPLAY", /*first=*/false);
    settings_build_brightness(list, row_w, y);
    y += FF_SETTINGS_BRIGHT_BLOCK_H + FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row(list, y, row_w, "CLOCK", "12H", "24H", s_settings.clock_24h ? 1 : 0, settings_clock_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row_ex(list, y, row_w, "SCREEN", "NORMAL", "FLIPPED", s_settings.screen_flip ? 1 : 0,
                                 FF_SETTINGS_SCREEN_PILL_W, settings_screen_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row_ex(list, y, row_w, "COLORBLIND", "ON", "OFF", s_settings.colorblind ? 0 : 1,
                                 FF_SETTINGS_ONOFF_PILL_W, settings_colorblind_cb);
    y += FF_SETTINGS_ROW_H; /* last row of DISPLAY: no trailing gap, the next header adds it */

    y = settings_build_section_header(list, y, row_w, "SOUND", /*first=*/false);
    settings_build_toggle_row_ex(list, y, row_w, "SOUNDS", "ON", "OFF", s_settings.sounds_on ? 0 : 1,
                                 FF_SETTINGS_ONOFF_PILL_W, settings_sounds_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row_ex(list, y, row_w, "UI TICKS", "ON", "OFF", s_settings.ui_ticks ? 0 : 1,
                                 FF_SETTINGS_ONOFF_PILL_W, settings_ui_ticks_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_quiet_preset_t const *quiet = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    bool const quiet_off = (quiet != NULL) && (quiet->from_min == 0) && (quiet->to_min == 0);
    settings_build_value_row(list, y, row_w, "QUIET HOURS", (quiet != NULL) ? quiet->label : "CUSTOM", quiet_off,
                             settings_quiet_cb);
    y += FF_SETTINGS_ROW_H; /* last row of SOUND */

    y = settings_build_section_header(list, y, row_w, "UNITS", /*first=*/false);
    settings_build_toggle_row(list, y, row_w, "UNITS", "FT", "M", s_settings.imperial ? 0 : 1, settings_units_cb);
    y += FF_SETTINGS_ROW_H; /* last (only) row of UNITS */

    y = settings_build_section_header(list, y, row_w, "DEVICE", /*first=*/false);
    settings_build_calibrate_row(list, y, row_w);
    y += FF_SETTINGS_ROW_STEP;
    /* S12 step 3 — CALIBRATE COMPASS follows CALIBRATE TOUCH. */
    settings_build_compass_cal_row(list, y, row_w, s_settings.compass_cal.cal_valid);
    y += FF_SETTINGS_ROW_STEP;
    /* DIAGNOSTICS — the new last row of DEVICE. */
    settings_build_diag_open_row(list, y, row_w);
    y += FF_SETTINGS_ROW_H; /* last row of DEVICE */

    /* NAME — task brief: "above CREW". Its own single-row section, same
     * "the section header repeats the one row's own name" shape UNITS
     * already establishes for a single-item category. */
    y = settings_build_section_header(list, y, row_w, "NAME", /*first=*/false);
    settings_build_name_row(list, y, row_w, s_settings.my_name, s_settings.mesh_name_confirmed,
                            s_settings.mesh_name_push_failed, s_settings.mesh_name_mismatch);
    y += FF_SETTINGS_ROW_H; /* last (only) row of NAME */

    /* ---------------------------------------------------------------------
     * Hidden rows (settings audit 2026-09-03) — SHARE, HAPTICS, GLOW, WATER
     * NUDGE. Each is gated by its own FF_SETTINGS_ROW_ENABLE_* flag (see
     * that table's own comment above for the audit finding + what would
     * re-enable it for real) and, while disabled, contributes NOTHING to
     * the cursor or the rendered list — this is the "flip a flag, get the
     * row back" contract the audit asked for.
     *
     * Placement (interpretation call, AGENTS.md): none of DISPLAY/SOUND/
     * UNITS/DEVICE is really these rows' home — SHARE is privacy, HAPTICS/
     * GLOW are feedback, WATER NUDGE is a reminder, and the audit's section
     * plan (this function's own top comment) only names where the eight
     * SURVIVING rows go. Rather than force a guess, a flipped-on row simply
     * APPENDS past DEVICE, unsectioned — correct and reachable (still one
     * scrolling list, still passes the hit-target sweep), just not yet
     * grouped; the PR that actually re-enables a row is expected to also
     * decide its section. -------------------------------------------- */
#if FF_SETTINGS_ROW_ENABLE_SHARE
    y += FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row(list, y, row_w, "SHARE", "LIVE", "GHOST",
                              (s_settings.share_mode == FF_SHARE_LIVE)    ? 0
                              : (s_settings.share_mode == FF_SHARE_GHOST) ? 1
                                                                          : -1,
                              settings_share_cb);
    y += FF_SETTINGS_ROW_H;
#endif
#if FF_SETTINGS_ROW_ENABLE_HAPTICS
    y += FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row_ex(list, y, row_w, "HAPTICS", "ON", "OFF", s_settings.haptics ? 0 : 1,
                                 FF_SETTINGS_ONOFF_PILL_W, settings_haptics_cb);
    y += FF_SETTINGS_ROW_H;
#endif
#if FF_SETTINGS_ROW_ENABLE_GLOW
    y += FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row_ex(list, y, row_w, "GLOW", "ON", "OFF", s_settings.night_glow ? 0 : 1,
                                 FF_SETTINGS_ONOFF_PILL_W, settings_night_glow_cb);
    y += FF_SETTINGS_ROW_H;
#endif
#if FF_SETTINGS_ROW_ENABLE_WATER
    y += FF_SETTINGS_ROW_GAP;
    char water_buf[16];
    settings_water_label(water_buf, sizeof(water_buf), s_settings.water_min);
    settings_build_value_row(list, y, row_w, "WATER NUDGE", water_buf, s_settings.water_min == 0, settings_water_cb);
    y += FF_SETTINGS_ROW_H;
#endif

    /* POWER — field-hardening ahead of Lost Lands (S26 slice b amendment;
     * see settings_build_power_open_row's own doc comment). Unconditionally
     * the LAST section, after the hidden #if rows above (whether or not any
     * of those are flipped on) — task brief: "placed at the bottom of the
     * list (CREW stays on top per #337)". Its own section header, same
     * "the header repeats the one row's own name" shape UNITS/NAME already
     * use for a single-item category. */
    y = settings_build_section_header(list, y, row_w, "POWER", /*first=*/false);
    settings_build_power_open_row(list, y, row_w);
    y += FF_SETTINGS_ROW_H; /* last (only) row of POWER */

    (void)y; /* the final cursor value is only informative once every #if above resolves */

    /* #bug4 — restore the scroll offset the previous build left (0 on a fresh
     * entry, cleared by ff_scr_settings_reset_scroll). The layout must be
     * resolved first so LVGL knows the scrollable range to clamp against. */
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, s_scroll_y, LV_ANIM_OFF);
}

/* #bug4 — see scr_settings.h. Clear the remembered offset so the next build
 * renders from the top; the face dispatcher calls this on a FRESH entry into
 * Settings (a not-Settings -> Settings face transition). Clears
 * s_diag_scroll_y too (fix/diag-scroll-persist) — a fresh arrival at
 * Settings must land at the top of whichever subview it opens on, DIAGNOSTICS
 * included, not wherever a previous visit to that subview left it. */
void ff_scr_settings_reset_scroll(void)
{
    s_scroll_y = 0;
    s_diag_scroll_y = 0;
}

/* Sim golden-harness hook — see scr_settings.h. Scrolls the live list to
 * `y` so a golden can capture a non-zero offset; a no-op for y<=0 or when no
 * list is built, so the live shell path (which always passes 0) never moves.
 * This writes back s_scroll_y (harness scaffolding): the golden runner renders
 * each fixture in a fresh process, so the offset never leaks into a following
 * Settings render; on the live path this function is never called. */
void ff_scr_settings_apply_scroll_hint(int32_t y)
{
    if (y <= 0 || s_list == NULL) {
        return;
    }
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, y, LV_ANIM_OFF); /* LVGL clamps to the scrollable range */
    s_scroll_y = lv_obj_get_scroll_y(s_list);
}
