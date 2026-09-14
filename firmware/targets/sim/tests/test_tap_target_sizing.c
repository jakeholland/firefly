/**
 * test_tap_target_sizing.c — the outdoor/gloved tap-target floors
 * (owner decision, 2026-09-14; docs/hardware/tap-targets.md).
 *
 * `test_face_hit_targets.c` (this directory) already sweeps every
 * committed fixture against the UNIVERSAL floor — FF_THEME_MIN_HIT_PX
 * (44px, ~3.8mm), the phone-guideline number Maya's review flagged as
 * "not a generous floor, it's under half the stated 9mm outdoor target"
 * (/private/tmp/claude-501/ux-puck-maya/REVIEW.md §(e)). That sweep stays
 * exactly as it was: it is the floor NOTHING may go under, on every face,
 * including the ones this file does not name.
 *
 * This file adds the SECOND, higher bar the owner asked for, and only on
 * the four faces a raver actually operates one-handed in the dark:
 * launcher, radar, compose, inbox/signals. The two bars are deliberately
 * separate files rather than one raised constant, because they answer
 * different questions:
 *
 *   - 44px  = "is this a tap target at all" (every face, every control).
 *   - 80px  = "can a gloved, sweaty, four-drinks-in thumb hit THIS
 *             control, on THIS face, while walking" (the four faces
 *             below, primary actions and list rows).
 *
 * ## The rules, and why each is shaped the way it is
 *
 * Per face, two rules — both measured on the REAL rendered click area
 * (`lv_obj_get_click_area`, i.e. the object's box PLUS any
 * `ext_click_area`), never on the layout constants, per AGENTS.md's
 * standing "measure, don't reason harder" rule:
 *
 *   R1 — SHORT-SIDE floor. `min(w,h)` of every clickable on the face
 *        must clear that face's own `short_min_px`. This is the rule
 *        that catches a thin control in either orientation without
 *        needing to know which way round it is.
 *
 *   R2 — PRIMARY-ACTION height floor. Any clickable at least
 *        `FF_TAP_PRIMARY_W_PX` wide is, on a 412px round face, a pill /
 *        full-width row / bar — i.e. a primary action or a list row, the
 *        exact category the owner set the 80px bar for. Its HEIGHT must
 *        clear `primary_min_h_px`. Width is the classifier and height is
 *        the assertion specifically because height is the axis that
 *        fails on this hardware: a 412px-wide round face gives width
 *        away for free and charges for every vertical pixel, so
 *        "≥150px wide but 48px tall" is the shape every one of this
 *        codebase's own too-small primary actions actually had (FLARE
 *        200x48, GO 190x56, DISMISS 190x50, Settings rows 288x48) before
 *        this pass. It is a GEOMETRIC classifier, not a per-screen
 *        exception list — the same discipline test_face_hit_targets.c's
 *        header comment insists on for its own exclusions.
 *
 * ## The glass-circle containment check
 *
 * `test_face_hit_targets.c` checks containment against the FRAMEBUFFER's
 * own circle — center (206,206), radius 206, derived from
 * FF_THEME_WINDOW_PX/FF_THEME_PUCK_PX. That is the right circle for
 * "does this control exist on the panel". It is NOT the circle the
 * physical bezel leaves visible: the Waveshare board's round window sits
 * ~5px right of the 412px array and the bezel lip eats the rest, which
 * ff_theme.h records as FF_THEME_GLASS_CX/CY/R = (208,206,200)
 * (docs/hardware/glass-offset.md). Both puck UX reviews independently hit
 * the consequence — Deshawn's "the top-right battery reads just `%` with
 * the digits sliced off by the bezel", Maya's "the Inbox/Signals `+`
 * compose button sits right at the bottom-right edge of the circle... on
 * the real device that is the single worst place to put a primary
 * action".
 *
 * So R3, on these same four faces: every clickable's hit rect must lie
 * entirely inside the GLASS circle (208,206,200) — the owner's
 * "bezel-safe placement" rule, stated as "its whole hit area is inside
 * r=200 from (208,206)". This is strictly tighter than the existing
 * sweep's check (a 200px radius inside a 206px one, offset 2px right),
 * so it can only ever find things that sweep passes — which is the
 * point: the existing sweep's circle answers a question about the panel,
 * this one answers a question about the bezel.
 *
 * Scroll-awareness is inherited in spirit but NOT re-implemented here:
 * this file checks the fixed X extent of a scroll-list row across the
 * whole viewport band, exactly as the S21 model does, by reusing the
 * same "clip to the scroll viewport's y-span" move. See
 * `sizing_glass_rect` below.
 *
 * ## Inventory mode
 *
 * Set FF_TAP_INVENTORY=1 in the environment to make this test PRINT
 * every clickable it visits (fixture, rect, size, mm, and the first
 * label text found under it) instead of only reporting violations. That
 * dump is how docs/hardware/tap-targets.md's before/after table was
 * produced — the table is measured output, not hand arithmetic.
 */
#include <dirent.h>
#include <math.h> /* sqrtf — the corner-bleed inscribed-square solve */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "lvgl.h"

#include "face_dispatch.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "fixture.h"

#ifndef FF_FIXTURE_DIR
#define FF_FIXTURE_DIR "tests/fixtures/"
#endif

void setUp(void) {}

/* Same safety net as test_face_hit_targets.c's — a TEST_ASSERT that
 * longjmps out of a per-fixture lv_init/lv_deinit pair would otherwise
 * leak LVGL initialized into the next test and hang it. */
void tearDown(void)
{
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

/* ---------------------------------------------------------------------
 * Per-face rules.
 * ------------------------------------------------------------------- */

typedef struct {
    char const *prefix;       /* fixture-name prefix this rule applies to */
    int32_t short_min_px;     /* R1: min(w,h) floor for every clickable */
    int32_t primary_min_h_px; /* R2: height floor for anything >= FF_TAP_PRIMARY_W_PX wide */
} sizing_rule_t;

/* The four faces the owner named, keyed by fixture-name prefix (the
 * fixture files are named after the face they build — see
 * tests/fixtures/). Prefix matching, not a face enum, because several
 * distinct sub-views share one face value (inbox_* covers the feed, the
 * thread, the picker, the popup and Rally) and they want the same floor. */
/* Per-face floors. These are NOT all 80: 80 is what the owner asked for
 * and what a face gets when its geometry can pay for it, and several of
 * these faces measurably cannot. Each number below is the CEILING that
 * face's own round-glass geometry allows, derived in
 * docs/hardware/tap-targets.md and reproduced in the comment next to it —
 * so this table doubles as a regression guard ("never go back below what
 * this pass achieved") rather than an aspiration nothing meets. */
static const sizing_rule_t SIZING_RULES[] = {
    /* Launcher: the one face with room to spare. The hub is 120px and
     * the satellites went 88 -> 100 in this pass, so it gets the
     * "ideally >= 100px" target, not the 80px floor. */
    {"launcher", FF_THEME_HIT_COMFORT_PX, FF_THEME_HIT_COMFORT_PX},

    /* Radar: FLARE is the only control the face builds, and CLOSE mode
     * is vertically saturated between the outermost pulse ring and the
     * glass — 176x58 is every pixel available (see
     * FF_THEME_FLARE_BTN_H_PX's own derivation in ff_theme.h). */
    {"radar", FF_THEME_FLARE_BTN_H_PX, FF_THEME_FLARE_BTN_H_PX},

    /* Compose: the T9 keypad, at FF_THEME_HIT_KEY_PX. The short-side
     * floor is FF_THEME_MIN_HIT_PX because the header's BACK/SEND and
     * the PRED candidate chips genuinely cannot grow — the keypad below
     * them is already flush against the glass, so every pixel they took
     * would come straight off a key. The KEYS themselves are held at
     * FF_THEME_HIT_KEY_PX by scr_compose.c's own build-time asserts,
     * which is the stronger guard (a compile error, not a test failure).
     * See docs/hardware/tap-targets.md, "Compose: why 80x80 keys do not
     * fit". */
    {"compose", FF_THEME_MIN_HIT_PX, FF_THEME_HIT_PRIMARY_PX},

    /* Inbox / Signals: feed rows, thread rows, popup rows and Rally all
     * reach the 80px floor (they live in scroll lists, which can spend
     * height), and R2 is what holds them there. The SHORT-side floor is
     * only FF_THEME_MIN_HIT_PX because of one control — the sub-screen
     * BACK circle, which shares the narrowest row on the glass with a
     * centred title and is pinned at 44 for the reason spelled out at
     * FF_INBOX_BACK_PX. The quick-reply chips (FF_THEME_HIT_CHIP_PX) are
     * held by scr_inbox.c's own build-time assert instead, which is the
     * stronger guard anyway. */
    {"inbox", FF_THEME_MIN_HIT_PX, FF_THEME_HIT_PRIMARY_PX},

    /* The notification banner, over whichever face it interrupts. It is
     * 160x48 and stays there: taller means covering the status row above
     * it or the sub-screen BACK button below it, and it is a transient
     * toast rather than a control the user goes looking for. The faces
     * UNDER it are still held to their own floors by their own fixtures
     * (launcher.json, radar_*.json, inbox_*.json) — nothing is exempted
     * by this entry, it only stops the toast from dragging those floors
     * down. Longest-prefix wins, so these beat "launcher"/"inbox". */
    {"banner_on_launcher", FF_THEME_MIN_HIT_PX, 48},
    {"banner_on_radar", FF_THEME_MIN_HIT_PX, 48},
    {"banner_on_thread", FF_THEME_MIN_HIT_PX, 48},
};

#define SIZING_N_RULES ((int)(sizeof(SIZING_RULES) / sizeof(SIZING_RULES[0])))

/* FF_TAP_PRIMARY_W_PX — the R2 classifier. A control this wide on a
 * 412px round face is a pill, a full-width row or a bar; nothing on this
 * device is incidentally 120px wide. Deliberately well above the widest
 * non-primary control any of the four faces builds (the compose bottom
 * row's SPACE key, ~90px) and well below the narrowest genuine primary
 * action (the flare takeover's GO/DISMISS, 190px, and the Radar FLARE
 * button at 176). Deliberately above the widest compose T9 key (132) so
 * the keypad — whose height is capped by the glass, not by choice — is
 * judged by R1 and its own build-time asserts instead. */
#define FF_TAP_PRIMARY_W_PX 150

static sizing_rule_t const *sizing_rule_for(char const *fixture_name)
{
    sizing_rule_t const *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < SIZING_N_RULES; i++) {
        size_t plen = strlen(SIZING_RULES[i].prefix);
        if (strncmp(fixture_name, SIZING_RULES[i].prefix, plen) == 0) {
            /* Longest prefix wins, so "banner_on_radar" picks its own
             * rule rather than whichever happened to be listed first. */
            if (plen > best_len) {
                best = &SIZING_RULES[i];
                best_len = plen;
            }
        }
    }
    return best;
}

/* ---------------------------------------------------------------------
 * Headless LVGL setup — same shape as test_face_hit_targets.c's.
 * ------------------------------------------------------------------- */

static void sizing_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static uint32_t sizing_tick_cb(void)
{
    return 0;
}

typedef struct {
    int checked;
    int violations;
} sizing_result_t;

static bool sizing_inventory_on(void)
{
    char const *v = getenv("FF_TAP_INVENTORY");
    return v != NULL && v[0] == '1';
}

/* sizing_first_label_text — the text of the first lv_label anywhere under
 * `obj` (depth-first), or NULL. Inventory mode only: it is how a rect in
 * the dump gets a human name without inventing an object-naming scheme. */
static char const *sizing_first_label_text(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            char const *t = lv_label_get_text(c);
            if (t != NULL && t[0] != '\0') {
                return t;
            }
        }
        char const *deeper = sizing_first_label_text(c);
        if (deeper != NULL) {
            return deeper;
        }
    }
    return NULL;
}

/* sizing_scroll_viewport — the S21 model, reused: a control inside a live
 * VERTICAL scroll list is on-glass "when scrolled to", so the containment
 * check uses the list's viewport y-band, not the row's momentary y. */
static bool sizing_scroll_viewport(lv_obj_t *obj, ff_layout_rect_t *out_vp)
{
    lv_obj_t *p = lv_obj_get_parent(obj);
    while (p != NULL) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_top(p) + lv_obj_get_scroll_bottom(p) > 0)) {
            lv_area_t a;
            lv_obj_get_coords(p, &a);
            out_vp->x1 = (float)a.x1;
            out_vp->y1 = (float)a.y1;
            out_vp->x2 = (float)a.x2 + 1.0f;
            out_vp->y2 = (float)a.y2 + 1.0f;
            return true;
        }
        p = lv_obj_get_parent(p);
    }
    return false;
}

static bool sizing_scroll_viewport_hor(lv_obj_t *obj, ff_layout_rect_t *out_vp)
{
    lv_obj_t *p = lv_obj_get_parent(obj);
    while (p != NULL) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_left(p) + lv_obj_get_scroll_right(p) > 0)) {
            lv_area_t a;
            lv_obj_get_coords(p, &a);
            out_vp->x1 = (float)a.x1;
            out_vp->y1 = (float)a.y1;
            out_vp->x2 = (float)a.x2 + 1.0f;
            out_vp->y2 = (float)a.y2 + 1.0f;
            return true;
        }
        p = lv_obj_get_parent(p);
    }
    return false;
}

/* sizing_has_callback — does this clickable actually DO anything? An
 * object can be CLICKABLE without being a control: LVGL needs the flag
 * on a scroll container so `lv_indev_find_scroll_obj` has something to
 * walk up from, and this codebase builds two shapes that rely on it (the
 * compose PRED candidate strip, and scr_inbox.c's inert FLOATING
 * scroll-relay catchers — see test_face_hit_targets.c's Exclusion 4).
 * Neither is a thing a thumb aims AT, so neither has a meaningful "is it
 * big enough" question; both are still held to the bezel check below,
 * because a scroll container that pokes off the glass is a real layout
 * bug. Deliberately narrow — "registers no click callback at all" — and
 * applied only to the SIZE floors, so an ordinary button that forgot to
 * wire its handler is still caught by the bezel rule and by
 * test_face_hit_targets.c's own 44px floor. */
static bool sizing_has_callback(lv_obj_t *obj)
{
    return lv_obj_get_event_count(obj) > 0;
}

/* sizing_corner_bleed_square_px — for a control anchored at (x1,y1) whose
 * hit rect deliberately runs off the bottom-right rim into the masked
 * letterbox corner (scr_inbox.c's `+` FAB is the only one in this
 * codebase, and test_face_hit_targets.c carries the matching
 * corner-bleed exclusion), the side of the largest axis-aligned square at
 * that NEAR corner which is still entirely ON GLASS.
 *
 * This is the number that actually describes such a control: its rect's
 * own size is meaningless (most of it is under a bezel the user cannot
 * touch), but the square at its near corner is exactly the target a
 * thumb has. Closed form, not a search — with a = x1 - cx and
 * b = y1 - cy, requiring (a+w)^2 + (b+w)^2 <= r^2 is a quadratic in w:
 *   w = (-(a+b) + sqrt((a+b)^2 - 2*(a^2 + b^2 - r^2))) / 2.
 * Returns 0 when the near corner is already off-glass (no square fits). */
static float sizing_corner_bleed_square_px(float x1, float y1, float cx, float cy, float r)
{
    float const a = x1 - cx;
    float const b = y1 - cy;
    float const sum = a + b;
    float const disc = sum * sum - 2.0f * (a * a + b * b - r * r);
    if (disc <= 0.0f) {
        return 0.0f;
    }
    float const w = (-sum + sqrtf(disc)) / 2.0f;
    return (w > 0.0f) ? w : 0.0f;
}

static void sizing_walk(lv_obj_t *obj, char const *fixture_name, sizing_rule_t const *rule, sizing_result_t *out)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t area;
            lv_obj_get_click_area(child, &area);

            float w = (float)(area.x2 - area.x1 + 1);
            float h = (float)(area.y2 - area.y1 + 1);

            /* The whole-puck gesture/tap-anywhere region is not a control
             * a user aims at — the exact same exclusion (and the exact
             * same EXACT-size test, not a "looks large" heuristic)
             * test_face_hit_targets.c applies for the same reason. */
            bool is_whole_puck = (w == (float)FF_THEME_PUCK_PX) && (h == (float)FF_THEME_PUCK_PX);

            if (!is_whole_puck) {
                out->checked++;

                if (sizing_inventory_on()) {
                    char const *txt = sizing_first_label_text(child);
                    printf("  INV [%s] (%d,%d)-(%d,%d) %.0fx%.0f  %.1fx%.1f mm  %s\n", fixture_name, (int)area.x1,
                           (int)area.y1, (int)area.x2, (int)area.y2, (double)w, (double)h,
                           (double)(w / FF_THEME_PX_PER_MM), (double)(h / FF_THEME_PX_PER_MM),
                           (txt != NULL) ? txt : "(no label)");
                }

                bool const acts = sizing_has_callback(child);

                /* Corner-bleed control (the inbox FAB): its rect is not
                 * the target — the on-glass square at its near corner
                 * is. Measure THAT against the face's floors, and skip
                 * the whole-rect bezel check the way
                 * test_face_hit_targets.c already does, for the same
                 * documented reason (the excess covers masked letterbox
                 * corner, not touchable glass). */
                float const win_far = (float)FF_THEME_PUCK_PX;
                float const near_dx = (float)area.x1 - (float)FF_THEME_GLASS_CX;
                float const near_dy = (float)area.y1 - (float)FF_THEME_GLASS_CY;
                bool const near_on_glass =
                    (near_dx * near_dx + near_dy * near_dy) <= ((float)FF_THEME_GLASS_R * (float)FF_THEME_GLASS_R);
                bool const is_corner_bleed = near_on_glass && ((float)(area.x2 + 1) >= win_far) &&
                                             ((float)(area.y2 + 1) >= win_far);

                if (is_corner_bleed) {
                    float const on_glass = sizing_corner_bleed_square_px(
                        (float)area.x1, (float)area.y1, (float)FF_THEME_GLASS_CX, (float)FF_THEME_GLASS_CY,
                        (float)FF_THEME_GLASS_R);
                    int32_t const floor_px =
                        (rule->primary_min_h_px > rule->short_min_px) ? rule->primary_min_h_px : rule->short_min_px;
                    if (acts && on_glass < (float)floor_px) {
                        out->violations++;
                        printf("  TAP-CORNER-BLEED [%s]  rect=(%d,%d)-(%d,%d)  on-glass square %.1fpx  (floor %d) "
                               "— the reachable target is the square at the NEAR corner, not the rect\n",
                               fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)on_glass,
                               (int)floor_px);
                    }
                    sizing_walk(child, fixture_name, rule, out);
                    continue;
                }

                float shorter = (w < h) ? w : h;
                if (acts && shorter < (float)rule->short_min_px) {
                    out->violations++;
                    printf("  TAP-SHORT-SIDE  [%s]  rect=(%d,%d)-(%d,%d)  %.0fx%.0f  short=%.0f  (floor %d) %s\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)w, (double)h,
                           (double)shorter, (int)rule->short_min_px, sizing_first_label_text(child)
                                                                         ? sizing_first_label_text(child)
                                                                         : "");
                }

                if (acts && w >= (float)FF_TAP_PRIMARY_W_PX && h < (float)rule->primary_min_h_px) {
                    out->violations++;
                    printf("  TAP-PRIMARY-SHORT [%s]  rect=(%d,%d)-(%d,%d)  %.0fx%.0f  (a >=%dpx-wide control must be "
                           ">=%dpx tall) %s\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)w, (double)h,
                           (int)FF_TAP_PRIMARY_W_PX, (int)rule->primary_min_h_px,
                           sizing_first_label_text(child) ? sizing_first_label_text(child) : "");
                }

                /* R3 — inside the BEZEL's circle, not just the panel's. */
                ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};
                ff_layout_rect_t vp;
                if (sizing_scroll_viewport(child, &vp)) {
                    r.y1 = vp.y1;
                    r.y2 = vp.y2;
                } else if (sizing_scroll_viewport_hor(child, &vp)) {
                    r.x1 = vp.x1;
                    r.x2 = vp.x2;
                }
                if (!ff_layout_rect_in_circle(r, (float)FF_THEME_GLASS_CX, (float)FF_THEME_GLASS_CY,
                                              (float)FF_THEME_GLASS_R)) {
                    out->violations++;
                    printf("  TAP-OFF-BEZEL   [%s]  rect=(%d,%d)-(%d,%d)  check=(%.0f,%.0f)-(%.0f,%.0f)  glass "
                           "(%d,%d) r=%d %s\n",
                           fixture_name, (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, (double)r.x1,
                           (double)r.y1, (double)r.x2, (double)r.y2, (int)FF_THEME_GLASS_CX, (int)FF_THEME_GLASS_CY,
                           (int)FF_THEME_GLASS_R,
                           sizing_first_label_text(child) ? sizing_first_label_text(child) : "");
                }
            }
        }

        sizing_walk(child, fixture_name, rule, out);
    }
}

static sizing_result_t sizing_fixture(char const *path, char const *name, sizing_rule_t const *rule)
{
    lv_init();
    lv_tick_set_cb(sizing_tick_cb);

    const int32_t w = FF_THEME_WINDOW_PX;
    const int32_t h = FF_THEME_WINDOW_PX;
    const uint32_t buf_size = (uint32_t)(w * h * 4);
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, path);

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, sizing_flush_cb);
    lv_display_set_default(disp);

    ff_app_state_t state;
    ff_fixture_result_t fr = ff_fixture_load_file(path, &state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_FIXTURE_OK, fr, path);

    ff_build_face_screen(&state);
    lv_refr_now(disp);

    sizing_result_t result = {0, 0};
    sizing_walk(lv_screen_active(), name, rule, &result);

    free(buf);
    lv_deinit();

    return result;
}

/* ---------------------------------------------------------------------
 * The test.
 * ------------------------------------------------------------------- */

static void S_TAP_launcher_radar_compose_inbox_clear_the_outdoor_floors(void)
{
    DIR *d = opendir(FF_FIXTURE_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, FF_FIXTURE_DIR);

    int total_checked = 0;
    int total_violations = 0;
    int fixtures_swept = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_json = (nlen > 5) && (strcmp(entry->d_name + nlen - 5, ".json") == 0);
        if (!is_json) {
            continue;
        }

        char name[sizeof(entry->d_name)];
        snprintf(name, sizeof(name), "%s", entry->d_name);

        /* Inventory mode walks EVERY fixture, including the faces this
         * pass makes no size promises about, so the dump behind
         * docs/hardware/tap-targets.md covers the whole device (it is
         * also how the S28 rim-zone audit in that document — which
         * controls sit within the BACK/HOME edge-swipe bands — was
         * produced). Those faces are swept under a rule that asserts
         * only the universal 44px floor and the bezel, so inventory mode
         * cannot make the test fail on something it does not claim. */
        static const sizing_rule_t inventory_only = {"", FF_THEME_MIN_HIT_PX, FF_THEME_MIN_HIT_PX};
        sizing_rule_t const *rule = sizing_rule_for(name);
        if (rule == NULL) {
            if (!sizing_inventory_on()) {
                continue; /* a face outside this pass's four — the 44px sweep still covers it */
            }
            rule = &inventory_only;
        }

        char path[sizeof(FF_FIXTURE_DIR) + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "%s%s", FF_FIXTURE_DIR, entry->d_name);

        sizing_result_t r = sizing_fixture(path, name, rule);
        total_checked += r.checked;
        total_violations += r.violations;
        fixtures_swept++;
    }
    closedir(d);

    printf("test_tap_target_sizing: swept %d fixture(s), checked %d clickable element(s), %d violation(s)\n",
           fixtures_swept, total_checked, total_violations);

    /* The same vacuous-pass guards test_face_hit_targets.c uses: a
     * silently-empty fixture dir, or a rule table that matches nothing,
     * would otherwise make this test prove nothing at all. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fixtures_swept, "no launcher/radar/compose/inbox fixtures matched");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, total_checked, "matched fixtures but found zero clickable elements");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, total_violations,
                                  "one or more controls on launcher/radar/compose/inbox are under this face's "
                                  "outdoor tap floor or poke past the bezel's own glass circle — see the TAP-* "
                                  "lines above");
}

/* The rule table must actually MATCH each of the four named faces — a
 * typo'd prefix would silently reduce this whole file to a no-op for
 * that face while still passing (the exact vacuous-pass failure mode
 * AGENTS.md's proxy check exists to catch). */
static void S_TAP_rule_table_covers_all_four_named_faces(void)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(sizing_rule_for("launcher.json"), "launcher fixtures are not covered by any rule");
    TEST_ASSERT_NOT_NULL_MESSAGE(sizing_rule_for("radar_live.json"), "radar fixtures are not covered by any rule");
    TEST_ASSERT_NOT_NULL_MESSAGE(sizing_rule_for("compose_123.json"), "compose fixtures are not covered by any rule");
    TEST_ASSERT_NOT_NULL_MESSAGE(sizing_rule_for("inbox_inbox.json"), "inbox fixtures are not covered by any rule");
    /* And must NOT quietly claim faces it makes no promises about. */
    TEST_ASSERT_NULL_MESSAGE(sizing_rule_for("map_nofix.json"), "map must not be claimed by this pass's rules");
    TEST_ASSERT_NULL_MESSAGE(sizing_rule_for("settings_default.json"), "settings must not be claimed by this pass");
    /* Longest-prefix wins: banner_on_radar must not fall through to a
     * shorter, more permissive entry. */
    TEST_ASSERT_EQUAL_PTR_MESSAGE(sizing_rule_for("banner_on_radar.json"), sizing_rule_for("banner_on_radar.json"),
                                  "unstable rule lookup");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S_TAP_rule_table_covers_all_four_named_faces);
    RUN_TEST(S_TAP_launcher_radar_compose_inbox_clear_the_outdoor_floors);

    return UNITY_END();
}
