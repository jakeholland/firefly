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

/* Rows — 48px tall clears the 44 floor with margin; 14px inter-row gap clears
 * the 8px adjacency floor with real slack. */
#define FF_SETTINGS_ROW_H   48
#define FF_SETTINGS_ROW_GAP 14
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP) /* 62 */
/* Every pill this file builds (settings_make_pill) uses FF_SETTINGS_ROW_H as
 * its height, and every pill's width (TOGGLE/SCREEN/VALUE, all below) is
 * wider than that — so this one assert is the binding shorter-dimension
 * floor check for all of them. */
_Static_assert(FF_SETTINGS_ROW_H >= FF_THEME_MIN_HIT_PX, "settings pill rows must clear the 44px hit-target floor");

/* Toggle-pair pills: two >=44px pills at a tight 6px gap (safe because a
 * pair shares one callback — see sweep composite-control exclusion). */
#define FF_SETTINGS_TOGGLE_PILL_W 58
#define FF_SETTINGS_TOGGLE_GAP    6
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
#define FF_SETTINGS_SCREEN_PILL_W 84

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
 * the minimum-size strip was hard to land a drag on. */
#define FF_SETTINGS_SLIDER_H         56
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
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SETTINGS_SAFETY_PX);
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
 * debt cleanup), byte-identical to this file's pre-refactor pixels: no
 * press-feedback style (settings rows use their own dim/highlight
 * conventions, decided by the caller before the pill is built) and
 * absolute positioning (settings lays out every row in list-relative
 * coords, not centered-with-offset like scr_flare.c/scr_power_menu.c's
 * pills).
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
        .press = FF_SCR_PILL_PRESS_NONE,
        .font = FF_THEME_FONT_CHIP,
        .letter_space = letter_space,
        .cb = cb,
        .user_data = user_data,
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
 * (tap either to flip) and one composite pair to the adjacency sweep.
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
    uint32_t const l_bg = (active_side == 0) ? FF_SETTINGS_PILL_ON_BG : FF_SETTINGS_PILL_OFF_BG;
    uint32_t const l_fg = (active_side == 0) ? FF_SETTINGS_PILL_ON_FG : FF_SETTINGS_PILL_OFF_FG;
    uint32_t const r_bg = (active_side == 1) ? FF_SETTINGS_PILL_ON_BG : FF_SETTINGS_PILL_OFF_BG;
    uint32_t const r_fg = (active_side == 1) ? FF_SETTINGS_PILL_ON_FG : FF_SETTINGS_PILL_OFF_FG;

    settings_make_pill(row, left_text, grp_x, 0, pill_w, FF_SETTINGS_ROW_H, l_bg, l_fg, 0, cb, NULL);
    settings_make_pill(row, right_text, grp_x + pill_w + FF_SETTINGS_TOGGLE_GAP, 0, pill_w, FF_SETTINGS_ROW_H, r_bg,
                       r_fg, 0, cb, NULL);
}

static void settings_build_toggle_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                      char const *left_text, char const *right_text, int active_side,
                                      lv_event_cb_t cb)
{
    settings_build_toggle_row_ex(list, rel_y, row_w, label, left_text, right_text, active_side,
                                 FF_SETTINGS_TOGGLE_PILL_W, cb);
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
 * UNITS (FT|MI).
 * ------------------------------------------------------------------- */
static void settings_units_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_IMPERIAL, s_settings.imperial ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * CLOCK (12H|24H) — S21 amendment. Same two-state toggle shape as UNITS
 * above: FF_SETTING_CLOCK_24H is bool-backed, "nonzero is true".
 * ------------------------------------------------------------------- */
static void settings_clock_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_CLOCK_24H, s_settings.clock_24h ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * SCREEN (NORMAL|FLIPPED) — format v8 amendment (maintainer ask,
 * 2026-09-02): the Fusion-designed case mounts the puck upside-down.
 * Same two-state toggle shape as UNITS/CLOCK above: FF_SETTING_SCREEN_FLIP
 * is bool-backed, "nonzero is true". The device applies a HARDWARE panel
 * mirror on change (app_main.c reads the shell's projected screen_flip
 * every tick, same pattern brightness_pct's live apply already uses) —
 * this row only ever emits the intent, never touches display HAL.
 * ------------------------------------------------------------------- */
static void settings_screen_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_SCREEN_FLIP, s_settings.screen_flip ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * SHARE (LIVE|GHOST). ZONES is deliberately NOT cycled into (PR #68 UX
 * review, blocking finding 1): selecting ZONES does not change sharing
 * behavior from LIVE in v1, so a tap moves LIVE<->GHOST only. A persisted
 * ZONES renders as neither pill active and one tap moves it to GHOST.
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
    (void)e;
    uint8_t const next = (s_settings.share_mode == FF_SHARE_GHOST) ? FF_SHARE_LIVE : FF_SHARE_GHOST;
    settings_emit_int(FF_SETTING_SHARE_MODE, next);
}
#endif

/* ---------------------------------------------------------------------
 * HAPTICS / SOUNDS / UI TICKS / GLOW / COLORBLIND — plain booleans.
 * HAPTICS and GLOW are hidden rows (settings audit 2026-09-03); their
 * callbacks are guarded the same way SHARE's is above.
 * ------------------------------------------------------------------- */
#if FF_SETTINGS_ROW_ENABLE_HAPTICS
static void settings_haptics_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_HAPTICS, s_settings.haptics ? 0 : 1);
}
#endif

/* SOUNDS (S27, docs/specs/S27-sounds.md) — the master switch for every
 * sound this puck plays. Same two-state toggle shape as HAPTICS above. */
static void settings_sounds_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_SOUNDS_ON, s_settings.sounds_on ? 0 : 1);
}

/* UI TICKS (S27) — the second, TAP-only gate. Same two-state toggle
 * shape; defaults OFF (ff_settings.h's doc comment on the field). */
static void settings_ui_ticks_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_UI_TICKS, s_settings.ui_ticks ? 0 : 1);
}

#if FF_SETTINGS_ROW_ENABLE_GLOW
static void settings_night_glow_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_NIGHT_GLOW, s_settings.night_glow ? 0 : 1);
}
#endif

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
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
    lv_obj_set_style_text_opa(lbl, LV_OPA_60, 0); /* dimmer than a row caption's full opacity */
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

#define FF_CREW_ACTION_PILL_W FF_SETTINGS_VALUE_PILL_W
#define FF_CREW_ACTION_GAP    FF_SETTINGS_VALUE_GAP

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

static void settings_crew_unpair_cb(lv_event_t *e)
{
    uintptr_t node = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_CREW_UNPAIR, .u = {0}};
    in.u.node_id = (uint32_t)node;
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
    case FF_PRESENCE_LOST:
        snprintf(buf, n, "LOST");
        *out_color = FF_THEME_COLOR_STALE_AMBER;
        break;
    case FF_PRESENCE_LINKED:
    default:
        snprintf(buf, n, "LINKED");
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

    settings_make_pill(row, "REMOVE", row_w - FF_CREW_ACTION_PILL_W, (FF_CREW_ROW_H - FF_SETTINGS_ROW_H) / 2,
                       FF_CREW_ACTION_PILL_W, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_STALE_AMBER,
                       0, settings_crew_unpair_cb, (void *)(uintptr_t)m->node_id);
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
                                  (FF_CREW_ROW_H - FF_SETTINGS_ROW_H) / 2, FF_CREW_ACTION_PILL_W, FF_SETTINGS_ROW_H,
                                  FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_DIM, 0, NULL, NULL);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    } else {
        pill = settings_make_pill(row, "ADD", row_w - FF_CREW_ACTION_PILL_W, (FF_CREW_ROW_H - FF_SETTINGS_ROW_H) / 2,
                                  FF_CREW_ACTION_PILL_W, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
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
    lv_label_set_text(lbl, link_connected ? "nobody heard yet" : "nobody heard yet - is the comms brain linked?");
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
}

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
    y = settings_build_section_header(list, y, row_w, "PAIRED", /*first=*/true);
    for (uint8_t i = 0; i < cw->paired_count; i++) {
        settings_crew_build_paired_row(list, y, row_w, &cw->paired[i]);
        y += FF_CREW_ROW_STEP;
    }
    y = settings_build_section_header(list, y, row_w, "HEARD", /*first=*/false);
    if (cw->heard_count == 0) {
        settings_crew_build_heard_empty(list, y, row_w, cw->link_connected);
        y += FF_SETTINGS_ROW_H;
    } else {
        for (uint8_t i = 0; i < cw->heard_count; i++) {
            settings_crew_build_heard_row(list, y, row_w, &cw->heard[i], cw->roster_full);
            y += FF_CREW_ROW_STEP;
        }
    }

    lv_obj_update_layout(list);
}

/* ---------------------------------------------------------------------
 * COMPASS CAL ritual page — S12 step 3 (docs/specs/S12-first-run.md
 * Step 3, the compass calibration figure-eight). A small fixed set of
 * centered elements on a self-contained puck — scr_power_menu.c's
 * `ff_scr_power_menu_build` shape, not CREW's scrolling list; nothing
 * here needs to scroll.
 * ------------------------------------------------------------------- */
#define FF_CALCAL_TITLE_Y    34
#define FF_CALCAL_INSTR_Y    78
#define FF_CALCAL_INSTR_H    50
#define FF_CALCAL_RING_DIAM  140
#define FF_CALCAL_RING_Y     130
#define FF_CALCAL_SAMPLES_Y  (FF_CALCAL_RING_Y + FF_CALCAL_RING_DIAM + 10)
#define FF_CALCAL_BTN_W      120
#define FF_CALCAL_BTN_H      FF_SETTINGS_ROW_H /* 48 — clears the hit floor */
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
     * later offset by hand). Order: DISPLAY (BRIGHTNESS, CLOCK, SCREEN,
     * COLORBLIND) -> SOUND (SOUNDS, UI TICKS, QUIET HOURS) -> UNITS (UNITS)
     * -> DEVICE (CALIBRATE TOUCH), per the audit's maintainer-decided
     * section order. The four hidden rows (SHARE/HAPTICS/GLOW/WATER NUDGE)
     * are NOT assigned a section here — see their own trailing block below
     * this one for why. ------------------------------------------------- */
    int32_t y = 0;

    y = settings_build_section_header(list, y, row_w, "DISPLAY", /*first=*/true);
    settings_build_brightness(list, row_w, y);
    y += FF_SETTINGS_BRIGHT_BLOCK_H + FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row(list, y, row_w, "CLOCK", "12H", "24H", s_settings.clock_24h ? 1 : 0, settings_clock_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row_ex(list, y, row_w, "SCREEN", "NORMAL", "FLIPPED", s_settings.screen_flip ? 1 : 0,
                                 FF_SETTINGS_SCREEN_PILL_W, settings_screen_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row(list, y, row_w, "COLORBLIND", "ON", "OFF", s_settings.colorblind ? 0 : 1,
                              settings_colorblind_cb);
    y += FF_SETTINGS_ROW_H; /* last row of DISPLAY: no trailing gap, the next header adds it */

    y = settings_build_section_header(list, y, row_w, "SOUND", /*first=*/false);
    settings_build_toggle_row(list, y, row_w, "SOUNDS", "ON", "OFF", s_settings.sounds_on ? 0 : 1,
                              settings_sounds_cb);
    y += FF_SETTINGS_ROW_STEP;
    settings_build_toggle_row(list, y, row_w, "UI TICKS", "ON", "OFF", s_settings.ui_ticks ? 0 : 1,
                              settings_ui_ticks_cb);
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

    y = settings_build_section_header(list, y, row_w, "CREW", /*first=*/false);
    settings_build_crew_open_row(list, y, row_w);
    y += FF_SETTINGS_ROW_H; /* last (only) row of CREW */

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
    settings_build_toggle_row(list, y, row_w, "HAPTICS", "ON", "OFF", s_settings.haptics ? 0 : 1,
                              settings_haptics_cb);
    y += FF_SETTINGS_ROW_H;
#endif
#if FF_SETTINGS_ROW_ENABLE_GLOW
    y += FF_SETTINGS_ROW_GAP;
    settings_build_toggle_row(list, y, row_w, "GLOW", "ON", "OFF", s_settings.night_glow ? 0 : 1,
                              settings_night_glow_cb);
    y += FF_SETTINGS_ROW_H;
#endif
#if FF_SETTINGS_ROW_ENABLE_WATER
    y += FF_SETTINGS_ROW_GAP;
    char water_buf[16];
    settings_water_label(water_buf, sizeof(water_buf), s_settings.water_min);
    settings_build_value_row(list, y, row_w, "WATER NUDGE", water_buf, s_settings.water_min == 0, settings_water_cb);
    y += FF_SETTINGS_ROW_H;
#endif
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
