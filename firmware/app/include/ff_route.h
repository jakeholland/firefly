/**
 * ff_route.h — where the next intent goes (S16 slice a).
 *
 * Spec: docs/specs/S16-app-shell.md, "App: routing".
 *
 * `ff_route_t` is the app's navigation state, and *only* that: which
 * face is under the finger (`base`), and whether a full-screen modal is
 * covering it (`modal`). It is pure C11 — no LVGL, no I/O, no core
 * state — so every rule below is unit-testable without a display
 * (app/tests/test_route.c).
 *
 * ## The horizontal carousel (scroll-vs-swipe rework)
 * Faces are ONE left-to-right sequence — Radar · Now · Signals · Map ·
 * Settings — navigated by horizontal swipe only. Map and Settings used
 * to be modals reached off the swipe axis (Map by a vertical top-swipe
 * on the tileview, Settings by a nav long-press), which is exactly why a
 * vertical list-scroll that a scrollable didn't fully claim could bubble
 * up as a GESTURE and jump to Map. Making every face a horizontal
 * neighbour lets vertical drags be scroll and nothing else: the swipe
 * axis below now holds all five, and Compose is the sole remaining
 * modal. `[api]` — this changes `base`'s and `modal`'s value ranges and
 * `ff_route_push_modal`'s accepted set, and adds `ff_route_goto`.
 *
 * ## S26 slice b [api] — a second modal face
 * `FF_APP_FACE_POWER_MENU` (docs/specs/S26-device-lifecycle.md "(b) Power
 * button -> power menu -> soft power-off") joins Compose as a value
 * `ff_route_push_modal` accepts — the PWR-button long-press menu. It is
 * NOT reached by swipe (it never joins the axis below), and it does not
 * change `base`'s value range; only `modal`'s and `ff_route_push_modal`'s
 * accepted set move. Every "Compose is the sole modal face" statement
 * elsewhere in this file predates this slice.
 *
 * ## S26 slice e, amended 2026-09-01 [api] — the launcher IS home
 * `docs/specs/S26-device-lifecycle.md`, "Nav model (slice e)" (amended
 * with the same date — the maintainer's on-glass decision after field
 * use supersedes that section's original "Radar is the watchface, the
 * launcher is a transient hub" model): BOOT (GPIO0) is the home button,
 * and home is the launcher itself, not Radar.
 *
 *  - `FF_APP_FACE_LAUNCHER` is a **BASE face** — the opening one
 *    (`ff_route_init` sets `base = FF_APP_FACE_LAUNCHER`) — not a modal.
 *    `ff_route_push_modal` no longer accepts it: nothing ever pushes the
 *    launcher, because `base` can simply equal it. This is a REVERSAL of
 *    this slice's original cut, which made `FF_APP_FACE_LAUNCHER` a
 *    third `ff_route_push_modal`-accepted value reached only from
 *    `base == RADAR`; every doc paragraph below describing the launcher
 *    as a modal predates this amendment.
 *  - Radar gets **no special handling**. It is an ordinary member of
 *    the five-face swipe axis and an ordinary launcher circle —
 *    `ff_route_launcher_select` accepts it exactly like the other four.
 *    The original cut's "Radar is not a circle, it IS home" rule is gone
 *    with the watchface concept it protected.
 *  - `ff_route_home()` is now simply "set `base` to the launcher": a
 *    no-op if `base` already is the launcher (BOOT on the launcher does
 *    nothing), else an unconditional overwrite of `base` — repairing an
 *    invalid `base` rather than masking it, the same "the TARGET's
 *    validity is what's checked, not the current base's" reasoning
 *    `ff_route_goto` already uses for its own jump — suppressed by any
 *    live modal (Compose, Power menu) exactly as before.
 *  - `ff_route_push_modal` pushing `FF_APP_FACE_POWER_MENU` (or
 *    `FF_APP_FACE_COMPOSE`, though nothing reaches Compose from the
 *    launcher today) over a `base == FF_APP_FACE_LAUNCHER` route now
 *    just works, through the ordinary one-modal-slot path (the base-
 *    validity guard `ff_route_push_modal` applies now treats LAUNCHER as
 *    a valid base, alongside the five swipe-axis faces) — no special
 *    "replace" case is needed, because the launcher no longer occupies
 *    the modal slot POWER_MENU would otherwise have had to fight for.
 *    The PR #142-era "POWER_MENU replaces a live LAUNCHER modal" rule
 *    and its tests are gone with the modal they were about; a power
 *    menu opened from the launcher and then cancelled simply reveals
 *    the launcher again, the same way it reveals any other base face.
 *  - The launcher's own transient auto-dismiss timeout
 *    (`FF_LAUNCHER_TIMEOUT_MS`, formerly in `ff_shell.h`/`ff_shell.c`)
 *    is REMOVED along with its four tests: home does not auto-dismiss.
 *    It is not a hub you pass through any more; it is where the device
 *    rests, and it is not a keep-awake source either (unchanged — see
 *    `ff_shell_keep_awake`'s own comment).
 *
 * This slice (both the original cut and this amendment) also RETIRES
 * the carousel as a live navigation mechanism: `scr_nav.c` no longer
 * emits `FF_INTENT_SWIPE` (its gesture handler is gone, along with the
 * page-dot row it used to drive), so no user action can move `base`
 * along the swipe axis any more — `ff_route_swipe()` below is
 * UNCHANGED and still fully tested (its existing test suite stays
 * green — a deliberate scope call, not an oversight: see this header's
 * own top-of-function note on it), but it is now a dormant pure
 * primitive with no live caller anywhere in the app. `ff_route_goto()`
 * is likewise unchanged and stays dormant for `scr_nav.c`'s retired
 * long-press, but it DOES still have two live shell callers unrelated
 * to that retirement — the Settings long-press jump and the message-
 * banner-tap-to-Signals jump — both of which jump directly BETWEEN
 * swipe-axis faces and never touch the launcher, so neither is affected
 * by this amendment. Both primitives are kept rather than deleted so
 * their extensive coverage need not be rewritten for a navigation
 * change that lives entirely in the SHELL's dispatch decision
 * (`ff_shell.c`'s `FF_INTENT_SWIPE` case is now a no-op) and the
 * SCREEN's gesture wiring (removed), not in this module's own
 * behaviour. Interpretation call, noted per AGENTS.md — see the PR body.
 *
 * ## Why app/, not core/
 * The obvious argument ("routing must be testable without LVGL") does
 * NOT select core/: `app/screens/` already holds four pure, LVGL-free,
 * unit-tested modules (`ff_layout`, `radar_layout`, `lineup_layout`,
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
 * bool would give a 2-state lifecycle (no modal / Compose) three
 * representable combinations, one of which contradicts itself —
 * exactly the shape PR #21's review ruled out for `now_state_t`, and the
 * same reasoning as `stage_color_valid` and `FF_FRESH_NEVER`: absence is
 * represented once, in the field itself.
 *
 * There is also deliberately **no `takeover` field** — see
 * `ff_route_visible()`.
 *
 * Zero-initialising an `ff_route_t` yields `{NONE, NONE}`, which is not
 * a valid route (`base` must be `FF_APP_FACE_LAUNCHER` or one of the
 * five swipe faces); call `ff_route_init()`. The zero value is left
 * invalid on purpose rather than being quietly defined as "the
 * launcher, no modal", so a forgotten init is a visible NONE rather
 * than a plausible-looking home screen.
 */
typedef struct {
    /** `FF_APP_FACE_LAUNCHER` (S26 slice e, amended 2026-09-01 — the
     *  opening value, home), or one of the five swipe-axis faces:
     *  `FF_APP_FACE_RADAR`, `_NOW`, `_SIGNALS`, `_MAP` or `_SETTINGS`.
     *  Never NONE, COMPOSE, POWER_MENU or FLARE after init. */
    ff_app_face_t base;
    /** The modal covering `base`: `FF_APP_FACE_COMPOSE` (reached from
     *  Signals' "+") or `FF_APP_FACE_POWER_MENU` (S26 slice b, the
     *  PWR-button long-press menu) — or `FF_APP_FACE_NONE` for "no
     *  modal". Map and Settings are NOT modals (the horizontal-carousel
     *  rework); the launcher is NOT a modal either, as of the S26 slice
     *  e amendment above — it is a `base` value now, so it never
     *  appears here. */
    ff_app_face_t modal;
} ff_route_t;

/**
 * Resets `r` to the app's opening route: the launcher, no modal.
 *
 * S26 slice e, amended 2026-09-01: the launcher IS home, so it is what
 * the route opens on (this REVERSES the original S16-era choice of
 * Radar as the opening face, which predates the launcher entirely).
 * Radar is reached the same way every other app is — a launcher-circle
 * tap (`ff_route_launcher_select`) — with no special first-boot
 * treatment.
 *
 * NULL-safe (no-op).
 */
void ff_route_init(ff_route_t *r);

/**
 * Moves `base` one step along the swipe axis. Returns true iff the route
 * actually changed — every rejection below is a silent no-op, never a
 * partial mutation.
 *
 * **Dormant**: no live caller reaches this any more (S26 slice e retired
 * the swipe carousel; see this header's top comment) — its existing
 * coverage stays as-is, unchanged by the slice-e amendment. The launcher
 * is not on the swipe axis (see `ff_route_home`/`_launcher_select`
 * below for how it is actually reached), so a route parked on it is, by
 * this function's own "base off the axis" rule, rejected exactly like
 * any other off-axis base.
 *
 * **`dir` is a ROUTE direction, not a gesture direction.** `-1` means
 * *toward Radar*, `+1` means *toward Settings*, along the fixed order
 * `RADAR < NOW < SIGNALS < MAP < SETTINGS`. The target decodes its own
 * gesture into one of these, and in every touch UI — including LVGL's
 * own tileview — a **rightward finger drag maps to `-1`**, because
 * dragging the content right brings the *previous* tile into view.
 * Wiring `LV_DIR_RIGHT` to `+1` yields a UI whose navigation is mirrored
 * end to end while still passing AC1 and AC2, since neither criterion
 * mentions fingers.
 *
 * Rejected (returns false, `r` untouched):
 * - a modal is up — **any** modal suppresses swipe entirely (AC2). A
 *   horizontal drag must never slide the composer away and lose a
 *   half-typed message. Only Compose is a modal now, so in practice
 *   this is "Compose is up", but the rule stays phrased as "a modal is
 *   up" so a future modal inherits the protection for free.
 * - `dir` is anything other than -1 or +1 (0, +2, a raw pixel delta…).
 * - the step would run off either end. Swipe is **bounded, not
 *   wrapping** (AC1): wrapping at 2 a.m. with one thumb means you never
 *   know which direction gets you home. RADAR is the left bound,
 *   SETTINGS the right.
 * - `r` is NULL, or `r->base` is not one of the five swipe faces —
 *   `FF_APP_FACE_LAUNCHER` included (an uninitialised, corrupted, or
 *   launcher-parked route stays put rather than being silently
 *   repaired to a guess).
 */
bool ff_route_swipe(ff_route_t *r, int8_t dir);

/**
 * Jumps `base` directly to swipe face `f`, skipping the intermediate
 * steps `ff_route_swipe` would take. Returns true iff the route changed.
 *
 * This is the seam for a shortcut that reaches a far face in one gesture
 * rather than a run of swipes — the two LIVE callers are the nav
 * long-press that jumps straight to Settings, and the message-banner
 * tap that jumps straight to Signals; both jump between ordinary
 * swipe-axis faces and are unaffected by the S26 slice e amendment (the
 * launcher is not a valid `f` here — see below). It steps OUTSIDE the
 * bounded one-at-a-time axis on purpose, so it is a separate entry point
 * from `ff_route_swipe` rather than a `dir` value, and it carries the
 * same guards:
 *
 * Rejected (returns false, `r` untouched):
 * - a modal is up (AC2) — a jump must not slide a half-typed Compose
 *   away any more than a swipe may.
 * - `f` is not one of the five swipe faces (Compose, NONE, FLARE,
 *   `FF_APP_FACE_LAUNCHER`, or a garbage value) — a bad target is a
 *   no-op, never a base set off the axis. `base` validity is implied: a
 *   jump overwrites `base` outright, so an uninitialised route is
 *   *repaired* by a valid jump rather than masked (the push_modal
 *   hazard does not apply — there is no modal to back out of onto the
 *   broken base). `ff_route_home()` reuses exactly this "overwrite
 *   without checking the CURRENT base" shape for its own jump to the
 *   launcher, just with `FF_APP_FACE_LAUNCHER` as the one fixed target
 *   this function itself never accepts.
 * - `r->base` already equals `f` — nothing to change.
 * - `r` is NULL.
 */
bool ff_route_goto(ff_route_t *r, ff_app_face_t f);

/**
 * Raises `f` as the modal over the current `base`. Returns true iff the
 * route changed.
 *
 * `f` must be `FF_APP_FACE_COMPOSE` or `FF_APP_FACE_POWER_MENU` (S26
 * slice b) — the two modal faces since the horizontal-carousel rework
 * moved Map and Settings onto the swipe axis and the S26 slice e
 * amendment moved the launcher onto `base`; anything else (a swipe
 * face, `FF_APP_FACE_LAUNCHER`, NONE, FLARE) is rejected. FLARE in
 * particular is never a modal: the takeover is not routed, it
 * overrides — see `ff_route_visible()`.
 *
 * **Pushing while a modal is already up is rejected**, rather than
 * replacing it. There is one modal slot, not a stack, so "replace" would
 * silently discard a half-typed Compose draft — the loss AC2 exists to
 * prevent, through a different door. (S16 does not state this case;
 * interpretation noted in the PR body.) This now has a real caller on
 * both sides: a PWR long-press while Compose is open is rejected outright
 * (the draft survives, same as any other modal-suppression), and
 * `ff_shell.c`'s FF_INTENT_POWER_MENU_OPEN handler relies on exactly this
 * rejection rather than re-deriving "is a modal already up" itself.
 *
 * **Pushing over the launcher base works through this ordinary path,
 * with no special case** (S26 slice e, amended 2026-09-01): since the
 * launcher is a `base` value, not something occupying the modal slot,
 * a PWR long-press from the launcher pushes `FF_APP_FACE_POWER_MENU`
 * exactly as it would over any other base — the base-validity check
 * just below treats `FF_APP_FACE_LAUNCHER` as valid alongside the five
 * swipe-axis faces. Cancelling that power menu (`ff_route_pop_modal`)
 * reveals the launcher again, the same way it reveals any other base.
 * This retires the earlier "PR #142 review FAIL 2" exception, which
 * existed only because that cut made the launcher a modal the power
 * menu had to fight past — there is nothing left to fight past now.
 *
 * **Pushing over an off-axis, non-launcher `base` is rejected**, the
 * same rule `ff_route_swipe()` applies — because a modal is the one
 * operation that could hide a forgotten `ff_route_init()` instead of
 * exposing it. The composer would open, render and accept input
 * perfectly normally; the invalid route would only appear on the way
 * back out, by which point nothing points at the cause. (PR #36 review,
 * D1.)
 */
bool ff_route_push_modal(ff_route_t *r, ff_app_face_t f);

/**
 * Drops the modal, revealing `base` unchanged. Returns true iff a modal
 * was up; false (no-op) when there was nothing to pop, so a stray BACK
 * intent on a bare face cannot be mistaken for a state change.
 *
 * Unlike `ff_route_push_modal()`, this does **not** check that `base`
 * is on the swipe axis (or is the launcher), and the asymmetry is
 * deliberate rather than an oversight: `push` raises new state over an
 * invalid base and conceals it, while `pop` removes state and moves an
 * invalid route toward the visible NONE the zero value is designed to
 * produce. Guarding here would strand a caller inside a modal with no
 * way out — the same masking failure, from the other side. (PR #36
 * review, D1.)
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

/**
 * ff_route_home — S26 slice e [api], amended 2026-09-01: the WHOLE
 * BOOT-button decision (docs/specs/S26-device-lifecycle.md, "Nav model
 * (slice e)"). Returns true iff the route changed.
 *
 * The launcher IS home now, so this is simply "make `base` the
 * launcher":
 *
 *  - any modal is up (Compose, Power menu) -> rejected (no-op), the
 *    same suppression `ff_route_swipe`/`ff_route_goto` apply — a
 *    half-typed draft or an open power menu must not be yanked away by
 *    a home press any more than by a swipe.
 *  - no modal, `base == FF_APP_FACE_LAUNCHER` already -> no-op (BOOT on
 *    the launcher does nothing — there is nowhere "home-er" to go).
 *  - no modal, any other `base` -> `base` is set to
 *    `FF_APP_FACE_LAUNCHER` unconditionally, the same "overwrite the
 *    target without re-validating the CURRENT base" shape
 *    `ff_route_goto` already uses (so an uninitialised or corrupted
 *    route is repaired by a home press, not masked by one).
 *
 * `r` NULL is a no-op.
 */
bool ff_route_home(ff_route_t *r);

/**
 * ff_route_launcher_select — S26 slice e [api], amended 2026-09-01: a
 * tap on one of the launcher's app circles. Returns true iff the route
 * changed.
 *
 * Only meaningful while the launcher is showing with nothing over it
 * (`r->modal == FF_APP_FACE_NONE && r->base == FF_APP_FACE_LAUNCHER`) —
 * rejected otherwise, so a stray or late-arriving tap after the
 * launcher has already been left (or while a modal covers it) cannot
 * jump `base` out from under whatever IS actually showing.
 *
 * `f` must be one of the five swipe-axis faces — `FF_APP_FACE_RADAR`
 * included, as of this amendment: Radar is an ordinary launcher circle
 * now, ranked no differently from Now/Signals/Map/Settings (the
 * original cut's "Radar is not a circle; it IS home" rule is retired
 * with the watchface concept it protected). `FF_APP_FACE_LAUNCHER`
 * itself, Compose, NONE, FLARE and Power menu are all rejected — none
 * of them is something the launcher can navigate you TO. A rejected
 * `f` leaves `r` untouched, including the launcher itself — a bad tap
 * does not silently close it.
 *
 * On success this sets `base = f` directly (the launcher was already
 * `base`, so there is no separate face to pop underneath it — unlike
 * the original modal-launcher cut, there is no pop-then-goto pair to
 * build this out of any more).
 */
bool ff_route_launcher_select(ff_route_t *r, ff_app_face_t f);

#ifdef __cplusplus
}
#endif

#endif /* FF_ROUTE_H */
