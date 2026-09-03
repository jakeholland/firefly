/**
 * test_scr_flare.c — headless LVGL geometry tests for the S10 flare UI
 * (issue #27's round-glass lock-disclosure chip sweep).
 *
 * S16 slice c2's `[api]` change dropped `ff_flare_t *rt` from
 * `ff_scr_flare_build_takeover`/`ff_scr_flare_build_sender_overlay`
 * (screens emit `FF_INTENT_TAKEOVER_GO`/`TAKEOVER_DISMISS`/`FLARE_END`
 * through the seam now, rather than mutating a live core struct handed
 * down through the build call) — the GO/DISMISS/CANCEL/FLARE
 * button-binding proofs this file used to carry moved to
 * test_scr_intent.c, alongside the seam's other emit-site tests (same
 * spy-sink pattern, same file, one canonical place for "which intent did
 * this button emit"). What remains here is the geometry this file's
 * header used to also cover: the lock-disclosure chip's round-glass
 * clamp (issue #27 / PR #41 UX review), which has nothing to do with the
 * intent seam and everything to do with `flare_make_chip`'s pixel math.
 *
 * fix/radar-lock-chip-clears-status-bar (PR #98 regression): added the
 * Radar-face lock chip's OWN geometry test, S10_lock_chip_clears_the_
 * status_bar — see that test's own comment for the bug it guards.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_layout.h"
#include "ff_radar.h"
#include "ff_theme.h"
#include "radar_layout.h"
#include "scr_flare.h"
#include "scr_radar.h"

/* Frozen tick — nothing in this file renders a frame, but lv_init still
 * wants a tick source. */
static uint32_t test_tick_cb(void)
{
    return 0;
}

void setUp(void)
{
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    /* No buffers/flush callback: this file never calls lv_refr_now() or
     * lv_timer_handler(), only lv_obj_create()-family widget building and
     * lv_obj_update_layout() — neither touches the flush path. */
    lv_display_t *disp = lv_display_create(FF_THEME_WINDOW_PX, FF_THEME_WINDOW_PX);
    lv_display_set_default(disp);
}

void tearDown(void)
{
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * Issue #27 — the lock-disclosure chip stays on the ROUND GLASS.
 *
 * The chip is 20px type on a CIRCLE: a chip that merely fits the 440px
 * bounding box can still hang off the visible display, which is exactly
 * how PR #25 shipped a back button 42px outside the round area. The
 * existing sweep (targets/sim/tests/test_face_hit_targets.c) can't catch
 * it — that one only examines CLICKABLE objects, and this chip is
 * deliberately an inert indicator.
 *
 * THIS TEST WAS PREVIOUSLY WRONG IN A WAY WORTH RECORDING (PR #41 code
 * review, blocking). It used "BARTHOLOMEWWWWW"/"MAXIMILIANOOOOO" and
 * claimed to cover "two maximum-length crew names" — true of LENGTH and
 * false of WIDTH. Montserrat is proportional, so the guard it was
 * protecting (an 11-BYTE name cap) bounded the wrong quantity entirely:
 * eleven `W`s render ~80px wider than eleven typical characters and put
 * the chip 25px past the bezel. The test passed, on a name that happened
 * to be narrow. A guard whose test passes for the wrong reason is worse
 * than no guard, because it is why the bug got signed off.
 *
 * So the sweep below is over WIDTH worst cases — the widest glyphs in
 * the font, at the longest length FF_APP_NAME_LEN permits — and includes
 * the reviewer's own off-glass repros. Crew names are Meshtastic
 * `User.long_name`: arbitrary UTF-8 chosen by someone else, arriving
 * over the radio, so these are reachable inputs and not hypotheticals.
 * ------------------------------------------------------------------- */

/* 15 characters each — the full FF_APP_NAME_LEN budget. The first four
 * are the reviewer's measured off-glass cases scaled to full length; the
 * last two are a realistic name and a narrow one, present so the sweep
 * also proves the clamp doesn't mangle names that fit. */
static char const *const k_lock_name_width_cases[] = {
    "WWWWWWWWWWWWWWW", /* widest glyph in the font, repeated */
    "MMMMMMMMMMMMMMM", /* reviewer's -3.64px case, at full length */
    "MWMWMWMWMWMWMWM", /* reviewer's -11.58px case, at full length */
    "WWW MMM WWW MMM", /* reviewer's +5.30px case, at full length */
    "BARTHOLOMEWWWWW", /* what this test used to check, and only this */
    "IIIIIIIIIIIIIII", /* narrowest — must survive uncut, see below */
};

static lv_obj_t *find_label_with_prefix(lv_obj_t *root, char const *prefix)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            char const *txt = lv_label_get_text(child);
            if (txt != NULL && strncmp(txt, prefix, strlen(prefix)) == 0) {
                return child;
            }
        }
        lv_obj_t *found = find_label_with_prefix(child, prefix);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass(void)
{
    float margin = (float)((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2);
    float cx = margin + (float)FF_THEME_PUCK_RADIUS_PX;
    float cy = margin + (float)FF_THEME_PUCK_RADIUS_PX;

    for (size_t i = 0; i < sizeof(k_lock_name_width_cases) / sizeof(k_lock_name_width_cases[0]); i++) {
        char const *locked = k_lock_name_width_cases[i];

        /* Fresh screen per case — this file's setUp/tearDown bracket the
         * whole test, so each iteration clears what the last one built. */
        lv_obj_clean(lv_screen_active());

        ff_app_flare_t disp;
        memset(&disp, 0, sizeof(disp));
        disp.takeover_active = true;
        /* snprintf, not strncpy: these names are exactly 15 characters,
         * i.e. exactly the strncpy bound, and GCC's
         * -Wstringop-truncation correctly points out no NUL gets copied
         * in that case. snprintf always terminates. */
        snprintf(disp.takeover_from_name, sizeof(disp.takeover_from_name), "%s", "MAXIMILIANOOOOO");
        disp.locked = true;
        snprintf(disp.locked_from_name, sizeof(disp.locked_from_name), "%s", locked);

        ff_scr_flare_build_takeover(&disp);

        /* Force layout without rendering a frame — this file has no draw
         * buffers (see setUp), and lv_obj_update_layout only runs the
         * size/position pass, never the draw pass. */
        lv_obj_update_layout(lv_screen_active());

        lv_obj_t *label = find_label_with_prefix(lv_screen_active(), "GO DROPS LOCK");
        TEST_ASSERT_NOT_NULL_MESSAGE(label, "the lock-disclosure chip must be built when GO would switch the lock");

        /* Assert on the PILL, not the text: the pill's padding is what
         * actually reaches furthest toward the bezel. */
        lv_obj_t *chip = lv_obj_get_parent(label);
        TEST_ASSERT_NOT_NULL(chip);

        lv_area_t area;
        lv_obj_get_coords(chip, &area);

        /* lv_area_t's x2/y2 are INCLUSIVE last-pixel coordinates;
         * ff_layout's are exclusive far edges — hence the +1 (same
         * conversion test_face_hit_targets.c documents). */
        ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};
        TEST_ASSERT_TRUE_MESSAGE(ff_layout_rect_in_circle(r, cx, cy, (float)FF_THEME_PUCK_RADIUS_PX),
                                  "the lock-disclosure chip must lie entirely within the round glass, "
                                  "for EVERY name width — not just the narrow one this test used to check");
    }
}

/* ---------------------------------------------------------------------
 * DO NOT CONSOLIDATE THIS WITH THE SWEEP ABOVE. They look redundant —
 * both build a takeover with an awkward name and look at the chip — and
 * they are not. Each catches a bug the other passes.
 *
 * Proven by mutation, not by argument (PR #41 code review round 2, which
 * found this by reverting the pin and watching which test moved):
 *   - Remove the label's one-line height pin in flare_make_chip, so
 *     LVGL's dots mode never fires and the text WRAPS to a second line
 *     instead: the sweep above still PASSES (a taller two-line chip
 *     happens to fit inside the circle) while this test FAILS (no
 *     ellipsis). The chip silently stops being the single-line height
 *     its slot is sized around, and only this test notices.
 *   - Remove the width clamp entirely: both fail.
 *
 * That first case is itself an instance of the failure this whole PR
 * kept hitting — the sweep uses NAMES as a proxy for "the chip fits its
 * slot", and a two-line chip satisfies the proxy while violating the
 * property.
 * ------------------------------------------------------------------- */

static void S10_ACn_lock_disclosure_only_truncates_names_that_dont_fit(void)
{
    /* The other half of the guarantee. Clamping in pixels is only an
     * improvement over the old byte cap if it ALSO stops cutting names
     * that fit: the byte cap truncated "IIIIIIIIIIIIIII" and
     * "WWWWWWWWWWWWWWW" identically, though one has room to spare.
     *
     * A narrow full-length name must render whole (no ellipsis), and a
     * name of the widest glyphs must be visibly cut. */
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    snprintf(disp.takeover_from_name, sizeof(disp.takeover_from_name), "%s", "KEV");
    disp.locked = true;
    snprintf(disp.locked_from_name, sizeof(disp.locked_from_name), "%s", "IIIIIIIIIIIIIII");

    ff_scr_flare_build_takeover(&disp);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *label = find_label_with_prefix(lv_screen_active(), "GO DROPS LOCK");
    TEST_ASSERT_NOT_NULL(label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("GO DROPS LOCK - IIIIIIIIIIIIIII", lv_label_get_text(label),
                                      "a full-length name of narrow glyphs fits and must not be cut");

    /* Now the wide one: same length, must be truncated by the renderer. */
    lv_obj_clean(lv_screen_active());
    snprintf(disp.locked_from_name, sizeof(disp.locked_from_name), "%s", "WWWWWWWWWWWWWWW");
    ff_scr_flare_build_takeover(&disp);
    lv_obj_update_layout(lv_screen_active());

    label = find_label_with_prefix(lv_screen_active(), "GO DROPS LOCK");
    TEST_ASSERT_NOT_NULL(label);
    /* LVGL rewrites the label's own text when LV_LABEL_LONG_MODE_DOTS
     * shortens it, so the ellipsis is observable here rather than only
     * in pixels. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(lv_label_get_text(label), "..."),
                                  "a name too wide for the glass must be visibly cut, not silently clipped");
}

/* ---------------------------------------------------------------------
 * The disclosure chip names only the lock it would cost you; the
 * incoming sender's name lives in the headline directly above it
 * (PR #41 UX review, BLOCKING 1 — "KEV is not the news, KEV is the whole
 * reason the screen woke up").
 *
 * That split is only honest while the headline is actually there. This
 * pins the pairing structurally, so a future change that drops or gates
 * the headline fails here instead of silently reducing the disclosure to
 * "you lose Dana" with no indication of what for.
 * ------------------------------------------------------------------- */

static void S10_ACn_lock_disclosure_is_always_accompanied_by_the_headline(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);
    disp.locked = true;
    strncpy(disp.locked_from_name, "DANA", sizeof(disp.locked_from_name) - 1);

    ff_scr_flare_build_takeover(&disp);

    lv_obj_t *chip_label = find_label_with_prefix(lv_screen_active(), "GO DROPS LOCK - DANA");
    TEST_ASSERT_NOT_NULL_MESSAGE(chip_label, "the disclosure chip must name the lock GO would cost");

    lv_obj_t *headline = find_label_with_prefix(lv_screen_active(), "KEV IS FLARING");
    TEST_ASSERT_NOT_NULL_MESSAGE(headline,
                                  "the incoming sender must be named on screen whenever the chip omits them");

    /* And the chip must NOT repeat the sender — that repetition is what
     * spent the chip's width budget on the largest thing already in
     * view. */
    TEST_ASSERT_NULL_MESSAGE(strstr(lv_label_get_text(chip_label), "KEV"),
                              "the chip must not repeat the sender already named in the headline");
}

/* ---------------------------------------------------------------------
 * fix/radar-lock-chip-clears-status-bar — the Radar face's own "LOCKED -
 * <name>" chip (ff_scr_flare_build_lock_chip) must sit BELOW the status
 * bar (clock/mesh/battery), never over it.
 *
 * REGRESSION THIS GUARDS. PR #98 (S15c, the 412-panel fit) moved
 * RADAR_LAYOUT_STATUS_BAR_DY from -195 to -160 but left the lock chip at
 * its own literal, -165 — unchanged, and now 5px ABOVE (more negative
 * than) the status bar instead of the ~30px below it the two constants'
 * original values put it. The golden (`radar_flare_locked.png`) was
 * regenerated at 412 with the overlap already in the reference image, so
 * the pixel-diff-against-itself golden check could never have caught it
 * — this is exactly the "assertion, not a golden" gap
 * app/screens/tests/test_radar_layout.c's own header comment already
 * explains for this face's other geometry.
 *
 * PROXY-PROOF, PER AGENTS.md'S "measuring, not reasoning harder": this
 * builds the REAL screen — `ff_scr_radar_build` then
 * `ff_scr_flare_build_lock_chip`, the same two calls in the same order
 * `scr_nav.c` makes for the live Radar face — and reads the actual
 * built-and-laid-out LVGL widget geometry (`lv_obj_get_coords`), not
 * `RADAR_LAYOUT_LOCK_CHIP_DY`'s own arithmetic. A test that re-derived
 * the same formula the constant is defined by would pass no matter what
 * the constant said, which is the proxy this file's own sibling tests
 * above were written to avoid falling into.
 *
 * MUTATION CHECK (run manually, see this PR's body for the transcript):
 * reverting RADAR_LAYOUT_LOCK_CHIP_DY to the old literal -165.0f makes
 * this test fail immediately — the chip's top edge lands ABOVE the
 * status bar's bottom edge, i.e. a negative gap, nowhere near the
 * required 8px clearance.
 */
static void S10_lock_chip_clears_the_status_bar(void)
{
    float margin = (float)((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2);
    float cx = margin + (float)FF_THEME_PUCK_RADIUS_PX;
    float cy = margin + (float)FF_THEME_PUCK_RADIUS_PX;

    /* Same content-container convention scr_nav.c uses: a PUCK_PX-square
     * parent, positioned so its own center lands on the round glass's
     * real center — both builders draw center-relative (LV_ALIGN_CENTER
     * + dy) against THIS object, exactly as they do against scr_nav.c's
     * `content`. */
    lv_obj_t *puck = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(puck, (int32_t)margin, (int32_t)margin);

    /* radar_flare_locked.json's own fixture values — the one Radar-face
     * fixture with flare.locked == true (grepped: no other Radar fixture
     * sets it), so this is the exact scenario the regression shipped in. */
    ff_radar_view_t radar;
    memset(&radar, 0, sizeof(radar));
    radar.mode = RADAR_LIVE;
    radar.arrow_deg = 42.0f;
    radar.arrow_valid = true;
    snprintf(radar.name, sizeof(radar.name), "%s", "DANA");
    snprintf(radar.dist_str, sizeof(radar.dist_str), "%s", "320 m");
    snprintf(radar.age_str, sizeof(radar.age_str), "%s", "8 SEC");
    radar.trend = 0;
    snprintf(radar.clock_str, sizeof(radar.clock_str), "%s", "9:41");
    radar.batt_pct = 78;
    radar.mesh_ok = true;
    radar.dots[0].ring_deg = 42.0f;
    radar.dots[0].initial = 'D';
    radar.dots[0].color_idx = 0;
    radar.n_dots = 1;

    ff_app_flare_t flare;
    memset(&flare, 0, sizeof(flare));
    flare.locked = true;
    snprintf(flare.locked_from_name, sizeof(flare.locked_from_name), "%s", "DANA");
    flare.locked_expires_in_ms = 60000;

    /* locked=true: matches `flare.locked = true` above — scr_nav.c always
     * passes the SAME boolean to both calls (see scr_radar.h's doc comment
     * on the `locked` parameter), so this test mirrors that invariant
     * rather than building an unrealistic combination. */
    ff_scr_radar_build(puck, &radar, /*colorblind=*/false, /*screen_flip=*/false, /*locked=*/true);
    ff_scr_flare_build_lock_chip(puck, &flare);
    lv_obj_update_layout(lv_screen_active());

    /* Status bar: the "MESH" label (mesh_ok == true) — same font/dy as
     * the clock and battery labels either side of it, so any one of the
     * three stands in for the row's real on-screen extent. */
    lv_obj_t *mesh_lbl = find_label_with_prefix(puck, "MESH");
    TEST_ASSERT_NOT_NULL_MESSAGE(mesh_lbl, "the status bar's MESH label must be built");
    lv_area_t status_area;
    lv_obj_get_coords(mesh_lbl, &status_area);

    lv_obj_t *chip_label = find_label_with_prefix(puck, "LOCKED - DANA");
    TEST_ASSERT_NOT_NULL_MESSAGE(chip_label, "the lock chip must be built when flare->locked");
    lv_obj_t *chip = lv_obj_get_parent(chip_label);
    TEST_ASSERT_NOT_NULL(chip);
    lv_area_t chip_area;
    lv_obj_get_coords(chip, &chip_area);

    /* lv_area_t's y2 is an INCLUSIVE last-pixel coordinate; +1 gives the
     * exclusive bottom edge — same conversion this file's sibling test
     * (and test_face_hit_targets.c) already documents. Kept as plain
     * `int` (not float/%.0f): on-screen pixel coordinates are small and
     * bounded, but GCC's -Wformat-truncation sizes a numeric conversion's
     * WORST CASE against the full width of the argument's TYPE (11 chars
     * incl. sign for a bare `int`, far more for `%f`/`double`), not this
     * variable's actual bound — CLAUDE.md: GCC is the CI authority, and
     * clang has no equivalent check to catch this locally, so the buffer
     * below is sized against that worst case (3 `int`s at 11 chars each)
     * rather than the short values this test actually produces. */
    int status_bottom = (int)status_area.y2 + 1;
    int chip_top = (int)chip_area.y1;
    int gap = chip_top - status_bottom;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "lock chip must clear the status bar by >= 8px (status bottom %d, chip top %d, gap %d) "
             "— this is exactly the PR #98 regression (chip stuck at a stale literal) if it fails",
             status_bottom, chip_top, gap);
    TEST_ASSERT_TRUE_MESSAGE(gap >= 8, msg);

    /* Both bands stay on the round glass at their own x-extents — the
     * chip's own FLARE_CHIP_GLASS_SAFETY_PX bound (scr_flare.c) plus the
     * status bar's pre-existing placement, both re-verified here rather
     * than assumed. */
    ff_layout_rect_t status_rect = {(float)status_area.x1, (float)status_area.y1, (float)status_area.x2 + 1.0f,
                                     (float)status_area.y2 + 1.0f};
    TEST_ASSERT_TRUE_MESSAGE(ff_layout_rect_in_circle(status_rect, cx, cy, (float)FF_THEME_PUCK_RADIUS_PX),
                              "the status bar's MESH label must lie entirely within the round glass");

    ff_layout_rect_t chip_rect = {(float)chip_area.x1, (float)chip_area.y1, (float)chip_area.x2 + 1.0f,
                                   (float)chip_area.y2 + 1.0f};
    TEST_ASSERT_TRUE_MESSAGE(ff_layout_rect_in_circle(chip_rect, cx, cy, (float)FF_THEME_PUCK_RADIUS_PX),
                              "the lock chip must lie entirely within the round glass");
}

/* ---------------------------------------------------------------------
 * Coordinator follow-up on this same fix: a due-north locked fixture
 * (arrow_deg 0, ring_deg 0 — `radar_flare_locked_north.json`) showed the
 * compass arrow's HEAD painted squarely under the lock chip. The
 * original PR's "the chip may paint over the arrow for a narrow bearing
 * cone" tradeoff was ruled unacceptable for the arrowhead specifically —
 * the arrow's direction is this product's whole point, unlike the status
 * bar it must never be COVERED, even for a narrow cone. See
 * `RADAR_LAYOUT_ARROW_REACH_LOCKED_PX`'s own derivation comment in
 * radar_layout.h.
 *
 * SAME "measure the real built thing" shape as
 * S10_lock_chip_clears_the_status_bar above, but this one measures the
 * arrow from radar_layout_resolve_arrow's own output rather than an LVGL
 * widget: that function IS the single source of truth scr_radar.c draws
 * from without any further placement math of its own (radar_layout.h's
 * top comment — "the thing under test is provably the same geometry
 * that ends up on screen"), so calling it directly with the SAME
 * registry/bearing/reach the real LIVE-mode, due-north, locked render
 * would use is not a re-derivation of the property under test — it is
 * the exact computation the render call site performs. The chip's own
 * band, by contrast, IS measured from the real built LVGL widget (same
 * as the sibling test above), so nothing here trusts
 * RADAR_LAYOUT_LOCK_CHIP_DY's arithmetic either.
 */
static void S10_locked_arrow_head_clears_the_lock_chip(void)
{
    float margin = (float)((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2);
    float center_offset = margin + (float)FF_THEME_PUCK_RADIUS_PX;

    /* The real chip, built for real (same as the sibling test): read its
     * ACTUAL bottom edge, converted to the same puck-center-relative
     * coordinate system radar_layout_resolve_arrow's output uses. */
    lv_obj_t *puck = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(puck, (int32_t)margin, (int32_t)margin);

    ff_app_flare_t flare;
    memset(&flare, 0, sizeof(flare));
    flare.locked = true;
    snprintf(flare.locked_from_name, sizeof(flare.locked_from_name), "%s", "DANA");
    flare.locked_expires_in_ms = 60000;

    ff_scr_flare_build_lock_chip(puck, &flare);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *chip_label = find_label_with_prefix(puck, "LOCKED - DANA");
    TEST_ASSERT_NOT_NULL_MESSAGE(chip_label, "the lock chip must be built when flare->locked");
    lv_obj_t *chip = lv_obj_get_parent(chip_label);
    TEST_ASSERT_NOT_NULL(chip);
    lv_area_t chip_area;
    lv_obj_get_coords(chip, &chip_area);
    float chip_bottom = ((float)chip_area.y2 + 1.0f) - center_offset;

    /* The arrow: radar_flare_locked_north.json's own values — LIVE mode,
     * due north (worst case, radar_layout.h's own bearing-0 proof),
     * locked. Same registry (RADAR_LIVE, not never_fixed) and locked
     * reach cap the real ff_scr_radar_build(..., locked=true) call for
     * this fixture uses. */
    radar_layout_registry_t reg;
    radar_layout_build_registry(RADAR_LIVE, /*never_fixed=*/false, &reg);
    radar_layout_arrow_t arrow;
    radar_layout_resolve_arrow(&reg, /*arrow_deg=*/0.0f, RADAR_LAYOUT_ARROW_REACH_LOCKED_PX, &arrow);

    /* The head's topmost (most-negative-y) pixel is whichever of the
     * tip/left-corner/right-corner reaches furthest — measured, not
     * assumed to be the tip (radar_layout.h's own derivation comment
     * proves the tip wins at bearing 0, but this test doesn't lean on
     * that proof either). */
    float head_top = arrow.tip_dy;
    if (arrow.left_dy < head_top) {
        head_top = arrow.left_dy;
    }
    if (arrow.right_dy < head_top) {
        head_top = arrow.right_dy;
    }

    /* Both `chip_bottom` and `head_top` are y-coordinates in the same
     * puck-center-relative system (negative = toward the top of the
     * glass). The head clears the chip when its topmost point sits
     * BELOW (numerically greater/less-negative than) the chip's own
     * bottom edge — i.e. `head_top > chip_bottom` — so the gap between
     * them is `head_top - chip_bottom`, not the other way around. */
    float gap = head_top - chip_bottom;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "locked arrow head must clear the lock chip's bottom edge by >= 8px "
             "(chip bottom %d, head top %d, gap %d) — the arrow's DIRECTION is the product; "
             "painting over its head is not an acceptable tradeoff even for a narrow bearing cone",
             (int)chip_bottom, (int)head_top, (int)gap);
    TEST_ASSERT_TRUE_MESSAGE(gap >= 8.0f, msg);
}

/* ---------------------------------------------------------------------
 * S10 Amendment (2026-09-03, "Wire honesty", fix/flare-wire-send) — the
 * sender overlay's status line reads off `flare->wire_state`: the
 * confident "you are flaring" copy only while SENT, the honest amber
 * "NO MESH" retry copy while WAITING. Screen-level guard for the P0 bug
 * fix (core/shell coverage lives in test_flare.c/test_shell.c) — this is
 * the proof the actual label text on the actual overlay changes, not
 * just the underlying enum.
 * ------------------------------------------------------------------- */

static void S10_wire_sender_overlay_sent_shows_flaring_copy(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.sending = true;
    disp.send_expires_in_ms = 245000;
    disp.wire_state = FF_FLARE_WIRE_SENT;

    ff_scr_flare_build_sender_overlay(lv_screen_active(), &disp);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *status = find_label_with_prefix(lv_screen_active(), "you are flaring");
    TEST_ASSERT_NOT_NULL_MESSAGE(status, "SENT must keep the original 'you are flaring' copy");
    TEST_ASSERT_NULL_MESSAGE(find_label_with_prefix(lv_screen_active(), "NO MESH"),
                              "SENT must not show the NO MESH retry copy");
    /* Review round 2 (2026-09-03): the COLOUR, not just the text — SENT
     * keeps the ordinary amber, never the WAITING alert shade. */
    TEST_ASSERT_TRUE_MESSAGE(
        lv_color_eq(lv_obj_get_style_text_color(status, LV_PART_MAIN), lv_color_hex(FF_THEME_COLOR_AMBER)),
        "SENT must render the status line in the ordinary amber, not the WAITING alert shade");
}

static void S10_wire_sender_overlay_waiting_shows_no_mesh_copy(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.sending = true;
    disp.send_expires_in_ms = 245000;
    disp.wire_state = FF_FLARE_WIRE_WAITING;

    ff_scr_flare_build_sender_overlay(lv_screen_active(), &disp);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *status = find_label_with_prefix(lv_screen_active(), "NO MESH");
    TEST_ASSERT_NOT_NULL_MESSAGE(status, "WAITING must show the honest NO MESH retry copy");
    /* Review round 2 (2026-09-03): the COLOUR, not just the text — WAITING
     * must NOT render in the ordinary "confident" amber the SENT copy
     * uses; it gets the same alert shade the Radar face's STALE
     * treatment already uses. */
    TEST_ASSERT_TRUE_MESSAGE(
        lv_color_eq(lv_obj_get_style_text_color(status, LV_PART_MAIN), lv_color_hex(FF_THEME_COLOR_STALE_AMBER)),
        "WAITING must render the status line in the alert (stale) amber, not the ordinary SENT amber");
    TEST_ASSERT_NULL_MESSAGE(
        find_label_with_prefix(lv_screen_active(), "you are flaring"),
        "WAITING must not claim 'you are flaring' while nothing has actually reached the mesh");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_wire_sender_overlay_sent_shows_flaring_copy);
    RUN_TEST(S10_wire_sender_overlay_waiting_shows_no_mesh_copy);

    RUN_TEST(S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass);
    RUN_TEST(S10_ACn_lock_disclosure_only_truncates_names_that_dont_fit);
    RUN_TEST(S10_ACn_lock_disclosure_is_always_accompanied_by_the_headline);
    RUN_TEST(S10_lock_chip_clears_the_status_bar);
    RUN_TEST(S10_locked_arrow_head_clears_the_lock_chip);

    return UNITY_END();
}
