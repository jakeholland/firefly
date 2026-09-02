/**
 * scr_settings.c — see scr_settings.h.
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
 *   - Toggle pairs (UNITS FT|MI, CLOCK 12H|24H, SHARE LIVE|GHOST,
 *     HAPTICS ON|OFF, GLOW ON|OFF, COLORBLIND ON|OFF) render two pills; the
 *     ACTIVE one is amber-on-ink, the inactive one surface-on-muted. Both
 *     pills of a pair
 *     forward to the SAME toggle callback with no user_data, so (a) tapping
 *     either flips the two-state setting and (b) the hit-target sweep treats
 *     them as ONE logical control (its composite-control exclusion keys off
 *     matching cb+user_data), letting the pair sit at a tight ~6px gap
 *     without tripping the 8px adjacency floor two INDEPENDENT controls owe.
 *   - Value rows (WATER NUDGE, QUIET HOURS) render one surface/ink value
 *     pill; the row LABEL is itself a tap target forwarding to the same
 *     callback (no dead left half — PR #68), again composite with its pill.
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

#include "ff_intent.h" /* the emit seam; FF_INTENT_CALIBRATE_TOUCH */
#include "ff_layout.h"
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST, FF_BRIGHTNESS_*_PCT */
#include "ff_theme.h"

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
 * BRIGHTNESS leads (its own taller block: caption over a slider), then the
 * uniform-step rows. */
#define FF_SETTINGS_REL_BRIGHT_CAP_Y 0
#define FF_SETTINGS_BRIGHT_CAP_H     22
#define FF_SETTINGS_REL_SLIDER_Y     30
/* Transparent hit strip. Raised from the 44 floor to 56 after field-test:
 * the minimum-size strip was hard to land a drag on. 56 keeps a comfortable
 * >=6px (in fact the full FF_SETTINGS_ROW_GAP, 14px) gap to the UNITS row
 * below — every REL_*_Y position downstream is derived from
 * FF_SETTINGS_BRIGHT_BLOCK_H, so growing this constant slides the whole list
 * down uniformly and the slider->UNITS gap stays exactly ROW_GAP (the
 * scroll-aware tap-target sweep re-verifies 0 adjacency violations). The
 * list simply scrolls a little further, which is fine (S21's whole point). */
#define FF_SETTINGS_SLIDER_H         56
#define FF_SETTINGS_BRIGHT_BLOCK_H   (FF_SETTINGS_REL_SLIDER_Y + FF_SETTINGS_SLIDER_H) /* 74 */

#define FF_SETTINGS_REL_UNITS_Y (FF_SETTINGS_BRIGHT_BLOCK_H + FF_SETTINGS_ROW_GAP)     /* 88  */
/* CLOCK sits right after UNITS — both are display-format toggles — ahead of
 * SHARE/HAPTICS/GLOW/WATER/QUIET/COLORBLIND/CALIBRATE, each of which simply
 * shifts down by one FF_SETTINGS_ROW_STEP (spacing itself unchanged; S21's
 * scroll list has no page to overflow, so the new row just scrolls into
 * view like every other). */
#define FF_SETTINGS_REL_CLOCK_Y (FF_SETTINGS_REL_UNITS_Y + FF_SETTINGS_ROW_STEP)       /* 150 */
/* SCREEN sits right after CLOCK — same "format v8 amendment, maintainer
 * ask, 2026-09-02" insertion CLOCK's own S21 amendment made above CLOCK:
 * every row from SHARE down simply shifts by one more FF_SETTINGS_ROW_STEP
 * (spacing unchanged; the scroll list absorbs it, no page to overflow). */
#define FF_SETTINGS_REL_SCREEN_Y (FF_SETTINGS_REL_CLOCK_Y + FF_SETTINGS_ROW_STEP)      /* 212 */
#define FF_SETTINGS_REL_SHARE_Y (FF_SETTINGS_REL_SCREEN_Y + FF_SETTINGS_ROW_STEP)      /* 274 */
#define FF_SETTINGS_REL_HAPTICS_Y (FF_SETTINGS_REL_SHARE_Y + FF_SETTINGS_ROW_STEP)     /* 336 */
#define FF_SETTINGS_REL_GLOW_Y  (FF_SETTINGS_REL_HAPTICS_Y + FF_SETTINGS_ROW_STEP)     /* 398 */
#define FF_SETTINGS_REL_WATER_Y (FF_SETTINGS_REL_GLOW_Y + FF_SETTINGS_ROW_STEP)        /* 460 */
#define FF_SETTINGS_REL_QUIET_Y (FF_SETTINGS_REL_WATER_Y + FF_SETTINGS_ROW_STEP)       /* 522 */
#define FF_SETTINGS_REL_CB_Y    (FF_SETTINGS_REL_QUIET_Y + FF_SETTINGS_ROW_STEP)       /* 584 */
#define FF_SETTINGS_REL_CAL_Y   (FF_SETTINGS_REL_CB_Y + FF_SETTINGS_ROW_STEP)          /* 646 */
#define FF_SETTINGS_CONTENT_H   (FF_SETTINGS_REL_CAL_Y + FF_SETTINGS_ROW_H)            /* 694 */

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
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_pill(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t bg_hex, uint32_t fg_hex, int32_t letter_space, lv_event_cb_t cb,
                                     void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, FF_SETTINGS_PILL_RADIUS, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    if (letter_space != 0) {
        lv_obj_set_style_text_letter_space(label, letter_space, 0);
    }
    lv_obj_center(label);
    return btn;
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
 * ------------------------------------------------------------------- */
static void settings_share_cb(lv_event_t *e)
{
    (void)e;
    uint8_t const next = (s_settings.share_mode == FF_SHARE_GHOST) ? FF_SHARE_LIVE : FF_SHARE_GHOST;
    settings_emit_int(FF_SETTING_SHARE_MODE, next);
}

/* ---------------------------------------------------------------------
 * HAPTICS / GLOW / COLORBLIND — plain booleans.
 * ------------------------------------------------------------------- */
static void settings_haptics_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_HAPTICS, s_settings.haptics ? 0 : 1);
}

static void settings_night_glow_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_NIGHT_GLOW, s_settings.night_glow ? 0 : 1);
}

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * WATER NUDGE — label + value pill cycling the spec's v1 presets.
 * ------------------------------------------------------------------- */
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

static void settings_build_brightness(lv_obj_t *list, int32_t row_w)
{
    uint8_t const pct = settings_brightness_clamped();

    /* A full-width base covering the whole brightness block, exactly like the
     * toggle rows' settings_make_row (#bug2). Its SCROLL_CHAIN (default, not
     * cleared) means a press ANYWHERE on the block chains to the list scroll —
     * without it, the block's only objects are a 6px level bar and some labels,
     * so a drag on the empty space around them landed on nothing scrollable and
     * the list would not scroll while the brightness row was on screen. The −/+
     * pills sit on top and still take their taps. */
    lv_obj_t *base = lv_obj_create(list);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, row_w, FF_SETTINGS_BRIGHT_BLOCK_H);
    lv_obj_set_pos(base, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);
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
 * Entry point.
 * ------------------------------------------------------------------- */
void ff_scr_settings_build(lv_obj_t *parent, ff_app_settings_t const *settings)
{
    if (parent == NULL || settings == NULL) {
        return;
    }

    s_settings = *settings;

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

    settings_build_brightness(list, row_w);
    settings_build_toggle_row(list, FF_SETTINGS_REL_UNITS_Y, row_w, "UNITS", "FT", "M",
                              s_settings.imperial ? 0 : 1, settings_units_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_CLOCK_Y, row_w, "CLOCK", "12H", "24H",
                              s_settings.clock_24h ? 1 : 0, settings_clock_cb);
    settings_build_toggle_row_ex(list, FF_SETTINGS_REL_SCREEN_Y, row_w, "SCREEN", "NORMAL", "FLIPPED",
                                 s_settings.screen_flip ? 1 : 0, FF_SETTINGS_SCREEN_PILL_W, settings_screen_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_SHARE_Y, row_w, "SHARE", "LIVE", "GHOST",
                              (s_settings.share_mode == FF_SHARE_LIVE)    ? 0
                              : (s_settings.share_mode == FF_SHARE_GHOST) ? 1
                                                                          : -1,
                              settings_share_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_HAPTICS_Y, row_w, "HAPTICS", "ON", "OFF",
                              s_settings.haptics ? 0 : 1, settings_haptics_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_GLOW_Y, row_w, "GLOW", "ON", "OFF",
                              s_settings.night_glow ? 0 : 1, settings_night_glow_cb);

    char water_buf[16];
    settings_water_label(water_buf, sizeof(water_buf), s_settings.water_min);
    settings_build_value_row(list, FF_SETTINGS_REL_WATER_Y, row_w, "WATER NUDGE", water_buf,
                             s_settings.water_min == 0, settings_water_cb);

    settings_quiet_preset_t const *quiet = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    bool const quiet_off = (quiet != NULL) && (quiet->from_min == 0) && (quiet->to_min == 0);
    settings_build_value_row(list, FF_SETTINGS_REL_QUIET_Y, row_w, "QUIET HOURS",
                             (quiet != NULL) ? quiet->label : "CUSTOM", quiet_off, settings_quiet_cb);

    settings_build_toggle_row(list, FF_SETTINGS_REL_CB_Y, row_w, "COLORBLIND", "ON", "OFF",
                              s_settings.colorblind ? 0 : 1, settings_colorblind_cb);
    settings_build_calibrate_row(list, FF_SETTINGS_REL_CAL_Y, row_w);

    /* #bug4 — restore the scroll offset the previous build left (0 on a fresh
     * entry, cleared by ff_scr_settings_reset_scroll). The layout must be
     * resolved first so LVGL knows the scrollable range to clamp against. */
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, s_scroll_y, LV_ANIM_OFF);
}

/* #bug4 — see scr_settings.h. Clear the remembered offset so the next build
 * renders from the top; the face dispatcher calls this on a FRESH entry into
 * Settings (a not-Settings -> Settings face transition). */
void ff_scr_settings_reset_scroll(void)
{
    s_scroll_y = 0;
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
