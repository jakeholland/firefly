/**
 * test_scr_intent.c — headless LVGL INTERACTION tests for the three
 * emit sites S16 slice c1 wires: nav long-press (-> OPEN_SETTINGS),
 * Compose back "<" (-> BACK), Signals "+" (-> OPEN_COMPOSE).
 *
 * Same pattern — and the same reason — as test_scr_flare.c (PR #20
 * review, HIGH finding): goldens are single-frame pixel diffs and never
 * fire an event, so nothing else in the suite can prove that clicking
 * "+" emits OPEN_COMPOSE rather than BACK, or anything at all. Build the
 * real screen, synthesize the event with `lv_obj_send_event`, and assert
 * on what came out of the seam — here, a spy sink bound via
 * `ff_intent_emit_bind`, since the seam's whole contract is that screens
 * know nothing past `ff_intent_emit()`.
 *
 * Each click must produce EXACTLY ONE intent (count asserted, not just
 * kind): a control double-registered on two callbacks would pass a
 * kind-only check while double-dispatching every tap.
 *
 * Mutation-check (hand-verified before pushing, per
 * docs/review/code-review.md item 6): swapping compose_back_cb's and
 * signals_open_compose_cb's emitted kinds fails both of their tests on
 * the kind assertion; removing an `ff_intent_emit` call entirely fails
 * on count == 0.
 */
#include <string.h>

#include "unity.h"

#include "ff_app_state.h"
#include "ff_intent.h"
#include "scr_compose.h"
#include "scr_nav.h"
#include "scr_signals.h"

/* Frozen tick — same convention as test_scr_flare.c: nothing here
 * renders a frame. */
static uint32_t test_tick_cb(void)
{
    return 0;
}

/* ------------------------------------------------------------------- */
/* spy sink                                                             */
/* ------------------------------------------------------------------- */

typedef struct {
    int count;
    ff_intent_t last; /* copied — the sink contract (ff_intent.h) */
} spy_sink_t;

static spy_sink_t s_spy;

static void spy_sink_cb(void *user, ff_intent_t const *in)
{
    spy_sink_t *s = (spy_sink_t *)user;
    s->count++;
    s->last = *in;
}

void setUp(void)
{
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    /* No buffers/flush callback: widget building + lv_obj_send_event
     * never touch the flush path (see test_scr_flare.c's setUp note). */
    lv_display_t *disp = lv_display_create(456, 456);
    lv_display_set_default(disp);

    memset(&s_spy, 0, sizeof(s_spy));
    ff_intent_emit_bind(spy_sink_cb, &s_spy);
}

void tearDown(void)
{
    /* The sink is process-global (ff_intent.h); never leak a binding to
     * a test that expects the unbound state. */
    ff_intent_emit_bind(NULL, NULL);
    lv_deinit();
}

/* find_button_with_label — same reusable lookup as test_scr_flare.c
 * (kept file-local there by design; duplicated rather than exported so
 * neither test file grows a shared-header dependency for one helper). */
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

/* =================================================================== */
/* nav long-press -> OPEN_SETTINGS                                      */
/* =================================================================== */

static void S16_c1_nav_long_press_emits_open_settings(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_RADAR;

    ff_scr_nav_build(&state, NULL);

    /* The long-press hook lives on the puck — scr_nav.c's first child of
     * the active screen (the same object it marks LV_OBJ_FLAG_CLICKABLE
     * for exactly this gesture). If the build order ever changes this
     * lookup fails loudly here rather than silently testing nothing. */
    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck);
    TEST_ASSERT_TRUE(lv_obj_has_flag(puck, LV_OBJ_FLAG_CLICKABLE));

    lv_result_t r = lv_obj_send_event(puck, LV_EVENT_LONG_PRESSED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_OPEN_SETTINGS, s_spy.last.kind);
}

/* =================================================================== */
/* Compose back "<" -> BACK                                             */
/* =================================================================== */

static void S16_c1_compose_back_emits_back(void)
{
    ff_app_compose_t compose;
    memset(&compose, 0, sizeof(compose)); /* empty broadcast draft, ABC mode */

    ff_scr_compose_build(&compose);

    click(find_button_with_label(lv_screen_active(), "<"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_BACK, s_spy.last.kind);

    /* SEND stays a c2 stub: clicking it must emit NOTHING yet — a
     * half-wired SEND that emitted, say, BACK would silently eat drafts. */
    click(find_button_with_label(lv_screen_active(), "SEND"));
    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
}

/* =================================================================== */
/* Signals "+" -> OPEN_COMPOSE, no explicit destination                 */
/* =================================================================== */

static void S16_c1_signals_plus_emits_open_compose_with_no_destination(void)
{
    ff_app_signals_t signals;
    memset(&signals, 0, sizeof(signals)); /* empty feed — the "+" is header chrome, always present */

    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    ff_scr_signals_build(parent, &signals);

    click(find_button_with_label(parent, "+"));

    TEST_ASSERT_EQUAL_INT(1, s_spy.count);
    TEST_ASSERT_EQUAL(FF_INTENT_OPEN_COMPOSE, s_spy.last.kind);
    /* node_id 0 = no explicit destination: the shell resolves S08's
     * "TO = selected crew member" rule — a pure-render screen cannot
     * know the selection, and must not guess one (scr_signals.c's
     * signals_open_compose_cb comment). */
    TEST_ASSERT_EQUAL_UINT32(0u, s_spy.last.u.node_id);
}

/* =================================================================== */
/* Unbound seam (goldens/headless) — every wired site is a safe no-op   */
/* =================================================================== */

static void S16_c1_wired_sites_are_noops_while_the_seam_is_unbound(void)
{
    ff_intent_emit_bind(NULL, NULL); /* the goldens/headless state */

    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    state.active_face = FF_APP_FACE_SIGNALS;
    ff_scr_nav_build(&state, NULL);

    lv_obj_t *puck = lv_obj_get_child(lv_screen_active(), 0);
    TEST_ASSERT_NOT_NULL(puck);
    (void)lv_obj_send_event(puck, LV_EVENT_LONG_PRESSED, NULL);
    click(find_button_with_label(lv_screen_active(), "+"));

    TEST_ASSERT_EQUAL_INT(0, s_spy.count); /* nothing reached the (unbound) spy — and nothing crashed */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_c1_nav_long_press_emits_open_settings);
    RUN_TEST(S16_c1_compose_back_emits_back);
    RUN_TEST(S16_c1_signals_plus_emits_open_compose_with_no_destination);
    RUN_TEST(S16_c1_wired_sites_are_noops_while_the_seam_is_unbound);

    return UNITY_END();
}
