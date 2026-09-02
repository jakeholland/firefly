/**
 * scr_launcher.c — see scr_launcher.h.
 *
 * Layout constants + the circle-button helper are a SELF-CONTAINED local
 * copy, not shared with `scr_power_menu.c`'s (near-identical) button
 * helper — same file-static, nothing-to-import tradeoff that file's own
 * top comment documents against `scr_flare.c`.
 *
 * PR #142 review, Design 1 + Design 2: the circles show a real LVGL
 * built-in symbol glyph (`LV_SYMBOL_*` — compiled into the Montserrat
 * bitmap fonts this codebase already ships, exactly like
 * `scr_banner.c`'s `LV_SYMBOL_ENVELOPE`/`LV_SYMBOL_GPS` and
 * `scr_signals.c`'s several `LV_SYMBOL_*` rows) rather than a text
 * abbreviation, and are 96px in diameter rather than 64px.
 *
 * AMENDED 2026-09-01 (the maintainer's on-glass decision — see
 * ff_route.h's header note): a FIFTH circle, Radar, joins with no
 * special treatment, which retires the old four-circle diagonal cross
 * (NW/NE/SW/SE) in favor of a 2-over-3 grid — see the layout comment
 * below for the geometry, the two hazards a ring/cross both run into,
 * and the numbers proving this shape clears both.
 */
#include "scr_launcher.h"

#include "ff_intent.h"
#include "ff_theme.h"
#include "scr_signals.h" /* ff_scr_signals_unread_count — the Signals circle's badge */
#include "lvgl.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 *
 * LVGL dispatches a touch against a widget's RECTANGULAR bounds, not
 * the visual circle drawn inside it (`lv_obj_set_style_radius` is
 * cosmetic) — so every geometry argument below is about a 96x96 SQUARE,
 * and both hazards that rule out a ring/cross layout follow directly
 * from that:
 *
 *  1. FAT-THUMB MID-DRAG MIS-TAP. The sim's `ctl swipe` command (and,
 *     on glass, an ordinary horizontal finger drag) sweeps a point
 *     straight across the screen's vertical center (y =
 *     FF_THEME_PUCK_RADIUS_PX) — not a navigation gesture this face
 *     acts on (`ff_route_swipe` has no live caller, S26 slice e), but a
 *     REAL LVGL button underneath a drag's path can still register a
 *     click on release. Any circle whose box crosses that line — i.e.
 *     whose center sits within LAUNCHER_CIRCLE_DIAM/2 (48px) of it —
 *     can register an accidental tap this way. A regular pentagon
 *     cannot clear this:
 *     for radius R, the two vertices nearest horizontal sit at
 *     |R*sin(best rotation)|, and the best achievable rotation for a
 *     5-fold-symmetric ring only reaches an 18-degree clearance angle
 *     (verified numerically), which needs R > ~155px to clear 48px —
 *     but a circle centered that far out already fails hazard 2 below
 *     by a wide margin (its farthest corner would land at ~240px,
 *     against a 206px glass radius).
 *  2. OFF-GLASS CORNERS. A box centered at (cx, cy) has its farthest-
 *     from-center corner at distance sqrt((|cx|+48)^2 + (|cy|+48)^2) —
 *     THAT whole rectangle, corner included, must stay inside
 *     FF_THEME_PUCK_RADIUS_PX (206px). For a fixed distance from center,
 *     this is CHEAPEST when a circle sits on a single axis (cx=0 or
 *     cy=0 — the "extra" term collapses to the 48px half-width alone)
 *     and MOST EXPENSIVE on the diagonal (both terms grow together,
 *     picking up a sqrt(2) factor at 45 degrees). The pre-amendment
 *     four-circle diamond already spent most of its margin on that
 *     diagonal placement (85,85) -> corner at (85+48)*sqrt(2) ~= 188px,
 *     18px of margin; adding a fifth circle anywhere that also clears
 *     hazard 1 (i.e. off the diamond's own diagonals) runs out of room
 *     fast — verified by direct search, no diamond-plus-one placement
 *     clears both hazards with the required FF_HIT_MIN_GAP_PX (8px)
 *     adjacency floor.
 *
 * The two-row grid below keeps every circle within ONE axis of center
 * (small |cx| for the two top circles, small... actually neither axis
 * is zero for the off-center ones, but both terms stay modest — see the
 * numbers just below) and every circle's |cy| >= 60px, twelve pixels
 * clear of hazard 1's 48px danger zone. Verified numerically (not just
 * argued): every circle's farthest corner sits at <= 195.6px (>= 10px
 * inside the 206px glass), and the tightest circle-to-circle gap is
 * 19px (more than double the 8px floor) — comfortably clearing both
 * hazards, unlike anything a ring/cross could reach at this circle
 * size. Reading order (top-left, top-right, bottom-left, bottom-center,
 * bottom-right) assigns the five faces in launcher_idx order — position
 * in that order is arbitrary, carrying no meaning about importance;
 * Radar happens to be first only because index 0 is.
 * ------------------------------------------------------------------- */

#define LAUNCHER_CIRCLE_DIAM     96    /* PR #142 review Design 2 — up from 64px: "easier-to-tap targets" */
#define LAUNCHER_TOP_DX          60.0f  /* top row: dx from center for each of the 2 circles */
#define LAUNCHER_TOP_DY          (-60.0f) /* top row: dy from center (negative = above) */
#define LAUNCHER_BOTTOM_SIDE_DX  115.0f /* bottom row: dx for the two OUTER of 3 circles */
#define LAUNCHER_BOTTOM_DY       60.0f  /* bottom row: dy from center (positive = below) */
#define LAUNCHER_TOP_CAPTION_DY  (-52.0f) /* top row: caption ABOVE its circle (room is above, not in the 24px middle gap) */
#define LAUNCHER_BOTTOM_CAPTION_DY 58.0f  /* bottom row: caption BELOW its circle, same convention pre-amendment used */
#define LAUNCHER_BADGE_PX        16    /* the unread badge scales with the circle (was 12px at 64px) */
#define LAUNCHER_GLYPH_FONT      (&lv_font_montserrat_24) /* readable at ~30px inside a 96px circle */

_Static_assert(LAUNCHER_CIRCLE_DIAM >= 56, "launcher circles must clear the spec's 56px floor");
_Static_assert(LAUNCHER_CIRCLE_DIAM >= FF_THEME_MIN_HIT_PX, "launcher circles must clear the shared 44px hit floor");

/* ---------------------------------------------------------------------
 * One app circle + its caption. `glyph`: an `LV_SYMBOL_*` string drawn
 * INSIDE the circle — the spec's "kind glyph" (PR #142 review Design 1:
 * this repo DOES vendor icon glyphs, compiled into its Montserrat
 * bitmap fonts by default; `ff_theme.h`'s font-substitution note is
 * about DEVICE FONTS/type sizes, not about symbols, and does not apply
 * here). `caption`: the full word, drawn `caption_dy` from the circle's
 * own center (POSITIVE = below, NEGATIVE = above — the top row passes
 * negative; see this file's layout comment). `launcher_idx`: this
 * file's own fixed circle order (0=Radar, 1=Now, 2=Signals, 3=Map,
 * 4=Settings — ff_intent.h's FF_INTENT_LAUNCHER_SELECT payload), passed
 * through LVGL event user_data (the scr_compose.c/scr_signals.c
 * precedent for a per-callback small int with no new global state).
 * ------------------------------------------------------------------- */

static void launcher_circle_click_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    in.u.launcher_idx = (uint8_t)idx;
    ff_intent_emit(&in);
}

static void launcher_make_circle(lv_obj_t *puck, char const *glyph, char const *caption_text, float dx, float dy,
                                  float caption_dy, uintptr_t launcher_idx, bool badge)
{
    lv_obj_t *btn = lv_button_create(puck);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LAUNCHER_CIRCLE_DIAM, LAUNCHER_CIRCLE_DIAM);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    /* Press feedback: tints toward amber on touch-down — the same
     * outlined-pill convention scr_power_menu.c's Reboot/Cancel buttons
     * use (LV_STATE_PRESSED). Every circle, Radar included, gets
     * IDENTICAL styling here — no visual privileging by size, color, or
     * border; position (this function's `dx`/`dy` arguments) is the
     * only thing that varies, and it varies for the geometric reasons
     * this file's layout comment proves, not to rank one face above
     * another. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);
    lv_obj_add_event_cb(btn, launcher_circle_click_cb, LV_EVENT_CLICKED, (void *)launcher_idx);

    lv_obj_t *glyph_label = lv_label_create(btn);
    lv_label_set_text(glyph_label, glyph);
    lv_obj_set_style_text_font(glyph_label, LAUNCHER_GLYPH_FONT, 0);
    lv_obj_set_style_text_color(glyph_label, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_center(glyph_label);

    /* Caption: a sibling on `puck`, NOT a child of `btn` — LVGL base
     * labels are non-clickable by default (scr_power_menu.c's "POWER"
     * headline is the precedent: a bare lv_label_create with no explicit
     * CLICKABLE clear), so this stays pure chrome, never a second hit
     * target the adjacency sweep would have to reason about. */
    lv_obj_t *caption_label = lv_label_create(puck);
    lv_label_set_text(caption_label, caption_text);
    lv_obj_set_style_text_font(caption_label, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(caption_label, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(caption_label, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)(dy + caption_dy));

    if (badge) {
        /* Small amber dot at the circle's upper-right — same badge shape
         * the old page-dot row used (scr_nav.c's retired
         * nav_build_page_dots), scaled up with the circle (PR #142
         * review Design 2). */
        lv_obj_t *dot = lv_obj_create(puck);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, LAUNCHER_BADGE_PX, LAUNCHER_BADGE_PX);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator, not a control */
        int32_t const corner = (int32_t)(LAUNCHER_CIRCLE_DIAM / 2 * 0.7071f); /* ~45 degrees out on the rim */
        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)dx + corner, (int32_t)dy - corner);
    }
}

void ff_scr_launcher_build(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    bool const signals_unread = ff_scr_signals_unread_count(&state->signals) > 0;

    /* Fixed circle order — MUST match ff_intent.h's FF_INTENT_LAUNCHER_SELECT
     * payload convention (0=Radar, 1=Now, 2=Signals, 3=Map, 4=Settings)
     * and ff_shell.c's k_launcher_faces mapping table. Reading order:
     * top-left, top-right, bottom-left, bottom-center, bottom-right —
     * see this file's layout comment for the geometry and why this
     * shape, not a ring/cross, is what clears the glass.
     *
     * Glyphs: `LV_SYMBOL_WIFI` for Radar (reviewer PR #144 round 2 —
     * `LV_SYMBOL_EYE_OPEN`, this file's first choice, ALREADY MEANS
     * FLARE ("come find me") in two other live places on this same
     * device: the Signals popup's flare row (scr_signals.c, the
     * `signals_popup_flare_cb` row) and the flare banner overlay
     * (scr_banner.c's `FF_NOTIFY_FLARE` case) — reusing it for Radar
     * would read as "this circle is about flares", a genuine
     * confusability defect, not just a style choice. `LV_SYMBOL_GPS` —
     * the obvious "location/compass" glyph — is already spoken for by
     * Map, which shows an actual geographic map, so reusing it for
     * Radar would make the two circles read as the same feature.
     * `LV_SYMBOL_WIFI`'s radiating arcs read as a sweep/scan — the
     * closest built-in match to "radar" as a concept — and, unlike
     * EYE_OPEN, is unused everywhere else in this codebase, confirmed
     * by grep before picking it. `LV_SYMBOL_BELL` (notifications) was
     * also considered and rejected: wrong domain, not this screen's
     * job. `LV_SYMBOL_LIST` for Now/Lineup (the schedule list),
     * `LV_SYMBOL_ENVELOPE` for Signals/Inbox (the same glyph
     * scr_banner.c already uses for an incoming MESSAGE), `LV_SYMBOL_
     * GPS` for Map, `LV_SYMBOL_SETTINGS` for Settings — the standard
     * LVGL gear glyph.
     *
     * Captions are the renamed user-facing names (2026-09-01: "Signals"
     * -> "Inbox", "Now" -> "Lineup" — see the PR body for the full list
     * of renamed strings); "RADAR", "MAP" and "SETTINGS" are unchanged. */
    launcher_make_circle(puck, LV_SYMBOL_WIFI, "RADAR", -LAUNCHER_TOP_DX, LAUNCHER_TOP_DY,
                          LAUNCHER_TOP_CAPTION_DY, 0, false);
    launcher_make_circle(puck, LV_SYMBOL_LIST, "LINEUP", LAUNCHER_TOP_DX, LAUNCHER_TOP_DY, LAUNCHER_TOP_CAPTION_DY, 1,
                          false);
    launcher_make_circle(puck, LV_SYMBOL_ENVELOPE, "INBOX", -LAUNCHER_BOTTOM_SIDE_DX, LAUNCHER_BOTTOM_DY,
                          LAUNCHER_BOTTOM_CAPTION_DY, 2, signals_unread);
    launcher_make_circle(puck, LV_SYMBOL_GPS, "MAP", 0.0f, LAUNCHER_BOTTOM_DY, LAUNCHER_BOTTOM_CAPTION_DY, 3, false);
    launcher_make_circle(puck, LV_SYMBOL_SETTINGS, "SETTINGS", LAUNCHER_BOTTOM_SIDE_DX, LAUNCHER_BOTTOM_DY,
                          LAUNCHER_BOTTOM_CAPTION_DY, 4, false);
}
