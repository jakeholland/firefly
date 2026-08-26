/**
 * scr_nav.c — see scr_nav.h. Shell chrome only: no domain logic (which
 * face is "correct" to show is decided entirely by the caller-supplied
 * ff_app_state_t; this file only lays out LVGL objects for whichever
 * state it's handed).
 */
#include "scr_nav.h"

#include "ff_intent.h" /* S16c1 — the emit seam; see nav_long_press_cb */
#include "ff_theme.h"
#include "scr_flare.h" /* S10 slice b — lock chip + sender overlay */
#include "scr_now.h" /* S07b — ff_scr_now_build, the Now face */
#include "scr_radar.h"
#include "scr_signals.h" /* S08c */

/* Long-press-anywhere -> Settings: emits FF_INTENT_OPEN_SETTINGS through
 * the intent seam (S16 slice c1 — this replaces the stub S06 reserved).
 * Whether anything HAPPENS is the shell's decision, not this file's: as
 * of S11 slice b the Settings renderer exists and `ff_shell_intent`
 * pushes the modal (see ff_shell.c's `k_settings_renderer_exists`,
 * flipped in that slice) — this screen just reports the gesture and
 * stays a pure renderer, unchanged by that flip. Unbound (goldens/
 * headless), the emit is a no-op.
 *
 * REACHABILITY (S16 slice c3, PR #54 review, HIGH — fixed here). A real
 * finger's press still resolves to the tileview/tile beneath it, not to
 * this object: `lv_indev_search_obj` walks to the deepest CLICKABLE hit
 * under the point, and the full-size tileview built below still covers
 * the puck. That half of the topology is unchanged, and
 * test_scr_intent.c's reachability probe confirms it's still true rather
 * than assuming it. What's fixed is what happens next: the tileview and
 * its three tiles (container objects ONLY — see ff_scr_nav_build) now
 * carry LV_OBJ_FLAG_EVENT_BUBBLE, so an unhandled LONG_PRESSED bubbles
 * tile -> tileview -> here. Deliberately NOT a blanket flag on every
 * clickable descendant: content buttons (FLARE, the reply chips, ...)
 * never get it, so a long-press ON one of those stays confined to the
 * button — the exact "would also re-route the tile content's own
 * clicks" failure a bare EVENT_BUBBLE everywhere would cause. */
static void nav_long_press_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_OPEN_SETTINGS, .u = {0}};
    ff_intent_emit(&in);
}

/* A horizontal drag on the tileview, now that its own native
 * drag-to-scroll is disabled (LV_DIR_NONE on every tile — S16 slice c3,
 * see ff_scr_nav_build): LVGL resolves an unclaimed drag as a GESTURE
 * instead of a scroll. Every LVGL object with a parent carries
 * LV_OBJ_FLAG_GESTURE_BUBBLE by default (`lv_obj.c`'s constructor), so a
 * gesture starting on a tile (there's nothing else to start it on — the
 * tileview fills the puck) walks up and is delivered here, on the
 * tileview itself — the tileview's OWN copy of that flag is explicitly
 * cleared in `ff_scr_nav_build`, which is what actually stops the walk
 * here instead of it continuing straight through the puck to the bare
 * screen, where nothing would ever receive it.
 *
 * `ff_route`'s dir convention is a ROUTE direction, not a finger
 * direction (ff_route.h's own warning): -1 toward RADAR, +1 toward
 * SIGNALS, and a RIGHTWARD drag maps to -1. LVGL's gesture_dir names the
 * direction the content was dragged, the same sense, so LV_DIR_RIGHT ->
 * -1 and LV_DIR_LEFT -> +1.
 *
 * LV_DIR_TOP (S09 [api], PR #73 review finding #4 — the Map face shipped
 * with a complete renderer and no way to reach it, which both
 * independent reviewers correctly refused to wave through as "under-
 * claiming honestly": a screen nobody can open is not a v1 face, it's
 * dead code) opens Map instead of moving `base` — see
 * `ff_app_state.h`'s `FF_APP_FACE_MAP` comment and this slice's spec
 * Amendment for the full routing rationale. Deliberately the LEAST
 * invasive entry point available: this handler is the ONLY thing that
 * changes, so it adds ZERO drawn pixels to any existing radar/now/
 * signals golden (verified: all 30 pre-existing goldens stayed
 * byte-identical through this fix round) — a visible tap-target
 * affordance was considered and deferred, see issue #76 and the PR
 * reply for why. Works from any of the three tiles, not gated to Radar
 * specifically: the shell's own routing already treats Map uniformly as
 * a modal over whatever base is current (`ff_route_push_modal`), so
 * restricting the gesture to one tile would be an arbitrary limitation
 * this handler has no principled reason to add — and it means one swipe
 * reaches Map regardless of which face you're already on, not "swipe
 * back to Radar first, then swipe up".
 *
 * LV_DIR_BOTTOM remains unclaimed (not a face swipe, not Map) — no
 * product need for it yet, and claiming it speculatively would be
 * exactly the kind of invented behavior CLAUDE.md rules out. */
static void nav_swipe_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_dir_t const dir = lv_indev_get_gesture_dir(indev);

    if (dir == LV_DIR_TOP) {
        ff_intent_t open_map = {.kind = FF_INTENT_OPEN_MAP, .u = {0}};
        ff_intent_emit(&open_map);
        return;
    }

    int8_t route_dir;
    switch (dir) {
    case LV_DIR_RIGHT: route_dir = -1; break;
    case LV_DIR_LEFT: route_dir = 1; break;
    default: return; /* LV_DIR_BOTTOM / none: not a face swipe, not Map */
    }

    ff_intent_t in = {.kind = FF_INTENT_SWIPE, .u = {0}};
    in.u.swipe_dir = route_dir;
    ff_intent_emit(&in);
}

/* Page-dot row: chrome that sits ON the puck (not inside any one tile),
 * so it survives a swipe instead of scrolling away with the content —
 * created after (so drawn on top of) the tileview.
 *
 * `signals_unread_count`: S08 spec ("unread count drives a badge on the
 * page dot") — the Signals tile is always dot index 2 (fixed tile order
 * Radar/Now/Signals, see ff_scr_nav_build below), so this only ever
 * decorates that one dot, regardless of which tile is currently active. */
static void nav_build_page_dots(lv_obj_t *puck, uint32_t active_idx, uint16_t signals_unread_count)
{
    enum { N_DOTS = 3, SIGNALS_DOT_IDX = 2 };
    const int32_t dot_px = 10;
    const int32_t gap_px = 20;
    const int32_t total_w = N_DOTS * dot_px + (N_DOTS - 1) * gap_px;
    const int32_t start_x = -(total_w / 2) + dot_px / 2;

    for (uint32_t i = 0; i < N_DOTS; i++) {
        int32_t dx = start_x + (int32_t)i * (dot_px + gap_px);

        lv_obj_t *dot = lv_obj_create(puck);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, dot_px, dot_px);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == active_idx ? FF_THEME_COLOR_AMBER : FF_THEME_COLOR_DIM), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator, not a control */
        /* y=186: inset from the puck's 220px radius, comfortably inside
         * the circular silhouette at this width (matches the puck's own
         * geometry — see ff_theme.h's FF_THEME_PUCK_RADIUS_PX). */
        lv_obj_align(dot, LV_ALIGN_CENTER, dx, 186);

        if (i == SIGNALS_DOT_IDX && signals_unread_count > 0) {
            /* Small badge dot above-right of the Signals page dot —
             * visible even when that tile isn't the active one (that's
             * the whole point: it tells you there's something to look at
             * on a tile you're NOT currently looking at). Always amber
             * regardless of active/inactive dot color underneath, so it
             * reads consistently. */
            lv_obj_t *badge = lv_obj_create(puck);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, 8, 8);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(badge, LV_ALIGN_CENTER, dx + 8, 186 - 8);
        }
    }
}

void ff_scr_nav_build(ff_app_state_t const *state)
{
    if (state == NULL) {
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

    /* Long-press-anywhere -> Settings: emits an intent (S16c1); the shell
     * decides (rejected until the S11b renderer exists). The tileview
     * built below still covers this object — a real finger's press still
     * resolves there, not here — but it and its tiles bubble an unhandled
     * LONG_PRESSED up to here now; see nav_long_press_cb's doc comment. */
    lv_obj_add_flag(puck, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(puck, nav_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *tileview = lv_tileview_create(puck);
    lv_obj_remove_style_all(tileview);
    lv_obj_set_size(tileview, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    /* S16 slice c3: `ff_route` owns face navigation now, not the
     * tileview's own native drag-to-scroll — EVENT_BUBBLE is long-press
     * reachability (nav_long_press_cb's doc comment); the GESTURE handler
     * is what a horizontal drag becomes instead of a scroll, now that
     * every tile below passes LV_DIR_NONE (used to be LV_DIR_HOR).
     *
     * GESTURE_BUBBLE is explicitly CLEARED here, not set — `lv_obj.c`'s
     * constructor already sets it on every object with a parent by
     * default (a fact this file learned the hard way: adding it to the
     * tiles below did nothing, since it was already there, and the walk
     * kept bubbling straight through the tileview and the puck to the
     * bare screen, which nothing listens on). Clearing it here is what
     * actually stops the walk AT the tileview, where the handler below
     * is registered — the tiles keep the default (no explicit flag
     * needed) so a gesture starting on either one still reaches here. */
    lv_obj_add_flag(tileview, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(tileview, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(tileview, nav_swipe_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* S17 slice b (AC2), PR #86 code review, should-fix: each tile is a
     * plain lv_obj (lv_tileview_add_tile), which defaults CLICKABLE like
     * every LVGL base object — never explicitly cleared here before, the
     * identical class of bug scr_signals.c's own header comment already
     * documents fixing for its container primitives ("several purely
     * decorative containers... were left CLICKABLE by omission"). Harmless
     * on its own (no LV_EVENT_CLICKED handler was ever attached to a
     * tile), but it meant all THREE tiles — the two currently scrolled
     * OFF-screen at their native, un-scrolled tileview-grid positions,
     * hundreds of px outside the window, plus the one currently in view —
     * were live entries in the AC2 adjacency sweep once that check went
     * global (this slice's own cousins-blind-spot fix): three
     * puck-sized, edge-touching rects that used to be silently exempted
     * by the OLD sibling-scoped check's whole-puck-size heuristic,
     * surfacing as spurious HIT-TARGETS-TOO-CLOSE findings between tiles
     * that were never independently reachable by a real touch in the
     * first place (only one tile is ever visible at a time; the other two
     * sit off the physical display entirely). These are pure layout/swap
     * surfaces — "the tileview's three tiles stay purely as the swap
     * surface `lv_tileview_set_tile_by_index` jumps between" (ISSUE #29's
     * own comment, right below) — never controls a thumb aims at, so
     * CLICKABLE is cleared on all three explicitly rather than relying on
     * a size-shaped exemption to paper over it. Long-press-to-Settings
     * reachability (nav_long_press_cb's own doc comment; verified, not
     * assumed, by test_scr_intent.c's `S16_c3_physical_long_press_on_
     * empty_puck_space_reaches_open_settings`) is unaffected: a touch
     * over empty tile space now resolves one level up, at the still-
     * CLICKABLE `tileview` (which still carries LV_OBJ_FLAG_EVENT_BUBBLE),
     * and bubbles to `puck` exactly as before — one hop shorter, same
     * destination. */
    lv_obj_t *tile_radar = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_NONE);
    lv_obj_set_style_pad_all(tile_radar, 0, 0);
    lv_obj_set_style_bg_opa(tile_radar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_radar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile_radar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile_radar, LV_OBJ_FLAG_EVENT_BUBBLE); /* GESTURE_BUBBLE: already on by default */

    lv_obj_t *tile_now = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_NONE);
    lv_obj_set_style_pad_all(tile_now, 0, 0);
    lv_obj_set_style_bg_opa(tile_now, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_now, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile_now, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile_now, LV_OBJ_FLAG_EVENT_BUBBLE); /* GESTURE_BUBBLE: already on by default */

    lv_obj_t *tile_signals = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_NONE);
    lv_obj_set_style_pad_all(tile_signals, 0, 0);
    lv_obj_set_style_bg_opa(tile_signals, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile_signals, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile_signals, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile_signals, LV_OBJ_FLAG_EVENT_BUBBLE); /* GESTURE_BUBBLE: already on by default */

    /* ISSUE #29, closed: build content into the ACTIVE tile ONLY. Before
     * S16 slice d, all three tiles' content was built on every render —
     * harmless while a screen only ever built once per process, but
     * three faces' worth of LVGL objects on every dirty-driven rebuild is
     * real, unbounded-per-tick cost, and the two inactive tiles' controls
     * sat at their un-scrolled tileview position: hundreds of pixels
     * outside the 456px window, not merely outside the round glass —
     * unreachable by any real touch, and the hit-target sweep
     * (test_face_hit_targets.c) had to carve out an exception for
     * exactly that. `ff_route` already drives face navigation and native
     * tileview swipe is disabled (LV_DIR_NONE, S16 slice c3), so nothing
     * depends on an inactive tile ever holding real content — the
     * tileview's three tiles stay purely as the swap surface
     * `lv_tileview_set_tile_by_index` jumps between (LV_ANIM_OFF: no
     * motion, so which tiles are populated is invisible to a real touch
     * either way). The tile index is resolved FIRST so exactly one
     * builder call fires. */
    uint32_t tile_idx;
    switch (state->active_face) {
    case FF_APP_FACE_RADAR:
        tile_idx = 0;
        break;
    case FF_APP_FACE_NOW:
        tile_idx = 1;
        break;
    case FF_APP_FACE_SIGNALS:
        tile_idx = 2;
        break;
    case FF_APP_FACE_SETTINGS:
    default:
        /* Settings has no tile of its own (reached by long-press, not
         * swipe) — default to Radar rather than an undefined tile. */
        tile_idx = 0;
        break;
    }

    switch (tile_idx) {
    case 0:
        ff_scr_radar_build(tile_radar, &state->radar, state->settings.colorblind);
        /* S10 slice b: the Radar face's lock chip. Added as a child of
         * tile_radar specifically (not the puck) — spec: "the Radar face
         * shows a lock indicator" — so it only ever appears alongside the
         * Radar tile's own content, not on Now/Signals. */
        ff_scr_flare_build_lock_chip(tile_radar, &state->flare);
        break;
    case 1:
        ff_scr_now_build(tile_now, &state->now); /* S07b */
        break;
    default:
        ff_scr_signals_build(tile_signals, &state->signals); /* S08c */
        break;
    }

    lv_tileview_set_tile_by_index(tileview, tile_idx, 0, LV_ANIM_OFF);

    /* PR #20 UX review (finding #4, BLOCKING — "flaring_self reads as an
     * error"): whatever face is showing underneath, its own headline-
     * shaped content (NOSEL's "NO CREW SELECTED", NOFIX's "NO FIX -
     * RADIO ONLY", a LIVE/STALE name label, ...) was drawn at full
     * opacity and visually outshouted the sender overlay's actual news
     * ("you are flaring"). Fixed STRUCTURALLY, at the tileview container
     * (one place), not per-renderer: dim the WHOLE base face when
     * sending, then draw the overlay at full opacity on top. This is
     * deliberately face-agnostic — it needs no "am I flaring" branch
     * threaded through scr_radar.c's per-mode renderers (or any future
     * Now/Signals screen's own headline), and unlike a per-label opacity
     * flag it can't miss a mode this reviewer didn't happen to check.
     * `lv_obj_set_style_opa` on a container blends its whole subtree as
     * one layer, so every child (status bar, arrow, dots, chips, ...)
     * dims together, not just top-level labels. */
    if (state->flare.sending) {
        lv_obj_set_style_opa(tileview, LV_OPA_30, 0);
    }

    nav_build_page_dots(puck, tile_idx, state->signals.unread_count);

    /* S10 slice b: the sender overlay, built LAST (on the puck itself,
     * not any one tile) so it paints on top of whichever tile/page-dots
     * are showing — spec: "own screen pulses amber" applies regardless
     * of which face is active. No-op internally when !state->flare.sending. */
    ff_scr_flare_build_sender_overlay(puck, &state->flare);
}
