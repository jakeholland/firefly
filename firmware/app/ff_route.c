/**
 * ff_route.c — see ff_route.h for the contract and the reasoning behind
 * every rule implemented here (S16 slice a; extended to the 5-face
 * horizontal carousel — see ff_route.h's header note).
 */
#include <stddef.h> /* NULL */

#include "ff_route.h"

/* The swipe axis, in order. `ff_route_swipe`'s `dir` steps along THIS
 * array, which is why -1 is "toward Radar" and +1 "toward Settings" —
 * see ff_route_swipe's doc comment on why that is not a finger
 * direction.
 *
 * As of the horizontal-carousel rework this is the WHOLE face set the
 * user swipes between: Radar · Now · Signals · Map · Settings, left to
 * right. Map and Settings used to be modals reached off-axis (Map by a
 * vertical top-swipe, Settings by a nav long-press); they are ordinary
 * horizontal neighbours now. Compose is still absent on purpose: it is
 * the one remaining modal (reached from Signals' "+"), not a swipe
 * tile. */
static ff_app_face_t const k_swipe_axis[] = {
    FF_APP_FACE_RADAR,
    FF_APP_FACE_NOW,
    FF_APP_FACE_SIGNALS,
    FF_APP_FACE_MAP,
    FF_APP_FACE_SETTINGS,
};
#define K_SWIPE_AXIS_N ((int)(sizeof(k_swipe_axis) / sizeof(k_swipe_axis[0])))

/* Position of `f` on the swipe axis, or false if `f` is not on it at
 * all (Compose, NONE, FLARE, or a garbage value from an uninitialised
 * route). Returning false rather than defaulting to 0 keeps
 * `ff_route_swipe` a no-op on a corrupted route instead of silently
 * "repairing" base to Radar mid-gesture. */
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
     * five +1 swipes from Radar land back on Radar. */
    int next = idx + (int)dir;
    if (next < 0 || next >= K_SWIPE_AXIS_N) {
        return false;
    }

    r->base = k_swipe_axis[next];
    return true;
}

bool ff_route_goto(ff_route_t *r, ff_app_face_t f)
{
    if (r == NULL) {
        return false;
    }
    /* A modal suppresses the jump exactly as it suppresses swipe (AC2):
     * a long-press-to-Settings must not slide a half-typed Compose away.
     * `modal == FF_APP_FACE_NONE` is the whole predicate. */
    if (r->modal != FF_APP_FACE_NONE) {
        return false;
    }
    /* Only a swipe-axis face is a valid jump target — Compose/NONE/FLARE
     * are rejected the same way route_axis_index rejects them for swipe,
     * so a bad `f` is a no-op rather than a base set off the axis. */
    int idx;
    if (!route_axis_index(f, &idx)) {
        return false;
    }
    if (r->base == f) {
        return false; /* already there: no change */
    }
    r->base = f;
    return true;
}

bool ff_route_push_modal(ff_route_t *r, ff_app_face_t f)
{
    if (r == NULL) {
        return false;
    }
    /* Compose is now the ONLY modal face (Map and Settings joined the
     * swipe axis in the horizontal-carousel rework). Rejecting FLARE
     * here is load-bearing, not defensive tidiness: the takeover is not
     * something the route holds (see ff_route_visible), so accepting it
     * as a modal would put the same fact in two places — the desync this
     * module's whole shape exists to prevent. A swipe face (RADAR..
     * SETTINGS), NONE, and everything else are rejected too — Compose is
     * the one value that is a modal and nothing else. */
    if (f != FF_APP_FACE_COMPOSE) {
        return false;
    }
    /* Same base-validity rule as ff_route_swipe, and for a sharper
     * reason than symmetry (PR #36 review, D1). A route whose base is
     * off the swipe axis was never initialised; raising a modal over it
     * would MASK that, because the modal works perfectly — it renders,
     * it accepts input, it sends — right up until the user backs out of
     * it and lands on a broken route with no visible cause. That is the
     * exact opposite of the fail-visibly-and-immediately behaviour this
     * module's deliberately-invalid zero value exists to produce. */
    int base_idx;
    if (!route_axis_index(r->base, &base_idx)) {
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
    /* Deliberately NOT guarded on base validity, unlike push_modal
     * directly above — see that function's comment, and ff_route.h's
     * note on the asymmetry. push raises new state over an invalid
     * base and hides it; pop drains state and moves an invalid route
     * TOWARD the visible NONE the design wants surfaced. Guarding here
     * would trap a caller inside a modal it could never leave, which is
     * the masking failure again wearing the other mask. */
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
