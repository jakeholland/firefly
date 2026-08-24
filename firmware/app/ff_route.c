/**
 * ff_route.c — see ff_route.h for the contract and the reasoning behind
 * every rule implemented here (S16 slice a).
 */
#include "ff_route.h"

/* The swipe axis, in order. `ff_route_swipe`'s `dir` steps along THIS
 * array, which is why -1 is "toward Radar" and +1 "toward Signals" —
 * see ff_route_swipe's doc comment on why that is not a finger
 * direction. Settings and Compose are absent on purpose: neither is a
 * swipe tile (scr_nav.c's tileview only ever has these three), they are
 * modals over one of them. */
static ff_app_face_t const k_swipe_axis[] = {
    FF_APP_FACE_RADAR,
    FF_APP_FACE_NOW,
    FF_APP_FACE_SIGNALS,
};
#define K_SWIPE_AXIS_N ((int)(sizeof(k_swipe_axis) / sizeof(k_swipe_axis[0])))

/* Position of `f` on the swipe axis, or false if `f` is not on it at
 * all (a modal face, NONE, FLARE, or a garbage value from an
 * uninitialised route). Returning false rather than defaulting to 0
 * keeps `ff_route_swipe` a no-op on a corrupted route instead of
 * silently "repairing" base to Radar mid-gesture. */
static bool route_axis_index(ff_app_face_t f, int *out_idx)
{
    for (int i = 0; i < K_SWIPE_AXIS_N; i++) {
        if (k_swipe_axis[i] == f) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

void ff_route_init(ff_route_t *r)
{
    if (r == NULL) {
        return;
    }
    r->base = FF_APP_FACE_RADAR;
    r->modal = FF_APP_FACE_NONE;
}

bool ff_route_swipe(ff_route_t *r, int8_t dir)
{
    if (r == NULL) {
        return false;
    }

    /* AC2 — checked FIRST, before `dir` is even looked at: any modal
     * suppresses swipe entirely. `modal == FF_APP_FACE_NONE` is the
     * whole predicate (there is no has_modal flag by design). */
    if (r->modal != FF_APP_FACE_NONE) {
        return false;
    }

    if (dir != -1 && dir != 1) {
        return false;
    }

    int idx;
    if (!route_axis_index(r->base, &idx)) {
        return false;
    }

    /* AC1 — bounded, NOT wrapping. Modular arithmetic here would let
     * three +1 swipes from Radar land back on Radar. */
    int next = idx + (int)dir;
    if (next < 0 || next >= K_SWIPE_AXIS_N) {
        return false;
    }

    r->base = k_swipe_axis[next];
    return true;
}

bool ff_route_push_modal(ff_route_t *r, ff_app_face_t f)
{
    if (r == NULL) {
        return false;
    }
    /* Compose and Settings are the only modal faces. Rejecting FLARE
     * here is load-bearing, not defensive tidiness: the takeover is not
     * something the route holds (see ff_route_visible), so accepting it
     * as a modal would put the same fact in two places — the desync
     * this module's whole shape exists to prevent. */
    if (f != FF_APP_FACE_COMPOSE && f != FF_APP_FACE_SETTINGS) {
        return false;
    }
    /* One slot, not a stack: replacing a live modal would silently
     * discard a half-typed Compose draft. */
    if (r->modal != FF_APP_FACE_NONE) {
        return false;
    }
    r->modal = f;
    return true;
}

bool ff_route_pop_modal(ff_route_t *r)
{
    if (r == NULL || r->modal == FF_APP_FACE_NONE) {
        return false;
    }
    r->modal = FF_APP_FACE_NONE;
    return true;
}

ff_app_face_t ff_route_visible(ff_route_t const *r, bool takeover)
{
    if (r == NULL) {
        return FF_APP_FACE_NONE;
    }
    /* The takeover outranks both modal and base, and mutates neither —
     * `r` is const precisely so this is a fact of the signature rather
     * than a promise in a comment (AC3). */
    if (takeover) {
        return FF_APP_FACE_FLARE;
    }
    if (r->modal != FF_APP_FACE_NONE) {
        return r->modal;
    }
    return r->base;
}
