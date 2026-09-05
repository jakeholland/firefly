/**
 * scr_nav.c — see scr_nav.h. Shell chrome only: no domain logic (which
 * face is "correct" to show is decided entirely by the caller-supplied
 * ff_app_state_t; this file only lays out LVGL objects for whichever
 * state it's handed).
 *
 * S26 slice e (docs/specs/S26-device-lifecycle.md "(e) Home button +
 * launcher") RETIRES the horizontal swipe carousel as a navigation
 * mechanism: BOOT (via `scr_launcher.c`) is the one global nav gesture
 * now, so this file no longer emits `FF_INTENT_SWIPE` (no gesture
 * handler at all — see the header comment on why) and no longer emits
 * `FF_INTENT_OPEN_SETTINGS` (the long-press-anywhere-to-Settings hook is
 * gone; Settings is a launcher circle). The page-dot row is gone too —
 * the Signals unread badge it used to carry moved to the launcher's
 * Signals circle (`scr_launcher.c`).
 *
 * ## The tileview -> plain container swap, and why
 * The old `lv_tileview` gave this file two things: a native
 * drag-to-scroll it immediately disabled (`LV_DIR_NONE` on every tile,
 * S16 slice c3) and `lv_tileview_set_tile_by_index`'s scroll-to-tile as
 * the "which face is showing" swap mechanism. With the swipe GESTURE
 * handler gone too, nothing here still uses anything a tileview offers
 * over a bare container — issue #29 already established that only the
 * ACTIVE face's content is ever built (never all five), so "which tile"
 * was always just "which face", one value, decided once per build. A
 * plain non-scrollable `lv_obj_create` container is the SIMPLER of the
 * two ways to keep serving as that one swap surface (the other being
 * "keep the tileview, stop touching its gesture/tile-index machinery")
 * — chosen because it deletes real code (the five-tile array, the
 * per-tile flag setup, `lv_tileview_add_tile`/`_set_tile_by_index`)
 * rather than leaving inert tileview machinery behind for a future
 * reader to wonder whether it still matters.
 */
#include "scr_nav.h"

#include "ff_sound_emit.h" /* S27 — the TAP sound seam every button reports through */
#include "ff_theme.h"
#include "scr_banner.h" /* S26 slice d — the ff_notify message banner overlay */
#include "scr_flare.h" /* S10 slice b — lock chip + sender overlay */
#include "scr_map.h" /* Map — an ordinary base face */
#include "scr_lineup.h" /* S07b — ff_scr_lineup_build, the Now face */
#include "scr_radar.h"
#include "scr_settings.h" /* Settings — the launcher's fourth circle */
#include "scr_inbox.h" /* S08c */

/* ---------------------------------------------------------------------
 * ff_scr_button_create — see scr_nav.h's doc comment for the full
 * rationale. One call, no branching: every screen file's buttons go
 * through this instead of `lv_button_create` directly, so the
 * PRESS_LOCK fix (#145/#148) lives in exactly one place instead of
 * being re-discovered per screen.
 *
 * S27 amendment (docs/specs/S27-sounds.md, "Shell seam") — this is also
 * the ONE choke point every button in the app funnels through, which is
 * exactly the coverage a "UI tick on button press" needs: wiring
 * `ff_sound_emit(FF_SOUND_TAP)` here fires it for every button, on every
 * screen, with no per-screen code to write or forget. Fired on
 * LV_EVENT_CLICKED (a completed tap — press-down AND release inside the
 * target), not LV_EVENT_PRESSED: a press that turns into a scroll drag
 * (the Settings list's own "any press inside it initiates the scroll"
 * design, scr_settings.c) or is dragged off the control is not a tap and
 * must not tick. Emitted UNCONDITIONALLY — this file makes no gating
 * decision at all; see ff_sound_emit.h's top comment for where the
 * sounds_on/ui_ticks/quiet-hours policy is actually applied. */
static void ff_scr_button_tap_sound_cb(lv_event_t *e)
{
    (void)e;
    ff_sound_emit(FF_SOUND_TAP);
}

lv_obj_t *ff_scr_button_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(btn, ff_scr_button_tap_sound_cb, LV_EVENT_CLICKED, NULL);
    /* S28 amendment (docs/specs/S28-gestures.md, "G3 LONG-PRESS FLARE"):
     * the ONE tag every real button in the app carries, for the ONE
     * consumer that needs to tell "this is a widget a thumb aims at"
     * apart from "this is empty glass" without knowing anything about
     * any particular screen — the gesture glue (app/ff_gesture_glue.c)
     * refuses a long-press flare when the press landed on an object (or
     * an ancestor of one) carrying this flag, so a held FLARE/CANCEL/
     * settings-row press on the Radar face never also arms the panic
     * gesture underneath it. `LV_OBJ_FLAG_USER_1` is otherwise unused
     * anywhere in this codebase (grepped clean before choosing it) —
     * safe to repurpose as a plain boolean marker here. Set on every
     * button unconditionally (not just Radar's): a widget built on
     * another face today could be reachable from Radar tomorrow, and
     * this costs nothing to carry everywhere. */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);
    return btn;
}

/* ---------------------------------------------------------------------
 * S26 slice d (round 3, orchestrator review on #157): the WIDER banner
 * (scr_banner.c's own geometry comment) now reaches far enough to
 * overlap real controls underneath it on some faces — the Inbox
 * thread/picker/popup/rally sub-views' pinned BACK button
 * (FF_INBOX_BACK_Y/PX, scr_inbox.c) sits close enough to center that
 * the banner's width overlaps it.
 *
 * Round 2 masked CLICKABLE on ANY object the banner's rect touched at
 * all, on the claim "LVGL's hit-testing already routes taps there to
 * the banner". Review round 3 measured that claim and found it false
 * for a PARTIAL overlap: the thread BACK button's rect is (109,30)-
 * (153,74) against a banner of (128,36)-(288,84) — only the right 25 of
 * its 44px width sits under the strip. The left 19px is a REAL, visibly
 * exposed sliver LVGL would route straight to BACK, and round 2 was
 * silently eating a tap there for the banner's whole 6s life.
 *
 * The honest rule, replacing that claim: mask an object only when its
 * UNCOVERED REMAINDER cannot itself be a usable target — i.e. the
 * largest rectangular piece of it left outside the banner has width OR
 * height under FF_THEME_MIN_HIT_PX (44), the exact floor
 * test_face_hit_targets.c's own sweep already enforces for every OTHER
 * control on glass. An object whose remainder still clears 44px in BOTH
 * dimensions keeps its clickability — LVGL routes correctly on its own
 * (banner on the covered part, the object on its own uncovered part),
 * so masking it would only make a real target unreachable for no
 * reason. This is the single source of truth for "too small to be a
 * target" already; this rule doesn't invent a second one, it applies
 * the existing one to a REMAINDER rect instead of the object's own full
 * rect.
 *
 * Checked against every real overlap this repo currently ships: the
 * Inbox thread BACK button's largest remainder (the 19px-wide left
 * sliver above) fails the width floor -> masked, same outcome as round
 * 2 for that control, now for the true reason. The launcher's Inbox
 * satellite has an 88px-wide, ~37px-tall remainder below the strip ->
 * fails the HEIGHT floor -> masked too (scr_launcher.c calls this SAME
 * exported pass — see its own call site comment — rather than growing a
 * second copy of the walk). No shipped fixture currently produces a
 * "kept clickable" case — if one ever does, and its FULL (uncovered-
 * remainder-aside) rect still trips test_face_hit_targets.c's own
 * adjacency/containment check against the banner, that sweep is the
 * tie-breaker: mask the object rather than keep a control the sweep
 * itself calls unsafe. This pass only ever WIDENS what's reachable
 * versus round 2, never narrows it, so it cannot be the thing that
 * newly fails the sweep.
 *
 * The WALK is confined to this file (`ff_scr_nav_mask_clickables_under_
 * banner`, exported so `scr_launcher.c` can reuse the identical logic
 * rather than duplicating it) so no screen file needs to know about any
 * OTHER screen file's controls — `scr_inbox.c`/`scr_launcher.c` stay as
 * banner-unaware as `scr_banner.h`'s own "no face-awareness" contract
 * promises. The remainder GEOMETRY itself
 * (ff_scr_nav_rect_best_remainder/_remainder_clears_floor, below) is
 * additionally exposed purely for direct unit testing
 * (test_scr_banner.c), the same "expose the pure mechanic so a test can
 * name it precisely" convention `ff_scr_launcher_satellite_deg` already
 * set.
 * ------------------------------------------------------------------- */

static int32_t nav_rect_w(lv_area_t const *r)
{
    return r->x2 - r->x1 + 1; /* lv_area_t's x2/y2 are INCLUSIVE */
}

static int32_t nav_rect_h(lv_area_t const *r)
{
    return r->y2 - r->y1 + 1;
}

static bool nav_rects_overlap(lv_area_t const *a, lv_area_t const *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/**
 * ff_scr_nav_rect_best_remainder — the largest-by-area of the (up to)
 * four axis-aligned SLICES of `obj` left over once `cover` is
 * subtracted: the part of `obj` entirely above, below, left of, or
 * right of `cover`. Deliberately NOT full rectilinear polygon
 * subtraction (which can leave an L/U/ring-shaped remainder for a
 * `cover` that pokes into the middle of `obj` on two axes) — a single
 * "is there still one big-enough rectangular target here" answer is
 * the only question FF_THEME_MIN_HIT_PX cares about, and every real
 * case this file handles (a banner strip clipping one edge of a
 * button/satellite) is exactly the shape this model is exact for. A
 * slice candidate with non-positive width or height (`cover` doesn't
 * clip that side of `obj` at all) is never considered. If `obj` and
 * `cover` don't overlap, returns `*obj` unchanged — nothing to
 * subtract. If `cover` fully contains `obj` (or matches it exactly),
 * no slice survives and a degenerate zero-area rect is returned, which
 * ff_scr_nav_remainder_clears_floor correctly reads as "not a target".
 */
lv_area_t ff_scr_nav_rect_best_remainder(lv_area_t obj, lv_area_t cover)
{
    if (!nav_rects_overlap(&obj, &cover)) {
        return obj;
    }

    lv_area_t candidates[4];
    int n = 0;
    if (cover.y1 > obj.y1) { /* the slice above cover */
        candidates[n] = obj;
        candidates[n].y2 = cover.y1 - 1;
        n++;
    }
    if (cover.y2 < obj.y2) { /* below cover */
        candidates[n] = obj;
        candidates[n].y1 = cover.y2 + 1;
        n++;
    }
    if (cover.x1 > obj.x1) { /* left of cover */
        candidates[n] = obj;
        candidates[n].x2 = cover.x1 - 1;
        n++;
    }
    if (cover.x2 < obj.x2) { /* right of cover */
        candidates[n] = obj;
        candidates[n].x1 = cover.x2 + 1;
        n++;
    }
    if (n == 0) {
        lv_area_t zero = {0, 0, -1, -1}; /* width=height=0 via the +1 convention above */
        return zero;
    }

    lv_area_t best = candidates[0];
    int64_t best_area = (int64_t)nav_rect_w(&best) * (int64_t)nav_rect_h(&best);
    for (int i = 1; i < n; i++) {
        int64_t area = (int64_t)nav_rect_w(&candidates[i]) * (int64_t)nav_rect_h(&candidates[i]);
        if (area > best_area) {
            best = candidates[i];
            best_area = area;
        }
    }
    return best;
}

/**
 * ff_scr_nav_remainder_clears_floor — true iff `obj`'s largest remaining
 * slice after `cover` is subtracted (ff_scr_nav_rect_best_remainder)
 * still measures >= FF_THEME_MIN_HIT_PX in BOTH dimensions — the exact
 * bar test_face_hit_targets.c's own sweep already holds every other
 * control to. false means the remainder is too small to be its own
 * usable target, i.e. `obj` should be masked while `cover` sits over
 * it; true means it stays clickable (LVGL's own z-order hit-testing
 * routes a tap correctly between the two without any help from this
 * file).
 */
bool ff_scr_nav_remainder_clears_floor(lv_area_t obj, lv_area_t cover)
{
    lv_area_t rem = ff_scr_nav_rect_best_remainder(obj, cover);
    return nav_rect_w(&rem) >= FF_THEME_MIN_HIT_PX && nav_rect_h(&rem) >= FF_THEME_MIN_HIT_PX;
}

void ff_scr_nav_mask_clickables_under_banner(lv_obj_t *root, lv_obj_t *banner, lv_area_t const *banner_area)
{
    if (root == NULL || root == banner) {
        return; /* never touch the banner's own subtree */
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child == banner) {
            continue;
        }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if (nav_rects_overlap(&a, banner_area) && !ff_scr_nav_remainder_clears_floor(a, *banner_area)) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        ff_scr_nav_mask_clickables_under_banner(child, banner, banner_area);
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

    /* The one swap surface: a plain, non-scrollable, non-clickable
     * container — see this file's header comment for why this replaced
     * the tileview. Pure chrome, never a control a thumb aims at. */
    lv_obj_t *content = lv_obj_create(puck);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);

    /* Which base face to render — the same five-way switch issue #29
     * established (build the ACTIVE face's content only), now writing
     * straight into `content` instead of picking a tile index first.
     * `default` (COMPOSE/FLARE/POWER_MENU/LAUNCHER/NONE — none of which
     * the dispatcher routes here) falls back to Radar rather than an
     * undefined build. */
    switch (state->active_face) {
    case FF_APP_FACE_RADAR:
        /* fix/radar-lock-chip-clears-status-bar follow-up: `locked`
         * passed straight from `state->flare` — the SAME fact
         * `ff_scr_flare_build_lock_chip` below reads — so the compass
         * arrow's reach cap and the chip's own visibility can never
         * disagree about whether the chip is on screen. See
         * scr_radar.h's doc comment on that parameter. */
        ff_scr_radar_build(content, &state->radar, state->settings.colorblind, state->settings.screen_flip,
                            state->flare.locked);
        /* S10 slice b: the Radar face's lock chip — a child of the
         * Radar content specifically, so it only ever appears alongside
         * Radar's own content. */
        ff_scr_flare_build_lock_chip(content, &state->flare);
        break;
    case FF_APP_FACE_LINEUP:
        ff_scr_lineup_build(content, &state->now); /* S07b */
        break;
    case FF_APP_FACE_INBOX:
        ff_scr_inbox_build(content, &state->inbox, state->settings.colorblind); /* S22b */
        break;
    case FF_APP_FACE_MAP:
        ff_scr_map_build(content, &state->map, state->settings.colorblind);
        break;
    case FF_APP_FACE_SETTINGS:
        /* The fresh-entry scroll reset and the sim golden scroll hint
         * stay the dispatcher's job, wrapped around this build
         * (targets/sim/face_dispatch.c) — unchanged by this file's
         * tileview -> container swap. */
        ff_scr_settings_build(content, &state->settings);
        break;
    default:
        ff_scr_radar_build(content, &state->radar, state->settings.colorblind, state->settings.screen_flip,
                            state->flare.locked);
        ff_scr_flare_build_lock_chip(content, &state->flare);
        break;
    }

    /* PR #20 UX review (finding #4, BLOCKING — "flaring_self reads as an
     * error"): whatever face is showing underneath, its own headline-
     * shaped content (NOSEL's "NO CREW SELECTED", NOFIX's "NO FIX -
     * RADIO ONLY", a LIVE/STALE name label, ...) was drawn at full
     * opacity and visually outshouted the sender overlay's actual news
     * ("you are flaring"). Fixed STRUCTURALLY, at this container (one
     * place), not per-renderer: dim the WHOLE base face when sending,
     * then draw the overlay at full opacity on top. `lv_obj_set_style_opa`
     * on a container blends its whole subtree as one layer, so every
     * child (status bar, arrow, chips, ...) dims together, not just
     * top-level labels. */
    if (state->flare.sending) {
        lv_obj_set_style_opa(content, LV_OPA_30, 0);
    }

    /* S10 slice b: the sender overlay, built on the puck itself (not
     * `content`) so it paints on top of whatever face is showing —
     * spec: "own screen pulses amber" applies regardless of which face
     * is active. No-op internally when !state->flare.sending. */
    ff_scr_flare_build_sender_overlay(puck, &state->flare, state->settings.screen_flip);

    /* S26 slice d — the message banner, built LAST (on the puck, after
     * the sender overlay) so it paints on top of everything else this
     * function draws. No-op internally when !state->banner.active. Note:
     * this is unreachable while the flare TAKEOVER is up — the dispatcher
     * (face_dispatch.c / ff_face.c) returns before ever calling
     * ff_scr_nav_build in that case, so a banner never competes with the
     * takeover for the same glass. */
    ff_scr_banner_build(puck, &state->banner, state->settings.colorblind);

    if (state->banner.active) {
        /* The strip is the LAST child ff_scr_banner_build just added to
         * `puck` (its own doc comment's contract). Mask whatever it now
         * covers — see nav_mask_clickables_under_banner's own comment. */
        lv_obj_t *strip = lv_obj_get_child(puck, lv_obj_get_child_count(puck) - 1);
        lv_obj_update_layout(puck); /* coords are lazily computed — force it before reading any (S99 precedent) */
        lv_area_t strip_area;
        lv_obj_get_coords(strip, &strip_area);
        ff_scr_nav_mask_clickables_under_banner(puck, strip, &strip_area);
    }
}
