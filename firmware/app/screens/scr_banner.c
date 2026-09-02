/**
 * scr_banner.c — see scr_banner.h.
 */
#include "scr_banner.h"

#include "ff_crew.h"  /* ff_fmt_age — the one shared age formatter (S22-a honesty note) */
#include "ff_intent.h"
#include "ff_layout.h"
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * A rounded pill, horizontally centered, near the top of the puck.
 * `BANNER_CY` is the CENTER-relative vertical offset (this codebase's
 * standing convention for *_DY constants, ff_layout.h's own doc comment
 * on ff_layout_centered_band_max_width) — negative = above the puck's
 * own center.
 *
 * Circle-fit math (radius = FF_THEME_PUCK_RADIUS_PX = 206), the same
 * hand-worked-and-commented check scr_power_menu.c's button geometry
 * uses (that file's BTN3_DY comment is the precedent for this style —
 * verified by eye here, then asserted empirically by
 * test_face_hit_targets.c's circle-containment + adjacency sweep, which
 * covers every committed fixture): the farther edge from the pill's own
 * center is |BANNER_CY| + BANNER_H/2 = 90 + 26 = 116px from the puck's
 * center; sqrt(206^2 - 116^2) = sqrt(28980) ~= 170.2px is the widest
 * half-chord at that height, less a 10px safety margin = ~160.2px, i.e.
 * a pill up to ~320px wide fits entirely on-glass at this height.
 * BANNER_W (240) clears that comfortably.
 *
 * BANNER_CY itself (not just BANNER_W) is chosen against a SECOND
 * constraint the chord math alone doesn't capture: `scr_inbox.c`'s
 * thread/picker/popup/rally sub-views all pin a >=44px BACK/close control
 * at puck-local y=[30,74] (FF_INBOX_BACK_Y/FF_INBOX_BACK_PX) — the
 * banner must clear that by the FF_HIT_MIN_GAP_PX (8px) adjacency floor
 * too, or it collides with a live control the sweep (correctly) flags.
 * At BANNER_CY=-90 the pill's top edge sits at puck-local y = 206-90-26 =
 * 90, a 16px clearance below the back button's y=74 bottom.
 * ------------------------------------------------------------------- */

#define BANNER_W  240
#define BANNER_H  52 /* spec: "≥ 48 px tall hit target" */
#define BANNER_CY (-90.0f)

_Static_assert(BANNER_H >= FF_THEME_MIN_HIT_PX, "banner strip must clear the 44px hit-target floor");

/* Inner padding / sub-element geometry, relative to the pill's own
 * top-left corner (0,0 .. BANNER_W,BANNER_H). */
#define BANNER_PAD_X    14
#define BANNER_GLYPH_W  24
#define BANNER_TEXT_X   (BANNER_PAD_X + BANNER_GLYPH_W + 8)
#define BANNER_TEXT_W   (BANNER_W - BANNER_TEXT_X - BANNER_PAD_X - 44) /* leaves room for the age chip, right */
#define BANNER_AGE_W    40

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
    lv_obj_align(strip, LV_ALIGN_CENTER, 0, (int32_t)BANNER_CY);
    lv_obj_set_style_radius(strip, BANNER_H / 2, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 2, 0);
    lv_obj_set_style_border_color(strip, lv_color_hex(banner_accent(banner->kind)), 0);
    lv_obj_set_style_border_opa(strip, LV_OPA_60, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
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
     * yet, renders as an honest blank rather than a fabricated one) +
     * the real age, right. */
    lv_obj_t *name = lv_label_create(strip);
    lv_label_set_text(name, banner->name);
    lv_obj_set_style_text_font(name, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(ff_theme_crew_color(banner->color_idx, colorblind)), 0);
    /* DOTS mode only truncates to ONE line when the label's HEIGHT is
     * bounded too — width alone makes LVGL wrap instead (scr_inbox.c's
     * own documented lesson, same fix applied here). */
    lv_obj_set_size(name, BANNER_TEXT_W, 18);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS); /* ellipsize, never overflow off-glass */
    lv_obj_set_pos(name, BANNER_TEXT_X, 6);

    char age_buf[12];
    ff_fmt_age(age_buf, sizeof(age_buf), banner->age_ms); /* honest, real age — never a fabricated "now" */
    lv_obj_t *age = lv_label_create(strip);
    lv_label_set_text(age, age_buf);
    lv_obj_set_style_text_font(age, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(age, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(age, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(age, BANNER_AGE_W);
    lv_obj_align(age, LV_ALIGN_TOP_RIGHT, -BANNER_PAD_X, 6);

    /* Row 2: the preview body (plain ink — the name above already carries
     * the color/identity cue, so this stays legible against any kind's
     * accent). */
    lv_obj_t *body = lv_label_create(strip);
    lv_label_set_text(body, banner->text);
    lv_obj_set_style_text_font(body, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_size(body, BANNER_W - BANNER_TEXT_X - BANNER_PAD_X, 18); /* DOTS + bounded height, see above */
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(body, BANNER_TEXT_X, 27);
}
