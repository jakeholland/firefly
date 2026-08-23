/**
 * ff_flare.h — S10 (slice a): the flare come-find-me state machine.
 *
 * Spec: docs/specs/S10-flare.md (this slice: "state machine + tests";
 * takeover/sender UI is slice b, gated on the S06 app shell). See that
 * spec's `## Amendments` (2026-08-23, PR #15) for the three product
 * rulings this shape implements — summarized in the "Independent state",
 * "Intent-aware dismiss/release" and "Judgment calls" sections below.
 *
 * Pure C11, no I/O, no allocation, no clock-reading of its own: every entry
 * point that needs "now" takes it as an explicit `uint32_t now_ms`
 * parameter rather than storing an `ff_clock_t` — the caller (app tick,
 * or a meshclient rx callback) always already has that timestamp in hand
 * at the point it calls in, and this keeps `ff_flare_t` a plain,
 * dependency-free struct (same "explicit now_ms in, no hidden clock"
 * shape `ff_radar_compute` uses in S06 for the same reason). Safe on the
 * stack or in a static.
 *
 * ## Independent state (PR #15 review, HIGH + MEDIUM findings; ruling
 * recorded in docs/specs/S10-flare.md's Amendments)
 * The original slice-a shape had one "current flare" slot shared between
 * "am I sending" and "am I being flared at" — review found this silently
 * broke `ff_flare_send_cancel()` the moment anyone else's flare arrived
 * mid-send (state got overwritten, so cancel became a permanent no-op for
 * the rest of that send's duration) and, separately, let a fresh incoming
 * flare silently yank an already-`GO`'d LOCKED selection out from under
 * the user mid-walk — exactly the "confident but wrong arrow" failure this
 * project exists to prevent (CLAUDE.md: "honest data over pretty data").
 * Both were ruled real bugs, not framing issues, and fixed by making
 * outbound send, the pending takeover, and the navigation lock three
 * independent fields on `ff_flare_t`:
 *  - `sending`/`send_expiry_ms` — my own outbound flare. Set only by
 *    `ff_flare_send_begin`, cleared only by `ff_flare_send_cancel` or its
 *    own expiry in `ff_flare_tick`. **Nothing inbound ever touches this**
 *    — `ff_flare_send_cancel()` now always works regardless of any
 *    incoming flare, which is exactly the HIGH finding's fix.
 *  - `takeover_active`/`takeover_node_id`/`takeover_expiry_ms` — the
 *    full-screen takeover currently awaiting a GO/DISMISS decision, if
 *    any. A new paired FLARE always wins this slot ("newest flare wins
 *    the takeover", spec) — unconditionally, regardless of `sending` or
 *    `locked_node_id` below.
 *  - `locked_node_id`/`locked_expiry_ms` — the node navigation is
 *    actually committed to (0 = not locked). Set only by `ff_flare_go()`
 *    (which also consumes/clears the pending takeover), cleared only by
 *    `ff_flare_release_lock()`, its own expiry, or a matching FLARE_END.
 *    **A newly-arriving takeover never touches this** — the MEDIUM
 *    finding's fix: an established lock is never silently replaced. It
 *    can coexist with a *different* pending takeover (Kev flares while
 *    I'm locked-and-walking toward Dana: I still see Kev's takeover, my
 *    arrow keeps pointing at Dana underneath until I explicitly decide).
 *
 * `ff_flare_result_t` did not need a second field to "express an outbound
 * intent and a UI takeover in the same tick": `intent` is the only thing
 * that ever needs to be *acted on* out-of-band (a network send), and it
 * is now fully orthogonal to receive state, since receiving no longer
 * touches sending at all — `ff_flare_on_flare_rx` never produces an
 * intent, and `ff_flare_tick` already inspects all three independent
 * deadlines (`send_expiry_ms`/`takeover_expiry_ms`/`locked_expiry_ms`) in
 * one call, applying every expiry that's due and returning
 * `FF_FLARE_INTENT_SEND_FLARE_END` if the send-side one fired. Whatever
 * the UI needs to render is always just a direct read of `ff_flare_t`'s
 * fields (fully-defined struct, not opaque) — nothing about that needs to
 * ride inside the result too.
 *
 * ## Intent-aware dismiss/release (PR #15 review, approved with a
 * recommendation taken immediately rather than deferred — see this
 * header's top comment and docs/specs/S10-flare.md's Amendments, third
 * entry, 2026-08-23)
 * The first pass at Ruling 2 gave `ff_flare_dismiss()` double duty:
 * "dismiss the pending takeover if one exists, else release the lock."
 * Reviewer's concrete repro: if a user taps "stop navigating" (intending
 * to release the lock) at the same instant a new flare arrives, that
 * single overloaded call silently takes the takeover-dismiss branch —
 * the user's actual intent (release the lock) is dropped, AND the new
 * takeover is swallowed unseen (never shown, never decided). A
 * mode-dependent API can't tell those two intents apart; only the caller
 * knows which one it meant. Fixed by splitting into two explicit,
 * single-purpose functions with no mode-sensing between them:
 *  - `ff_flare_dismiss_takeover()` — clears a pending takeover only. No
 *    effect on `locked_node_id` either way (present or absent).
 *  - `ff_flare_release_lock()` — releases the navigation lock only. No
 *    effect on `takeover_active` either way (present or absent) — so the
 *    race case above is now handled correctly: `locked_node_id` clears,
 *    `takeover_active` stays exactly as it was (still pending, still
 *    due to be shown) — nothing is silently swallowed.
 * This was taken as a same-slice fix (not deferred to a follow-up PR)
 * specifically because nothing in this repo calls `ff_flare_dismiss()`
 * yet (slice b/UI wiring hasn't started) — changing the signature now is
 * a pure addition-then-removal with zero call sites to update, where
 * doing it after slice b existed would have been a breaking `[api]`
 * change against real call sites.
 *
 * ## Dependency-light by design (documented per the task brief)
 *  - **No `ff_crew.h` include.** "Is this sender paired?" is passed in as
 *    a plain `bool paired` argument to `ff_flare_on_flare_rx` — the caller
 *    (app layer) is the one who already has an `ff_crew_t` and calls
 *    something like `ff_crew_upsert(...)->paired` before forwarding here.
 *    A callback/vtable seam was considered and rejected: pairing is a
 *    single cheap synchronous lookup with no I/O, so a callback would just
 *    be indirection without buying anything, and a plain `bool` is what
 *    keeps this module honestly zero-dependency (extraction-grade, like
 *    meshclient/ff_proto — see docs/ARCHITECTURE.md's reusable-libraries
 *    table) rather than merely *claiming* to be.
 *  - **No `ff_proto.h` include.** Decoding wire bytes into a `dur_s` (for
 *    FLARE) or recognizing FLARE_END is `ff_proto_decode`'s job, already
 *    done by the caller before it reaches this module; conversely, this
 *    module never emits wire bytes itself — `ff_flare_result_t.intent`
 *    tells the caller *what* to send (SEND_FLARE with `dur_s`, or
 *    SEND_FLARE_END), and the caller is the one who already owns
 *    `ff_proto_encode_flare`/`_flare_end` + `mc_send_private`. This is the
 *    "consume, don't duplicate" instruction from the task brief: the
 *    encode/decode logic lives in exactly one place (ff_proto.c), and this
 *    module only ever sees/produces already-parsed values.
 *  - **No `ff_crew.h`/meshclient dependency for the LOCKED selection
 *    either.** `ff_flare_locked_node()` returns the locked node id (or 0)
 *    for S06's radar/crew selection code to *consult* — this module never
 *    reaches into `ff_crew_t` to enforce the lock itself. Wiring
 *    `ff_crew_select_next` to no-op while locked (spec AC3) is S06's job
 *    once its shell exists; this slice only guarantees the accessor is
 *    correct (see test_flare.c's S10_AC3_* tests).
 *
 * ## Judgment calls (flagged per AGENTS.md; see the PR body for the same
 * notes)
 *  1. **Expiry boundary is INCLUSIVE.** `ff_flare_tick(f, now_ms)` treats
 *     `now_ms == expiry_ms` as already expired (fires each of the three
 *     independent expiries — send/takeover/lock — at that exact tick, not
 *     one tick later). Chosen to match `ff_crew_freshness`'s own "boundary
 *     rolls forward" convention (S02: age exactly 45000ms is already
 *     STALE, not LIVE) — "expiry" reads naturally as the instant a flare
 *     stops being valid, not the last instant it's still valid.
 *  2. **`ff_flare_send_begin` is unconditional** with respect to receive
 *     state (it always was, but is now trivially true rather than a
 *     special case — see "Independent state" above: sending and receiving
 *     share no field to conflict over in the first place).
 *  3. **A newly-received paired FLARE always wins the `takeover_active`
 *     slot** ("newest flare wins the takeover", spec) regardless of
 *     `sending` or an existing `locked_node_id` — see "Independent state"
 *     above for the full rationale and docs/specs/S10-flare.md's
 *     Amendments for the ruling.
 *  4. **`ff_flare_dismiss_takeover()` and `ff_flare_release_lock()` are
 *     separate, single-purpose functions, not one mode-dependent
 *     `dismiss()`.** See "Intent-aware dismiss/release" above — this
 *     superseded an earlier double-duty `ff_flare_dismiss()` in the same
 *     PR, before anything called it, precisely so the API never shipped
 *     the mode-sensing footgun the reviewer's race case describes. The
 *     spec's original "DISMISS -> IDLE" language (predating both Ruling 2
 *     and this split) now maps to whichever of the two the UI's DISMISS
 *     button means in context: dismissing a shown takeover calls
 *     `ff_flare_dismiss_takeover()`; a separate "stop navigating" affordance
 *     (if/when slice b adds one) calls `ff_flare_release_lock()`. Slice b
 *     decides the UI mapping; this module just refuses to guess at it.
 */
#ifndef FF_FLARE_H
#define FF_FLARE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default flare duration when the caller doesn't specify one (`dur_s ==
 * 0` passed to `ff_flare_send_begin`), per spec ("expiry = now + dur,
 * default 300 s"). Receive-side `dur_s` (from a decoded wire FLARE) is
 * always used as-is, never defaulted — see this header's top comment. */
#define FF_FLARE_DEFAULT_DUR_S ((uint16_t)300u)

/** Outbound network intent a transition wants the caller to act on. This
 * module never touches wire bytes or a transport — the caller already
 * owns `ff_proto_encode_flare`/`_flare_end` + `mc_send_private` (see this
 * header's top comment). */
typedef enum {
    FF_FLARE_INTENT_NONE = 0,
    FF_FLARE_INTENT_SEND_FLARE,     /* caller: ff_proto_encode_flare(..., dur_s), broadcast want_ack */
    FF_FLARE_INTENT_SEND_FLARE_END, /* caller: ff_proto_encode_flare_end(...), broadcast */
} ff_flare_intent_t;

/** Result of any transition entry point below. */
typedef struct {
    ff_flare_intent_t intent;
    uint16_t dur_s;    /* valid iff intent == FF_FLARE_INTENT_SEND_FLARE */
    bool should_alert; /* true iff the app should fire the flare haptic
                         * (3-long pattern) right now. This OVERRIDES
                         * quiet hours per spec — `ff_flare` doesn't know
                         * what quiet hours even are (no ff_settings
                         * dependency), so there is no quiet-hours branch
                         * here to skip: the caller must honor this flag
                         * unconditionally rather than running it through
                         * its own `ff_quiet_now` gate. The caller (app)
                         * owns the actual buzzing. Only ever set by
                         * ff_flare_on_flare_rx (a paired receive) — never
                         * by tick-driven expiry. */
} ff_flare_result_t;

/**
 * ff_flare_t — the whole flare state: three independent pieces (outbound
 * send, pending takeover, navigation lock — see this header's top comment
 * "Independent state" section for why they're separate). Fully-defined
 * (not opaque) so callers can put it on the stack or in a static with
 * zero heap allocation, and so the app can read any field directly to
 * render UI; zero-initialize or call `ff_flare_init` before use.
 */
typedef struct {
    /* Outbound: am I currently sending a flare? Independent of anything
     * inbound. */
    bool     sending;
    uint32_t send_expiry_ms; /* meaningful iff sending */

    /* The full-screen takeover currently pending a GO/DISMISS decision,
     * if any. */
    bool     takeover_active;
    uint32_t takeover_node_id;   /* meaningful iff takeover_active */
    uint32_t takeover_expiry_ms; /* meaningful iff takeover_active */

    /* The node my selection is actually locked to (navigating toward),
     * or 0 if not locked. */
    uint32_t locked_node_id;
    uint32_t locked_expiry_ms; /* meaningful iff locked_node_id != 0 */
} ff_flare_t;

/** ff_flare_init — zero a flare state machine (nothing sending, no
 * takeover pending, not locked). */
void ff_flare_init(ff_flare_t *f);

/**
 * ff_flare_send_begin — start (or restart) sending a flare: records
 * `send_expiry_ms = now_ms + dur_s*1000` (or `+ FF_FLARE_DEFAULT_DUR_S*1000`
 * if `dur_s == 0`) and sets `sending = true`. Always returns
 * FF_FLARE_INTENT_SEND_FLARE with the actual duration used (post-default)
 * in `dur_s`. Never reads or writes `takeover_*`/`locked_*` — completely
 * independent of receive state (see this header's top comment).
 */
ff_flare_result_t ff_flare_send_begin(ff_flare_t *f, uint16_t dur_s, uint32_t now_ms);

/**
 * ff_flare_send_cancel — cancel an in-progress send: `sending` -> false,
 * returning FF_FLARE_INTENT_SEND_FLARE_END exactly once. A no-op
 * (FF_FLARE_INTENT_NONE, no state change) when not currently sending —
 * including a second call right after the first, which is what guarantees
 * "emits FLARE_END once, not twice" (spec AC1). Always works regardless
 * of `takeover_active`/`locked_node_id` (the HIGH-finding fix, PR #15) —
 * this never reads or writes them.
 */
ff_flare_result_t ff_flare_send_cancel(ff_flare_t *f);

/**
 * ff_flare_on_flare_rx — handle an already-decoded incoming FLARE
 * (`ff_proto_decode`'s FF_PROTO_TYPE_FLARE case) from `node_id`, sent for
 * `dur_s` seconds, received at `now_ms`.
 *
 * `paired` is the caller's answer to "is this sender in my crew?" (see
 * this header's top comment) — if false, the message is ignored entirely:
 * no state change, FF_FLARE_INTENT_NONE, should_alert=false. This is the
 * unpaired-sender guard from the spec's Receive section.
 *
 * If `paired`, this always sets `takeover_active = true` with
 * `takeover_node_id`/`takeover_expiry_ms` from this message —
 * unconditionally, discarding whatever takeover was previously pending
 * ("newest flare wins the takeover", spec). Never reads or writes
 * `sending`/`send_expiry_ms` (HIGH finding fix) or `locked_node_id`/
 * `locked_expiry_ms` (MEDIUM finding fix) — see this header's top comment.
 * should_alert is always true on this path (haptic override, spec AC2).
 */
ff_flare_result_t ff_flare_on_flare_rx(ff_flare_t *f, uint32_t node_id, bool paired, uint16_t dur_s, uint32_t now_ms);

/**
 * ff_flare_on_flare_end_rx — handle an already-decoded incoming
 * FLARE_END from `node_id`. Clears whichever of the pending takeover or
 * the navigation lock currently belongs to exactly this `node_id` (either,
 * both, or neither can match — they're independent). A no-op on any field
 * that doesn't match `node_id`, including doing nothing at all if neither
 * matches (FLARE_END from a node that's neither the pending takeover nor
 * the lock is ignored, spec AC1).
 */
ff_flare_result_t ff_flare_on_flare_end_rx(ff_flare_t *f, uint32_t node_id);

/**
 * ff_flare_go — user pressed GO on the pending takeover: consumes it into
 * the lock (`locked_node_id`/`locked_expiry_ms` <- `takeover_node_id`/
 * `takeover_expiry_ms`, `takeover_active` -> false), REPLACING any
 * previous lock — GO is an explicit user decision, so a deliberate switch
 * is allowed even though a passive newly-arriving takeover alone is not
 * (MEDIUM finding: "GO explicitly switches to B"). No-op if no takeover
 * is pending.
 */
ff_flare_result_t ff_flare_go(ff_flare_t *f);

/**
 * ff_flare_dismiss_takeover — user dismissed the currently pending
 * takeover: `takeover_active` -> false, clearing `takeover_node_id`/
 * `takeover_expiry_ms`. Has NO effect on `locked_node_id`/
 * `locked_expiry_ms` either way — present or absent, they are simply not
 * read or written (MEDIUM finding: "DISMISS returns to the intact A
 * lock"; see this header's "Intent-aware dismiss/release" section for why
 * this is a separate function from `ff_flare_release_lock` rather than
 * one mode-dependent call). Per spec, the feed item itself is NOT cleared
 * by this — feed ownership is out of scope for this module entirely
 * (S08), so there is nothing here that could clear it anyway. A no-op if
 * no takeover is pending.
 */
ff_flare_result_t ff_flare_dismiss_takeover(ff_flare_t *f);

/**
 * ff_flare_release_lock — user released the navigation lock (e.g. a
 * "stop navigating" affordance): `locked_node_id` -> 0, clearing
 * `locked_expiry_ms`. Has NO effect on `takeover_active`/
 * `takeover_node_id`/`takeover_expiry_ms` either way — present or absent,
 * they are simply not read or written. This is what makes the reviewer's
 * race case safe: releasing the lock at the exact instant a new flare's
 * takeover is pending clears the lock and leaves that takeover fully
 * intact and still due to be shown, rather than one call silently
 * consuming the other's intent (see this header's "Intent-aware
 * dismiss/release" section). A no-op if not currently locked.
 */
ff_flare_result_t ff_flare_release_lock(ff_flare_t *f);

/**
 * ff_flare_tick — periodic expiry check, called with the current clock
 * reading. Evaluates all three independent deadlines in one call:
 *  - `sending` past `send_expiry_ms` auto-ends (-> `sending = false`,
 *    FF_FLARE_INTENT_SEND_FLARE_END).
 *  - `takeover_active` past `takeover_expiry_ms` expires (-> false; no
 *    outbound intent — a receiver has nothing to announce when someone
 *    else's flare times out).
 *  - `locked_node_id != 0` past `locked_expiry_ms` unlocks (-> 0; spec
 *    AC3: "unlock on expiry restores cycling").
 * Each check is independent — any subset (including all three, or none)
 * can fire in a single call depending on which deadlines are due. A no-op
 * for any field already at its resting state, or before its deadline. See
 * this header's judgment call (1) for the inclusive boundary: `now_ms ==
 * expiry_ms` already fires. Wraparound-safe against `now_ms`/`*_expiry_ms`
 * uint32_t rollover, matching `ff_clock_t`'s documented convention.
 */
ff_flare_result_t ff_flare_tick(ff_flare_t *f, uint32_t now_ms);

/**
 * ff_flare_locked_node — the node this puck's selection is currently
 * locked to, or 0 if not locked. For S06's radar/crew selection code to
 * consult (e.g. `ff_crew_select_next` should no-op while this is nonzero)
 * — this module never reaches into `ff_crew` itself (see top comment).
 */
uint32_t ff_flare_locked_node(ff_flare_t const *f);

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_H */
