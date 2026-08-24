/**
 * test_scr_flare.c — headless LVGL INTERACTION tests for the S10 flare
 * UI's button bindings (GO/DISMISS/CANCEL/FLARE -> core).
 *
 * PR #20 independent code review, HIGH finding: goldens are single-frame
 * pixel diffs — they never fire a click, so nothing in the committed
 * suite proved which button calls which core function. The reviewer's
 * exact repro: swapping `flare_go_cb`/`flare_dismiss_takeover_cb`'s
 * bodies in scr_flare.c would compile clean, pass all of ctest, and pass
 * every golden byte-identical. That's precisely the class of regression
 * Amendment 3 (docs/specs/S10-flare.md) exists to prevent, and it was
 * invisible to every gate this PR had before this file.
 *
 * ## The pattern (reusable — read this if you're adding the next face's
 * input tests; S06 deferred this exact capability for lack of it)
 * Build the real screen against a REAL `ff_flare_t` (core/include/
 * ff_flare.h) — not the flattened `ff_app_flare_t` display struct, which
 * only exists to drive pixels — then locate the actual `lv_obj_t` button
 * by walking the tree for an `lv_button_class` object whose label child's
 * text matches, and fire `lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL)`
 * (LVGL v9's documented way to synthesize an event without going through
 * indev press/release simulation — it invokes every callback registered
 * via `lv_obj_add_event_cb(..., LV_EVENT_CLICKED, ...)` exactly as a real
 * touch would). Then assert on the REAL `ff_flare_t`'s fields — never on
 * the display struct, which the screen builder never writes back to.
 * This needs a live LVGL context (lv_init + a display, see setUp/
 * tearDown below) but explicitly does NOT need to render a frame
 * (no lv_refr_now/lv_timer_handler call anywhere in this file) — building
 * the widget tree and dispatching an event both work against an
 * unbuffered display, which keeps this file fast and free of the
 * PNG/golden machinery entirely.
 *
 * Mutation-check (hand-verified before pushing, per the task brief):
 * temporarily swapping `flare_go_cb`'s and `flare_dismiss_takeover_cb`'s
 * bodies in scr_flare.c (so GO calls ff_flare_dismiss_takeover and
 * DISMISS calls ff_flare_go — the reviewer's exact regression) fails
 * BOTH `S10_ACn_go_click_locks_to_takeover_node` (locked_node_id stays 0)
 * and `S10_ACn_dismiss_click_leaves_existing_lock_intact` (takeover_active
 * stays true instead of clearing, and locked_node_id gets overwritten
 * instead of staying put) — reverted after confirming.
 */
#include <string.h>

#include "unity.h"

#include "ff_flare.h"
#include "ff_layout.h"
#include "ff_theme.h"
#include "ff_radar.h"
#include "scr_flare.h"
#include "scr_radar.h"

/* Frozen tick — same convention as targets/sim/main.c's ff_mock_tick_cb;
 * nothing in this file renders a frame, so the tick value is only ever
 * used as `now_ms` for the ff_flare_t setup calls below, which pass their
 * own explicit literals anyway (ff_flare.h: "no clock-reading of its
 * own"). */
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
     * lv_obj_send_event() — neither touches the flush path. */
    lv_display_t *disp = lv_display_create(456, 456);
    lv_display_set_default(disp);
}

void tearDown(void)
{
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * find_button_with_label — the reusable lookup this file's header
 * comment describes: depth-first search for an lv_button_class object
 * whose direct label child's text exactly matches. Recursive because
 * scr_flare.c's exact child ordering/depth is an implementation detail
 * this test deliberately does NOT pin (only "a button that says GO
 * exists somewhere" is asserted) — pinning child indices would make this
 * test break on cosmetic reordering instead of on the actual regression
 * class it exists to catch.
 * ------------------------------------------------------------------- */
static lv_obj_t *find_button_with_label(lv_obj_t *root, char const *label_text)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_button_class)) {
            uint32_t nc = lv_obj_get_child_count(child);
            for (uint32_t j = 0; j < nc; j++) {
                lv_obj_t *maybe_label = lv_obj_get_child(child, j);
                if (lv_obj_check_type(maybe_label, &lv_label_class)) {
                    char const *txt = lv_label_get_text(maybe_label);
                    if (txt != NULL && strcmp(txt, label_text) == 0) {
                        return child;
                    }
                }
            }
        }
        lv_obj_t *found = find_button_with_label(child, label_text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void click(lv_obj_t *obj)
{
    TEST_ASSERT_NOT_NULL(obj);
    lv_result_t r = lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);
}

/* ---------------------------------------------------------------------
 * GO / DISMISS — the takeover screen, against a real ff_flare_t.
 * ------------------------------------------------------------------- */

static void S10_ACn_go_click_locks_to_takeover_node(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    /* Real core takeover, paired sender, node 42 — the display struct
     * below is a SEPARATE, independently-populated snapshot (same "two
     * structs, one real, one for pixels" split main.c's window mode
     * uses); the point of this test is that clicking GO drives the REAL
     * struct, not that the two structs' contents match each other. */
    (void)ff_flare_on_flare_rx(&f, 42u, true, 300u, 1000u);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(0u, f.locked_node_id);

    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);

    ff_scr_flare_build_takeover(&disp, &f);

    lv_obj_t *go_btn = find_button_with_label(lv_screen_active(), "GO");
    click(go_btn);

    TEST_ASSERT_EQUAL_UINT32(42u, f.locked_node_id);
    TEST_ASSERT_FALSE(f.takeover_active);
}

static void S10_ACn_dismiss_click_leaves_existing_lock_intact(void)
{
    /* Ruling 2/3's exact race case: locked on node 7 (via an earlier
     * GO), a NEW takeover from a DIFFERENT node (99) arrives and is
     * showing. DISMISS must clear only the pending takeover and leave
     * the node-7 lock untouched — this is the reviewer's repro for why
     * ff_flare_dismiss_takeover() must never be swapped with
     * ff_flare_release_lock() (or with ff_flare_go()). */
    ff_flare_t f;
    ff_flare_init(&f);
    (void)ff_flare_on_flare_rx(&f, 7u, true, 300u, 1000u);
    (void)ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(7u, f.locked_node_id);
    TEST_ASSERT_FALSE(f.takeover_active);

    (void)ff_flare_on_flare_rx(&f, 99u, true, 300u, 2000u);
    TEST_ASSERT_TRUE(f.takeover_active);
    TEST_ASSERT_EQUAL_UINT32(99u, f.takeover_node_id);

    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);
    disp.locked = true;
    strncpy(disp.locked_from_name, "DANA", sizeof(disp.locked_from_name) - 1);

    ff_scr_flare_build_takeover(&disp, &f);

    lv_obj_t *dismiss_btn = find_button_with_label(lv_screen_active(), "DISMISS");
    click(dismiss_btn);

    TEST_ASSERT_FALSE(f.takeover_active); /* the pending takeover cleared */
    TEST_ASSERT_EQUAL_UINT32(7u, f.locked_node_id); /* the EXISTING lock is untouched */
}

static void S10_ACn_go_click_replaces_an_existing_lock(void)
{
    /* GO, unlike DISMISS, is the one path where REPLACING an existing
     * lock is correct (Amendment Ruling 2: "GO is an explicit user
     * decision, so a deliberate switch is allowed"). */
    ff_flare_t f;
    ff_flare_init(&f);
    (void)ff_flare_on_flare_rx(&f, 7u, true, 300u, 1000u);
    (void)ff_flare_go(&f);
    TEST_ASSERT_EQUAL_UINT32(7u, f.locked_node_id);

    (void)ff_flare_on_flare_rx(&f, 99u, true, 300u, 2000u);

    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);
    disp.locked = true;
    strncpy(disp.locked_from_name, "DANA", sizeof(disp.locked_from_name) - 1);

    ff_scr_flare_build_takeover(&disp, &f);

    click(find_button_with_label(lv_screen_active(), "GO"));

    TEST_ASSERT_EQUAL_UINT32(99u, f.locked_node_id); /* switched, per Ruling 2 */
    TEST_ASSERT_FALSE(f.takeover_active);
}

/* ---------------------------------------------------------------------
 * CANCEL — the sender overlay, against a real ff_flare_t.
 * ------------------------------------------------------------------- */

static void S10_ACn_cancel_click_ends_send_exactly_once(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    (void)ff_flare_send_begin(&f, 300u, 1000u);
    TEST_ASSERT_TRUE(f.sending);

    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.sending = true;
    disp.send_expires_in_ms = 299000;

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_flare_build_sender_overlay(parent, &disp, &f);

    click(find_button_with_label(parent, "CANCEL"));
    TEST_ASSERT_FALSE(f.sending);

    /* A second CANCEL press (e.g. a doubletap) must stay a no-op, not
     * re-emit FLARE_END or corrupt state — ff_flare_send_cancel's own
     * documented "no-op when not currently sending" contract, exercised
     * here through the actual UI binding rather than only at the core
     * layer (core/tests/test_flare.c already covers the core function in
     * isolation; this pins that the BUTTON forwards to it faithfully on
     * a second press too, not just the first). */
    lv_obj_t *cancel_btn = find_button_with_label(parent, "CANCEL");
    click(cancel_btn);
    TEST_ASSERT_FALSE(f.sending);
}

/* ---------------------------------------------------------------------
 * FLARE (close-range Radar button) — against a real ff_flare_t.
 * ------------------------------------------------------------------- */

static void S10_ACn_flare_button_click_begins_send(void)
{
    ff_flare_t f;
    ff_flare_init(&f);
    TEST_ASSERT_FALSE(f.sending);

    ff_radar_view_t r;
    memset(&r, 0, sizeof(r));
    r.mode = RADAR_CLOSE;
    strncpy(r.name, "DANA", sizeof(r.name) - 1);
    strncpy(r.dist_str, "15 m", sizeof(r.dist_str) - 1);

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_radar_build(parent, &r, &f);

    click(find_button_with_label(parent, "FLARE"));

    TEST_ASSERT_TRUE(f.sending);
    /* radar_flare_stub_cb calls ff_flare_send_begin(flare_rt, 0, lv_tick_get())
     * — dur_s=0 defaults to FF_FLARE_DEFAULT_DUR_S (300s), and
     * test_tick_cb() always returns 0, so the expected expiry is exactly
     * 0 + 300*1000. A wrong/missing duration argument, or a swapped-in
     * "cancel" call instead of "begin", would both fail this exact value
     * — not just the `sending` flag above. */
    TEST_ASSERT_EQUAL_UINT32(300000u, f.send_expiry_ms);
}

/* ---------------------------------------------------------------------
 * NULL flare_rt (golden/headless rendering) — every button must stay a
 * safe no-op, never a crash, per scr_flare.h's documented contract.
 * ------------------------------------------------------------------- */

static void S10_ACn_go_click_with_null_rt_is_safe_noop(void)
{
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "KEV", sizeof(disp.takeover_from_name) - 1);

    ff_scr_flare_build_takeover(&disp, NULL);

    click(find_button_with_label(lv_screen_active(), "GO"));
    click(find_button_with_label(lv_screen_active(), "DISMISS"));
    /* No assertion beyond "did not crash" — NULL rt has nothing to
     * observe; this is the guard-path test for the contract documented
     * in scr_flare.h's top comment ("passing NULL for rt is always
     * safe"). */
}

/* ---------------------------------------------------------------------
 * Issue #27 — the lock-disclosure chip stays on the ROUND GLASS.
 *
 * The chip grew from 14px to 20px type as part of shortening it from a
 * sentence to a glance, and the puck is a CIRCLE: a chip that merely fits
 * the 440px bounding box can still hang off the visible display, which is
 * exactly how PR #25 shipped a back button 42px outside the round area.
 * The existing sweep (targets/sim/tests/test_face_hit_targets.c) can't
 * catch it — that one only examines CLICKABLE objects, and this chip is
 * deliberately an inert indicator.
 *
 * Uses the worst case the formatter can produce: a locked name at the
 * full FF_APP_NAME_LEN budget (15 characters — PR #41 code review caught
 * the previous strings being 14, so the comment and the inputs agree
 * now), which truncates and therefore also carries the ellipsis, making
 * this the widest string ff_flare_fmt_lock_cost can emit. If
 * FF_FLARE_FMT_LOCK_NAME_MAX is ever raised past what the glass holds at
 * this type size, this fails instead of shipping a name past the bezel.
 * ------------------------------------------------------------------- */

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
    ff_app_flare_t disp;
    memset(&disp, 0, sizeof(disp));
    disp.takeover_active = true;
    strncpy(disp.takeover_from_name, "MAXIMILIANOOOOO", sizeof(disp.takeover_from_name) - 1);
    disp.locked = true;
    strncpy(disp.locked_from_name, "BARTHOLOMEWWWWW", sizeof(disp.locked_from_name) - 1);

    ff_scr_flare_build_takeover(&disp, NULL);

    /* Force layout without rendering a frame — this file has no draw
     * buffers (see setUp), and lv_obj_update_layout only runs the
     * size/position pass, never the draw pass. */
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *label = find_label_with_prefix(lv_screen_active(), "GO DROPS LOCK");
    TEST_ASSERT_NOT_NULL_MESSAGE(label, "the lock-disclosure chip must be built when GO would switch the lock");

    /* The chip is the label's parent — assert on the PILL, not the text:
     * the pill's padding is what actually reaches furthest toward the
     * bezel. */
    lv_obj_t *chip = lv_obj_get_parent(label);
    TEST_ASSERT_NOT_NULL(chip);

    lv_area_t area;
    lv_obj_get_coords(chip, &area);

    /* lv_area_t's x2/y2 are INCLUSIVE last-pixel coordinates; ff_layout's
     * are exclusive far edges — hence the +1 (same conversion
     * test_face_hit_targets.c documents). Circle center is the puck's
     * center within the window, radius the puck's own radius. */
    float margin = (float)((FF_THEME_WINDOW_PX - FF_THEME_PUCK_PX) / 2);
    float cx = margin + (float)FF_THEME_PUCK_RADIUS_PX;
    float cy = margin + (float)FF_THEME_PUCK_RADIUS_PX;

    ff_layout_rect_t r = {(float)area.x1, (float)area.y1, (float)area.x2 + 1.0f, (float)area.y2 + 1.0f};
    TEST_ASSERT_TRUE_MESSAGE(ff_layout_rect_in_circle(r, cx, cy, (float)FF_THEME_PUCK_RADIUS_PX),
                              "the lock-disclosure chip must lie entirely within the round glass, "
                              "even with two maximum-length crew names");
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

    ff_scr_flare_build_takeover(&disp, NULL);

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

    RUN_TEST(S10_ACn_go_click_locks_to_takeover_node);
    RUN_TEST(S10_ACn_dismiss_click_leaves_existing_lock_intact);
    RUN_TEST(S10_ACn_go_click_replaces_an_existing_lock);
    RUN_TEST(S10_ACn_cancel_click_ends_send_exactly_once);
    RUN_TEST(S10_ACn_flare_button_click_begins_send);
    RUN_TEST(S10_ACn_go_click_with_null_rt_is_safe_noop);
    RUN_TEST(S10_ACn_lock_disclosure_chip_stays_inside_the_round_glass);
    RUN_TEST(S10_ACn_lock_disclosure_is_always_accompanied_by_the_headline);

    return UNITY_END();
}
