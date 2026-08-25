/**
 * ff_route.h — where the next intent goes (S16 slice a).
 *
 * Spec: docs/specs/S16-app-shell.md, "App: routing".
 *
 * `ff_route_t` is the app's navigation state, and *only* that: which of
 * the three swipe faces is under the finger (`base`), and whether a
 * full-screen modal is covering it (`modal`). It is pure C11 — no LVGL,
 * no I/O, no core state — so every rule below is unit-testable without a
 * display (app/tests/test_route.c).
 *
 * ## Why app/, not core/
 * The obvious argument ("routing must be testable without LVGL") does
 * NOT select core/: `app/screens/` already holds four pure, LVGL-free,
 * unit-tested modules (`ff_layout`, `radar_layout`, `now_layout`,
 * `flare_fmt`). The deciding argument runs the other way — `core/` today
 * has zero knowledge that screens exist at all, and putting a face enum
 * there would make core's contents change every time a face is added.
 * So routing lives in app/, next to the enum it routes over. (S16 §"App:
 * routing", PR #34 review §2.)
 *
 * ## Why ff_app_face_t and not a routing-private enum
 * A parallel `ff_route_face_t` mirroring `ff_app_face_t` member-for-
 * member would re-open the DRIFT GUARD problem `ff_app_state.h:23-37`
 * records this repo paying for once already (two identical anonymous
 * structs under different typedef names, kept in sync by hand). So
 * `ff_app_face_t` is extended with the two members it lacked
 * (`FF_APP_FACE_NONE`, `FF_APP_FACE_FLARE`) and routing uses it. `[api]`.
 *
 * ## What this module deliberately does NOT own
 * - **The takeover.** It is `ff_flare_t.takeover_active`'s single fact,
 *   and `ff_flare_tick()` clears it autonomously on expiry. See
 *   `ff_route_visible()` for why caching a copy here would desync.
 * - **Gesture decoding.** `ff_route_swipe`'s `dir` is a route direction,
 *   not a finger direction. See its doc comment — getting this backwards
 *   produces an inverted UI that still passes every acceptance criterion.
 * - **Rendering.** `ff_route_visible()` answers "where does the next
 *   intent go?", an input-dispatch question. Renderers keep reading
 *   `flare.takeover_active` directly (S16 AC13).
 */
#ifndef FF_ROUTE_H
#define FF_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_app_state.h" /* ff_app_face_t — deliberately not a second enum */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The whole navigation state.
 *
 * There is deliberately **no `has_modal` flag**: `modal ==
 * FF_APP_FACE_NONE` is the entire "is a modal up" predicate. A separate
 * bool would give a 3-state lifecycle (no modal / Compose / Settings)
 * four representable combinations, two of which contradict each other —
 * exactly the shape PR #21's review ruled out for `now_state_t`, and the
 * same reasoning as `stage_color_valid` and `FF_FRESH_NEVER`: absence is
 * represented once, in the field itself.
 *
 * There is also deliberately **no `takeover` field** — see
 * `ff_route_visible()`.
 *
 * Zero-initialising an `ff_route_t` yields `{NONE, NONE}`, which is not
 * a valid route (`base` must be one of the three swipe faces); call
 * `ff_route_init()`. The zero value is left invalid on purpose rather
 * than being quietly defined as "RADAR, no modal", so a forgotten init
 * is a visible NONE rather than a plausible-looking Radar.
 */
typedef struct {
    /** The swipe face underneath: `FF_APP_FACE_RADAR`, `_NOW` or
     *  `_SIGNALS`. Never NONE, SETTINGS, COMPOSE or FLARE after init. */
    ff_app_face_t base;
    /** The modal covering `base`: `FF_APP_FACE_COMPOSE`,
     *  `FF_APP_FACE_SETTINGS`, `FF_APP_FACE_MAP`, or `FF_APP_FACE_NONE`
     *  for "no modal". None of these is a swipe tile of its own —
     *  Compose is reached from Signals' "+", Settings from a nav
     *  long-press, Map (S09 [api]) from Radar. */
    ff_app_face_t modal;
} ff_route_t;

/**
 * Resets `r` to the app's opening route: Radar, no modal.
 *
 * Radar is the interpretation call (S16 states no explicit initial
 * face): it is the leftmost tile, the face the mockups treat as home,
 * and the one AC1 phrases its bound check from ("from RADAR, swipe(-1)
 * returns false"). Noted in the PR body per CLAUDE.md's "if a spec is
 * ambiguous, note the interpretation" rule.
 *
 * NULL-safe (no-op).
 */
void ff_route_init(ff_route_t *r);

/**
 * Moves `base` one step along the swipe axis. Returns true iff the route
 * actually changed — every rejection below is a silent no-op, never a
 * partial mutation.
 *
 * **`dir` is a ROUTE direction, not a gesture direction.** `-1` means
 * *toward Radar*, `+1` means *toward Signals*, along the fixed order
 * `RADAR < NOW < SIGNALS`. The target decodes its own gesture into one
 * of these, and in every touch UI — including LVGL's own tileview — a
 * **rightward finger drag maps to `-1`**, because dragging the content
 * right brings the *previous* tile into view. Wiring `LV_DIR_RIGHT` to
 * `+1` yields a UI whose navigation is mirrored end to end while still
 * passing AC1 and AC2, since neither criterion mentions fingers.
 *
 * Rejected (returns false, `r` untouched):
 * - a modal is up — **any** modal suppresses swipe entirely (AC2). A
 *   horizontal drag must never slide the composer away and lose a
 *   half-typed message, and Settings gets the same protection so the
 *   rule is "a modal is up", not "Compose is up".
 * - `dir` is anything other than -1 or +1 (0, +2, a raw pixel delta…).
 * - the step would run off either end. Swipe is **bounded, not
 *   wrapping** (AC1): wrapping at 2 a.m. with one thumb means you never
 *   know which direction gets you home.
 * - `r` is NULL, or `r->base` is not one of the three swipe faces
 *   (an uninitialised or corrupted route stays put rather than being
 *   silently repaired to a guess).
 */
bool ff_route_swipe(ff_route_t *r, int8_t dir);

/**
 * Raises `f` as the modal over the current `base`. Returns true iff the
 * route changed.
 *
 * `f` must be `FF_APP_FACE_COMPOSE`, `FF_APP_FACE_SETTINGS` or
 * `FF_APP_FACE_MAP` (S09 [api]); anything else (a swipe face, NONE,
 * FLARE) is rejected. FLARE in particular is never a modal: the
 * takeover is not routed, it overrides — see `ff_route_visible()`.
 *
 * **Pushing while a modal is already up is rejected**, rather than
 * replacing it. There is one modal slot, not a stack, so "replace" would
 * silently discard a half-typed Compose draft — the loss AC2 exists to
 * prevent, through a different door. (S16 does not state this case;
 * interpretation noted in the PR body.) No real flow reaches it today:
 * Compose has no nav bar to long-press Settings from.
 *
 * **Pushing over an off-axis `base` is also rejected**, the same rule
 * `ff_route_swipe()` applies — because a modal is the one operation
 * that could hide a forgotten `ff_route_init()` instead of exposing it.
 * The composer would open, render and accept input perfectly normally;
 * the invalid route would only appear on the way back out, by which
 * point nothing points at the cause. (PR #36 review, D1.)
 */
bool ff_route_push_modal(ff_route_t *r, ff_app_face_t f);

/**
 * Drops the modal, revealing `base` unchanged. Returns true iff a modal
 * was up; false (no-op) when there was nothing to pop, so a stray BACK
 * intent on a bare face cannot be mistaken for a state change.
 *
 * Unlike `ff_route_push_modal()`, this does **not** check that `base`
 * is on the swipe axis, and the asymmetry is deliberate rather than an
 * oversight: `push` raises new state over an invalid base and conceals
 * it, while `pop` removes state and moves an invalid route toward the
 * visible NONE the zero value is designed to produce. Guarding here
 * would strand a caller inside a modal with no way out — the same
 * masking failure, from the other side. (PR #36 review, D1.)
 */
bool ff_route_pop_modal(ff_route_t *r);

/**
 * The face the next intent should be dispatched to: `FF_APP_FACE_FLARE`
 * when a takeover is up, else the modal if there is one, else `base`.
 *
 * **`takeover` is a parameter, not a field, and that is deliberate.**
 * `ff_flare_tick()` clears `takeover_active` on expiry all by itself,
 * with no route involved — so a cached copy in `ff_route_t` would
 * outlive the fact it copied and leave the shell dispatching to a
 * takeover that expired seconds ago. Passing it per call means the
 * answer cannot be staler than the caller's own read. Same rationale as
 * `ff_flare_on_flare_rx()` taking a plain `bool paired` instead of
 * holding the roster.
 *
 * A takeover **overrides without mutating**: `base` and `modal` are
 * byte-identical across the call (the parameter is `const`), so clearing
 * the takeover restores the exact prior face — Compose with its draft
 * intact, not `base` (AC3).
 *
 * Input dispatch must target this function's answer and **never
 * `base`** (AC3b): while a takeover is up, Compose receives no intents
 * at all, so a touch landing where SEND used to be does not send. That
 * is S10 Ruling 3's principle — a new arrival must not be swallowed by
 * the control it happens to land on — applied at the routing layer.
 *
 * FLARE is the one distinctive value this returns, and it is also the
 * one value AC13 forbids storing in `ff_app_state_t.active_face`. That
 * reads oddly and is worth stating plainly: this is a routing answer,
 * not a render instruction. Rendering keeps reading
 * `flare.takeover_active` directly (`targets/sim/face_dispatch.c`), so
 * the takeover stays one fact in one place.
 *
 * Returns `FF_APP_FACE_NONE` for a NULL route — the least-claiming
 * answer, and one no dispatcher will match a face against.
 */
ff_app_face_t ff_route_visible(ff_route_t const *r, bool takeover);

#ifdef __cplusplus
}
#endif

#endif /* FF_ROUTE_H */
