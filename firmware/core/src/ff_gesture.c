/**
 * ff_gesture.c — see ff_gesture.h for the contract this implements.
 */
#include "ff_gesture.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check */

void ff_gesture_cfg_default(ff_gesture_cfg_t *cfg, int16_t cx, int16_t cy, int16_t r)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->cx = cx;
    cfg->cy = cy;
    cfg->r  = r;
    cfg->back_rim_px     = 44; /* ff_gesture.h's "Edge tolerance" section — widened from a shared rim_px=28 */
    cfg->home_rim_px     = 64; /* ditto — widened from a shared rim_px=28 */
    cfg->back_travel_px  = 56;
    cfg->home_travel_px  = 64;
    cfg->axis_lock_px    = 24;
    cfg->window_ms       = 500;
    cfg->long_ms         = 1200;
    cfg->long_slop_px    = 12;
    cfg->long_press_enabled = false; /* the glue arms this per active face */
    cfg->stall_gap_ms   = 150; /* ff_gesture.h's "Stall tolerance" section */
    cfg->edge_slop_px   = 16;  /* ff_gesture.h's "Edge tolerance" section */
    cfg->panel_size_px  = 412; /* ff_gesture.h's "Edge tolerance, part 3" section; matches FF_THEME_PUCK_PX */
}

void ff_gesture_init(ff_gesture_t *g, const ff_gesture_cfg_t *cfg)
{
    if (g == NULL) return;
    memset(g, 0, sizeof(*g));
    if (cfg != NULL) {
        g->cfg = *cfg;
    }
    g->phase = FF_GESTURE_PHASE_IDLE;
}

void ff_gesture_set_long_press(ff_gesture_t *g, bool enabled)
{
    if (g == NULL) return;
    g->cfg.long_press_enabled = enabled;
}

/* in_circle — squared-distance compare, no sqrt needed: dist^2 <= r^2.
 * Padded by cfg->edge_slop_px (ff_gesture.h's "Edge tolerance" section)
 * — admission only; every OTHER use of cfg->r (the rim-zone formulas)
 * stays exact. */
static bool gesture_in_circle(ff_gesture_cfg_t const *cfg, int16_t x, int16_t y)
{
    int32_t const ddx = (int32_t)x - (int32_t)cfg->cx;
    int32_t const ddy = (int32_t)y - (int32_t)cfg->cy;
    int32_t const dist_sq = ddx * ddx + ddy * ddy;
    int32_t const r = (int32_t)cfg->r + (int32_t)cfg->edge_slop_px;
    return dist_sq <= r * r;
}

/* gesture_down_admitted — the full DOWN-time admission gate (ff_gesture.h's
 * "Edge tolerance, part 3" section). A DOWN is admitted if it is a sane
 * panel coordinate AND (it lands in the padded glass circle OR it lands
 * in one of the two rim zones G1/G2 already key off, bounded on the
 * PERPENDICULAR axis to the circle's own unpadded span so a corner touch
 * that merely shares one coordinate with a real rim touch — S28_AC9 —
 * is still rejected). The rim-zone thresholds here are exactly
 * `back_alive`/`home_alive`'s own formulas; nothing downstream changes
 * shape because of this — a rim-admitted touch is disqualified by axis
 * lock/ratio/window exactly as it always was. */
static bool gesture_down_admitted(ff_gesture_cfg_t const *cfg, int16_t x, int16_t y)
{
    if (x < 0 || x >= cfg->panel_size_px || y < 0 || y >= cfg->panel_size_px) {
        /* Not a real panel coordinate at all — e.g. the driver's
         * invalid-point sentinel (-1,-1), or a raw 0xFFFF already
         * narrowed to int16_t -1 by the caller. */
        return false;
    }

    if (gesture_in_circle(cfg, x, y)) {
        return true;
    }

    int32_t const back_rim_edge = (int32_t)cfg->cx - (int32_t)cfg->r + (int32_t)cfg->back_rim_px;
    bool const in_back_rim =
        ((int32_t)x <= back_rim_edge) &&
        ((int32_t)y >= (int32_t)cfg->cy - (int32_t)cfg->r) &&
        ((int32_t)y <= (int32_t)cfg->cy + (int32_t)cfg->r);
    if (in_back_rim) {
        return true;
    }

    int32_t const home_rim_edge = (int32_t)cfg->cy + (int32_t)cfg->r - (int32_t)cfg->home_rim_px;
    bool const in_home_rim =
        ((int32_t)y >= home_rim_edge) &&
        ((int32_t)x >= (int32_t)cfg->cx - (int32_t)cfg->r) &&
        ((int32_t)x <= (int32_t)cfg->cx + (int32_t)cfg->r);
    return in_home_rim;
}

ff_gesture_kind_t ff_gesture_feed(ff_gesture_t *g, bool down, int16_t x, int16_t y, uint32_t now_ms)
{
    if (g == NULL) {
        return FF_GESTURE_NONE;
    }

    if (!g->touch_active) {
        if (!down) {
            /* A stray RELEASED sample with no matching press in
             * progress (e.g. the very first sample this indev ever
             * delivers). Nothing to do. */
            return FF_GESTURE_NONE;
        }

        /* DOWN edge: a brand-new touch starts here. */
        g->touch_active = true;
        g->x0 = x;
        g->y0 = y;
        g->t0 = now_ms;
        g->last_x = x;
        g->last_y = y;
        g->back_threshold_evaluated = false;
        g->home_threshold_evaluated = false;
        g->stall_checked = false;

        if (!gesture_down_admitted(&g->cfg, x, y)) {
            /* S28_AC9 — a DOWN admitted by neither the glass circle nor
             * either rim zone can never become G1/G2/G3. Latched for
             * this touch's whole lifetime; only the matching UP clears
             * it. */
            g->phase = FF_GESTURE_PHASE_ABORTED;
            g->back_alive = false;
            g->home_alive = false;
            g->long_alive = false;
            return FF_GESTURE_NONE;
        }

        g->phase = FF_GESTURE_PHASE_TRACKING;
        g->back_alive = (x <= (int16_t)(g->cfg.cx - g->cfg.r + g->cfg.back_rim_px));
        g->home_alive = (y >= (int16_t)(g->cfg.cy + g->cfg.r - g->cfg.home_rim_px));
        g->long_alive = g->cfg.long_press_enabled;
        return FF_GESTURE_NONE; /* a DOWN sample itself never recognises anything */
    }

    /* touch_active: this sample is either a MOVE (down still true) or
     * the matching UP (down now false). */
    if (!down) {
        g->touch_active = false;
        g->phase = FF_GESTURE_PHASE_IDLE;
        return FF_GESTURE_NONE; /* BACK/HOME are mid-drag only; nothing fires on release itself */
    }

    g->last_x = x;
    g->last_y = y;

    if (g->phase != FF_GESTURE_PHASE_TRACKING) {
        /* Already DONE (a gesture already fired this touch, S28_AC8) or
         * ABORTED (S28_AC9) — every further sample until UP is a no-op. */
        return FF_GESTURE_NONE;
    }

    /* Stall tolerance (ff_gesture.h's own section on this) — checked
     * ONCE, on the very first sample fed after DOWN, never again for
     * this touch. A gap this large proves the DEVICE, not the finger,
     * was slow (a stalled poll loop — real bench evidence, see that
     * header section), so the window's own clock is moved forward to
     * this sample rather than charging the stall against `window_ms`.
     * Only `t0` moves; `x0`/`y0` (the touch's real spatial origin) are
     * untouched, so `dx`/`dy` below still measure genuine displacement
     * from where the finger actually started (S28_AC11). A normal gap
     * (<= stall_gap_ms) changes nothing (S28_AC12 — this is a stall
     * exception, not a general timing relaxation). */
    if (!g->stall_checked) {
        g->stall_checked = true;
        uint32_t const gap_ms = now_ms - g->t0; /* wraparound-safe: same subtraction convention ff_time_reached itself uses */
        if (gap_ms > (uint32_t)g->cfg.stall_gap_ms) {
            g->t0 = now_ms;
        }
    }

    int32_t const dx  = (int32_t)x - (int32_t)g->x0;
    int32_t const dy  = (int32_t)y - (int32_t)g->y0;
    int32_t const adx = (dx < 0) ? -dx : dx;
    int32_t const ady = (dy < 0) ? -dy : dy;

    if (g->long_alive) {
        int32_t const dist_sq = dx * dx + dy * dy;
        int32_t const slop = (int32_t)g->cfg.long_slop_px;
        if (dist_sq > slop * slop) {
            g->long_alive = false; /* S28_AC7 — moved too far to still be a long press */
        }
    }

    /* --- G1 BACK ------------------------------------------------- */
    if (g->back_alive && !g->back_threshold_evaluated) {
        if (dx < (int32_t)g->cfg.back_travel_px) {
            if (ady > (int32_t)g->cfg.axis_lock_px) {
                /* S28_AC3 — off-axis travel before reaching the
                 * threshold reads as a scroll, not a swipe. */
                g->back_alive = false;
            }
        } else {
            g->back_threshold_evaluated = true; /* evaluated exactly once, per this header's own doc comment */
            bool const ratio_ok = ((float)ady) <= 0.6f * (float)dx;
            bool const in_window = !ff_time_reached(now_ms, g->t0 + (uint32_t)g->cfg.window_ms);
            if (ratio_ok && in_window) {
                g->phase = FF_GESTURE_PHASE_DONE;
                return FF_GESTURE_BACK;
            }
            g->back_alive = false; /* S28_AC4 (too slow) or S28_AC6 (ratio) */
        }
    }

    /* --- G2 HOME --------------------------------------------------
     * `up` is the upward travel so far (positive = finger moved up,
     * toward the negative-y direction the spec's "dy <= -home_travel_px"
     * describes). */
    if (g->home_alive && !g->home_threshold_evaluated) {
        int32_t const up = -dy;
        if (up < (int32_t)g->cfg.home_travel_px) {
            if (adx > (int32_t)g->cfg.axis_lock_px) {
                g->home_alive = false;
            }
        } else {
            g->home_threshold_evaluated = true;
            bool const ratio_ok = ((float)adx) <= 0.6f * (float)up;
            bool const in_window = !ff_time_reached(now_ms, g->t0 + (uint32_t)g->cfg.window_ms);
            uint32_t const elapsed_ms = now_ms - g->t0; /* wraparound-safe: same subtraction convention as ff_time_reached */
            bool const speed_ok = ((float)up) >= 0.25f * (float)elapsed_ms;
            if (ratio_ok && in_window && speed_ok) {
                g->phase = FF_GESTURE_PHASE_DONE;
                return FF_GESTURE_HOME;
            }
            g->home_alive = false;
        }
    }

    return FF_GESTURE_NONE;
}

ff_gesture_kind_t ff_gesture_tick(ff_gesture_t *g, uint32_t now_ms)
{
    if (g == NULL) {
        return FF_GESTURE_NONE;
    }
    if (!g->touch_active || g->phase != FF_GESTURE_PHASE_TRACKING) {
        return FF_GESTURE_NONE;
    }
    if (!g->long_alive || !g->cfg.long_press_enabled) {
        return FF_GESTURE_NONE;
    }
    if (!ff_time_reached(now_ms, g->t0 + (uint32_t)g->cfg.long_ms)) {
        return FF_GESTURE_NONE;
    }

    g->phase = FF_GESTURE_PHASE_DONE;
    return FF_GESTURE_LONG_PRESS;
}
