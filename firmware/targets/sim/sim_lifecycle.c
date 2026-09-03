/**
 * sim_lifecycle.c — see sim_lifecycle.h.
 */
#include "sim_lifecycle.h"

#include <stddef.h>

#include <SDL.h>

#include "face_dispatch.h"

void ff_sim_lifecycle_init(ff_sim_lifecycle_t *lc)
{
    if (lc == NULL) return;
    ff_idle_init(&lc->idle);
    ff_idle_touch_gate_init(&lc->touch_gate);
    lc->rebuild_pending = false;
    lc->rebuild_count = 0u;
}

ff_idle_state_t ff_sim_lifecycle_pump(ff_idle_t *idle, bool *rebuild_pending, uint32_t *rebuild_count,
                                       uint32_t now_ms, bool dirty, bool shell_wake, bool finger_down,
                                       bool keep_awake, bool sleep_inhibit, ff_app_state_t const *state)
{
    if (shell_wake) { /* S26(c)+(d) — a pushed banner wakes a dim/off screen. */
        ff_idle_input(idle, now_ms);
    }
    ff_idle_state_t const idle_state = ff_idle_tick(idle, now_ms, keep_awake, sleep_inhibit);

    /* Accumulate the dirty bit into the pending latch — never cleared by
     * skipping, only by an actual rebuild below (see this file's header
     * comment on ff_sim_lifecycle_pump for the full contract). */
    if (dirty) {
        *rebuild_pending = true;
    }

    bool const screen_blank = (idle_state == FF_IDLE_STATE_OFF) || (idle_state == FF_IDLE_STATE_SLEEP);
    if (*rebuild_pending && !finger_down && !screen_blank) {
        lv_obj_clean(lv_screen_active());
        ff_build_face_screen(state);
        *rebuild_pending = false;
        (*rebuild_count)++;
    }
    return idle_state;
}

/* Lazily-created, reused across calls — see sim_lifecycle.h's doc
 * comment on ff_sim_lifecycle_apply_blank_overlay. One per process:
 * every current window-mode caller (main.c, ff_demo_run.c) opens at
 * most one SDL window per run, same "single active session" convention
 * ctl_loop.c's own g_ctl_loop_ctx relies on. */
static lv_obj_t *s_blank_overlay = NULL;

void ff_sim_lifecycle_apply_blank_overlay(ff_idle_state_t state)
{
    bool const blank = (state == FF_IDLE_STATE_OFF) || (state == FF_IDLE_STATE_SLEEP);

    if (s_blank_overlay == NULL) {
        if (!blank) return; /* nothing to hide; don't create it just to hide it */
        lv_obj_t *top = lv_layer_top();
        s_blank_overlay = lv_obj_create(top);
        lv_obj_remove_style_all(s_blank_overlay);
        lv_obj_set_size(s_blank_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(s_blank_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_blank_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_blank_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_blank_overlay, 0, 0);
        lv_obj_set_style_radius(s_blank_overlay, 0, 0);
        lv_obj_clear_flag(s_blank_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_blank_overlay, LV_OBJ_FLAG_CLICKABLE); /* the pointer indev still reaches the real UI below it, so the wake-only gate (not this overlay) decides delivery */
    }

    if (blank) {
        lv_obj_clear_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_blank_overlay);
    } else {
        lv_obj_add_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void ff_sim_lifecycle_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ff_sim_lifecycle_pointer_ctx_t *pctx = lv_indev_get_user_data(indev);

    int x = 0, y = 0;
    Uint32 const buttons = SDL_GetMouseState(&x, &y);
    bool const pressed = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0u;
    uint32_t const now_ms = pctx->now_ms_cb();

    /* Same ordering ctl_loop.c's ctl_loop_pointer_read_cb documents and
     * relies on: consult the gate FIRST, against whatever state `idle`
     * is ALREADY in, THEN re-pin on a press sample — see that
     * function's doc comment for why the order matters. */
    bool const deliver = ff_idle_touch_gate(&pctx->lc->idle, &pctx->lc->touch_gate, now_ms, pressed);
    if (pressed) {
        ff_idle_input(&pctx->lc->idle, now_ms);
    }

    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = (pressed && deliver) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
