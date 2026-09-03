/**
 * lv_test_harness.h — shared headless-LVGL scaffolding for the
 * screen-level input tests (test_scr_intent.c, test_scr_banner.c):
 * setUp/tearDown, the ff_intent_emit_bind spy sink, a frozen-but-
 * advanceable tick source, the label/button tree-search helpers, and
 * the synthetic-pointer-indev press/move/release helpers (click,
 * tap_at, drag, drag_v).
 *
 * Extracted from test_scr_intent.c and test_scr_banner.c (debt/test-
 * naming-harness): both files hand-rolled identical copies of every
 * one of these — each file's own header comment used to say so
 * explicitly ("same pattern test_scr_intent.c uses, duplicated
 * file-local ... that file's own established convention"). One
 * canonical copy now, same behavior — every existing test in both
 * files is unchanged (a rename/extraction, not a coverage change).
 *
 * test_scr_flare.c does NOT include this: it has only a trivial
 * lv_init()/lv_deinit() setUp/tearDown pair (no spy sink, no indev
 * drag helpers — it never fires a synthetic input event at all), so
 * it isn't the same duplication and is left as-is.
 *
 * ## The UAF workaround (drag()/drag_v()'s one real hazard)
 * A release with residual velocity schedules LVGL's scroll-THROW
 * momentum animation, keyed on `var = indev` (lv_indev.c's
 * indev_scroll_throw_anim_cb) — NOT on the scrolled object, so
 * lv_obj_del/lv_obj_clean on the scrolled tree never cancels it, and
 * lv_indev_delete() alone does not either (measured: it frees the
 * indev struct directly with no animation cleanup at all). Deleting
 * the indev while that animation is still pending leaves a dangling
 * `var` that crashes the NEXT lv_timer_handler() call — including one
 * from a LATER, unrelated drag well after this function returned —
 * whenever the freed memory happens to get reused (a rebuilt screen
 * being the everyday way that happens). `ff_test_release_probe_indev`
 * cancels the pending throw animation before freeing the indev, which
 * is what makes these helpers safely reusable across an in-between
 * lv_obj_clean + rebuild. No observable effect on any existing
 * caller: nothing reads the throw's own further motion — every
 * assertion runs off the position drag/drag_v/tap_at already reached
 * at release. Both drag() and drag_v() funnel through this ONE
 * function, so the fix (and this derivation) live in exactly one
 * place instead of three (test_scr_intent.c's drag() and drag_v() and
 * test_scr_banner.c's drag_v() each previously carried their own
 * copy of just the two lines, only one of the three next to this
 * comment).
 *
 * Every function here is `static inline` (header-only — no .c file,
 * no CMake wiring) and every piece of state (`s_spy`, `s_fake_tick_ms`,
 * `s_probe_pt`/`s_probe_state`) is a plain file-scope `static`: each
 * including translation unit gets its OWN private copy, exactly as
 * before this extraction. test_scr_intent and test_scr_banner are two
 * separate test executables that never link together, so this is
 * shared *source*, never shared *state*.
 */
#ifndef FF_LV_TEST_HARNESS_H
#define FF_LV_TEST_HARNESS_H

#include <string.h>

#include "lvgl.h"
#include "unity.h"

#include "ff_intent.h"

/* ---------------------------------------------------------------------
 * Frozen-by-default tick. Nothing that only uses click() ever renders a
 * frame, so most tests never observe it moving. drag()/drag_v() advance
 * it: LVGL's indev read timer is scheduled on real elapsed ticks
 * (`LV_DEF_REFR_PERIOD`, 33 ms) even in a tight synchronous test loop, so
 * a genuinely-frozen tick would let the FIRST lv_timer_handler() call
 * read the press and then silently stop reading every subsequent
 * position update — a drag that "moves" but is never actually seen
 * moving, and no gesture accumulates. Reset to 0 in setUp() so every
 * OTHER test keeps seeing a frozen 0, exactly as before this extraction.
 * ------------------------------------------------------------------- */
static uint32_t s_fake_tick_ms;

static inline uint32_t test_tick_cb(void)
{
    return s_fake_tick_ms;
}

/* ---------------------------------------------------------------------
 * spy sink — ff_intent_emit_bind's receiving end for every test that
 * asserts on which intent (and how many) a click/drag emitted.
 * ------------------------------------------------------------------- */
typedef struct {
    int count;
    ff_intent_t last; /* copied — the sink contract (ff_intent.h) */
} spy_sink_t;

static spy_sink_t s_spy;

static inline void spy_sink_cb(void *user, ff_intent_t const *in)
{
    spy_sink_t *s = (spy_sink_t *)user;
    s->count++;
    s->last = *in;
}

/* Call from each file's own setUp()/tearDown() — Unity requires those
 * two exact names, so they still live one per file (a header can't
 * define them once for both — each test executable needs its own), but
 * now as a one-line call in here. `win_px` lets a caller pass
 * FF_THEME_WINDOW_PX or a literal; both existing call sites already
 * used the same value (412) under two different spellings — passing it
 * explicitly keeps that choice at the call site instead of hard-coding
 * a theme dependency into this shared header. */
static inline void ff_test_lv_setup(int32_t win_px)
{
    s_fake_tick_ms = 0;
    lv_init();
    lv_tick_set_cb(test_tick_cb);
    /* No buffers/flush callback: widget building + lv_obj_send_event/a
     * synthetic indev never touch the flush path. */
    lv_display_t *disp = lv_display_create(win_px, win_px);
    lv_display_set_default(disp);

    memset(&s_spy, 0, sizeof(s_spy));
    ff_intent_emit_bind(spy_sink_cb, &s_spy);
}

static inline void ff_test_lv_teardown(void)
{
    /* The sink is process-global (ff_intent.h); never leak a binding to
     * a test that expects the unbound state. */
    ff_intent_emit_bind(NULL, NULL);
    lv_deinit();
}

/* ---------------------------------------------------------------------
 * Tree-search helpers.
 * ------------------------------------------------------------------- */

/* An exact-text label lookup, depth-first. */
static inline lv_obj_t *find_label_exact(lv_obj_t *root, char const *text)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            char const *txt = lv_label_get_text(child);
            if (txt != NULL && strcmp(txt, text) == 0) {
                return child;
            }
        }
        lv_obj_t *found = find_label_exact(child, text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* An lv_button whose (any-depth) label child matches exactly. */
static inline lv_obj_t *find_button_with_label(lv_obj_t *root, char const *label_text)
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

/* ---------------------------------------------------------------------
 * click() — direct LV_EVENT_CLICKED injection (most tests in both
 * files); drag()/drag_v()/tap_at() — a real press/move/release through
 * a synthetic pointer indev, the only way to exercise
 * LV_OBJ_FLAG_PRESS_LOCK or an actual scroll gesture at all.
 * ------------------------------------------------------------------- */

static inline void click(lv_obj_t *obj)
{
    TEST_ASSERT_NOT_NULL(obj);
    lv_result_t r = lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    TEST_ASSERT_EQUAL(LV_RESULT_OK, r);
}

static lv_point_t s_probe_pt;
static lv_indev_state_t s_probe_state;

static inline void probe_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_probe_pt;
    data->state = s_probe_state;
}

/* THE workaround, in exactly one place — see this header's own top
 * comment for the full derivation. Every drag helper below funnels
 * through here once, at the very end, before the indev goes away. */
static inline void ff_test_release_probe_indev(lv_indev_t *indev)
{
    lv_anim_delete(indev, NULL);
    lv_indev_delete(indev);
}

/* Horizontal press -> several move steps -> release, same shape as
 * targets/sim/main.c's ff_loop_swipe (the production ctl-socket "swipe"
 * command). */
static inline void drag(int32_t from_x, int32_t to_x, int32_t y)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    /* LVGL's indev read timer only re-fires on real elapsed ticks (see
     * s_fake_tick_ms's comment above) — advance past LV_DEF_REFR_PERIOD
     * (33 ms) before every handler call, not just the first, or every
     * position update after the initial press is silently never read. */
    s_probe_pt.x = (lv_coord_t)from_x;
    s_probe_pt.y = (lv_coord_t)y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        s_probe_pt.x = (lv_coord_t)(from_x + (to_x - from_x) * i / STEPS);
        s_fake_tick_ms += 40u;
        lv_timer_handler();
    }

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    ff_test_release_probe_indev(indev);
}

/* Vertical counterpart to drag() above — same real-indev press/move/
 * release shape, varying y instead of x. */
static inline void drag_v(int32_t from_y, int32_t to_y, int32_t x)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, probe_read_cb);

    s_probe_pt.x = (lv_coord_t)x;
    s_probe_pt.y = (lv_coord_t)from_y;
    s_probe_state = LV_INDEV_STATE_PRESSED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    enum { STEPS = 6 };
    for (int i = 1; i <= STEPS; i++) {
        s_probe_pt.y = (lv_coord_t)(from_y + (to_y - from_y) * i / STEPS);
        s_fake_tick_ms += 40u;
        lv_timer_handler();
    }

    s_probe_state = LV_INDEV_STATE_RELEASED;
    s_fake_tick_ms += 40u;
    lv_timer_handler();

    ff_test_release_probe_indev(indev);
}

/* A single press-then-release at one point, no movement — a real tap
 * through the same synthetic pointer indev drag()/drag_v() use, as
 * opposed to click()'s direct LV_EVENT_CLICKED injection most tests
 * use. Proves a control routes a REAL coordinate tap correctly, not
 * just that its CLICKABLE flag reads true. */
static inline void tap_at(int32_t x, int32_t y)
{
    drag_v(y, y, x);
}

#endif /* FF_LV_TEST_HARNESS_H */
