/**
 * scr_banner.c — see scr_banner.h.
 */
#include "scr_banner.h"

#include "ff_crew.h"  /* ff_fmt_age — the one shared age formatter (S22-a honesty note) */
#include "ff_intent.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "radar_layout.h" /* RADAR_LAYOUT_STATUS_BAR_DY — the row this strip now centers on */

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * MAINTAINER DECISION (B, 2026-09-02, docs/specs/S26-device-lifecycle.md
 * "Notifications (slice d)"): a transient banner should hide the LEAST
 * valuable row on whatever face is showing. At the old BANNER_CY (-90)
 * the strip sat just below the status bar and covered the top of
 * Radar's compass/close-range readout, or a thread's first bubble —
 * both more valuable than the clock/mesh/battery row. Moved to COVER
 * the status bar instead, computed from `RADAR_LAYOUT_STATUS_BAR_DY`
 * (-160) directly — not a second hand-typed number — so the two rows
 * can never drift apart.
 *
 * `BANNER_CY` is the CENTER-relative vertical offset (this codebase's
 * standing convention for *_DY constants, ff_layout.h's own doc comment
 * on ff_layout_centered_band_max_width) — negative = above the puck's
 * own center.
 *
 * ## Width: two chord checks, not one
 * At dy=-160 the round glass is already 80% of the way to the pole, so
 * the chord is narrow — this needed BOTH of the checks this codebase has
 * learned to run for a corner point near the bezel (the S99 compose SEND
 * lesson, test_scr_intent.c: "chord math... is a DIFFERENT, WEAKER
 * quantity for a corner point" than the true 2D distance):
 *
 *  1. The PERMISSIVE bound: `ff_layout_chord_half_width`/
 *     `ff_layout_centered_band_max_width`'s own per-axis math. The
 *     pill's farther edge from center (its TOP, since it sits above
 *     center) is at |BANNER_CY| + BANNER_H/2 = 160 + 24 = 184px, so
 *     against FF_THEME_GLASS_R (200): sqrt(200^2 - 184^2) =
 *     sqrt(6144) ~= 78.4px half-chord, less 10px safety = ~68.4px, i.e.
 *     up to ~137px wide by this measure alone. This is the number a
 *     first pass at this problem lands on (and the wrong one to ship —
 *     see next).
 *  2. The BINDING bound: the TRUE Euclidean distance of each corner from
 *     FF_THEME_GLASS_CX/CY, which is what "N px inside the glass radius"
 *     actually means for a rectangle's hit-rect (test_scr_banner.c's
 *     S26d_AC2_banner_corners_clear_glass_by_10px, the same check
 *     S99_compose_send_corner_clears_bezel_margin_bar runs for SEND). A
 *     corner at (dx, 184) clears a 10px margin iff
 *     dx^2 + 184^2 <= (200-10)^2 = 190^2 = 36100, i.e.
 *     dx <= sqrt(36100 - 33856) = sqrt(2244) ~= 47.4px — a MUCH tighter
 *     bound than #1's 68.4px, because near the pole the circle curves
 *     away faster than a straight chord offset suggests. BANNER_W (90,
 *     half-width 45) clears this by ~2.4px of straight-line slack —
 *     verified precisely, not just "looks like it fits": at dx=45,
 *     distance = sqrt(45^2 + 184^2) = sqrt(35881) ~= 189.42px, a 10.58px
 *     margin inside FF_THEME_GLASS_R (200).
 *
 * Centered on FF_THEME_GLASS_CX (208), not the puck's own 206 — the same
 * "edge-hugging elements centre on the VISIBLE glass, not the
 * framebuffer" rule scr_radar.c's rim tint follows (ff_theme.h's own
 * doc comment on FF_THEME_GLASS_*): at only a ~10.6px margin, the 2px
 * asymmetry between the two centers is no longer "invisible" the way
 * FF_THEME_GLASS_*'s own comment says it is for content away from the
 * edge — it is ~20% of the whole safety budget.
 * ------------------------------------------------------------------- */

#define BANNER_W  90
#define BANNER_H  48 /* the hit floor itself now — narrower at this height leaves no room to spare, see above */
#define BANNER_CY ((float)RADAR_LAYOUT_STATUS_BAR_DY) /* computed from the row it now covers, never a second magic number */
#define BANNER_DX (FF_THEME_GLASS_CX - FF_THEME_PUCK_RADIUS_PX) /* +2: recentre on the visible glass, not the framebuffer */

_Static_assert(BANNER_H >= FF_THEME_MIN_HIT_PX, "banner strip must clear the 44px hit-target floor");

/* Inner padding / sub-element geometry, relative to the pill's own
 * top-left corner (0,0 .. BANNER_W,BANNER_H). Every one of these shrank
 * from the pre-move layout to fit BANNER_W's much smaller budget (240 ->
 * 90px) — glyph/name/age all keep their own slot, just a narrower one;
 * DOTS ellipsis (below) is what keeps that honest under real content
 * instead of silently clipping. */
#define BANNER_PAD_X    6
#define BANNER_GLYPH_W  14
#define BANNER_TEXT_X   (BANNER_PAD_X + BANNER_GLYPH_W + 6)
#define BANNER_AGE_W    24
#define BANNER_NAME_AGE_GAP 2
#define BANNER_TEXT_W   (BANNER_W - BANNER_TEXT_X - BANNER_PAD_X - BANNER_AGE_W - BANNER_NAME_AGE_GAP)
#define BANNER_ROW1_Y   4
#define BANNER_ROW2_Y   25
#define BANNER_ROW_H    18

_Static_assert(BANNER_TEXT_W > 0, "narrow banner: name column would invert (negative width)");

/* ---------------------------------------------------------------------
 * Kind -> glyph + accent color. MESSAGE/RALLY are the only kinds this
 * slice's shell ever pushes (S26(d) spec: "MESSAGE or RALLY"); FLARE/
 * SYSTEM are given honest, distinct treatments anyway so a later slice
 * that starts pushing them needs no change here — same "the vocabulary
 * is complete even where unexercised" posture core/include/ff_notify.h
 * documents for the TAKEOVER tier.
 * ------------------------------------------------------------------- */

static char const *banner_glyph(ff_notify_kind_t kind)
{
    switch (kind) {
    case FF_NOTIFY_MESSAGE: return LV_SYMBOL_ENVELOPE;
    case FF_NOTIFY_RALLY:   return LV_SYMBOL_GPS;
    case FF_NOTIFY_FLARE:   return LV_SYMBOL_EYE_OPEN; /* mirrors the popup's flare row, scr_inbox.c */
    case FF_NOTIFY_SYSTEM:  return LV_SYMBOL_BELL;
    default:                return LV_SYMBOL_BELL;
    }
}

static uint32_t banner_accent(ff_notify_kind_t kind)
{
    switch (kind) {
    case FF_NOTIFY_MESSAGE: return FF_THEME_CREW_GREEN;   /* mirrors the popup's Compose accent */
    case FF_NOTIFY_RALLY:   return FF_THEME_CREW_VIOLET;  /* mirrors the popup's Rally accent */
    case FF_NOTIFY_FLARE:   return FF_THEME_COLOR_AMBER;  /* mirrors the popup's Flare accent */
    case FF_NOTIFY_SYSTEM:  return FF_THEME_COLOR_MUTED;
    default:                return FF_THEME_COLOR_MUTED;
    }
}

/* ---------------------------------------------------------------------
 * Tap.
 * ------------------------------------------------------------------- */

static void banner_open_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_banner_build(lv_obj_t *parent, ff_app_banner_t const *banner, bool colorblind)
{
    if (parent == NULL || banner == NULL || !banner->active) {
        return; /* nothing to show — the honest "no banner queued" state */
    }

    lv_obj_t *strip = lv_button_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, BANNER_W, BANNER_H);
    lv_obj_align(strip, LV_ALIGN_CENTER, (int32_t)BANNER_DX, (int32_t)BANNER_CY);
    lv_obj_set_style_radius(strip, BANNER_H / 2, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 2, 0);
    lv_obj_set_style_border_color(strip, lv_color_hex(banner_accent(banner->kind)), 0);
    lv_obj_set_style_border_opa(strip, LV_OPA_60, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Clear LVGL's default LV_OBJ_FLAG_PRESS_LOCK ("keep the object
     * pressed, and still fire CLICKED on release, even if the press slid
     * off it") — the #145 launcher lesson (scr_launcher.c/scr_compose.c's
     * own copies of this fix; see either's comment for the full LVGL
     * mechanism). Without this, a real finger that presses the banner
     * and drags away before lifting still opens it on release — a
     * scroll/dismiss gesture across the strip must never be read as a
     * deliberate tap. See test_scr_banner.c's
     * S26d_AC2_banner_drag_off_emits_nothing for the real-indev proof. */
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(strip, banner_open_cb, LV_EVENT_CLICKED, NULL);
    /* Press feedback (every tappable control, S24's standing convention):
     * a visible amber-toward-accent wash on touch-down. */
    lv_obj_set_style_bg_color(strip, lv_color_hex(banner_accent(banner->kind)), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(strip, LV_OPA_30, LV_STATE_PRESSED);

    /* Kind glyph, left, in the kind's accent color. */
    lv_obj_t *glyph = lv_label_create(strip);
    lv_label_set_text(glyph, banner_glyph(banner->kind));
    lv_obj_set_style_text_font(glyph, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(glyph, lv_color_hex(banner_accent(banner->kind)), 0);
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, BANNER_PAD_X, 0);

    /* Row 1: sender name (their crew color — the "prove you meant this"
     * honest-identity convention every other name in this app follows:
     * an empty name, e.g. a paired member whose NodeInfo hasn't arrived
     * yet, renders as an honest blank rather than a fabricated one)
     * beside the real age — at this width there is no room for a
     * separate age "chip" in a top-right corner (spec: "the age may
     * drop to a single line beside the name if the width demands", and
     * here it does), so both live on row 1, name first, age immediately
     * after it. */
    lv_obj_t *name = lv_label_create(strip);
    lv_label_set_text(name, banner->name);
    lv_obj_set_style_text_font(name, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(ff_theme_crew_color(banner->color_idx, colorblind)), 0);
    /* DOTS mode only truncates to ONE line when the label's HEIGHT is
     * bounded too — width alone makes LVGL wrap instead (scr_inbox.c's
     * own documented lesson, same fix applied here). */
    lv_obj_set_size(name, BANNER_TEXT_W, BANNER_ROW_H);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS); /* ellipsize, never overflow off-glass */
    lv_obj_set_pos(name, BANNER_TEXT_X, BANNER_ROW1_Y);

    char age_buf[12];
    ff_fmt_age(age_buf, sizeof(age_buf), banner->age_ms); /* honest, real age — never a fabricated "now" */
    lv_obj_t *age = lv_label_create(strip);
    lv_label_set_text(age, age_buf);
    lv_obj_set_style_text_font(age, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(age, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    /* Same DOTS + bounded-height ellipsis as name/body above: even the
     * coarsest ff_fmt_age output ("59 MIN") can outrun BANNER_AGE_W at
     * this width, and this must still never overflow off the strip. */
    lv_obj_set_size(age, BANNER_AGE_W, BANNER_ROW_H);
    lv_label_set_long_mode(age, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(age, BANNER_W - BANNER_PAD_X - BANNER_AGE_W, BANNER_ROW1_Y);

    /* Row 2: the preview body (plain ink — the name above already carries
     * the color/identity cue, so this stays legible against any kind's
     * accent). */
    lv_obj_t *body = lv_label_create(strip);
    lv_label_set_text(body, banner->text);
    lv_obj_set_style_text_font(body, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_size(body, BANNER_W - BANNER_TEXT_X - BANNER_PAD_X, BANNER_ROW_H); /* DOTS + bounded height, see above */
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(body, BANNER_TEXT_X, BANNER_ROW2_Y);
}
