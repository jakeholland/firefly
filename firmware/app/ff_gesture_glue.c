/**
 * ff_gesture_glue.c — see ff_gesture_glue.h for the contract.
 */
#include "ff_gesture_glue.h"

#include <stdlib.h>

#include "ff_gesture.h"
#include "ff_intent.h"
#include "ff_sound.h"
#include "ff_sound_emit.h"
#include "ff_theme.h"

/* Polling period for the shared timer (MOVE-sample polling + G3's
 * time-driven long-press check) — comfortably finer-grained than
 * LVGL's own 33ms default indev read period (LV_DEF_REFR_PERIOD), and
 * tiny next to the spec's 500ms/1200ms deadlines, so the exact value
 * has no bearing on correctness, only on how promptly a recognition is
 * observed. */
#define FF_GESTURE_GLUE_TIMER_MS 20u

typedef struct {
    ff_shell_t *sh;    /* not owned; must outlive indev, per this header's contract */
    lv_indev_t *indev; /* not owned; this ctx's own lifetime is tied to it (see the DELETE handler below) */
    ff_gesture_t g;
    lv_timer_t *timer; /* owned; deleted alongside this ctx */
} ff_gesture_glue_ctx_t;

/* takeover_active — "Gestures are ignored entirely while a takeover is
 * active (FLARE, POWER_MENU)" (S28-gestures.md). "FLARE" here covers
 * BOTH shapes the spec's own AC list exercises: an INCOMING flare
 * (`flare.takeover_active`, a true full-screen replacement of whatever
 * face was showing — ff_face_dispatch_build's own doc comment) and MY
 * OWN outbound flare countdown (`flare.sending` — visually just as much
 * a takeover: scr_nav.c dims the whole base face to 30% opacity and
 * draws the sender overlay + CANCEL on top of it, per that file's own
 * "flaring_self reads as an error" fix) — S28's AC17 is specifically
 * the sending case ("during the flare countdown a bottom-rim swipe does
 * nothing"). A NULL view (sh not ticked yet) is treated as "a takeover
 * is active" — fail toward doing nothing, never toward navigating off
 * of a state we can't actually see. */
static bool gesture_glue_takeover_active(ff_app_state_t const *view)
{
    if (view == NULL) {
        return true;
    }
    return view->flare.takeover_active || view->flare.sending || view->active_face == FF_APP_FACE_POWER_MENU;
}

/* Walks `obj` and its ancestors up to the screen root looking for the
 * "this is a real widget, not empty glass" tag ff_scr_button_create
 * (app/screens/scr_nav.c) sets on every button. */
static bool gesture_glue_press_is_interactive(lv_obj_t *obj)
{
    for (lv_obj_t *o = obj; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) {
            return true;
        }
    }
    return false;
}

/* The one place a recognised gesture turns into an actual effect —
 * gated on the takeover check (this header's own doc comment explains
 * why this is done HERE, at dispatch, rather than by refusing to feed
 * samples). */
static void gesture_glue_on_recognized(ff_gesture_glue_ctx_t *ctx, ff_gesture_kind_t kind)
{
    if (kind == FF_GESTURE_NONE) {
        return;
    }
    ff_app_state_t const *view = ff_shell_view(ctx->sh);
    if (gesture_glue_takeover_active(view)) {
        return;
    }

    switch (kind) {
    case FF_GESTURE_BACK:
    case FF_GESTURE_HOME: {
        /* No click, no scroll continues for the widget under the
         * finger — this header's own top comment has the mechanism. */
        lv_indev_wait_release(ctx->indev);
        ff_sound_emit(FF_SOUND_TAP); /* respects ui_ticks downstream, ff_shell_sound_sink */
        ff_intent_t const in = {.kind = (kind == FF_GESTURE_BACK) ? FF_INTENT_BACK : FF_INTENT_HOME, .u = {0}};
        ff_shell_intent(ctx->sh, &in);
        break;
    }
    case FF_GESTURE_LONG_PRESS: {
        ff_intent_t const in = {.kind = FF_INTENT_QUICK_FLARE, .u = {0}};
        ff_shell_intent(ctx->sh, &in);
        break;
    }
    default:
        break;
    }
}

/* Re-armed at every DOWN: geometry (flip-aware) and G3's face/
 * interactive-widget gate, both computed fresh — see this header's own
 * doc comment on why nothing here needs to be "un-armed" mid-touch. */
static void gesture_glue_on_pressed(ff_gesture_glue_ctx_t *ctx, lv_obj_t *pressed_obj, int16_t x, int16_t y,
                                     uint32_t now_ms)
{
    ff_app_state_t const *view = ff_shell_view(ctx->sh);
    bool const flip = (view != NULL) && view->settings.screen_flip;
    ctx->g.cfg.cx = (int16_t)ff_theme_glass_cx(flip);
    ctx->g.cfg.cy = (int16_t)ff_theme_glass_cy(flip);

    bool const on_radar = (view != NULL) && (view->active_face == FF_APP_FACE_RADAR);
    bool const interactive = gesture_glue_press_is_interactive(pressed_obj);
    ff_gesture_set_long_press(&ctx->g, on_radar && !interactive);

    ff_gesture_kind_t const k = ff_gesture_feed(&ctx->g, true, x, y, now_ms);
    gesture_glue_on_recognized(ctx, k); /* always NONE for a DOWN sample, per ff_gesture.h's own contract */
}

static void gesture_glue_event_cb(lv_event_t *e)
{
    ff_gesture_glue_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL) {
        return;
    }
    lv_event_code_t const code = lv_event_get_code(e);

    if (code == LV_EVENT_DELETE) {
        /* The indev this ctx is tied to is going away — see this
         * header's Lifetime section. */
        lv_timer_delete(ctx->timer);
        free(ctx);
        return;
    }

    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) {
        return; /* LV_EVENT_PRESSING never reaches here — see this file's top comment */
    }

    /* For an event pushed via lv_indev_send_event (which is how PRESSED/
     * RELEASED reach an indev-level lv_indev_add_event_cb listener like
     * this one), the TARGET is the indev itself and the PARAM is the
     * pressed lv_obj_t — the reverse of the far more common object-level
     * event shape lv_event_get_indev()'s own implementation assumes.
     * Verified empirically against the vendored LVGL 9.5 sources (a
     * throwaway probe: lv_event_get_target() on a callback registered
     * via lv_indev_add_event_cb reads back the indev pointer; calling
     * lv_event_get_indev() from that SAME callback returns the pressed
     * object instead and silently yields garbage (-1,-1) from
     * lv_indev_get_point on it) — using lv_event_get_indev() here would
     * have been a real, silent bug. */
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    uint32_t const now_ms = ff_shell_now_ms(ctx->sh);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_t *pressed_obj = (lv_obj_t *)lv_event_get_param(e);
        gesture_glue_on_pressed(ctx, pressed_obj, (int16_t)p.x, (int16_t)p.y, now_ms);
    } else { /* LV_EVENT_RELEASED */
        ff_gesture_kind_t const k = ff_gesture_feed(&ctx->g, false, (int16_t)p.x, (int16_t)p.y, now_ms);
        gesture_glue_on_recognized(ctx, k); /* always NONE for an UP sample, per ff_gesture.h's own contract */
    }
}

static void gesture_glue_timer_cb(lv_timer_t *timer)
{
    ff_gesture_glue_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    uint32_t const now_ms = ff_shell_now_ms(ctx->sh);

    /* Stand-in for LV_EVENT_PRESSING (this file's top comment): poll the
     * indev's OWN current point/state directly — safe to call outside
     * an event callback, and independent of whichever indev happened to
     * fire most recently. Only feeds a MOVE sample for a touch THIS
     * glue's own PRESSED handler already started (`g.touch_active`) —
     * never lets a race with the event callback initiate a fresh touch
     * from here with stale (pre-DOWN) geometry/gating. */
    if (ctx->g.touch_active) {
        lv_point_t p;
        lv_indev_get_point(ctx->indev, &p);
        bool const down = (lv_indev_get_state(ctx->indev) == LV_INDEV_STATE_PRESSED);
        ff_gesture_kind_t const k = ff_gesture_feed(&ctx->g, down, (int16_t)p.x, (int16_t)p.y, now_ms);
        gesture_glue_on_recognized(ctx, k);
    }

    ff_gesture_kind_t const k = ff_gesture_tick(&ctx->g, now_ms);
    gesture_glue_on_recognized(ctx, k);
}

void ff_gesture_glue_attach(lv_indev_t *indev, ff_shell_t *sh)
{
    if (indev == NULL || sh == NULL) {
        return;
    }

    ff_gesture_glue_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->sh = sh;
    ctx->indev = indev;

    ff_gesture_cfg_t cfg;
    /* cx/cy are placeholders here — gesture_glue_on_pressed recomputes
     * them (flip-aware) fresh at every DOWN before they are ever read. */
    ff_gesture_cfg_default(&cfg, FF_THEME_GLASS_CX, FF_THEME_GLASS_CY, FF_THEME_GLASS_R);
    ff_gesture_init(&ctx->g, &cfg);

    ctx->timer = lv_timer_create(gesture_glue_timer_cb, FF_GESTURE_GLUE_TIMER_MS, ctx);

    lv_indev_add_event_cb(indev, gesture_glue_event_cb, LV_EVENT_ALL, ctx);
}
