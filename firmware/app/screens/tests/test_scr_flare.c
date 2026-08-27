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
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_layout.h"
#include "ff_theme.h"
#include "scr_flare.h"

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

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass);
    RUN_TEST(S10_ACn_lock_disclosure_only_truncates_names_that_dont_fit);
    RUN_TEST(S10_ACn_lock_disclosure_is_always_accompanied_by_the_headline);

    return UNITY_END();
}
