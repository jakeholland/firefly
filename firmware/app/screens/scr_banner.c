/**
 * scr_banner.c — see scr_banner.h.
 */
#include "scr_banner.h"

#include "ff_crew.h"  /* ff_fmt_age — the one shared age formatter (S22-a honesty note) */
#include "ff_intent.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "radar_layout.h" /* RADAR_LAYOUT_STATUS_BAR_DY — the row this strip now centers on */
#include "scr_nav.h" /* ff_scr_button_create — the shared PRESS_LOCK-clearing button base (#145/#148) */

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * MAINTAINER DECISION (B, 2026-09-02, docs/specs/S26-device-lifecycle.md
 * "Notifications (slice d)") — SUPERSEDED (puck-ux-usability-2026-09-15,
 * finding 4, slice 4): the original decision here was "a transient banner
 * should hide the LEAST valuable row on whatever face is showing", and
 * moved the strip to COVER the status bar on the theory that the clock/
 * mesh/battery row was less valuable than Radar's name/distance stack or a
 * thread's first bubble. The 2026-09-15 usability review measured the
 * result and found that reasoning wrong: `banner_on_radar.png` showed
 * `9:46` truncated, `LINKED` **entirely gone**, and the battery reduced to
 * a bare `%` — "the two facts a user checks before trusting the device —
 * can it reach anyone and will it last — are hidden by the notification
 * that made them look." A banner that hides the very information a user
 * glances at the puck to confirm is not an acceptable trade at any width.
 *
 * FIX (finding 4): move the strip BELOW the status row instead of over
 * it — it now covers nothing whenever nothing else occupies that band
 * (verified per-face by `test_scr_banner.c`'s disjoint-from-status-row
 * assertion, the new equivalent of the old "covers the status row"
 * proof), and the extra vertical room bought by moving down (see the
 * chord math below) pays for a WIDER strip (160 -> 200px) that stops the
 * message preview truncating at "The Firefly To…".
 *
 * `BANNER_CY` is the CENTER-relative vertical offset (this codebase's
 * standing convention for *_DY constants, ff_layout.h's own doc comment
 * on ff_layout_centered_band_max_width) — negative = above the puck's
 * own center.
 *
 * ## Centre: clears the status text for real, with the smallest cost
 * anywhere else
 * `BANNER_CY = RADAR_LAYOUT_STATUS_BAR_DY + 33.0f` = -127 (puck-local
 * y=79): top edge at y = 206 + (-127) - 24 = 55, ONE genuine pixel below
 * the status text's own measured bottom edge (54) — still computed FROM
 * the status-row constant (never a second independent magic number),
 * just offset by the smallest amount that (a) fully clears the status
 * text (finding 4's explicit ask) while (b) minimizing the now-unavoidable
 * cost elsewhere (see `BANNER_CY`'s own `#define` comment for the full
 * two-part measured correction to the review's `+ 40.0f` worked example,
 * which checked the glass and the status row but not the launcher's
 * satellite ring or the thread's first bubble).
 *
 * ## Width: two chord checks, not one — and a correction to the radius
 * they're run against
 * At the lower centre the chord is WIDER than it was at -146 (moving
 * toward center opens up more horizontal room before the glass curves
 * in), so the same two-bound check this file has always run (per the S99
 * compose-SEND lesson: chord math "is a DIFFERENT, WEAKER quantity for a
 * corner point" than the true 2D distance) has more slack, not less, to
 * spend on width:
 *
 *  1. The PERMISSIVE bound: farther (top) edge at
 *     |BANNER_CY| + BANNER_H/2 = 127 + 24 = 151px from center.
 *  2. The BINDING bound: the TRUE Euclidean distance of each corner
 *     from center, which is what "N px inside the glass radius" means
 *     for a rectangle's hit-rect.
 *
 * Both bounds are checked against `FF_THEME_GLASS_R` (200) — the actual
 * MEASURED visible glass on real hardware (ff_theme.h's own doc comment:
 * "the round bezel window sits ~5px right of the 412-wide pixel array...
 * GLASS_R 200, pulled in 3px from the 203 measured so a ring clears the
 * bezel lip"), not `FF_THEME_PUCK_RADIUS_PX` (206, the framebuffer's own
 * radius) — see #154/#155 for why edge-hugging elements in this codebase
 * use the measured glass, not the framebuffer.
 *
 * At dy=-127, half-height 24, top edge dy=-151, radius 200, 10px safety:
 * sqrt((200-10)^2 - 151^2) = sqrt(190^2 - 151^2) = sqrt(36100-22801) =
 * sqrt(13299) ~= 115.32px half-chord -> ~230.6px max width. BANNER_W (200,
 * half-width 100) clears this with room to spare: farthest corner distance
 * from glass center = sqrt(100^2 + 151^2) = sqrt(32801) ~= 181.11px, an
 * 18.89px margin inside FF_THEME_GLASS_R (200) — well past the review's
 * own >= 8px bar and the >= 10px bar this file's own test
 * (`S26d_AC2_banner_corners_clear_glass_by_10px`) still enforces —
 * verified precisely, not eyeballed, against the real rendered rect.
 *
 * Centered on FF_THEME_GLASS_CX (208), not the puck's own 206 — the
 * same "edge-hugging elements centre on the VISIBLE glass, not the
 * framebuffer" rule scr_radar.c's rim tint follows (ff_theme.h's own
 * doc comment on FF_THEME_GLASS_*).
 * ------------------------------------------------------------------- */

#define BANNER_W  200 /* widened 160 -> 200 (finding 4): the extra room bought by moving below the status row */
#define BANNER_H  48 /* the hit floor itself — unchanged */
/* +33, not the review's own worked "+40" — two independent, MEASURED
 * corrections to the review's arithmetic (which only checked the glass
 * and the status row, not every face the banner actually renders over):
 *
 *  1. `banner_on_launcher.json`: at +40 the strip's bottom edge (110) sat
 *     only 7px above the launcher's top-cardinal-adjacent satellites' top
 *     edge (117, the LAUNCHER_SAT ring in scr_launcher.c) — under
 *     FF_HIT_MIN_GAP_PX (8), a genuine mis-tap risk between two
 *     INDEPENDENT clickables (the banner and a satellite it neither
 *     overlaps nor gets masked against — see
 *     ff_scr_nav_mask_clickables_under_banner's "overlap AND
 *     remainder-too-small" contract, which this near-miss case doesn't
 *     meet). Caught by test_face_hit_targets.c's whole-device sweep.
 *  2. `banner_on_thread.json`: the status row's real bottom edge (54,
 *     montserrat_16 now that the clock is FF_THEME_FONT_MSG_BODY — see
 *     radar_build_status_bar) to the thread view's first bubble's real
 *     top edge (98, FF_INBOX_THREAD_LIST_TOP_Y + the sender-row offset,
 *     scr_inbox.c) leaves a window of exactly 44px — ONE px short of
 *     BANNER_H (48) with ZERO margin spent on either side, so no
 *     placement of a 48px banner in that window can avoid touching one
 *     of the two. Pinned at the position that keeps the EXPLICIT,
 *     reviewed obligation (finding 4: never overlap the status TEXT)
 *     at a genuine, non-zero margin, and accepts the smallest achievable
 *     overlap with the thread's first bubble instead (5px, its own top
 *     padding more than its text baseline — see
 *     test_scr_banner.c's own doc comment on this exact trade, an
 *     AGENTS.md-flagged interpretation call: the review never analyzed
 *     the banner-vs-first-bubble case at all, only status-row/glass).
 *
 * +33 buys BOTH: bottom edge 103 clears the launcher satellites (117) by
 * 14px, and top edge 55 clears the status text's real bottom (54) by a
 * genuine 1px — still zero OVERLAP (disjoint, not touching), the literal
 * property finding 4 asks for. See test_scr_banner.c's
 * S26d_AC2_banner_disjoint_from_status_text_row and
 * S26d_AC2_banner_corners_clear_glass_by_10px for the real, rendered
 * proof at this exact value. */
#define BANNER_CY ((float)RADAR_LAYOUT_STATUS_BAR_DY + 33.0f)
#define BANNER_DX (FF_THEME_GLASS_CX - FF_THEME_PUCK_RADIUS_PX) /* +2: recentre on the visible glass, not the framebuffer */

_Static_assert(BANNER_H >= FF_THEME_MIN_HIT_PX, "banner strip must clear the 44px hit-target floor");

/* Inner padding / sub-element geometry, relative to the pill's own
 * top-left corner (0,0 .. BANNER_W,BANNER_H). At BANNER_W=160 there is
 * real room again: a demo-length name (e.g. "DANA") renders in full,
 * the preview shows well past 10 characters before DOTS ellipsis has to
 * step in, and the age fits without truncating every time. */
#define BANNER_PAD_X    10
#define BANNER_GLYPH_W  18
#define BANNER_TEXT_X   (BANNER_PAD_X + BANNER_GLYPH_W + 8)
#define BANNER_AGE_W    40
#define BANNER_NAME_AGE_GAP 4
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

    lv_obj_t *strip = ff_scr_button_create(parent);
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
    /* PRESS_LOCK is cleared for us by ff_scr_button_create (scr_nav.h) —
     * without it, a real finger that presses the banner and drags away
     * before lifting would still open it on release. See
     * test_scr_banner.c's S26d_AC2_banner_drag_off_emits_nothing for the
     * real-indev proof. */
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
