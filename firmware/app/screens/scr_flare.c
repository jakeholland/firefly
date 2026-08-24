/**
 * scr_flare.c — see scr_flare.h.
 */
#include "scr_flare.h"

#include <math.h>
#include <stdio.h>

#include "ff_layout.h"
#include "ff_theme.h"
#include "flare_fmt.h"

/* ---------------------------------------------------------------------
 * Layout constants. Deliberately local to this file (not radar_layout.h)
 * — see scr_flare.h's ff_scr_flare_build_lock_chip doc comment for why
 * these never need to compete for space in that module's collision
 * registry.
 * ------------------------------------------------------------------- */

/* The Firefly flare mark: an 8-ray burst, unequal ray lengths, a long
 * north ray (TRADEMARKS.md: "the eight-ray burst logo with unequal rays
 * and a long north ray"). PR #20 UX review (BLOCKING): the previous pass
 * drew three plain concentric rings, which read as "scanning / loading /
 * alert" — the opposite of "a person needs you" — and this is the ONE
 * screen where the mark IS the content. Reconstructed here from
 * TRADEMARKS.md's description (no in-repo glyph exists yet — S12's
 * first-run flow, the mark's other consumer, hasn't landed); flagged as
 * an interpretation call per AGENTS.md.
 *
 * Kept well inside the puck's circular silhouette at this cy (the
 * longest ray's tip must stay within FF_THEME_PUCK_RADIUS_PX of center —
 * LVGL doesn't clip children to a parent's rounded/circular shape by
 * default, so a ray sized past that boundary would visibly poke outside
 * the puck's drawn edge). */
#define FLARE_MARK_CY (-128.0f)
#define FLARE_MARK_MAX_LEN 42.0f
#define FLARE_MARK_CENTER_R 7.0f
#define FLARE_MARK_N_RAYS 8
/* Fractions of FLARE_MARK_MAX_LEN, indexed clockwise from north (index 0
 * = straight up). North is the deliberate standout (spec: "a long north
 * ray"); the rest taper UNEVENLY rather than a repeating long/short
 * alternation, so the shape reads as "a burst with a direction," not a
 * generic sunburst/loading-spinner silhouette (the review's exact
 * complaint about the old rings). */
static const float FLARE_MARK_RAY_FRAC[FLARE_MARK_N_RAYS] = {
    1.00f, 0.52f, 0.62f, 0.46f, 0.58f, 0.46f, 0.62f, 0.52f,
};
/* One lv_line point-pair PER ray, not a single reused buffer — lv_line
 * keeps a POINTER to whatever array it's given (same hazard scr_radar.c's
 * top comment documents for its own line-point pool), so each of the 8
 * simultaneously-alive lv_line objects below needs its own slot. */
static lv_point_precise_t s_flare_mark_ray_pts[FLARE_MARK_N_RAYS][2];

/* Avoid the POSIX-only M_PI (undefined under strict -std=c11 on some
 * libcs) — same rationale as core/src/ff_geo.c's own FF_GEO_PI. */
#define FLARE_MARK_PI 3.14159265358979323846f

#define FLARE_TAKEOVER_HEADLINE_DY (-72.0f)
#define FLARE_TAKEOVER_BEARING_DY  (-34.0f)
/* Reserved slot for the lock-disclosure line (BLOCKING finding #3 —
 * "GO must disclose what it costs"), whether or not it's actually shown
 * for a given fixture — keeping GO/DISMISS at a FIXED position regardless
 * of `flare->locked` means the two-button gap (finding #2) never has to
 * be re-verified per-fixture.
 *
 * Deliberately UNCHANGED by issue #27, which grew this chip's type from
 * FF_THEME_FONT_CHIP (14px, line height 16) to FF_THEME_FONT_HEADLINE
 * (20px, line height 22). The chip is LV_SIZE_CONTENT-tall with 6px of
 * vertical padding, so it went from 28px to 34px — and because it is
 * CENTER-aligned at this dy, those 6px split evenly, 3px onto each edge,
 * rather than eating one neighbour's clearance. That leaves ~14px to the
 * bearing line's ink above (the 36px line BOX bottom is closer, but its
 * glyphs are not — caps are ~26px in a 40px box) and 15px to GO's top
 * edge below, both verified against an actual headless render rather
 * than this arithmetic alone. */
#define FLARE_TAKEOVER_LOCK_LINE_DY 10.0f
#define FLARE_TAKEOVER_GO_DY       70.0f
#define FLARE_TAKEOVER_DISMISS_DY  140.0f
#define FLARE_TAKEOVER_BTN_W       190
/* >= FF_THEME_MIN_HIT_PX (44) with real margin — docs/review/ux-raver.md
 * checklist item 2, "fat thumb test": this screen shows up at 2 AM with
 * one thumb and possibly gloves, so both buttons get MORE than the bare
 * floor, not exactly it. GO bottom edge (70 + 56/2 = 98) to DISMISS top
 * edge (140 - 50/2 = 115) leaves a 17px gap — PR #20 UX review
 * (BLOCKING): the previous pass left only ~9px between two buttons with
 * opposite, high-stakes outcomes; the review asked for >= 16px (~1.5mm),
 * so this clears it with a whole pixel of margin, not exactly at the
 * floor. */
#define FLARE_TAKEOVER_GO_BTN_H       56
#define FLARE_TAKEOVER_DISMISS_BTN_H  50

/* Kept clear of both NOSEL's "Pair a friend in Settings" sub-line
 * (RADAR_LAYOUT_NOSEL_SUB_DY == 40) above and the puck's own bottom edge
 * (FF_THEME_PUCK_RADIUS_PX == 220) below — verified against an actual
 * headless render, not just arithmetic, per this repo's screenshot-review
 * habit (see radar_layout.h's whole reason for existing). */
#define FLARE_SENDER_STATUS_DY    95.0f
#define FLARE_SENDER_COUNTDOWN_DY 135.0f
#define FLARE_SENDER_CANCEL_DY    178.0f
#define FLARE_SENDER_CANCEL_W     140
#define FLARE_SENDER_CANCEL_H     48

/* Chip padding, named because the round-glass clamp in flare_make_chip
 * has to subtract it from the available width to get the label's budget.
 * A literal here and a different literal there is exactly how a bound
 * drifts away from the thing it is bounding. */
#define FLARE_CHIP_PAD_X 14
#define FLARE_CHIP_PAD_Y 6

/* Slack left between a chip's corners and the bezel. Non-zero on
 * purpose: the chord bound is a float square root, so a zero-safety
 * element sits exactly on the knife edge of the in-circle test (see
 * test_ff_layout.c's centered_band_round_trips_through_rect_in_circle),
 * and on real glass there is a bezel, not a mathematical boundary. */
#define FLARE_CHIP_GLASS_SAFETY_PX 8.0f

#define FLARE_LOCK_CHIP_DY (-165.0f) /* clear of RADAR_LAYOUT_STATUS_BAR_DY (-195) and every mode's top content */

/* ---------------------------------------------------------------------
 * Small shared builders (deliberately NOT shared with scr_radar.c's
 * near-identical private helpers — this file's own small, self-contained
 * copies, same "duplicated rather than cross-coupling two render files
 * over a couple dozen lines" tradeoff fixture_view.c's header comment
 * already documents for this codebase).
 * ------------------------------------------------------------------- */

/* `font` is a parameter rather than a hardcoded FF_THEME_FONT_CHIP
 * (issue #27): the lock-disclosure chip is the one chip on this screen
 * carrying a decision's cost rather than a status readout, and it earns a
 * bigger step of the type scale than the countdown/lock chips do. */
/* `font` is a parameter rather than a hardcoded FF_THEME_FONT_CHIP
 * (issue #27): the lock-disclosure chip is the one chip on this screen
 * carrying a decision's cost rather than a status readout, and it earns a
 * bigger step of the type scale than the countdown/lock chips do.
 *
 * ROUND-GLASS SIZING (PR #41 code review, blocking). Every chip built
 * here is clamped to the width actually available on the circular
 * display at its own vertical offset, and truncated in PIXELS if its
 * text doesn't fit.
 *
 * The previous attempt bounded the disclosure chip's content by a BYTE
 * count (an 11-character name cap) and called that a round-glass guard.
 * It isn't one: Montserrat is proportional, so eleven bytes is anywhere
 * from ~310px of `I`s to ~487px of `W`s, and the reviewer's sweep put
 * eleven `W`s 25px past the bezel — the PR #25 class of bug, reachable
 * from untrusted input, since crew names arrive as Meshtastic
 * `User.long_name` off the radio. Worse, the test asserted the bug could
 * not happen: its "maximum-length crew names" were a LENGTH worst case,
 * not a WIDTH one, so the one guard that existed passed for the wrong
 * reason.
 *
 * So the bound is now taken in the units the constraint is expressed in.
 * ff_layout_centered_band_max_width answers "how wide may an element of
 * this height, centered at this dy, be inside the glass" (the primitive
 * ff_layout.h says exists so a layout is "sized to fit the glass by
 * construction, rather than built rectangle-first and only checked after
 * the fact"), and LVGL's own LV_LABEL_LONG_MODE_DOTS places the ellipsis
 * at the correct pixel. No character count can be wrong, because no
 * character count is consulted.
 *
 * The chip is measured before it is aligned: LV_SIZE_CONTENT needs a
 * layout pass to have a height, and the height is what decides which
 * edge of the band binds. */
static lv_obj_t *flare_make_chip(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex,
                                  lv_font_t const *font, int32_t dy)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(chip, FLARE_CHIP_PAD_X, 0);
    lv_obj_set_style_pad_right(chip, FLARE_CHIP_PAD_X, 0);
    lv_obj_set_style_pad_top(chip, FLARE_CHIP_PAD_Y, 0);
    lv_obj_set_style_pad_bottom(chip, FLARE_CHIP_PAD_Y, 0);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_height(chip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    /* Force the content pass so the chip has a real height, then clamp
     * the LABEL (not the chip: the chip is content-sized, so clamping the
     * label is what makes the pill shrink with it, padding intact). */
    lv_obj_update_layout(chip);
    float chip_h = (float)lv_obj_get_height(chip);
    float max_chip_w = ff_layout_centered_band_max_width((float)dy, chip_h, (float)FF_THEME_PUCK_RADIUS_PX,
                                                          FLARE_CHIP_GLASS_SAFETY_PX);
    int32_t max_label_w = (int32_t)max_chip_w - (FLARE_CHIP_PAD_X * 2);
    if (max_label_w > 0 && lv_obj_get_width(label) > max_label_w) {
        /* Both dimensions, in this order. LVGL's DOTS mode triggers on
         * VERTICAL overflow (lv_label.c: `size.y > lv_area_get_height(
         * &txt_coords)`), so a width-only clamp makes the text WRAP to a
         * second line and the chip grow taller instead of gaining an
         * ellipsis — which would quietly break the single-line height
         * this chip's slot is sized around. Pinning the height to one
         * line is what turns the overflow into dots. */
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(label, max_label_w, lv_font_get_line_height(font));
    }

    lv_obj_align(chip, LV_ALIGN_CENTER, 0, dy);

    return chip;
}

static void flare_anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* The Firefly flare mark itself — 8 rays at 45-degree intervals (index 0
 * = north/straight up), unequal lengths, north deliberately the longest —
 * plus a filled center dot. Same "explicit full-puck-size object pinned
 * at (0,0), points offset by the puck's half-size" positioning convention
 * scr_radar.c's radar_draw_segment documents (lv_line draws each point at
 * object_top_left + point, no auto-centering of arbitrary/negative
 * points). `cy` is the mark's center, puck-center-relative. */
static void flare_build_mark(lv_obj_t *parent, float cy)
{
    const int32_t half = FF_THEME_PUCK_PX / 2;

    for (int i = 0; i < FLARE_MARK_N_RAYS; i++) {
        float angle_deg = (float)i * (360.0f / (float)FLARE_MARK_N_RAYS);
        float rad = angle_deg * FLARE_MARK_PI / 180.0f;
        float len = FLARE_MARK_MAX_LEN * FLARE_MARK_RAY_FRAC[i];
        /* North (i==0) is straight up: screen +Y is down, so "up" is -Y. */
        float dx = sinf(rad) * len;
        float dy = -cosf(rad) * len;

        lv_point_precise_t *pts = s_flare_mark_ray_pts[i];
        pts[0].x = half;
        pts[0].y = half + (int32_t)cy;
        pts[1].x = half + (int32_t)dx;
        pts[1].y = half + (int32_t)cy + (int32_t)dy;

        lv_obj_t *line = lv_line_create(parent);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(line, pts, 2);
        lv_obj_set_style_line_width(line, 5, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
    }

    /* Center dot, built last so it sits on top of the 8 ray origins. */
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, (int32_t)(FLARE_MARK_CENTER_R * 2), (int32_t)(FLARE_MARK_CENTER_R * 2));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, (int32_t)cy);
}

/* A visually solid, distinctly-shaped pill button (never text-only — see
 * scr_flare.h's ff_scr_flare_build_takeover doc comment for why). `filled`
 * true: solid `bg_hex` fill (GO). `filled` false: outlined pill over the
 * surface color (DISMISS/CANCEL) — still a filled, bordered shape, not
 * bare text on the background. */
static lv_obj_t *flare_make_button(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, bool filled,
                                    int32_t w, int32_t h, int32_t dy, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(bg_hex), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (int32_t)dy);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return btn;
}

/* ---------------------------------------------------------------------
 * Button callbacks — each forwards to exactly one core entry point, no
 * branching (see this file's header comment).
 *
 * The `printf` in each is diagnostic-only (stdout, not a return value or
 * a rendered pixel) — PR #20 code review, MEDIUM finding: this window
 * has no live redraw (issue #17), so a click that mutates the real
 * `ff_flare_t` produces NO visible on-screen change at all, which reads
 * to a user driving `ffsim` interactively as "the button did nothing."
 * This is the minimum honest feedback that a press was actually received
 * and forwarded correctly, without pretending to solve the redraw gap
 * itself (targets/sim/main.c's window-mode load path prints the fuller
 * one-time explanation for takeover screens specifically, since those
 * have no other way to be dismissed). Harmless during the headless
 * golden path (no click ever fires there) and during
 * screens/tests/test_scr_flare.c's interaction tests (stdout noise, not
 * a test failure) — this module owns no other I/O.
 * ------------------------------------------------------------------- */

static void flare_go_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_go(rt);
        printf("ffsim: GO pressed -> ff_flare_go() (locked_node_id now %u)\n", (unsigned)rt->locked_node_id);
    }
}

static void flare_dismiss_takeover_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_dismiss_takeover(rt);
        printf("ffsim: DISMISS pressed -> ff_flare_dismiss_takeover() (takeover_active now %s)\n",
               rt->takeover_active ? "true" : "false");
    }
}

static void flare_cancel_send_cb(lv_event_t *e)
{
    ff_flare_t *rt = (ff_flare_t *)lv_event_get_user_data(e);
    if (rt != NULL) {
        (void)ff_flare_send_cancel(rt);
        printf("ffsim: CANCEL pressed -> ff_flare_send_cancel() (sending now %s)\n", rt->sending ? "true" : "false");
    }
}

/* ---------------------------------------------------------------------
 * Entry points.
 * ------------------------------------------------------------------- */

void ff_scr_flare_build_takeover(ff_app_flare_t const *flare, ff_flare_t *rt)
{
    if (flare == NULL || !flare->takeover_active) {
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

    flare_build_mark(puck, FLARE_MARK_CY);

    char headline[40];
    ff_flare_fmt_headline(headline, sizeof(headline), flare->takeover_from_name);
    lv_obj_t *headline_lbl = lv_label_create(puck);
    lv_label_set_text(headline_lbl, headline);
    lv_obj_set_style_text_font(headline_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(headline_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(headline_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_TAKEOVER_HEADLINE_DY);

    /* Bearing/distance read: "NE - 320 m" (their compass bearing + honest
     * distance — dist_str is already "" when unknown, same convention as
     * ff_radar_view_t.dist_str, never fabricated here). A plain hyphen,
     * not U+00B7 MIDDLE DOT — same substitution scr_radar.c's
     * radar_render_nofix already documents (LVGL's built-in Montserrat
     * bitmap fonts don't cover that codepoint; it renders as tofu).
     *
     * PR #20 code review (LOW finding): `takeover_bearing_deg` has no
     * honest way to represent "unknown" on its own (0.0 is
     * indistinguishable from "genuinely due north") — gated on the
     * companion `takeover_bearing_valid` flag (ff_app_state.h's doc
     * comment), same "prove you meant this" pattern
     * `ff_radar_view_t.arrow_valid` already uses on the sibling Radar
     * face. An invalid bearing renders "bearing unknown" rather than
     * calling ff_flare_fmt_compass8 at all — CLAUDE.md: never fake a
     * position. */
    char bearing_line[40];
    char const *dist = (flare->takeover_dist_str[0] != '\0') ? flare->takeover_dist_str : "-- m";
    if (flare->takeover_bearing_valid) {
        snprintf(bearing_line, sizeof(bearing_line), "%s - %s", ff_flare_fmt_compass8(flare->takeover_bearing_deg),
                  dist);
    } else {
        snprintf(bearing_line, sizeof(bearing_line), "bearing unknown - %s", dist);
    }
    lv_obj_t *bearing_lbl = lv_label_create(puck);
    lv_label_set_text(bearing_lbl, bearing_line);
    lv_obj_set_style_text_font(bearing_lbl, FF_THEME_FONT_DISTANCE, 0);
    lv_obj_set_style_text_color(bearing_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(bearing_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_TAKEOVER_BEARING_DY);

    /* PR #20 UX review (finding #5, "cut the explain line"): the old
     * "they lit their puck so you can spot them..." line sat at/below the
     * legibility floor (14px raster, dim, near-black) carrying nothing
     * the headline + bearing + buttons don't already say. Removed
     * outright rather than fixed — the freed vertical room is exactly
     * what finding #2 (button spacing) needed. */

    /* PR #20 UX review (finding #3, BLOCKING — "GO must disclose what it
     * costs"): if a DIFFERENT node is already locked, pressing GO
     * silently drops it (ff_flare_go() REPLACES any existing lock — see
     * ff_flare.h's doc comment on that function). That finding is the
     * source of this chip; S10's Amendment Ruling 2 is the source of the
     * *reason it matters* ("the currently-locked node must always be a
     * fact the user chose"), but states nothing about a chip or a
     * disclosure — PR #41's code review caught this file conflating the
     * two, and the citation is split correctly here now.
     *
     * Shown as a solid amber chip (the same treatment
     * ff_scr_flare_build_lock_chip uses on the Radar face, which an
     * earlier review called "clean, immediate, correct") in a FIXED slot
     * so GO/DISMISS never move based on whether it's shown (keeps the
     * button-gap math in one place). Only shown when the lock would
     * actually change (same sender re-flaring while already locked on
     * them costs nothing to confirm again).
     *
     * ISSUE #27 / PR #41 UX review BLOCKING 1 — the wording. The original
     * was a 36-character sentence ("LOCKED ON DANA - GO SWITCHES TO KEV")
     * at 14px: correct, but a read rather than a glance on the one screen
     * that interrupts the user mid-panic. This PR's first attempt at
     * shortening it ("GO: DANA > KEV") was faster to SEE and slower to
     * UNDERSTAND — it deleted the sentence's verb, and the verb was the
     * disclosure; "A > B" reads as "via"/"then" everywhere else a person
     * meets it, so it parsed as an itinerary, i.e. as KEEPING the lock.
     * It also dropped the word LOCK, severing the only vocabulary link to
     * the Radar face's own "LOCKED - DANA" chip.
     *
     * Now "GO DROPS LOCK - DANA": a verb of loss, the noun the user
     * already knows, and the same " - <name>" tail the Radar chip uses.
     * See ff_flare_fmt_lock_cost's doc comment for why the incoming
     * sender's name is deliberately not repeated here (it is the 22px
     * headline directly above, built unconditionally by this same
     * function).
     *
     * FF_THEME_FONT_HEADLINE (20px), not FF_THEME_FONT_NAME (22px): the
     * GO button's own label is 22px, and this chip is an amber pill with
     * dark text sitting 15px above an amber pill with dark text. Matching
     * GO's type as well would push an INERT indicator (the chip is not
     * clickable) further toward looking like a second button — a mis-tap
     * invitation, docs/review/ux-raver.md checklist item 2. One step down
     * the scale, a 34px height against GO's 56px, and a width the longer
     * wording now pushes clear of GO's 190px keep the two ranked. The
     * full-pill radius is kept DELIBERATELY rather than squared off: it
     * is what makes this read as the same component as the Radar face's
     * lock chip, which is the connection BLOCKING 1 asked for. */
    if (flare->locked && ff_flare_fmt_go_switches_lock(flare->locked_from_name, flare->takeover_from_name)) {
        char lock_line[48];
        ff_flare_fmt_lock_cost(lock_line, sizeof(lock_line), flare->locked_from_name);
        flare_make_chip(puck, lock_line, FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, FF_THEME_FONT_HEADLINE,
                         (int32_t)FLARE_TAKEOVER_LOCK_LINE_DY);
    }

    /* GO: solid amber fill — the primary, unmistakably-pressable action. */
    flare_make_button(puck, "GO", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, true, FLARE_TAKEOVER_BTN_W,
                       FLARE_TAKEOVER_GO_BTN_H, (int32_t)FLARE_TAKEOVER_GO_DY, flare_go_cb, rt);

    /* DISMISS: a distinct, visually solid bordered pill (surface fill +
     * amber border), NOT plain text on the background — the previous UX
     * review's exact finding on this screen's earlier pass. */
    flare_make_button(puck, "DISMISS", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_INK, false, FLARE_TAKEOVER_BTN_W,
                       FLARE_TAKEOVER_DISMISS_BTN_H, (int32_t)FLARE_TAKEOVER_DISMISS_DY, flare_dismiss_takeover_cb,
                       rt);
}

void ff_scr_flare_build_sender_overlay(lv_obj_t *parent, ff_app_flare_t const *flare, ff_flare_t *rt)
{
    if (parent == NULL || flare == NULL || !flare->sending) {
        return;
    }

    /* Pulsing amber rim tint hugging the puck's own edge — same visual
     * language as scr_radar.c's STALE rim tint (amber ring around the
     * whole puck), but animated (STALE's is a static 50%-opacity ring;
     * this one pulses, per spec "own screen pulses amber") so a glance at
     * ANY face while sending reads unmistakably as "I am the one
     * flaring", not "someone/something near me is stale". */
    lv_obj_t *rim = lv_obj_create(parent);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, FF_THEME_PUCK_PX - 4, FF_THEME_PUCK_PX - 4);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rim, 5, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(rim, LV_OPA_70, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(rim);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rim);
    lv_anim_set_exec_cb(&a, flare_anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_70, LV_OPA_20);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_reverse_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *status_lbl = lv_label_create(parent);
    lv_label_set_text(status_lbl, "you are flaring - crew arrows locked on you");
    lv_obj_set_style_text_font(status_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_width(status_lbl, 280);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_lbl, LV_ALIGN_CENTER, 0, (int32_t)FLARE_SENDER_STATUS_DY);

    char countdown[16];
    ff_flare_fmt_countdown(countdown, sizeof(countdown), flare->send_expires_in_ms);
    char countdown_line[32];
    snprintf(countdown_line, sizeof(countdown_line), "ends in %s", countdown);
    flare_make_chip(parent, countdown_line, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED, FF_THEME_FONT_CHIP,
                     (int32_t)FLARE_SENDER_COUNTDOWN_DY);

    /* CANCEL button, on the puck edge below the status line — >=
     * FF_THEME_MIN_HIT_PX and visually distinct (outlined pill), matching
     * the takeover screen's DISMISS treatment. */
    flare_make_button(parent, "CANCEL", FF_THEME_COLOR_AMBER, FF_THEME_COLOR_AMBER, false, FLARE_SENDER_CANCEL_W,
                       FLARE_SENDER_CANCEL_H, (int32_t)FLARE_SENDER_CANCEL_DY, flare_cancel_send_cb, rt);
}

void ff_scr_flare_build_lock_chip(lv_obj_t *parent, ff_app_flare_t const *flare)
{
    if (parent == NULL || flare == NULL || !flare->locked) {
        return;
    }

    char text[40];
    char const *name = (flare->locked_from_name[0] != '\0') ? flare->locked_from_name : "?";
    snprintf(text, sizeof(text), "LOCKED - %s", name); /* plain hyphen, not U+00B7 — see build_takeover's note */

    flare_make_chip(parent, text, FF_THEME_COLOR_AMBER, FF_THEME_COLOR_BG, FF_THEME_FONT_CHIP,
                     (int32_t)FLARE_LOCK_CHIP_DY);
}

bool ff_scr_flare_selection_locked(ff_flare_t const *rt)
{
    if (rt == NULL) {
        return false;
    }
    return ff_flare_locked_node(rt) != 0;
}
